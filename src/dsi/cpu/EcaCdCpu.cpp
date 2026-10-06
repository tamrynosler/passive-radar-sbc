// ECA-CD on the CPU: the correctness reference for the frequency-domain
// canceller.
//
// ---------------------------------------------------------------------------
// What the algorithm does
// ---------------------------------------------------------------------------
//
// The time-domain ECAs describe the clutter as K delayed copies of the
// reference and solve for K weights. ECA-CD uses the fact that a delay in
// time is a phase shift in frequency: at any ONE frequency, every delayed copy
// of the reference is just the reference at that frequency times a complex
// number. So at one frequency the whole clutter -- all K delays at once -- is
// a single complex weight, and there is nothing to solve for but that.
//
// To get "one frequency" the CPI is cut into M batches of L samples and each
// batch is FFT'd. R[m][k] is the reference's carrier k in batch m, S[m][k]
// the surveillance's. Stationary clutter obeys S[m][k] = H[k] R[m][k] for
// every m, so across the batches the clutter at carrier k lies along the
// single M-vector q[m] = R[m][k]. Clutter moving slowly enough to shift by p
// Doppler bins over the CPI lies along q[m] exp(j 2 pi p m / M) instead --
// the bin spacing is 1/T_CPI, and exp(j 2 pi p m / M) is exactly the DFT
// twiddle across the batches. So for each carrier k:
//
//     Q  = [ q_{-P}  ...  q_0  ...  q_{+P} ]      M x (2P+1), q_p[m] = R[m][k] e^{j2 pi p m/M}
//     y  = S[:, k]                                 M x 1
//     y -= Q (Q^H Q)^{-1} Q^H y                    project the clutter out
//
// Q^H Q is (2P+1) square -- 3 x 3 at P = 1 -- so the solve is trivial and
// the work is one pass over the batches per carrier. Then each batch of S is
// inverse FFT'd back to time, and the CAF stage downstream sees a cleaned
// surveillance signal like any other.
//
// The reference is Schuepbach et al., RadarConf 2020, Section III-C and the
// ECA-CD branch of their AlgorithmComparison.m, which does this per carrier
// with Q = [L' q, q, L q].
//
// ---------------------------------------------------------------------------
// The approximation
// ---------------------------------------------------------------------------
//
// "A delay is a phase shift" is exact for a CIRCULAR delay within one batch.
// A real delay of k samples also drags the last k samples of the previous
// batch into this one, which the batch's own spectrum knows nothing about.
// OFDM avoids this with a cyclic prefix; FM has none. The error is confined to
// the first (delay spread) samples of every batch, so it scales with
// (delay spread / L), which is why the batch length is a parameter and why
// the Doppler ambiguity it sets (1/T_batch) is not the only thing it trades.
// Measured, not assumed: see tests/test_ecacd.cpp.
//
// Also unlike the time-domain ECAs, this cancels stationary clutter at EVERY
// delay, not out to K taps. There is no range limit to set.
//
// ---------------------------------------------------------------------------
// Implementation notes
// ---------------------------------------------------------------------------
//
// The FFT is a plain iterative radix-2, written here because the CPU build has
// no FFT library and correctness matters more than speed. It needs L to be a
// power of two; init() and the config loader both check that. Q^H Q is Toeplitz Hermitian
// (entry (a, b) depends only on b - a), so only 2P+1 sums are accumulated for
// it. Precision follows bFloat32Math as in EcaCpu: computed in Acc, stored
// exactly in double.

#include "pbr/DSICanceller.hpp"
#include "pbr/check.hpp"
#include "../DsiInternal.hpp"

#include <cmath>
#include <complex>
#include <vector>

namespace pbr {

namespace {

typedef std::complex<double> ComplexAcc;

const uint64_t kMaxCols = 2 * kMaxCdDopplerTaps + 1;

// Index into an M-point table of exp(+j 2 pi i / M) for exp(j 2 pi n / M),
// with n possibly negative.
uint64_t twiddleIndex(int64_t iN, uint64_t uM)
{
    const int64_t iM = static_cast<int64_t>(uM);
    const int64_t iR = ((iN % iM) + iM) % iM;
    return static_cast<uint64_t>(iR);
}

// Solve acSystem x = acSolution in place by Gaussian elimination with partial
// pivoting; on return acSolution holds x. Tiny -- at most 7 x 7 -- so clarity
// wins over cleverness. False on a zero pivot, which means the reference has
// no energy at this carrier (an all-zero window, say): nothing to cancel.
template <typename Acc>
bool solveSmallSystemT(std::complex<Acc> acSystem[kMaxCols][kMaxCols],
                       std::complex<Acc> acSolution[kMaxCols], uint64_t uCols)
{
    typedef std::complex<Acc> C;

    for (uint64_t uCol = 0; uCol < uCols; ++uCol) {
        uint64_t uPivot = uCol;
        for (uint64_t uRow = uCol + 1; uRow < uCols; ++uRow)
            if (std::abs(acSystem[uRow][uCol]) > std::abs(acSystem[uPivot][uCol]))
                uPivot = uRow;
        if (!(std::abs(acSystem[uPivot][uCol]) > Acc(0)))
            return false;
        if (uPivot != uCol) {
            for (uint64_t u = 0; u < uCols; ++u) {
                const C tSwap = acSystem[uCol][u];
                acSystem[uCol][u] = acSystem[uPivot][u];
                acSystem[uPivot][u] = tSwap;
            }
            const C tSwap = acSolution[uCol];
            acSolution[uCol] = acSolution[uPivot];
            acSolution[uPivot] = tSwap;
        }
        for (uint64_t uRow = uCol + 1; uRow < uCols; ++uRow) {
            const C tFactor = acSystem[uRow][uCol] / acSystem[uCol][uCol];
            for (uint64_t u = uCol; u < uCols; ++u)
                acSystem[uRow][u] -= tFactor * acSystem[uCol][u];
            acSolution[uRow] -= tFactor * acSolution[uCol];
        }
    }
    for (uint64_t uCol = uCols; uCol-- > 0; ) {
        C tSum = acSolution[uCol];
        for (uint64_t u = uCol + 1; u < uCols; ++u)
            tSum -= acSystem[uCol][u] * acSolution[u];
        acSolution[uCol] = tSum / acSystem[uCol][uCol];
    }
    return true;
}

class EcaCdCpu : public DSICanceller {
public:
    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override
    {
        return moParams.bFloat32Math ? "ECA-CD (CPU FP32)" : "ECA-CD (CPU)";
    }

private:
    // The twiddle and bit-reversal tables of the length-L radix-2 FFT.
    void buildFftTables();

    // In-place FFT of one batch of muBatch samples. No 1/L on the inverse;
    // the caller folds it into the projection.
    template <typename Acc>
    void fftT(ComplexAcc* pc, bool bInverse) const;

    // Project the clutter out of carrier k of mvcSurSpec, and scale by 1/L.
    template <typename Acc>
    void projectCarrierT(uint64_t k);

    template <typename Acc>
    void cancelT(const ComplexF* pcRef, const ComplexF* pcSur, ComplexF* pcOut);

    DSIParams               moParams;
    uint64_t                muBatch    = 0;   // L
    uint64_t                muBatches  = 0;   // M
    uint64_t                muCols     = 0;   // 2P+1

    std::vector<ComplexAcc> mvcRefSpec;       // N, [m * L + k]
    std::vector<ComplexAcc> mvcSurSpec;       // N, same layout; becomes the output
    std::vector<ComplexAcc> mvcFftTwiddle;    // L/2: exp(-j 2 pi i / L)
    std::vector<uint32_t>   mvuBitReverse;    // L
    std::vector<ComplexAcc> mvcBatchTwiddle;  // M:   exp(+j 2 pi i / M)

    bool                    mbReady = false;
};

bool EcaCdCpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "EcaCdCpu::init called twice");
    if (!ecaCdParamsOk(oParams))
        return false;

    moParams  = oParams;
    muBatch   = oParams.uCdBatchSamples;
    muBatches = oParams.uCpiSamples / oParams.uCdBatchSamples;
    muCols    = 2 * oParams.uCdDopplerTaps + 1;

    mvcRefSpec.assign(static_cast<size_t>(oParams.uCpiSamples), ComplexAcc());
    mvcSurSpec.assign(static_cast<size_t>(oParams.uCpiSamples), ComplexAcc());
    buildFftTables();

    // Rounded to float when bFloat32Math is set, so the FP32 mode uses the
    // same table the GPU does.
    mvcBatchTwiddle = batchTwiddles(muBatches);
    if (oParams.bFloat32Math) {
        for (size_t u = 0; u < mvcBatchTwiddle.size(); ++u) {
            const ComplexAcc cExact = mvcBatchTwiddle[u];
            mvcBatchTwiddle[u] = ComplexAcc(static_cast<float>(cExact.real()),
                                            static_cast<float>(cExact.imag()));
        }
    }

    mbReady = true;
    return true;
}

void EcaCdCpu::buildFftTables()
{
    mvcFftTwiddle.resize(static_cast<size_t>(muBatch / 2));
    for (uint64_t i = 0; i < muBatch / 2; ++i) {
        const double dAngle = -2.0 * kPi * static_cast<double>(i) / static_cast<double>(muBatch);
        mvcFftTwiddle[static_cast<size_t>(i)] = ComplexAcc(std::cos(dAngle), std::sin(dAngle));
    }

    // Bit reversal for the in-place radix-2 ordering.
    uint32_t uBits = 0;
    while ((1ULL << uBits) < muBatch)
        ++uBits;
    mvuBitReverse.resize(static_cast<size_t>(muBatch));
    for (uint64_t i = 0; i < muBatch; ++i) {
        uint32_t uRev = 0;
        for (uint32_t uBit = 0; uBit < uBits; ++uBit)
            if (i & (1ULL << uBit))
                uRev |= 1u << (uBits - 1 - uBit);
        mvuBitReverse[static_cast<size_t>(i)] = uRev;
    }
}

template <typename Acc>
void EcaCdCpu::fftT(ComplexAcc* pc, bool bInverse) const
{
    const uint64_t uL = muBatch;

    for (uint64_t i = 0; i < uL; ++i) {
        const uint64_t j = mvuBitReverse[static_cast<size_t>(i)];
        if (j > i) {
            const ComplexAcc cSwap = pc[i];
            pc[i] = pc[j];
            pc[j] = cSwap;
        }
    }

    // Butterflies of span 2, 4, ..., L. The twiddle for position i in a span
    // of uLen is exp(-j 2 pi i / uLen) = table[i * L / uLen].
    for (uint64_t uLen = 2; uLen <= uL; uLen <<= 1) {
        const uint64_t uHalf = uLen / 2;
        const uint64_t uStep = uL / uLen;

        for (uint64_t uBlock = 0; uBlock < uL; uBlock += uLen) {
            for (uint64_t i = 0; i < uHalf; ++i) {
                const ComplexAcc& cTw = mvcFftTwiddle[static_cast<size_t>(i * uStep)];
                const Acc tTr = Acc(cTw.real());
                const Acc tTi = bInverse ? -Acc(cTw.imag()) : Acc(cTw.imag());

                ComplexAcc& cA = pc[uBlock + i];
                ComplexAcc& cB = pc[uBlock + i + uHalf];
                const Acc tBr = Acc(cB.real()), tBi = Acc(cB.imag());
                const Acc tPr = tBr * tTr - tBi * tTi;   // b * twiddle
                const Acc tPi = tBr * tTi + tBi * tTr;
                const Acc tAr = Acc(cA.real()), tAi = Acc(cA.imag());

                cA = ComplexAcc(static_cast<double>(tAr + tPr), static_cast<double>(tAi + tPi));
                cB = ComplexAcc(static_cast<double>(tAr - tPr), static_cast<double>(tAi - tPi));
            }
        }
    }
}

template <typename Acc>
void EcaCdCpu::projectCarrierT(uint64_t k)
{
    typedef std::complex<Acc> C;

    const uint64_t uL = muBatch;
    const uint64_t uM = muBatches;
    const int64_t  iP = static_cast<int64_t>(moParams.uCdDopplerTaps);
    const uint64_t uCols = muCols;
    const Acc      tInvL = Acc(1) / Acc(uL);

    // g[d] = sum over m of |R_m|^2 e^{+j 2 pi d m / M},  d = 0..2P
    //        (Q^H Q)(a, b) = g[b - a], with g[-d] = conj(g[d])
    // v[p] = sum over m of conj(R_m) e^{-j 2 pi p m / M} S_m,  p = -P..P
    //        = Q^H y
    C acG[kMaxCols];
    C acV[kMaxCols];
    for (uint64_t u = 0; u < uCols; ++u) {
        acG[u] = C(0, 0);
        acV[u] = C(0, 0);
    }

    for (uint64_t m = 0; m < uM; ++m) {
        const ComplexAcc& cR = mvcRefSpec[static_cast<size_t>(m * uL + k)];
        const ComplexAcc& cS = mvcSurSpec[static_cast<size_t>(m * uL + k)];
        const C tR(Acc(cR.real()), Acc(cR.imag()));
        const C tS(Acc(cS.real()), Acc(cS.imag()));
        const Acc tPower = tR.real() * tR.real() + tR.imag() * tR.imag();
        const C tRS = std::conj(tR) * tS;

        for (uint64_t uD = 0; uD < uCols; ++uD) {
            const ComplexAcc& cTw = mvcBatchTwiddle[static_cast<size_t>(
                twiddleIndex(static_cast<int64_t>(uD * m), uM))];
            acG[uD] += tPower * C(Acc(cTw.real()), Acc(cTw.imag()));
        }
        for (int64_t iDoppler = -iP; iDoppler <= iP; ++iDoppler) {
            const ComplexAcc& cTw = mvcBatchTwiddle[static_cast<size_t>(
                twiddleIndex(-iDoppler * static_cast<int64_t>(m), uM))];
            acV[static_cast<size_t>(iDoppler + iP)] += tRS * C(Acc(cTw.real()), Acc(cTw.imag()));
        }
    }

    // (Q^H Q) a = v, with Q^H Q Toeplitz Hermitian.
    C acSystem[kMaxCols][kMaxCols];
    C acWeights[kMaxCols];
    for (uint64_t uRow = 0; uRow < uCols; ++uRow) {
        for (uint64_t uCol = 0; uCol < uCols; ++uCol)
            acSystem[uRow][uCol] = (uCol >= uRow) ? acG[uCol - uRow] : std::conj(acG[uRow - uCol]);
        acWeights[uRow] = acV[uRow];
    }
    const bool bSolved = solveSmallSystemT<Acc>(acSystem, acWeights, uCols);

    // y -= Q a, and the 1/L the inverse FFT will not apply. A carrier with
    // no reference energy is left alone.
    for (uint64_t m = 0; m < uM; ++m) {
        const ComplexAcc& cR = mvcRefSpec[static_cast<size_t>(m * uL + k)];
        ComplexAcc&       cS = mvcSurSpec[static_cast<size_t>(m * uL + k)];
        C tS(Acc(cS.real()), Acc(cS.imag()));

        if (bSolved) {
            const C tR(Acc(cR.real()), Acc(cR.imag()));
            C tClutter(0, 0);
            for (int64_t iDoppler = -iP; iDoppler <= iP; ++iDoppler) {
                const ComplexAcc& cTw = mvcBatchTwiddle[static_cast<size_t>(
                    twiddleIndex(iDoppler * static_cast<int64_t>(m), uM))];
                tClutter += acWeights[static_cast<size_t>(iDoppler + iP)] * tR *
                            C(Acc(cTw.real()), Acc(cTw.imag()));
            }
            tS -= tClutter;
        }
        cS = ComplexAcc(static_cast<double>(tS.real() * tInvL),
                        static_cast<double>(tS.imag() * tInvL));
    }
}

template <typename Acc>
void EcaCdCpu::cancelT(const ComplexF* pcRef, const ComplexF* pcSur, ComplexF* pcOut)
{
    const uint64_t uN = moParams.uCpiSamples;
    const uint64_t uL = muBatch;

    for (uint64_t n = 0; n < uN; ++n) {
        mvcRefSpec[static_cast<size_t>(n)] = ComplexAcc(pcRef[n].real(), pcRef[n].imag());
        mvcSurSpec[static_cast<size_t>(n)] = ComplexAcc(pcSur[n].real(), pcSur[n].imag());
    }

    // Forward: every batch of both channels.
    for (uint64_t m = 0; m < muBatches; ++m) {
        fftT<Acc>(&mvcRefSpec[static_cast<size_t>(m * uL)], false);
        fftT<Acc>(&mvcSurSpec[static_cast<size_t>(m * uL)], false);
    }

    // Project, one carrier at a time. Carriers are independent.
    for (uint64_t k = 0; k < uL; ++k)
        projectCarrierT<Acc>(k);

    // Back to time. The 1/L was applied in the projection.
    for (uint64_t m = 0; m < muBatches; ++m)
        fftT<Acc>(&mvcSurSpec[static_cast<size_t>(m * uL)], true);

    for (uint64_t n = 0; n < uN; ++n) {
        const ComplexAcc& cOut = mvcSurSpec[static_cast<size_t>(n)];
        pcOut[n] = ComplexF(static_cast<float>(cOut.real()), static_cast<float>(cOut.imag()));
    }
}

bool EcaCdCpu::cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                      ComplexF* pcOut, DSIStats& oStats)
{
    CHECK(mbReady, "EcaCdCpu::cancel before a successful init");
    CHECK(pcRef != nullptr && pcSur != nullptr && pcOut != nullptr,
          "null buffer passed to EcaCdCpu::cancel");
    CHECK(pcOut != pcRef && pcOut != pcSur,
          "the output buffer must not alias either input");

    oStats = DSIStats();

    if (moParams.bFloat32Math)
        cancelT<float>(pcRef, pcSur, pcOut);
    else
        cancelT<double>(pcRef, pcSur, pcOut);

    oStats.dSuppressionDb = hostSuppressionDb(pcSur, pcOut, moParams.uCpiSamples);
    oStats.uIterations    = 0;   // direct
    return true;
}

} // namespace

std::unique_ptr<DSICanceller> makeEcaCdCancellerCpu()
{
    return std::unique_ptr<DSICanceller>(new EcaCdCpu());
}

} // namespace pbr
