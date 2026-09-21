// ECA-m, ECA-B and ECA-S on the CPU: the correctness reference for DSI
// cancellation.
//
// ---------------------------------------------------------------------------
// What the algorithm does
// ---------------------------------------------------------------------------
//
// The surveillance antenna points away from the transmitter, but it still
// hears the transmitter: directly, and via echoes off everything stationary
// in the scene. That is the direct signal interference, and it is orders of
// magnitude stronger than any aircraft echo. Every part of it is some
// delayed, scaled copy of what the reference antenna hears.
//
// So we look for the combination of delayed reference copies that best
// explains the surveillance signal, and subtract it. Formally, with r the
// reference, s the surveillance and K delay taps:
//
//     out[n] = s[n] - sum over k of  w[k] * r[n-k]
//
// and w is chosen to minimise the leftover power. That is an ordinary
// least-squares problem, and its solution is the normal equations
//
//     G w = v     where  G[i][j] = sum over n of conj(r[n-i]) * r[n-j]
//                        v[i]    = sum over n of conj(r[n-i]) * s[n]
//
// G is the Gram matrix: entry [i][j] measures how much the reference delayed
// by i looks like the reference delayed by j. v measures how much the
// reference delayed by i looks like the surveillance signal.
//
// ECA-m does this once over the whole CPI. ECA-B splits the CPI into
// fragments and solves each independently, so the weights can follow clutter
// that changes during the second. ECA-S solves for each fragment from a
// LONGER window centred on it, so a short fragment (which pushes ECA-B's
// Doppler ambiguities out of the span of interest) can still have its
// weights estimated from enough samples. All three are the same code; only
// the window bounds differ.
//
// ---------------------------------------------------------------------------
// Two implementation choices worth knowing about
// ---------------------------------------------------------------------------
//
// The matrix R whose columns are the delayed copies is NEVER FORMED. At
// 204800 samples and 120 taps it would be 197 MB, which is most of the
// Jetson's shared memory for something we only ever need as a 120x120
// summary. Each Gram entry is computed directly as a dot product of two
// shifted slices of the reference. The MATLAB reference builds R explicitly;
// this does not, and the answers agree because the arithmetic is the same.
//
// Accumulation is in DOUBLE, even though the samples are float. A Gram entry
// sums 204800 products; accumulating that in float loses roughly four decimal
// digits, which alone would breach the 1e-4 agreement the golden-file check
// demands. The GPU version will have to face this question properly, since
// cuBLAS accumulates in the precision it is given.
//
// ---------------------------------------------------------------------------
// Cost
// ---------------------------------------------------------------------------
//
// The Gram dominates everything: K(K+1)/2 entries, each summing over the
// window, which is about 1.2e10 floating-point operations per CPI at K=120
// and 204800 samples. Splitting into batches does not change that total --
// batching buys adaptivity, bounded memory and parallelism, not fewer
// operations. Expect seconds per CPI. This is the correctness reference, not
// a real-time implementation; that is what the GPU backend is for.
//
// ECA-S multiplies that by T_A / T_S, because consecutive estimation windows
// overlap and each is summed from scratch. At 25 ms over 2.5 ms that is 10x
// ECA-B. Colone's Section VI sliding update (subtract the samples leaving,
// add the samples entering) removes the factor; deliberately not done yet.
//
// A roughly 60x exact saving is available (successive Gram entries share
// almost all of their terms), and a ~100x approximate one via FFTs and the
// Toeplitz structure, per Colone et al. TAES 52(3) 2016 Section VI. Neither
// is done here. Correct first.

#include "pbr/DSICanceller.hpp"
#include "pbr/check.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pbr {

namespace {

// Accumulator type. Deliberately double while the samples are float -- see
// the note at the top of this file.
typedef std::complex<double> ComplexAcc;

class EcaCpu : public DSICanceller {
public:
    EcaCpu() { std::memset(machName, 0, sizeof(machName)); }

    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override { return machName; }

private:
    // Fill mvcGram and mvcRhs from the samples in [uStart, uEnd).
    void buildNormalEquations(const ComplexF* pcRef, const ComplexF* pcSur,
                              uint64_t uStart, uint64_t uEnd);
    template <typename Acc>
    void buildNormalEquationsT(const ComplexF* pcRef, const ComplexF* pcSur,
                               uint64_t uStart, uint64_t uEnd);

    // Add the diagonal loading, then solve G w = v into mvcWeights.
    bool solveWeights(uint64_t uWindowIndex);

    // out[n] = s[n] - sum_k w[k] r[n-k], for n in [uStart, uEnd).
    void applyWeights(const ComplexF* pcRef, const ComplexF* pcSur,
                      ComplexF* pcOut, uint64_t uStart, uint64_t uEnd) const;

    DSIParams               moParams;
    uint64_t                muTaps    = 0;   // K, cached from moParams
    uint64_t                muWindows = 0;   // CPI / update window

    std::vector<ComplexAcc> mvcGram;         // K x K, row-major, Hermitian
    std::vector<ComplexAcc> mvcRhs;          // K
    std::vector<ComplexAcc> mvcChol;         // K x K, lower triangle of G = L L^H
    std::vector<ComplexAcc> mvcWeights;      // K

    char                    machName[32];
    bool                    mbReady   = false;
};

bool EcaCpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "EcaCpu::init called twice");

    CHECK(oParams.eAlgorithm == DSIAlgorithm::EcaM ||
          oParams.eAlgorithm == DSIAlgorithm::EcaB ||
          oParams.eAlgorithm == DSIAlgorithm::EcaS,
          "EcaCpu handles ECA-m, ECA-B and ECA-S only");
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uClutterTaps > 0, "clutter taps must be non-zero");
    CHECK(oParams.uEstimateSamples > 0 && oParams.uUpdateSamples > 0,
          "estimation and update windows must be non-zero");

    // A non-zero Doppler tap count is a real algorithm, not a tuning knob.
    // Refuse it rather than silently cancelling only stationary clutter while
    // the config file claims otherwise.
    CHECK(oParams.uDopplerTaps == 0,
          "ECA_DOPPLER_TAPS > 0 is not implemented; the clutter subspace is "
          "delay-only for now");

    // The window rule is what separates the names. ECA-m and ECA-B estimate
    // from the window they apply to; ECA-S estimates from a longer one.
    // Neither is allowed to masquerade as the other, or a benchmark row would
    // carry the wrong label.
    if (oParams.eAlgorithm == DSIAlgorithm::EcaS) {
        CHECK(oParams.uEstimateSamples > oParams.uUpdateSamples,
              "ECA-S needs an estimation window longer than the update window; "
              "equal windows are ECA-B");
    } else {
        CHECK(oParams.uEstimateSamples == oParams.uUpdateSamples,
              "an estimation window longer than the update window is ECA-S, "
              "not ECA-B");
    }
    CHECK(oParams.uEstimateSamples <= oParams.uCpiSamples,
          "the estimation window cannot be longer than the CPI");

    CHECK(oParams.uCpiSamples % oParams.uUpdateSamples == 0,
          "the update window must divide the CPI exactly");
    // Keeps every estimation window on an update-window boundary, which is
    // what lets the GPU share partial Grams; enforced here too so the two
    // backends place their windows identically.
    CHECK(oParams.uEstimateSamples % oParams.uUpdateSamples == 0,
          "the update window must divide the estimation window exactly");
    CHECK(oParams.uEstimateSamples > oParams.uClutterTaps,
          "the estimation window must hold more samples than there are taps "
          "to solve for");
    CHECK(oParams.dDiagonalLoading >= 0.0, "diagonal loading must not be negative");

    moParams  = oParams;
    muTaps    = oParams.uClutterTaps;
    muWindows = oParams.uCpiSamples / oParams.uUpdateSamples;

    // Every allocation happens here and is reused for the life of the object.
    // Nothing allocates inside cancel(), or the benchmark would be timing the
    // allocator.
    mvcGram.assign(static_cast<size_t>(muTaps * muTaps), ComplexAcc());
    mvcChol.assign(static_cast<size_t>(muTaps * muTaps), ComplexAcc());
    mvcRhs.assign(static_cast<size_t>(muTaps), ComplexAcc());
    mvcWeights.assign(static_cast<size_t>(muTaps), ComplexAcc());

    std::snprintf(machName, sizeof(machName), "%s (CPU%s)",
                  dsiAlgorithmName(oParams.eAlgorithm),
                  oParams.bFloat32Math ? " FP32" : "");

    mbReady = true;
    return true;
}

void EcaCpu::buildNormalEquations(const ComplexF* pcRef, const ComplexF* pcSur,
                                  uint64_t uStart, uint64_t uEnd)
{
    if (moParams.bFloat32Math)
        buildNormalEquationsT<float>(pcRef, pcSur, uStart, uEnd);
    else
        buildNormalEquationsT<double>(pcRef, pcSur, uStart, uEnd);
}

// The working precision is a template parameter rather than a runtime test,
// so the innermost loop never branches. Acc = double is the reference: a
// float-to-double conversion is exact and the product of two floats fits a
// double exactly, so every term is computed without error and only the
// summation rounds. Acc = float reproduces what the GPU does, where both the
// products and the sum round -- and the products turn out to dominate
// (docs/design_decisions.md 1d.4).
template <typename Acc>
void EcaCpu::buildNormalEquationsT(const ComplexF* pcRef, const ComplexF* pcSur,
                                   uint64_t uStart, uint64_t uEnd)
{
    const uint64_t uK = muTaps;

    // G is Hermitian, so only the upper triangle is computed and the lower
    // half is its conjugate. That halves the work, which matters because this
    // loop is essentially the entire cost of the algorithm.
    for (uint64_t i = 0; i < uK; ++i) {
        for (uint64_t j = i; j < uK; ++j) {
            // r[n-i] and r[n-j] are both zero before the start of the CPI, so
            // the sum only starts once both delayed copies have real samples
            // under them. This is what the MATLAB reference's leading
            // zero-padding of each column amounts to.
            const uint64_t uFirst = (uStart > j) ? uStart : j;

            Acc aRe = Acc(0);
            Acc aIm = Acc(0);

            // conj(a) * b, written out. Doing the complex multiply by hand
            // keeps the compiler from routing it through the library's
            // NaN-handling path, and makes the working precision explicit.
            for (uint64_t n = uFirst; n < uEnd; ++n) {
                const ComplexF& cA = pcRef[n - i];
                const ComplexF& cB = pcRef[n - j];
                const Acc aAr = Acc(cA.real()), aAi = Acc(cA.imag());
                const Acc aBr = Acc(cB.real()), aBi = Acc(cB.imag());
                aRe += aAr * aBr + aAi * aBi;
                aIm += aAr * aBi - aAi * aBr;
            }

            const double dRe = static_cast<double>(aRe);
            const double dIm = static_cast<double>(aIm);
            mvcGram[static_cast<size_t>(i * uK + j)] = ComplexAcc(dRe, dIm);
            if (j != i)
                mvcGram[static_cast<size_t>(j * uK + i)] = ComplexAcc(dRe, -dIm);
        }
    }

    // v[i]: how much the reference delayed by i resembles the surveillance
    // signal. K sums rather than K^2/2, so this is cheap by comparison.
    for (uint64_t i = 0; i < uK; ++i) {
        const uint64_t uFirst = (uStart > i) ? uStart : i;

        Acc aRe = Acc(0);
        Acc aIm = Acc(0);
        for (uint64_t n = uFirst; n < uEnd; ++n) {
            const ComplexF& cA = pcRef[n - i];
            const ComplexF& cB = pcSur[n];
            const Acc aAr = Acc(cA.real()), aAi = Acc(cA.imag());
            const Acc aBr = Acc(cB.real()), aBi = Acc(cB.imag());
            aRe += aAr * aBr + aAi * aBi;
            aIm += aAr * aBi - aAi * aBr;
        }
        mvcRhs[static_cast<size_t>(i)] = ComplexAcc(static_cast<double>(aRe),
                                                    static_cast<double>(aIm));
    }
}

bool EcaCpu::solveWeights(uint64_t uWindowIndex)
{
    const uint64_t uK = muTaps;

    // Diagonal loading. G's diagonal entries are the energy of the reference
    // under each delay, so their mean is the natural scale of the matrix.
    // Adding a fixed fraction of it keeps the solve well conditioned and means
    // the same thing whatever amplitude the samples arrived at.
    //
    // ECA-m instead adds the MATLAB reference's absolute 1e-3, because
    // reproducing that chain bit for bit is its entire job.
    double dLambda = 0.0;
    if (moParams.bAbsoluteLoading) {
        dLambda = moParams.dDiagonalLoading;
    } else if (moParams.dDiagonalLoading > 0.0) {
        // Summed in the working precision, so the FP32 mode computes the same
        // lambda the GPU kernel does. A lambda that differed would show up as
        // a disagreement that had nothing to do with the code being compared.
        if (moParams.bFloat32Math) {
            float fTrace = 0.0f;
            for (uint64_t i = 0; i < uK; ++i)
                fTrace += static_cast<float>(mvcGram[static_cast<size_t>(i * uK + i)].real());
            dLambda = moParams.dDiagonalLoading *
                      (static_cast<double>(fTrace) / static_cast<double>(uK));
        } else {
            double dTrace = 0.0;
            for (uint64_t i = 0; i < uK; ++i)
                dTrace += mvcGram[static_cast<size_t>(i * uK + i)].real();
            dLambda = moParams.dDiagonalLoading * (dTrace / static_cast<double>(uK));
        }
    }

    // Cholesky, G = L L^H. G is Hermitian and, with the loading, positive
    // definite, so this is both the cheapest and the most numerically stable
    // way to solve it -- about K^3/3 operations, which is nothing next to
    // building G in the first place.
    for (uint64_t j = 0; j < uK; ++j) {
        // Diagonal: G[j][j] minus the energy already accounted for by the
        // columns to its left.
        double dDiag = mvcGram[static_cast<size_t>(j * uK + j)].real() + dLambda;
        for (uint64_t k = 0; k < j; ++k) {
            const ComplexAcc& cL = mvcChol[static_cast<size_t>(j * uK + k)];
            dDiag -= cL.real() * cL.real() + cL.imag() * cL.imag();
        }

        // A non-positive pivot means the matrix is singular or indefinite.
        // The usual cause is an all-zero window: a dropped packet leaves a
        // zero-filled gap, and zeros carry no clutter to cancel. Report it
        // rather than returning quiet nonsense.
        if (!(dDiag > 0.0)) {
            std::fprintf(stderr,
                         "EcaCpu: window %llu is not positive definite at tap "
                         "%llu (pivot %g). The reference signal is probably "
                         "zero or nearly constant over this window; raise "
                         "ECA_DIAGONAL_LOADING if it is genuinely ill "
                         "conditioned.\n",
                         static_cast<unsigned long long>(uWindowIndex),
                         static_cast<unsigned long long>(j), dDiag);
            return false;
        }

        const double dLjj = std::sqrt(dDiag);
        mvcChol[static_cast<size_t>(j * uK + j)] = ComplexAcc(dLjj, 0.0);

        for (uint64_t i = j + 1; i < uK; ++i) {
            ComplexAcc cSum = mvcGram[static_cast<size_t>(i * uK + j)];
            for (uint64_t k = 0; k < j; ++k) {
                cSum -= mvcChol[static_cast<size_t>(i * uK + k)] *
                        std::conj(mvcChol[static_cast<size_t>(j * uK + k)]);
            }
            mvcChol[static_cast<size_t>(i * uK + j)] = cSum / dLjj;
        }
    }

    // Forward substitution, L y = v. y is written into mvcWeights and
    // overwritten by the real answer in the back substitution below.
    for (uint64_t i = 0; i < uK; ++i) {
        ComplexAcc cSum = mvcRhs[static_cast<size_t>(i)];
        for (uint64_t k = 0; k < i; ++k)
            cSum -= mvcChol[static_cast<size_t>(i * uK + k)] *
                    mvcWeights[static_cast<size_t>(k)];
        mvcWeights[static_cast<size_t>(i)] =
            cSum / mvcChol[static_cast<size_t>(i * uK + i)].real();
    }

    // Back substitution, L^H w = y.
    for (uint64_t i = uK; i-- > 0; ) {
        ComplexAcc cSum = mvcWeights[static_cast<size_t>(i)];
        for (uint64_t k = i + 1; k < uK; ++k)
            cSum -= std::conj(mvcChol[static_cast<size_t>(k * uK + i)]) *
                    mvcWeights[static_cast<size_t>(k)];
        mvcWeights[static_cast<size_t>(i)] =
            cSum / mvcChol[static_cast<size_t>(i * uK + i)].real();
    }

    return true;
}

void EcaCpu::applyWeights(const ComplexF* pcRef, const ComplexF* pcSur,
                          ComplexF* pcOut, uint64_t uStart, uint64_t uEnd) const
{
    const uint64_t uK = muTaps;

    for (uint64_t n = uStart; n < uEnd; ++n) {
        // Taps that would reach back before the start of the CPI contribute
        // nothing, because the reference is zero there.
        const uint64_t uTaps = (n + 1 < uK) ? (n + 1) : uK;

        double dRe = 0.0;
        double dIm = 0.0;
        for (uint64_t k = 0; k < uTaps; ++k) {
            const ComplexAcc& cW = mvcWeights[static_cast<size_t>(k)];
            const ComplexF&   cR = pcRef[n - k];
            const double dWr = cW.real(), dWi = cW.imag();
            const double dRr = cR.real(), dRi = cR.imag();
            dRe += dWr * dRr - dWi * dRi;
            dIm += dWr * dRi + dWi * dRr;
        }

        pcOut[n] = ComplexF(static_cast<float>(pcSur[n].real() - dRe),
                            static_cast<float>(pcSur[n].imag() - dIm));
    }
}

bool EcaCpu::cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                    ComplexF* pcOut, DSIStats& oStats)
{
    CHECK(mbReady, "EcaCpu::cancel before a successful init");
    CHECK(pcRef != nullptr && pcSur != nullptr && pcOut != nullptr,
          "null buffer passed to EcaCpu::cancel");
    // Aliasing would destroy the surveillance signal partway through and make
    // the suppression figure meaningless.
    CHECK(pcOut != pcRef && pcOut != pcSur,
          "the output buffer must not alias either input");

    oStats = DSIStats();

    const uint64_t uUpdate   = moParams.uUpdateSamples;
    const uint64_t uEstimate = moParams.uEstimateSamples;

    for (uint64_t w = 0; w < muWindows; ++w) {
        const uint64_t uStart = w * uUpdate;
        const uint64_t uEnd   = uStart + uUpdate;

        // ECA-m and ECA-B estimate the weights from the window they are
        // applied to. ECA-S estimates them from a longer window centred on it,
        // clamped at the ends of the CPI -- see dsiEstimationStart(). The
        // weights are applied to [uStart, uEnd) in every case, so each output
        // sample is written exactly once.
        const uint64_t uEstStart = dsiEstimationStart(uStart, uUpdate, uEstimate,
                                                      moParams.uCpiSamples);
        buildNormalEquations(pcRef, pcSur, uEstStart, uEstStart + uEstimate);
        if (!solveWeights(w))
            return false;
        applyWeights(pcRef, pcSur, pcOut, uStart, uEnd);
    }

    // Suppression: how much total power the cancellation removed. A headline
    // number, and on its own a misleading one -- an algorithm scores well here
    // whether it removed clutter or the targets. Target attenuation has to be
    // measured from the range-Doppler map.
    double dInput  = 0.0;
    double dOutput = 0.0;
    for (uint64_t n = 0; n < moParams.uCpiSamples; ++n) {
        const double dSr = pcSur[n].real(), dSi = pcSur[n].imag();
        const double dOr = pcOut[n].real(), dOi = pcOut[n].imag();
        dInput  += dSr * dSr + dSi * dSi;
        dOutput += dOr * dOr + dOi * dOi;
    }

    oStats.dSuppressionDb = (dInput > 0.0 && dOutput > 0.0)
                          ? 10.0 * std::log10(dInput / dOutput)
                          : 0.0;
    oStats.uIterations = 0;   // direct solve, not iterative

    return true;
}

} // namespace

// The other CPU algorithms live in their own files; this is the one place
// that knows all of them.
std::unique_ptr<DSICanceller> makeCglsCancellerCpu();
std::unique_ptr<DSICanceller> makeEcaCdCancellerCpu();

std::unique_ptr<DSICanceller> makeDSICancellerCpu(DSIAlgorithm eAlgorithm)
{
    switch (eAlgorithm) {
    case DSIAlgorithm::EcaM:
    case DSIAlgorithm::EcaB:
    case DSIAlgorithm::EcaS:
        return std::unique_ptr<DSICanceller>(new EcaCpu());
    case DSIAlgorithm::EcaCd:
        return makeEcaCdCancellerCpu();
    case DSIAlgorithm::Cgls:
        return makeCglsCancellerCpu();
    }

    std::fprintf(stderr, "dsi: unknown algorithm %d on the CPU backend\n",
                 static_cast<int>(eAlgorithm));
    return std::unique_ptr<DSICanceller>();
}

} // namespace pbr
