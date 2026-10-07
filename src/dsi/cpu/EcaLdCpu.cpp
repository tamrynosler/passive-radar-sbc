// ECA-LD on the CPU: batched cancellation with a Toeplitz Gram matrix, solved
// by Levinson-Durbin. The correctness reference for the GPU version, and the
// C++ copy of matlab/eca_ld.m and matlab/levinson_solve.m.
//
// ---------------------------------------------------------------------------
// What the algorithm does
// ---------------------------------------------------------------------------
//
// K = taps, L = batch length, B = CPI / L batches, M = the smallest power of
// two >= L + 2K - 2. For each batch (samples n0 .. n0+L-1):
//
//   1. Build three length-M vectors, zero elsewhere:
//        a  = reference, n0-K+1 .. n0+L-1   (zero before the CPI start)
//        dr = reference of the batch, placed at index K-1
//        ds = surveillance of the batch, placed at index K-1
//   2. A, DR, DS = FFT of each. cRef = IFFT(conj(A) DR), cCross = IFFT(conj(A)
//      DS), both divided by M. For lags k = 0..K-1 these are
//        rho[k]  = sum over the batch of conj(r[n-k]) r[n]
//        bvec[k] = sum over the batch of conj(r[n-k]) d[n]
//      (Lu et al. 2026, eqs. 18-24). M is large enough that nothing wraps.
//   3. Loading: rr[k] = rho[k], and rr[0] = Re(rho[0]) (1 + loading).
//   4. Solve R w = bvec, with R the Hermitian Toeplitz matrix of first column
//      rr, by the Levinson recursion for a general right-hand side (Golub &
//      Van Loan, Matrix Computations, Sec. 4.7), O(K^2). The paper's Table 2
//      has index slips, so the recursion was derived again and checked against
//      MATLAB's backslash (see levinson_solve.m).
//   5. out[n] = sur[n] - sum_k w[k] r[n-k]. The reference is zero before the
//      CPI start, so taps reaching before it contribute nothing (the rule of
//      CglsCpu's forwardT). A batch's taps do reach back into the previous
//      batch.
//
// ---------------------------------------------------------------------------
// What is exact and what is approximate
// ---------------------------------------------------------------------------
//
// rho and bvec are exact correlations of the batch. The true Gram matrix is
// X^H X with X(n,k) = r[n-k]; its first column is exactly rho, but its other
// entries differ from a Toeplitz matrix by edge terms (the sums start and end
// at slightly different samples). Replacing it by the Toeplitz matrix is the
// only approximation. It is good when L >> K and shrinks as L grows.
//
// ---------------------------------------------------------------------------
// The degenerate-batch guard
// ---------------------------------------------------------------------------
//
// The recursion divides by beta, the prediction error, which must stay
// positive. If it does not (an all-zero batch, or a numerically singular R),
// the weights of that batch are set to zero, so the output is the
// surveillance unchanged. The number of such batches is reported once per
// cancel() call on stderr.
//
// ---------------------------------------------------------------------------
// State and precision
// ---------------------------------------------------------------------------
//
// Stateless: weights are solved afresh for every batch and nothing is kept
// across cancel() calls. Precision as FblmsCpu: every operation in Acc (float
// with bFloat32Math, else double), every stored vector value held exactly in
// double. The FFT is HostFft.

#include "pbr/DSICanceller.hpp"
#include "pbr/check.hpp"
#include "../DsiInternal.hpp"
#include "HostFft.hpp"

#include <complex>
#include <cstdio>
#include <vector>

namespace pbr {

namespace {

typedef std::complex<double> ComplexAcc;

class EcaLdCpu : public DSICanceller {
public:
    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override
    {
        return moParams.bFloat32Math ? "ECA-LD (CPU FP32)" : "ECA-LD (CPU)";
    }

private:
    template <typename Acc>
    uint64_t cancelT(const ComplexF* pcRef, const ComplexF* pcSur, ComplexF* pcOut);

    // Solve R w = bvec by Levinson, R Hermitian Toeplitz of first column rr.
    // Result in mvcW. Returns false on a degenerate batch.
    template <typename Acc>
    bool levinson();

    DSIParams               moParams;
    HostFft                 moFft;
    uint64_t                muTaps    = 0;   // K
    uint64_t                muBatch   = 0;   // L
    uint64_t                muBatches = 0;   // B
    uint64_t                muLength  = 0;   // M

    // FFT scratch, length M.
    std::vector<ComplexAcc> mvcA;
    std::vector<ComplexAcc> mvcDr;
    std::vector<ComplexAcc> mvcDs;
    std::vector<ComplexAcc> mvcRef;      // IFFT(conj(A) DR)
    std::vector<ComplexAcc> mvcCross;    // IFFT(conj(A) DS)

    // Levinson work, length K.
    std::vector<ComplexAcc> mvcRr;
    std::vector<ComplexAcc> mvcBvec;
    std::vector<ComplexAcc> mvcW;        // x, then the weights
    std::vector<ComplexAcc> mvcY;
    std::vector<ComplexAcc> mvcYNew;

    bool                    mbReady = false;
};

bool EcaLdCpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "EcaLdCpu::init called twice");
    if (!ecaLdParamsOk(oParams))
        return false;

    moParams  = oParams;
    muTaps    = oParams.uClutterTaps;
    muBatch   = oParams.uEcaLdBatchSamples;
    muBatches = oParams.uCpiSamples / muBatch;

    muLength = 2;
    while (muLength < muBatch + 2 * muTaps - 2)
        muLength <<= 1;

    if (!moFft.init(muLength))
        return false;

    // Every allocation happens here and is reused for the life of the object.
    const size_t uM = static_cast<size_t>(muLength);
    const size_t uK = static_cast<size_t>(muTaps);
    mvcA.assign(uM, ComplexAcc());
    mvcDr.assign(uM, ComplexAcc());
    mvcDs.assign(uM, ComplexAcc());
    mvcRef.assign(uM, ComplexAcc());
    mvcCross.assign(uM, ComplexAcc());
    mvcRr.assign(uK, ComplexAcc());
    mvcBvec.assign(uK, ComplexAcc());
    mvcW.assign(uK, ComplexAcc());
    mvcY.assign(uK, ComplexAcc());
    mvcYNew.assign(uK, ComplexAcc());

    mbReady = true;
    return true;
}

// The recursion of matlab/levinson_solve.m, 0-based. x is mvcW, y is the
// Yule-Walker solution of the leading block, beta its real prediction error.
template <typename Acc>
bool EcaLdCpu::levinson()
{
    typedef std::complex<Acc> C;
    const uint64_t uK = muTaps;

    const Acc tRr0 = Acc(mvcRr[0].real());
    if (!(tRr0 > Acc(0)))
        return false;

    mvcW[0] = ComplexAcc(static_cast<double>(Acc(mvcBvec[0].real()) / tRr0),
                         static_cast<double>(Acc(mvcBvec[0].imag()) / tRr0));
    if (uK == 1)
        return true;

    const C tRr1(Acc(mvcRr[1].real()), Acc(mvcRr[1].imag()));
    const C tY0 = -tRr1 / tRr0;
    mvcY[0] = ComplexAcc(static_cast<double>(tY0.real()), static_cast<double>(tY0.imag()));
    Acc tBeta = tRr0 + (std::conj(tRr1) * tY0).real();

    for (uint64_t m = 1; m < uK; ++m) {
        if (!(tBeta > Acc(0)))
            return false;

        // mu = (bvec[m] - sum_{i<m} rr[m-i] x[i]) / beta
        C tSum(Acc(0), Acc(0));
        for (uint64_t i = 0; i < m; ++i)
            tSum += C(Acc(mvcRr[m - i].real()), Acc(mvcRr[m - i].imag())) *
                    C(Acc(mvcW[i].real()), Acc(mvcW[i].imag()));
        const C tMu = (C(Acc(mvcBvec[m].real()), Acc(mvcBvec[m].imag())) - tSum) / tBeta;

        // x[i] += mu conj(y[m-1-i]), old y;  x[m] = mu
        for (uint64_t i = 0; i < m; ++i) {
            const C tX(Acc(mvcW[i].real()), Acc(mvcW[i].imag()));
            const C tYr(Acc(mvcY[m - 1 - i].real()), Acc(mvcY[m - 1 - i].imag()));
            const C tNew = tX + tMu * std::conj(tYr);
            mvcW[i] = ComplexAcc(static_cast<double>(tNew.real()), static_cast<double>(tNew.imag()));
        }
        mvcW[m] = ComplexAcc(static_cast<double>(tMu.real()), static_cast<double>(tMu.imag()));

        if (m < uK - 1) {
            // alpha = -(rr[m+1] + sum_{i<m} rr[m-i] y[i]) / beta
            C tSumY(Acc(0), Acc(0));
            for (uint64_t i = 0; i < m; ++i)
                tSumY += C(Acc(mvcRr[m - i].real()), Acc(mvcRr[m - i].imag())) *
                         C(Acc(mvcY[i].real()), Acc(mvcY[i].imag()));
            const C tAlpha = -(C(Acc(mvcRr[m + 1].real()), Acc(mvcRr[m + 1].imag())) + tSumY) / tBeta;

            // ynew[i] = y[i] + alpha conj(y[m-1-i]), from the old y
            for (uint64_t i = 0; i < m; ++i) {
                const C tYi(Acc(mvcY[i].real()), Acc(mvcY[i].imag()));
                const C tYr(Acc(mvcY[m - 1 - i].real()), Acc(mvcY[m - 1 - i].imag()));
                const C tNew = tYi + tAlpha * std::conj(tYr);
                mvcYNew[i] = ComplexAcc(static_cast<double>(tNew.real()), static_cast<double>(tNew.imag()));
            }
            mvcYNew[m] = ComplexAcc(static_cast<double>(tAlpha.real()), static_cast<double>(tAlpha.imag()));
            for (uint64_t i = 0; i <= m; ++i)
                mvcY[i] = mvcYNew[i];

            tBeta = (Acc(1) - std::norm(tAlpha)) * tBeta;
        }
    }
    return true;
}

template <typename Acc>
uint64_t EcaLdCpu::cancelT(const ComplexF* pcRef, const ComplexF* pcSur, ComplexF* pcOut)
{
    const uint64_t uK = muTaps;
    const uint64_t uL = muBatch;
    const uint64_t uM = muLength;
    const Acc tInvM   = Acc(1) / Acc(uM);
    const Acc tLoad   = Acc(1) + Acc(moParams.dDiagonalLoading);
    uint64_t uDegenerate = 0;

    for (uint64_t b = 0; b < muBatches; ++b) {
        const uint64_t n0 = b * uL;

        // 1. a, dr, ds
        for (uint64_t j = 0; j < uM; ++j) {
            mvcA[j]  = ComplexAcc();
            mvcDr[j] = ComplexAcc();
            mvcDs[j] = ComplexAcc();
        }
        for (uint64_t j = 0; j < uL + uK - 1; ++j) {
            if (n0 + j + 1 < uK)        // n0 - K + 1 + j < 0: zero before the CPI
                continue;
            const ComplexF& cR = pcRef[n0 + j + 1 - uK];
            mvcA[j] = ComplexAcc(cR.real(), cR.imag());
        }
        for (uint64_t i = 0; i < uL; ++i) {
            mvcDr[uK - 1 + i] = ComplexAcc(pcRef[n0 + i].real(), pcRef[n0 + i].imag());
            mvcDs[uK - 1 + i] = ComplexAcc(pcSur[n0 + i].real(), pcSur[n0 + i].imag());
        }

        // 2. correlations: IFFT(conj(A) X) / M
        moFft.transform<Acc>(mvcA.data(), false);
        moFft.transform<Acc>(mvcDr.data(), false);
        moFft.transform<Acc>(mvcDs.data(), false);
        for (uint64_t m = 0; m < uM; ++m) {
            const Acc tAr = Acc(mvcA[m].real()), tAi = -Acc(mvcA[m].imag());   // conj(A)
            const Acc tRr = Acc(mvcDr[m].real()), tRi = Acc(mvcDr[m].imag());
            const Acc tSr = Acc(mvcDs[m].real()), tSi = Acc(mvcDs[m].imag());
            mvcRef[m]   = ComplexAcc(static_cast<double>(tAr * tRr - tAi * tRi),
                                     static_cast<double>(tAr * tRi + tAi * tRr));
            mvcCross[m] = ComplexAcc(static_cast<double>(tAr * tSr - tAi * tSi),
                                     static_cast<double>(tAr * tSi + tAi * tSr));
        }
        moFft.transform<Acc>(mvcRef.data(), true);
        moFft.transform<Acc>(mvcCross.data(), true);

        // 3. rr and bvec for lags 0..K-1, with the loading on rr[0]
        for (uint64_t k = 0; k < uK; ++k) {
            mvcRr[k]   = ComplexAcc(static_cast<double>(Acc(mvcRef[k].real()) * tInvM),
                                    static_cast<double>(Acc(mvcRef[k].imag()) * tInvM));
            mvcBvec[k] = ComplexAcc(static_cast<double>(Acc(mvcCross[k].real()) * tInvM),
                                    static_cast<double>(Acc(mvcCross[k].imag()) * tInvM));
        }
        mvcRr[0] = ComplexAcc(static_cast<double>(Acc(mvcRr[0].real()) * tLoad), 0.0);

        // 4. weights
        if (!levinson<Acc>()) {
            ++uDegenerate;
            for (uint64_t k = 0; k < uK; ++k)
                mvcW[k] = ComplexAcc();
        }

        // 5. out = sur - sum_k w[k] r[n-k]
        for (uint64_t i = 0; i < uL; ++i) {
            const uint64_t n = n0 + i;
            const uint64_t uTaps = (n + 1 < uK) ? (n + 1) : uK;
            Acc tRe = Acc(0), tIm = Acc(0);
            for (uint64_t k = 0; k < uTaps; ++k) {
                const Acc tWr = Acc(mvcW[k].real()), tWi = Acc(mvcW[k].imag());
                const Acc tRr = Acc(pcRef[n - k].real()), tRi = Acc(pcRef[n - k].imag());
                tRe += tWr * tRr - tWi * tRi;
                tIm += tWr * tRi + tWi * tRr;
            }
            pcOut[n] = ComplexF(static_cast<float>(Acc(pcSur[n].real()) - tRe),
                                static_cast<float>(Acc(pcSur[n].imag()) - tIm));
        }
    }
    return uDegenerate;
}

bool EcaLdCpu::cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                      ComplexF* pcOut, DSIStats& oStats)
{
    CHECK(mbReady, "EcaLdCpu::cancel before a successful init");
    CHECK(pcRef != nullptr && pcSur != nullptr && pcOut != nullptr,
          "null buffer passed to EcaLdCpu::cancel");
    CHECK(pcOut != pcRef && pcOut != pcSur,
          "the output buffer must not alias either input");

    oStats = DSIStats();

    uint64_t uDegenerate;
    if (moParams.bFloat32Math)
        uDegenerate = cancelT<float>(pcRef, pcSur, pcOut);
    else
        uDegenerate = cancelT<double>(pcRef, pcSur, pcOut);

    if (uDegenerate > 0)
        std::fprintf(stderr, "ECA-LD: %llu of %llu batches were degenerate, weights set to zero\n",
                     static_cast<unsigned long long>(uDegenerate),
                     static_cast<unsigned long long>(muBatches));

    oStats.dSuppressionDb = hostSuppressionDb(pcSur, pcOut, moParams.uCpiSamples);
    oStats.uIterations    = 0;   // not an iterative solver
    return true;
}

} // namespace

std::unique_ptr<DSICanceller> makeEcaLdCancellerCpu()
{
    return std::unique_ptr<DSICanceller>(new EcaLdCpu());
}

} // namespace pbr
