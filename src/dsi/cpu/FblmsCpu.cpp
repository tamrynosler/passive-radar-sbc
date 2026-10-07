// FBLMS on the CPU: the correctness reference for the adaptive canceller.
//
// ---------------------------------------------------------------------------
// What the algorithm does
// ---------------------------------------------------------------------------
//
// Overlap-save frequency-domain block LMS (Ferrara 1980, eqs. 9-15) with the
// per-bin step normalisation of Shynk 1992, Table I. The filter has N taps and
// the CPI is cut into blocks of N samples; M = 2N is the FFT length. The
// weights W live in the frequency domain and are updated once per block. For
// each block k, with reference r, surveillance d and output e:
//
//   1. x = [the N reference samples before the block ; the block]
//   2. X = FFT(x)
//   3. the first block ever seen sets P = |X|^2
//   4. P = lambda P + (1 - lambda) |X|^2     per-bin reference power
//   5. y = IFFT(X W); keep the last N samples (the linear convolution; the
//      first N are wrapped around and are discarded)
//   6. e = d - y                             the output
//   7. E = FFT([0 ; e])
//   8. G = conj(X) E mu / P                  per-bin step: strong bins do not
//                                            take larger steps than weak ones
//   9. g = IFFT(G); keep the first N samples, zero the rest (the gradient
//      constraint: the update may only touch N taps, not M)
//  10. W += 2 FFT(g)
//
// The P guard: a bin with P = 0 had no reference energy, so it gets no update
// (G = 0) instead of 0 * inf.
//
// The inverse FFT is unnormalised, so the two 1/M scalings are applied in
// steps 5 and 9, where the GPU version applies them.
//
// ---------------------------------------------------------------------------
// State across CPIs
// ---------------------------------------------------------------------------
//
// W, P and the last N reference samples are kept from one cancel() call to the
// next, so the filter keeps adapting (the recommendation of Garry 2017). The
// first CPI converges from zero weights; later ones start where the last
// ended. A fresh canceller is a cold start, and resetState() returns to one:
// the chain calls it after its warm-up pass, so the synthetic warm-up signal
// does not seed the first real CPI.
//
// ---------------------------------------------------------------------------
// Precision
// ---------------------------------------------------------------------------
//
// As in CglsCpu: every operation in Acc (float with bFloat32Math, else
// double), every stored value held exactly in double, so the FP32 mode does
// the GPU's arithmetic and the FP64 mode is the reference. The FFT is HostFft,
// the same arithmetic as EcaCdCpu's.

#include "pbr/DSICanceller.hpp"
#include "pbr/check.hpp"
#include "../DsiInternal.hpp"
#include "HostFft.hpp"

#include <complex>
#include <vector>

namespace pbr {

namespace {

// Storage type: double, so a float value is held exactly (see EcaCpu.cpp).
typedef std::complex<double> ComplexAcc;

class FblmsCpu : public DSICanceller {
public:
    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override
    {
        return moParams.bFloat32Math ? "FBLMS (CPU FP32)" : "FBLMS (CPU)";
    }
    bool resetState() override;

private:
    template <typename Acc>
    void cancelT(const ComplexF* pcRef, const ComplexF* pcSur, ComplexF* pcOut);

    DSIParams               moParams;
    HostFft                 moFft;
    uint64_t                muTaps   = 0;   // N
    uint64_t                muLength = 0;   // M = 2N
    uint64_t                muBlocks = 0;   // B

    // State kept across CPIs.
    std::vector<ComplexAcc> mvcW;           // M, frequency-domain weights
    std::vector<double>     mvP;            // M, per-bin reference power
    std::vector<ComplexAcc> mvcTail;        // N, last N reference samples
    bool                    mbPowerSet = false;

    // Scratch, reused every block.
    std::vector<ComplexAcc> mvcX;           // M, FFT of the reference block
    std::vector<ComplexAcc> mvcY;           // M, Y = X W, then its inverse FFT
    std::vector<ComplexAcc> mvcE;           // M, [0 ; e], then E
    std::vector<ComplexAcc> mvcG;           // M, G, then g, then gc, then Gc

    bool                    mbReady = false;
};

bool FblmsCpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "FblmsCpu::init called twice");
    if (!fblmsParamsOk(oParams))
        return false;

    moParams = oParams;
    muTaps   = oParams.uFblmsTaps;
    muLength = 2 * muTaps;
    muBlocks = oParams.uCpiSamples / muTaps;

    if (!moFft.init(muLength))
        return false;

    // Every allocation happens here and is reused for the life of the object.
    mvcW.assign(static_cast<size_t>(muLength), ComplexAcc());
    mvP.assign(static_cast<size_t>(muLength), 0.0);
    mvcTail.assign(static_cast<size_t>(muTaps), ComplexAcc());
    mbPowerSet = false;

    mvcX.assign(static_cast<size_t>(muLength), ComplexAcc());
    mvcY.assign(static_cast<size_t>(muLength), ComplexAcc());
    mvcE.assign(static_cast<size_t>(muLength), ComplexAcc());
    mvcG.assign(static_cast<size_t>(muLength), ComplexAcc());

    mbReady = true;
    return true;
}

bool FblmsCpu::resetState()
{
    CHECK(mbReady, "FblmsCpu::resetState before a successful init");
    mvcW.assign(mvcW.size(), ComplexAcc());
    mvP.assign(mvP.size(), 0.0);
    mvcTail.assign(mvcTail.size(), ComplexAcc());
    mbPowerSet = false;
    return true;
}

template <typename Acc>
void FblmsCpu::cancelT(const ComplexF* pcRef, const ComplexF* pcSur, ComplexF* pcOut)
{
    const uint64_t uN = muTaps;
    const uint64_t uM = muLength;
    const Acc tMu     = Acc(moParams.dFblmsStep);
    const Acc tLambda = Acc(moParams.dFblmsForget);
    const Acc tOneMinus = Acc(1) - tLambda;
    const Acc tInvM   = Acc(1) / Acc(uM);

    for (uint64_t k = 0; k < muBlocks; ++k) {
        const uint64_t uStart = k * uN;

        // 1-2. x = [previous N reference samples ; this block];  X = FFT(x)
        for (uint64_t n = 0; n < uN; ++n) {
            if (k == 0) {
                mvcX[n] = mvcTail[n];
            } else {
                const ComplexF& cPrev = pcRef[uStart - uN + n];
                mvcX[n] = ComplexAcc(cPrev.real(), cPrev.imag());
            }
            const ComplexF& cRef = pcRef[uStart + n];
            mvcX[uN + n] = ComplexAcc(cRef.real(), cRef.imag());
        }
        moFft.transform<Acc>(mvcX.data(), false);

        // 3. the very first block sets the power estimate
        if (!mbPowerSet) {
            for (uint64_t m = 0; m < uM; ++m) {
                const Acc tXr = Acc(mvcX[m].real()), tXi = Acc(mvcX[m].imag());
                mvP[m] = static_cast<double>(tXr * tXr + tXi * tXi);
            }
            mbPowerSet = true;
        }

        // 4. P = lambda P + (1 - lambda) |X|^2
        for (uint64_t m = 0; m < uM; ++m) {
            const Acc tXr = Acc(mvcX[m].real()), tXi = Acc(mvcX[m].imag());
            mvP[m] = static_cast<double>(tLambda * Acc(mvP[m]) + tOneMinus * (tXr * tXr + tXi * tXi));
        }

        // 5. Y = X W;  y = IFFT(Y) / M;  keep the last N samples
        for (uint64_t m = 0; m < uM; ++m) {
            const Acc tXr = Acc(mvcX[m].real()), tXi = Acc(mvcX[m].imag());
            const Acc tWr = Acc(mvcW[m].real()), tWi = Acc(mvcW[m].imag());
            mvcY[m] = ComplexAcc(static_cast<double>(tXr * tWr - tXi * tWi),
                                 static_cast<double>(tXr * tWi + tXi * tWr));
        }
        moFft.transform<Acc>(mvcY.data(), true);

        // 6. e = d - y;  7. E = FFT([0 ; e])
        for (uint64_t n = 0; n < uN; ++n) {
            const Acc tYr = Acc(mvcY[uN + n].real()) * tInvM;
            const Acc tYi = Acc(mvcY[uN + n].imag()) * tInvM;
            const Acc tEr = Acc(pcSur[uStart + n].real()) - tYr;
            const Acc tEi = Acc(pcSur[uStart + n].imag()) - tYi;
            pcOut[uStart + n] = ComplexF(static_cast<float>(tEr), static_cast<float>(tEi));
            mvcE[n]      = ComplexAcc();
            mvcE[uN + n] = ComplexAcc(static_cast<double>(tEr), static_cast<double>(tEi));
        }
        moFft.transform<Acc>(mvcE.data(), false);

        // 8. G = conj(X) E (mu / P), or zero where there was no reference energy
        for (uint64_t m = 0; m < uM; ++m) {
            if (mvP[m] == 0.0) {
                mvcG[m] = ComplexAcc();
                continue;
            }
            const Acc tXr = Acc(mvcX[m].real()), tXi = Acc(mvcX[m].imag());
            const Acc tEr = Acc(mvcE[m].real()), tEi = Acc(mvcE[m].imag());
            const Acc tScale = tMu / Acc(mvP[m]);
            const Acc tPr = tXr * tEr + tXi * tEi;    // conj(X) E
            const Acc tPi = tXr * tEi - tXi * tEr;
            mvcG[m] = ComplexAcc(static_cast<double>(tPr * tScale),
                                 static_cast<double>(tPi * tScale));
        }

        // 9. g = IFFT(G) / M, first N samples kept, the rest zeroed
        moFft.transform<Acc>(mvcG.data(), true);
        for (uint64_t m = 0; m < uM; ++m) {
            if (m < uN)
                mvcG[m] = ComplexAcc(static_cast<double>(Acc(mvcG[m].real()) * tInvM),
                                     static_cast<double>(Acc(mvcG[m].imag()) * tInvM));
            else
                mvcG[m] = ComplexAcc();
        }

        // 10. W += 2 FFT(gc)
        moFft.transform<Acc>(mvcG.data(), false);
        for (uint64_t m = 0; m < uM; ++m) {
            mvcW[m] = ComplexAcc(static_cast<double>(Acc(mvcW[m].real()) + Acc(2) * Acc(mvcG[m].real())),
                                 static_cast<double>(Acc(mvcW[m].imag()) + Acc(2) * Acc(mvcG[m].imag())));
        }
    }

    // The next CPI's first block needs the last N reference samples of this one.
    const uint64_t uTailStart = moParams.uCpiSamples - uN;
    for (uint64_t n = 0; n < uN; ++n)
        mvcTail[n] = ComplexAcc(pcRef[uTailStart + n].real(), pcRef[uTailStart + n].imag());
}

bool FblmsCpu::cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                      ComplexF* pcOut, DSIStats& oStats)
{
    CHECK(mbReady, "FblmsCpu::cancel before a successful init");
    CHECK(pcRef != nullptr && pcSur != nullptr && pcOut != nullptr,
          "null buffer passed to FblmsCpu::cancel");
    CHECK(pcOut != pcRef && pcOut != pcSur,
          "the output buffer must not alias either input");

    oStats = DSIStats();

    if (moParams.bFloat32Math)
        cancelT<float>(pcRef, pcSur, pcOut);
    else
        cancelT<double>(pcRef, pcSur, pcOut);

    oStats.dSuppressionDb = hostSuppressionDb(pcSur, pcOut, moParams.uCpiSamples);
    oStats.uIterations    = 0;   // not an iterative solver
    return true;
}

} // namespace

std::unique_ptr<DSICanceller> makeFblmsCancellerCpu()
{
    return std::unique_ptr<DSICanceller>(new FblmsCpu());
}

} // namespace pbr
