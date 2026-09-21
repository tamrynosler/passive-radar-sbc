// CGLS on the CPU: the correctness reference for the iterative canceller.
//
// ---------------------------------------------------------------------------
// What the algorithm does
// ---------------------------------------------------------------------------
//
// The same problem as ECA-B (see EcaCpu.cpp): for each window of the CPI,
// find the K weights w that minimise || s - R w ||, where column k of R is
// the reference delayed by k samples, and output s - R w.
//
// ECA solves that in one shot by forming the K x K Gram matrix R^H R and
// factorising it. CGLS never forms it. It walks towards the minimum, and each
// step needs only two products with R itself:
//
//     q = R p       q[n] = sum over k of p[k] r[n-k]        a convolution
//     s = R^H v     s[k] = sum over n of conj(r[n-k]) v[n]  a correlation
//
// Each costs N*K per window, the same as ONE row of the Gram, so I iterations
// cost about 2I/K of what ECA-B's Gram does: at I = 30 and K = 120, half.
//
// The iteration (Hestenes and Stiefel, 1952; the form Schuepbach's
// CGLS_Cancellation.m uses):
//
//     x = start;  r = b - R x;  s = R^H r;  p = s;  gamma = |s|^2
//     repeat I times:
//         q     = R p
//         alpha = Re(p^H s) / |q|^2  the exact best step length along p
//                                    (= gamma / |q|^2 in exact arithmetic; see below)
//         x    += alpha p
//         r    -= alpha q            the residual, kept up to date cheaply
//         s     = R^H r              the new gradient
//         beta  = |s|^2 / gamma      how much of the old direction to keep
//         gamma = |s|^2
//         p     = s + beta p         the next direction
//
// Each new direction is built so that progress made along earlier directions
// is never undone -- that is what "conjugate" means here, and it is why this
// converges in far fewer steps than steepest descent, which zig-zags. In exact
// arithmetic it reaches the least-squares answer in at most K steps.
//
// ---------------------------------------------------------------------------
// Why it stops after a fixed count, and why there is no diagonal loading
// ---------------------------------------------------------------------------
//
// CGLS fits the components of b in order of how strongly the columns of R
// carry them: the direct signal and strong clutter first, the noise last. The
// true error therefore falls and then RISES as later iterations start fitting
// noise -- semiconvergence. Stopping early is the regularisation, doing the
// job diagonal loading does for ECA, so running to a tolerance would mean
// running past the best answer. A fixed count, as Schuepbach et al. used, and
// it fixes the execution time too.
//
// ---------------------------------------------------------------------------
// Three precision details
// ---------------------------------------------------------------------------
//
// The step length is alpha = Re(p^H s) / |q|^2, not the textbook
// alpha = gamma / |q|^2 = |s|^2 / |q|^2. The two are equal in exact arithmetic,
// because p^H s = |s|^2 when p is conjugate to the earlier directions. Once
// the iteration has converged, s is rounding noise, that conjugacy is gone,
// and the textbook alpha is no longer the minimiser along p: the step can
// make the residual LARGER, which feeds s, which feeds the next step. Measured
// on a well-conditioned test problem (64 white-noise taps): converged by
// iteration 12, then exponential growth from iteration 20 in FP32 and 120 in
// FP64, reaching 1e14 by iteration 200. Re(p^H s) / |q|^2 is the exact line
// minimiser whatever p is, so the residual can never grow, and the iteration
// sits harmlessly at the floor instead. It matters here because PRIMING starts
// from a converged point on purpose. One extra K-length dot product.
//
// The cheap update r -= alpha q is exact algebra but accumulates rounding, and
// after enough steps r drifts from the true b - R x. Every kResidualRefresh
// iterations it is recomputed from scratch, and the output is always computed
// fresh as b - R x rather than taken from r.
//
// Precision follows bFloat32Math as in EcaCpu: every operation in Acc, every
// stored value held exactly in double, so the FP32 mode performs the GPU's
// arithmetic and the FP64 mode is the reference.
//
// ---------------------------------------------------------------------------
// Priming
// ---------------------------------------------------------------------------
//
// With bCglsPrime, window w starts from the weights the SAME window reached in
// the previous CPI. The scene barely moves between CPIs, so the start is
// already close and the fixed iteration count buys more. Same-window rather
// than previous-window because the GPU solves every window at once, and a
// chain through the windows would serialise it. The first CPI starts from
// zero either way.

#include "pbr/DSICanceller.hpp"
#include "pbr/check.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pbr {

namespace {

// Storage type: double, so a float value is held exactly (see EcaCpu.cpp).
typedef std::complex<double> ComplexAcc;

// Recompute r = b - R x from scratch this often. Ten is a guess that the FP64
// versus FP32 comparison in tests/test_cgls.cpp is there to check.
const uint64_t kResidualRefresh = 10;

// out[n] = base[n] - sum over k of w[k] r[n-k], for n in [uStart, uEnd), or
// just the sum when pcBase is null. The first form is the residual b - R x
// and the output; the second is q = R p. Written to pcOut[n - uStart].
//
// Taps that would reach back before the start of the CPI contribute nothing,
// because the reference is zero there -- the same rule as EcaCpu.
template <typename Acc>
void forwardT(const ComplexF* pcRef, const ComplexAcc* pcW, uint64_t uK,
              uint64_t uStart, uint64_t uEnd, const ComplexF* pcBase,
              ComplexAcc* pcOut)
{
    for (uint64_t n = uStart; n < uEnd; ++n) {
        const uint64_t uTaps = (n + 1 < uK) ? (n + 1) : uK;

        Acc aRe = Acc(0);
        Acc aIm = Acc(0);
        for (uint64_t k = 0; k < uTaps; ++k) {
            const Acc aWr = Acc(pcW[k].real()), aWi = Acc(pcW[k].imag());
            const Acc aRr = Acc(pcRef[n - k].real()), aRi = Acc(pcRef[n - k].imag());
            aRe += aWr * aRr - aWi * aRi;
            aIm += aWr * aRi + aWi * aRr;
        }

        if (pcBase != nullptr) {
            aRe = Acc(pcBase[n].real()) - aRe;
            aIm = Acc(pcBase[n].imag()) - aIm;
        }
        pcOut[n - uStart] = ComplexAcc(static_cast<double>(aRe),
                                       static_cast<double>(aIm));
    }
}

// s[k] = sum over n in [uStart, uEnd) of conj(r[n-k]) v[n - uStart].
template <typename Acc>
void adjointT(const ComplexF* pcRef, const ComplexAcc* pcV, uint64_t uK,
              uint64_t uStart, uint64_t uEnd, ComplexAcc* pcS)
{
    for (uint64_t k = 0; k < uK; ++k) {
        const uint64_t uFirst = (uStart > k) ? uStart : k;

        Acc aRe = Acc(0);
        Acc aIm = Acc(0);
        for (uint64_t n = uFirst; n < uEnd; ++n) {
            const Acc aAr = Acc(pcRef[n - k].real()), aAi = Acc(pcRef[n - k].imag());
            const Acc aBr = Acc(pcV[n - uStart].real()), aBi = Acc(pcV[n - uStart].imag());
            aRe += aAr * aBr + aAi * aBi;
            aIm += aAr * aBi - aAi * aBr;
        }
        pcS[k] = ComplexAcc(static_cast<double>(aRe), static_cast<double>(aIm));
    }
}

template <typename Acc>
Acc normSqT(const ComplexAcc* pc, uint64_t uCount)
{
    Acc a = Acc(0);
    for (uint64_t u = 0; u < uCount; ++u) {
        const Acc aRe = Acc(pc[u].real()), aIm = Acc(pc[u].imag());
        a += aRe * aRe + aIm * aIm;
    }
    return a;
}

// Re(a^H b)
template <typename Acc>
Acc dotRealT(const ComplexAcc* pcA, const ComplexAcc* pcB, uint64_t uCount)
{
    Acc a = Acc(0);
    for (uint64_t u = 0; u < uCount; ++u)
        a += Acc(pcA[u].real()) * Acc(pcB[u].real()) +
             Acc(pcA[u].imag()) * Acc(pcB[u].imag());
    return a;
}

// y += a x, with a real.
template <typename Acc>
void axpyT(Acc a, const ComplexAcc* pcX, ComplexAcc* pcY, uint64_t uCount)
{
    for (uint64_t u = 0; u < uCount; ++u) {
        pcY[u] = ComplexAcc(static_cast<double>(Acc(pcY[u].real()) + a * Acc(pcX[u].real())),
                            static_cast<double>(Acc(pcY[u].imag()) + a * Acc(pcX[u].imag())));
    }
}

// p = s + beta p, with beta real.
template <typename Acc>
void newDirectionT(Acc aBeta, const ComplexAcc* pcS, ComplexAcc* pcP, uint64_t uCount)
{
    for (uint64_t u = 0; u < uCount; ++u) {
        pcP[u] = ComplexAcc(static_cast<double>(Acc(pcS[u].real()) + aBeta * Acc(pcP[u].real())),
                            static_cast<double>(Acc(pcS[u].imag()) + aBeta * Acc(pcP[u].imag())));
    }
}

class CglsCpu : public DSICanceller {
public:
    CglsCpu() { std::memset(machName, 0, sizeof(machName)); }

    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override { return machName; }

private:
    template <typename Acc>
    void cancelT(const ComplexF* pcRef, const ComplexF* pcSur, ComplexF* pcOut,
                 uint64_t& uIterations);

    DSIParams               moParams;
    uint64_t                muTaps          = 0;   // K
    uint64_t                muWindows       = 0;
    uint64_t                muWindowSamples = 0;

    std::vector<ComplexAcc> mvcX;       // windows x K; kept across CPIs for priming
    std::vector<ComplexAcc> mvcP;       // K, search direction
    std::vector<ComplexAcc> mvcS;       // K, gradient
    std::vector<ComplexAcc> mvcR;       // one window, residual (and the output)
    std::vector<ComplexAcc> mvcQ;       // one window, R p

    bool                    mbHaveWeights = false;   // mvcX holds a previous CPI
    char                    machName[32];
    bool                    mbReady       = false;
};

bool CglsCpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "CglsCpu::init called twice");
    CHECK(oParams.eAlgorithm == DSIAlgorithm::Cgls, "CglsCpu handles CGLS only");
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uClutterTaps > 0, "clutter taps must be non-zero");
    CHECK(oParams.uCglsBatchSamples > 0, "the CGLS batch must be non-zero");
    CHECK(oParams.uMaxIterations > 0, "CGLS needs at least one iteration");
    CHECK(oParams.uDopplerTaps == 0,
          "ECA_DOPPLER_TAPS > 0 is not implemented; the clutter subspace is "
          "delay-only for now");
    // CGLS is batched like ECA-B: the weights are estimated from the window
    // they are applied to, on its own window length.
    CHECK(oParams.uCpiSamples % oParams.uCglsBatchSamples == 0,
          "the CGLS batch must divide the CPI exactly");
    CHECK(oParams.uCglsBatchSamples > oParams.uClutterTaps,
          "the CGLS batch must hold more samples than there are taps to solve for");

    moParams        = oParams;
    muTaps          = oParams.uClutterTaps;
    muWindowSamples = oParams.uCglsBatchSamples;
    muWindows       = oParams.uCpiSamples / oParams.uCglsBatchSamples;

    // Every allocation happens here and is reused for the life of the object.
    mvcX.assign(static_cast<size_t>(muWindows * muTaps), ComplexAcc());
    mvcP.assign(static_cast<size_t>(muTaps), ComplexAcc());
    mvcS.assign(static_cast<size_t>(muTaps), ComplexAcc());
    mvcR.assign(static_cast<size_t>(muWindowSamples), ComplexAcc());
    mvcQ.assign(static_cast<size_t>(muWindowSamples), ComplexAcc());

    std::snprintf(machName, sizeof(machName), "CGLS (CPU%s)",
                  oParams.bFloat32Math ? " FP32" : "");

    mbReady = true;
    return true;
}

template <typename Acc>
void CglsCpu::cancelT(const ComplexF* pcRef, const ComplexF* pcSur, ComplexF* pcOut,
                      uint64_t& uIterations)
{
    const uint64_t uK = muTaps;
    const uint64_t uW = muWindowSamples;
    uIterations = 0;

    for (uint64_t w = 0; w < muWindows; ++w) {
        const uint64_t uStart = w * uW;
        const uint64_t uEnd   = uStart + uW;
        ComplexAcc* pcX = &mvcX[static_cast<size_t>(w * uK)];

        // Start from zero, or from where this window ended up last CPI.
        if (!moParams.bCglsPrime || !mbHaveWeights)
            std::memset(pcX, 0, static_cast<size_t>(uK) * sizeof(ComplexAcc));

        // r = b - R x;  s = R^H r;  p = s;  gamma = |s|^2
        forwardT<Acc>(pcRef, pcX, uK, uStart, uEnd, pcSur, mvcR.data());
        adjointT<Acc>(pcRef, mvcR.data(), uK, uStart, uEnd, mvcS.data());
        Acc aGamma = normSqT<Acc>(mvcS.data(), uK);
        mvcP = mvcS;

        uint64_t uCount = 0;
        for (uint64_t it = 1; it <= moParams.uMaxIterations; ++it) {
            // q = R p;  alpha = Re(p^H s) / |q|^2  -- the line minimiser along
            // p, which equals gamma / |q|^2 only while p and s are conjugate.
            // See the file header for why the difference matters.
            forwardT<Acc>(pcRef, mvcP.data(), uK, uStart, uEnd, nullptr, mvcQ.data());
            const Acc aDelta = normSqT<Acc>(mvcQ.data(), uW);

            // A zero here means p is zero: the residual is already orthogonal
            // to every column and there is nothing left to fit. Only happens
            // on degenerate input such as an all-zero window.
            if (!(aDelta > Acc(0)))
                break;
            const Acc aAlpha = dotRealT<Acc>(mvcP.data(), mvcS.data(), uK) / aDelta;

            // x += alpha p
            axpyT<Acc>(aAlpha, mvcP.data(), pcX, uK);

            // r -= alpha q, or every so often the honest version.
            if (it % kResidualRefresh == 0)
                forwardT<Acc>(pcRef, pcX, uK, uStart, uEnd, pcSur, mvcR.data());
            else
                axpyT<Acc>(-aAlpha, mvcQ.data(), mvcR.data(), uW);

            // s = R^H r;  beta = |s|^2 / gamma;  p = s + beta p
            adjointT<Acc>(pcRef, mvcR.data(), uK, uStart, uEnd, mvcS.data());
            const Acc aGammaNew = normSqT<Acc>(mvcS.data(), uK);
            const Acc aBeta     = aGammaNew / aGamma;
            aGamma = aGammaNew;
            newDirectionT<Acc>(aBeta, mvcS.data(), mvcP.data(), uK);

            ++uCount;
        }
        if (uCount > uIterations)
            uIterations = uCount;

        // The output is b - R x computed fresh, not the drifted residual.
        forwardT<Acc>(pcRef, pcX, uK, uStart, uEnd, pcSur, mvcR.data());
        for (uint64_t n = uStart; n < uEnd; ++n) {
            const ComplexAcc& c = mvcR[static_cast<size_t>(n - uStart)];
            pcOut[n] = ComplexF(static_cast<float>(c.real()),
                                static_cast<float>(c.imag()));
        }
    }

    mbHaveWeights = true;
}

bool CglsCpu::cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                     ComplexF* pcOut, DSIStats& oStats)
{
    CHECK(mbReady, "CglsCpu::cancel before a successful init");
    CHECK(pcRef != nullptr && pcSur != nullptr && pcOut != nullptr,
          "null buffer passed to CglsCpu::cancel");
    CHECK(pcOut != pcRef && pcOut != pcSur,
          "the output buffer must not alias either input");

    oStats = DSIStats();

    uint64_t uIterations = 0;
    if (moParams.bFloat32Math)
        cancelT<float>(pcRef, pcSur, pcOut, uIterations);
    else
        cancelT<double>(pcRef, pcSur, pcOut, uIterations);

    // Suppression, as in EcaCpu: a headline number that cannot tell clutter
    // removed from targets removed.
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
    oStats.uIterations = uIterations;

    return true;
}

} // namespace

std::unique_ptr<DSICanceller> makeCglsCancellerCpu()
{
    return std::unique_ptr<DSICanceller>(new CglsCpu());
}

} // namespace pbr
