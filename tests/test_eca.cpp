// Unit tests for the ECA cancellers (ECA-m, ECA-B and ECA-S).
//
// The golden-file comparison against MATLAB is a separate, larger check that
// needs a real captured CPI. These tests cover what can be checked with
// synthetic signals, where the right answer is known exactly:
//
//   * clutter built from a known set of taps is cancelled essentially to zero
//   * a target outside the cancelled delay range survives
//   * batching beats a single solve when the clutter changes during the CPI
//   * ECA-S's estimation window lands where it should, including at the edges
//   * every parameter that is not implemented is refused, not ignored
//
// The CPU runs in FP32 throughout, so every figure here is comparable with
// the GPU, which has no other option.

#include "pbr/Config.hpp"
#include "pbr/DSICanceller.hpp"
#include "pbr/types.hpp"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using namespace pbr;

namespace {

// Deterministic generator, so a failure is always reproducible, and the same
// on every platform. splitmix64.
//
// Two mistakes were made here before, both of which quietly corrupted the
// tests that compare two independent signals:
//
//   1. The original LCG returned  uint32_t(state >> 33) / 2^31 - 1, which is
//      31 bits over 2^31 -- a range of [-1, 0), not [-1, 1). Every sample
//      carried a -0.5 DC offset, and a constant offset is exactly the kind of
//      thing a clutter canceller removes perfectly. "Uncorrelated" signals
//      cancelled by 6.3 dB when theory says 0.28.
//   2. Seeding splitmix64 as  state = seed * GAMMA  and then stepping by
//      += GAMMA means seed s+1 produces precisely stream s offset by one
//      output. Adjacent seeds gave the same signal, shifted.
//
// Hence the seed is hashed below, and testRngIsUsable() checks both
// properties before anything depends on them.
struct Rng {
    uint64_t uState;

    explicit Rng(uint64_t uSeed)
    {
        uint64_t z = uSeed + 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        uState = z ^ (z >> 31);
    }

    double next()   // uniform in [-1, 1)
    {
        uState += 0x9E3779B97F4A7C15ULL;
        uint64_t z = uState;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z =  z ^ (z >> 31);
        return (static_cast<double>(z >> 11) / 9007199254740992.0) * 2.0 - 1.0;
    }
};

std::vector<ComplexF> makeNoise(uint64_t uCount, uint64_t uSeed)
{
    Rng oRng(uSeed);
    std::vector<ComplexF> vc(static_cast<size_t>(uCount));
    for (size_t u = 0; u < vc.size(); ++u)
        vc[u] = ComplexF(static_cast<float>(oRng.next()),
                         static_cast<float>(oRng.next()));
    return vc;
}

double totalPower(const std::vector<ComplexF>& vc)
{
    double d = 0.0;
    for (size_t u = 0; u < vc.size(); ++u)
        d += double(vc[u].real()) * vc[u].real() + double(vc[u].imag()) * vc[u].imag();
    return d;
}

// Estimation and update windows given separately: ECA-S.
DSIParams slidingParams(DSIAlgorithm eAlgorithm, uint64_t uCpi, uint64_t uTaps,
                        uint64_t uEstimate, uint64_t uUpdate)
{
    DSIParams o;
    o.eAlgorithm       = eAlgorithm;
    o.eBackend         = DSIBackend::Cpu;
    o.uCpiSamples      = uCpi;
    o.uClutterTaps     = uTaps;
    o.uEstimateSamples = uEstimate;
    o.uUpdateSamples   = uUpdate;
    o.uDopplerTaps     = 0;
    o.dDiagonalLoading = 0.0;
    o.bAbsoluteLoading = false;
    o.bFloat32Math     = true;
    return o;
}

// One window for both: ECA-m and ECA-B.
DSIParams baseParams(DSIAlgorithm eAlgorithm, uint64_t uCpi, uint64_t uTaps,
                     uint64_t uWindow)
{
    return slidingParams(eAlgorithm, uCpi, uTaps, uWindow, uWindow);
}

// ---------------------------------------------------------------------------

// The test data generator is itself load-bearing: two bugs in it previously
// hid a real result by three orders of magnitude. Check it before anything
// depends on it.
void testRngIsUsable()
{
    const uint64_t uCount = 100000;

    // Zero mean. A DC offset is perfectly cancellable, so it would show up as
    // suppression that has nothing to do with the algorithm.
    const std::vector<ComplexF> vcA = makeNoise(uCount, 101);
    double dRe = 0.0, dIm = 0.0;
    for (size_t u = 0; u < vcA.size(); ++u) {
        dRe += vcA[u].real();
        dIm += vcA[u].imag();
    }
    assert(std::fabs(dRe / double(uCount)) < 0.01);
    assert(std::fabs(dIm / double(uCount)) < 0.01);

    // Adjacent seeds must give unrelated streams, not the same one shifted.
    // Peak cross-correlation over the delays a canceller searches; chance
    // alone gives about 1/sqrt(N).
    const std::vector<ComplexF> vcB = makeNoise(uCount, 102);
    double dPa = 0.0, dPb = 0.0;
    for (size_t u = 0; u < uCount; ++u) {
        dPa += std::norm(std::complex<double>(vcA[u].real(), vcA[u].imag()));
        dPb += std::norm(std::complex<double>(vcB[u].real(), vcB[u].imag()));
    }
    double dPeak = 0.0;
    for (uint64_t uLag = 0; uLag < 64; ++uLag) {
        std::complex<double> cAcc(0.0, 0.0);
        for (uint64_t n = uLag; n < uCount; ++n) {
            cAcc += std::conj(std::complex<double>(vcA[n - uLag].real(),
                                                   vcA[n - uLag].imag())) *
                    std::complex<double>(vcB[n].real(), vcB[n].imag());
        }
        dPeak = std::max(dPeak, std::abs(cAcc) / std::sqrt(dPa * dPb));
    }
    assert(dPeak < 0.05);

    std::printf("  test RNG: zero mean, adjacent seeds uncorrelated (%.4f)  ok\n",
                dPeak);
}

void testRejectsWhatIsNotImplemented()
{
#if !defined(PBR_WITH_CUDA)
    // No CUDA in this build: asking for the GPU must fail rather than hand
    // back a CPU canceller that would be labelled GPU in the results.
    assert(!makeDSICanceller(DSIAlgorithm::EcaB, DSIBackend::Gpu));
#endif

    std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaB,
                                                       DSIBackend::Cpu);
    assert(p);

    // Doppler taps are a different algorithm, not a tuning knob.
    DSIParams o = baseParams(DSIAlgorithm::EcaB, 1024, 8, 256);
    o.uDopplerTaps = 1;
    assert(!p->init(o));

    // An estimation window longer than the update window is ECA-S, and equal
    // windows are ECA-B. Each name is refused the other's windows, so a
    // benchmark row cannot carry the wrong label.
    o = baseParams(DSIAlgorithm::EcaB, 1024, 8, 256);
    o.uEstimateSamples = 512;
    assert(!p->init(o));
    {
        std::unique_ptr<DSICanceller> pS = makeDSICanceller(DSIAlgorithm::EcaS,
                                                            DSIBackend::Cpu);
        assert(pS);
        assert(!pS->init(baseParams(DSIAlgorithm::EcaS, 1024, 8, 256)));
        // Nor can the estimation window be longer than the CPI it is cut from.
        assert(!pS->init(slidingParams(DSIAlgorithm::EcaS, 1024, 8, 2048, 256)));
        assert(pS->init(slidingParams(DSIAlgorithm::EcaS, 1024, 8, 512, 256)));
    }

    // A window that does not divide the CPI leaves a short trailing fragment.
    o = baseParams(DSIAlgorithm::EcaB, 1024, 8, 300);
    assert(!p->init(o));

    // Fewer samples than unknowns is an underdetermined problem.
    o = baseParams(DSIAlgorithm::EcaB, 1024, 300, 256);
    assert(!p->init(o));

    // A good one, and then a second init on the same object.
    o = baseParams(DSIAlgorithm::EcaB, 1024, 8, 256);
    assert(p->init(o));
    assert(!p->init(o));

    std::printf("  rejects unimplemented options ...................... ok\n");
}

void testRejectsBadBuffers()
{
    const uint64_t uCpi = 1024;
    std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaB,
                                                       DSIBackend::Cpu);
    assert(p && p->init(baseParams(DSIAlgorithm::EcaB, uCpi, 8, 256)));

    std::vector<ComplexF> vcRef = makeNoise(uCpi, 1);
    std::vector<ComplexF> vcSur = makeNoise(uCpi, 2);
    std::vector<ComplexF> vcOut(static_cast<size_t>(uCpi));
    DSIStats oStats;

    assert(!p->cancel(nullptr, vcSur.data(), vcOut.data(), oStats));
    assert(!p->cancel(vcRef.data(), nullptr, vcOut.data(), oStats));
    assert(!p->cancel(vcRef.data(), vcSur.data(), nullptr, oStats));

    // Writing the output over an input would destroy the signal partway
    // through and make the suppression figure meaningless.
    assert(!p->cancel(vcRef.data(), vcSur.data(), vcSur.data(), oStats));
    assert(!p->cancel(vcRef.data(), vcSur.data(), vcRef.data(), oStats));

    assert(p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats));

    std::printf("  rejects null and aliased buffers ................... ok\n");
}

// ECA-S's estimation window is centred on the update window and clamped to
// the CPI. Worked by hand for a 1024-sample CPI, 512-sample estimation
// window, 128-sample update window: the centred start is
// uStart + 64 - 256 = uStart - 192, which is negative for the first two
// windows and runs past the end for the last two.
void testEstimationWindowClamp()
{
    const uint64_t uCpi = 1024, uEst = 512, uUpd = 128;

    const uint64_t auExpected[8] = { 0, 0, 64, 192, 320, 448, 512, 512 };
    for (uint64_t w = 0; w < 8; ++w) {
        const uint64_t uStart = dsiEstimationStart(w * uUpd, uUpd, uEst, uCpi);
        assert(uStart == auExpected[w]);
        assert(uStart + uEst <= uCpi);
    }

    // Equal windows collapse to the update window itself: that is ECA-B.
    for (uint64_t w = 0; w < 8; ++w)
        assert(dsiEstimationStart(w * uUpd, uUpd, uUpd, uCpi) == w * uUpd);

    // An estimation window as long as the CPI always starts at zero.
    for (uint64_t w = 0; w < 8; ++w)
        assert(dsiEstimationStart(w * uUpd, uUpd, uCpi, uCpi) == 0);

    std::printf("  ECA-S estimation window centres and clamps ......... ok\n");
}

// Clutter built from a known set of taps must be cancelled almost perfectly:
// the answer is exactly in the subspace the canceller searches.
void testCancelsKnownClutter(DSIAlgorithm eAlgorithm, uint64_t uEstimate,
                             uint64_t uWindow, const char* pLabel)
{
    const uint64_t uCpi  = 4096;
    const uint64_t uTaps = 8;

    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 11);

    // An arbitrary but fixed impulse response, strongest at zero delay the way
    // the direct signal is.
    std::complex<double> acTrue[8];
    for (uint64_t k = 0; k < uTaps; ++k)
        acTrue[k] = std::complex<double>(1.0 / (1.0 + double(k)),
                                         0.3 / (1.0 + double(k)));

    std::vector<ComplexF> vcSur(static_cast<size_t>(uCpi));
    for (uint64_t n = 0; n < uCpi; ++n) {
        std::complex<double> cAcc(0.0, 0.0);
        const uint64_t uK = (n + 1 < uTaps) ? (n + 1) : uTaps;
        for (uint64_t k = 0; k < uK; ++k) {
            const ComplexF& cR = vcRef[static_cast<size_t>(n - k)];
            cAcc += acTrue[k] * std::complex<double>(cR.real(), cR.imag());
        }
        vcSur[static_cast<size_t>(n)] = ComplexF(static_cast<float>(cAcc.real()),
                                                 static_cast<float>(cAcc.imag()));
    }

    std::unique_ptr<DSICanceller> p = makeDSICanceller(eAlgorithm, DSIBackend::Cpu);
    assert(p);
    // No loading: the problem is well posed, and loading would deliberately
    // leave some clutter behind.
    assert(p->init(slidingParams(eAlgorithm, uCpi, uTaps, uEstimate, uWindow)));

    std::vector<ComplexF> vcOut(static_cast<size_t>(uCpi));
    DSIStats oStats;
    assert(p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats));

    // Nothing should be left but float rounding in the constructed input.
    assert(oStats.dSuppressionDb > 60.0);
    assert(totalPower(vcOut) < 1e-6 * totalPower(vcSur));

    std::printf("  %s cancels known clutter (%.1f dB) ......... ok\n",
                pLabel, oStats.dSuppressionDb);
}

// With an estimation window as long as the CPI, every ECA-S update window is
// solved from the same samples, [0, N), so every window gets the same
// weights -- the ones ECA-m computes. Applying one set of weights in four
// pieces or in one piece is the same arithmetic, so the outputs must be
// identical, not merely close. This pins the estimation-window plumbing
// without needing a reference implementation of ECA-S.
void testSlidingWithCpiLongEstimateMatchesEcaM()
{
    const uint64_t uCpi  = 2048;
    const uint64_t uTaps = 6;

    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 61);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 62);

    std::vector<ComplexF> vcOutM(static_cast<size_t>(uCpi));
    std::vector<ComplexF> vcOutS(static_cast<size_t>(uCpi));
    DSIStats oStatsM, oStatsS;

    {
        std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaM,
                                                           DSIBackend::Cpu);
        assert(p && p->init(baseParams(DSIAlgorithm::EcaM, uCpi, uTaps, uCpi)));
        assert(p->cancel(vcRef.data(), vcSur.data(), vcOutM.data(), oStatsM));
    }
    {
        std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaS,
                                                           DSIBackend::Cpu);
        assert(p && p->init(slidingParams(DSIAlgorithm::EcaS, uCpi, uTaps,
                                          uCpi, uCpi / 4)));
        assert(p->cancel(vcRef.data(), vcSur.data(), vcOutS.data(), oStatsS));
    }

    for (uint64_t n = 0; n < uCpi; ++n) {
        assert(vcOutS[n].real() == vcOutM[n].real());
        assert(vcOutS[n].imag() == vcOutM[n].imag());
    }

    std::printf("  ECA-S with a CPI-long estimate matches ECA-m ....... ok\n");
}

// A target beyond the last cancelled delay bin is outside the clutter
// subspace, so it must come through essentially untouched. An algorithm that
// suppresses well by eating the targets would fail this.
void testPreservesTargetBeyondTaps()
{
    const uint64_t uCpi  = 4096;
    const uint64_t uTaps = 8;
    const uint64_t uDelay = 40;          // well past the last cancelled tap
    const double   dTargetAmp = 0.02;    // far below the clutter

    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 23);

    std::vector<ComplexF> vcSur(static_cast<size_t>(uCpi));
    std::vector<ComplexF> vcTarget(static_cast<size_t>(uCpi), ComplexF(0.0f, 0.0f));

    for (uint64_t n = 0; n < uCpi; ++n) {
        // Strong stationary clutter across the first few taps.
        std::complex<double> cAcc(0.0, 0.0);
        const uint64_t uK = (n + 1 < uTaps) ? (n + 1) : uTaps;
        for (uint64_t k = 0; k < uK; ++k) {
            const ComplexF& cR = vcRef[static_cast<size_t>(n - k)];
            cAcc += std::complex<double>(1.0 / (1.0 + double(k)), 0.0) *
                    std::complex<double>(cR.real(), cR.imag());
        }

        // A weak moving echo, delayed and Doppler shifted.
        if (n >= uDelay) {
            const double dPhase = 2.0 * 3.14159265358979323846 * 0.01 * double(n);
            const ComplexF& cR  = vcRef[static_cast<size_t>(n - uDelay)];
            const std::complex<double> cEcho =
                dTargetAmp * std::complex<double>(std::cos(dPhase), std::sin(dPhase)) *
                std::complex<double>(cR.real(), cR.imag());
            vcTarget[static_cast<size_t>(n)] = ComplexF(static_cast<float>(cEcho.real()),
                                                        static_cast<float>(cEcho.imag()));
            cAcc += cEcho;
        }

        vcSur[static_cast<size_t>(n)] = ComplexF(static_cast<float>(cAcc.real()),
                                                 static_cast<float>(cAcc.imag()));
    }

    std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaB,
                                                       DSIBackend::Cpu);
    assert(p);
    assert(p->init(baseParams(DSIAlgorithm::EcaB, uCpi, uTaps, 1024)));

    std::vector<ComplexF> vcOut(static_cast<size_t>(uCpi));
    DSIStats oStats;
    assert(p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats));

    // The clutter goes; the target stays. Compare what is left against the
    // target that was put in.
    const double dTargetPower = totalPower(vcTarget);
    double dResidual = 0.0;
    for (uint64_t n = 0; n < uCpi; ++n) {
        const double dRe = double(vcOut[n].real()) - vcTarget[n].real();
        const double dIm = double(vcOut[n].imag()) - vcTarget[n].imag();
        dResidual += dRe * dRe + dIm * dIm;
    }

    assert(oStats.dSuppressionDb > 20.0);
    assert(dResidual < 0.05 * dTargetPower);   // target recovered to within 5%

    std::printf("  preserves a target past the taps (%.1f dB) ......... ok\n",
                oStats.dSuppressionDb);
}

// The reason ECA-B exists: when the clutter changes during the CPI, one set of
// weights for the whole second fits none of it well.
void testBatchingBeatsSingleSolveOnVaryingClutter()
{
    const uint64_t uCpi  = 4096;
    const uint64_t uTaps = 4;

    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 37);

    std::vector<ComplexF> vcSur(static_cast<size_t>(uCpi));
    for (uint64_t n = 0; n < uCpi; ++n) {
        // The first tap's weight sweeps across the CPI -- a scatterer whose
        // reflectivity or phase drifts, which is exactly the nonstationary
        // case Colone's batches version was designed for.
        const double dFrac  = double(n) / double(uCpi);
        const double dPhase = 2.0 * 3.14159265358979323846 * dFrac;

        std::complex<double> cAcc(0.0, 0.0);
        const uint64_t uK = (n + 1 < uTaps) ? (n + 1) : uTaps;
        for (uint64_t k = 0; k < uK; ++k) {
            const std::complex<double> cW =
                (k == 0) ? std::complex<double>(std::cos(dPhase), std::sin(dPhase))
                         : std::complex<double>(0.5 / (1.0 + double(k)), 0.0);
            const ComplexF& cR = vcRef[static_cast<size_t>(n - k)];
            cAcc += cW * std::complex<double>(cR.real(), cR.imag());
        }
        vcSur[static_cast<size_t>(n)] = ComplexF(static_cast<float>(cAcc.real()),
                                                 static_cast<float>(cAcc.imag()));
    }

    std::vector<ComplexF> vcOut(static_cast<size_t>(uCpi));
    DSIStats oSingle;
    DSIStats oBatched;

    {
        std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaM,
                                                           DSIBackend::Cpu);
        assert(p);
        DSIParams o = baseParams(DSIAlgorithm::EcaM, uCpi, uTaps, uCpi);
        assert(p->init(o));
        assert(p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oSingle));
    }
    {
        std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaB,
                                                           DSIBackend::Cpu);
        assert(p);
        assert(p->init(baseParams(DSIAlgorithm::EcaB, uCpi, uTaps, 128)));
        assert(p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oBatched));
    }

    assert(oBatched.dSuppressionDb > oSingle.dSuppressionDb + 10.0);

    std::printf("  batching beats one solve on varying clutter\n");
    std::printf("      ECA-m %.1f dB, ECA-B %.1f dB ................... ok\n",
                oSingle.dSuppressionDb, oBatched.dSuppressionDb);
}

// ECA-B with a single window is doing arithmetically the same thing as ECA-m,
// so on stationary clutter they must agree closely. The only deliberate
// difference is the diagonal loading policy.
void testSingleWindowBatchMatchesEcaM()
{
    const uint64_t uCpi  = 2048;
    const uint64_t uTaps = 6;

    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 51);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 52);

    std::vector<ComplexF> vcOutM(static_cast<size_t>(uCpi));
    std::vector<ComplexF> vcOutB(static_cast<size_t>(uCpi));
    DSIStats oStatsM, oStatsB;

    {
        std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaM,
                                                           DSIBackend::Cpu);
        assert(p && p->init(baseParams(DSIAlgorithm::EcaM, uCpi, uTaps, uCpi)));
        assert(p->cancel(vcRef.data(), vcSur.data(), vcOutM.data(), oStatsM));
    }
    {
        std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaB,
                                                           DSIBackend::Cpu);
        assert(p && p->init(baseParams(DSIAlgorithm::EcaB, uCpi, uTaps, uCpi)));
        assert(p->cancel(vcRef.data(), vcSur.data(), vcOutB.data(), oStatsB));
    }

    double dDiff = 0.0;
    for (uint64_t n = 0; n < uCpi; ++n) {
        const double dRe = double(vcOutM[n].real()) - vcOutB[n].real();
        const double dIm = double(vcOutM[n].imag()) - vcOutB[n].imag();
        dDiff += dRe * dRe + dIm * dIm;
    }
    assert(dDiff < 1e-8 * totalPower(vcOutM));

    std::printf("  ECA-B with one window matches ECA-m ............... ok\n");
}

void testConfigMapping()
{
    PbrConfig oCfg;
    oCfg.uCpiSamples         = 204800;
    oCfg.uClutterTaps        = 120;
    oCfg.uEcaEstimateSamples = 5120;
    oCfg.uEcaUpdateSamples   = 5120;
    oCfg.uEcaDopplerTaps     = 0;
    oCfg.dEcaDiagLoading     = 1e-6;
    oCfg.uCdBatchSamples     = 512;
    oCfg.uCdDopplerTaps      = 1;
    oCfg.uCglsIterations     = 30;
    oCfg.bCglsPrime          = true;
    oCfg.sDsiBackend         = "CPU";

    DSIParams oParams;

    // ECA-B takes the configured windows as they are.
    oCfg.sDsiAlgorithm = "ECA-B";
    assert(dsiParamsFromConfig(oCfg, oParams));
    assert(oParams.eAlgorithm == DSIAlgorithm::EcaB);
    assert(oParams.eBackend == DSIBackend::Cpu);
    assert(oParams.uEstimateSamples == 5120);
    assert(oParams.uUpdateSamples == 5120);
    assert(!oParams.bAbsoluteLoading);
    assert(oParams.dDiagonalLoading == 1e-6);
    // The other algorithms' fields ride along unchanged.
    assert(oParams.uCdBatchSamples == 512);
    assert(oParams.uCdDopplerTaps == 1);
    assert(oParams.uMaxIterations == 30);
    assert(oParams.bCglsPrime);

    // ECA-m overrides them: one window over the whole CPI, and the MATLAB
    // reference's absolute loading rather than a relative fraction.
    oCfg.sDsiAlgorithm = "ECA-m";
    assert(dsiParamsFromConfig(oCfg, oParams));
    assert(oParams.uEstimateSamples == 204800);
    assert(oParams.uUpdateSamples == 204800);
    assert(oParams.bAbsoluteLoading);
    assert(oParams.dDiagonalLoading == 1e-3);

    // Names round-trip in the spelling the config file uses.
    DSIAlgorithm eAlgorithm = DSIAlgorithm::EcaM;
    assert(dsiAlgorithmFromString("ECA-CD", eAlgorithm));
    assert(eAlgorithm == DSIAlgorithm::EcaCd);
    assert(std::string(dsiAlgorithmName(eAlgorithm)) == "ECA-CD");
    assert(!dsiAlgorithmFromString("eca-b", eAlgorithm));   // case matters
    assert(!dsiAlgorithmFromString("ECA", eAlgorithm));

    DSIBackend eBackend = DSIBackend::Cpu;
    assert(dsiBackendFromString("GPU", eBackend));
    assert(eBackend == DSIBackend::Gpu);
    assert(!dsiBackendFromString("cuda", eBackend));

    oCfg.sDsiAlgorithm = "nonsense";
    assert(!dsiParamsFromConfig(oCfg, oParams));

    std::printf("  config maps onto DSIParams ........................ ok\n");
}

// ---------------------------------------------------------------------------
// GPU backend. Only built where CUDA is.
// ---------------------------------------------------------------------------
#if defined(PBR_WITH_CUDA)

// Run one algorithm on one backend over the given signals.
bool runBackend(DSIAlgorithm eAlgorithm, DSIBackend eBackend, bool bFloat32,
                const std::vector<ComplexF>& vcRef,
                const std::vector<ComplexF>& vcSur,
                uint64_t uTaps, uint64_t uEstimate, uint64_t uWindow,
                std::vector<ComplexF>& vcOut, DSIStats& oStats)
{
    DSIParams o = slidingParams(eAlgorithm, vcRef.size(), uTaps, uEstimate, uWindow);
    o.eBackend     = eBackend;
    o.bFloat32Math = bFloat32;

    std::unique_ptr<DSICanceller> p = makeDSICanceller(eAlgorithm, eBackend);
    if (!p || !p->init(o))
        return false;

    vcOut.assign(vcRef.size(), ComplexF());
    return p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats);
}

double relativeL2(const std::vector<ComplexF>& vcA, const std::vector<ComplexF>& vcB)
{
    double dDiff = 0.0, dRef = 0.0;
    for (size_t u = 0; u < vcA.size(); ++u) {
        const double dRe = double(vcA[u].real()) - vcB[u].real();
        const double dIm = double(vcA[u].imag()) - vcB[u].imag();
        dDiff += dRe * dRe + dIm * dIm;
        dRef  += double(vcB[u].real()) * vcB[u].real() +
                 double(vcB[u].imag()) * vcB[u].imag();
    }
    return (dRef > 0.0) ? std::sqrt(dDiff / dRef) : 0.0;
}

// The real structural check on the GPU port.
//
// On deeply-cancelled real data the GPU and the CPU cannot be compared
// tightly: both work in single precision, but cuBLAS sums in a different
// ORDER, and at 50 dB of suppression that difference is amplified by the
// suppression ratio into ~1e-2. That tolerance is too loose to catch an
// off-by-one.
//
// With UNCORRELATED signals there is almost nothing to cancel, so there is no
// amplification, and the two backends must agree closely. Any indexing or
// race bug still shows up at full size, because the arithmetic is identical.
void testGpuMatchesCpuWithoutAmplification()
{
    const uint64_t uCpi = 8192, uTaps = 64, uWindow = 1024;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 101);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 202);   // uncorrelated

    std::vector<ComplexF> vcCpu, vcGpu;
    DSIStats oCpu, oGpu;
    assert(runBackend(DSIAlgorithm::EcaB, DSIBackend::Cpu, true,
                      vcRef, vcSur, uTaps, uWindow, uWindow, vcCpu, oCpu));
    assert(runBackend(DSIAlgorithm::EcaB, DSIBackend::Gpu, true,
                      vcRef, vcSur, uTaps, uWindow, uWindow, vcGpu, oGpu));

    const double dRel = relativeL2(vcGpu, vcCpu);
    std::printf("  GPU vs CPU-FP32, no amplification: relL2 %.2e, "
                "suppression %.3f dB\n", dRel, oCpu.dSuppressionDb);

    // Fitting K complex taps over an N-sample window absorbs about K/N of the
    // energy even from uncorrelated noise, so a little suppression is
    // expected. What matters is that it is nowhere near the 50 dB of real
    // data, because that is the factor the Gram rounding gets amplified by.
    assert(oCpu.dSuppressionDb < 1.0);
    // Measured 3.4e-07. This is the tolerance that actually catches GPU bugs:
    // against the deeply-cancelled real CPI the same comparison is only good
    // to ~1e-2, because the suppression ratio amplifies the difference in
    // cuBLAS's summation order.
    assert(dRel < 1e-6);

    // ECA-S: the estimation window is 4x the update window, so the first two
    // and last two windows are clamped and the rest centred. An edge mistake
    // in either backend's chunk loop would show up here.
    assert(runBackend(DSIAlgorithm::EcaS, DSIBackend::Cpu, true,
                      vcRef, vcSur, uTaps, 4 * uWindow, uWindow, vcCpu, oCpu));
    assert(runBackend(DSIAlgorithm::EcaS, DSIBackend::Gpu, true,
                      vcRef, vcSur, uTaps, 4 * uWindow, uWindow, vcGpu, oGpu));
    const double dRelS = relativeL2(vcGpu, vcCpu);
    std::printf("  GPU vs CPU-FP32, ECA-S:            relL2 %.2e, "
                "suppression %.3f dB\n", dRelS, oCpu.dSuppressionDb);
    assert(dRelS < 1e-6);
}

// Clutter built from known taps must still cancel enormously on the GPU. An
// indexing bug would leave the strongest component uncancelled and collapse
// this figure to near zero, so it is a blunt but very sensitive check.
void testGpuCancelsKnownClutter()
{
    const uint64_t uCpi = 8192, uTaps = 8, uWindow = 1024;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 303);

    std::vector<ComplexF> vcSur(static_cast<size_t>(uCpi));
    for (uint64_t n = 0; n < uCpi; ++n) {
        std::complex<double> cAcc(0.0, 0.0);
        const uint64_t uK = (n + 1 < uTaps) ? (n + 1) : uTaps;
        for (uint64_t k = 0; k < uK; ++k) {
            const ComplexF& cR = vcRef[static_cast<size_t>(n - k)];
            cAcc += std::complex<double>(1.0 / (1.0 + double(k)), 0.2) *
                    std::complex<double>(cR.real(), cR.imag());
        }
        vcSur[static_cast<size_t>(n)] = ComplexF(float(cAcc.real()), float(cAcc.imag()));
    }

    std::vector<ComplexF> vcGpu;
    DSIStats oGpu;
    assert(runBackend(DSIAlgorithm::EcaB, DSIBackend::Gpu, true,
                      vcRef, vcSur, uTaps, uWindow, uWindow, vcGpu, oGpu));
    std::printf("  GPU cancels known clutter: %.1f dB\n", oGpu.dSuppressionDb);
    assert(oGpu.dSuppressionDb > 60.0);
}

// A race would show up as run-to-run variation. The same input must give
// bit-identical output every time.
void testGpuIsDeterministic()
{
    const uint64_t uCpi = 8192, uTaps = 32, uWindow = 2048;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 404);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 505);

    DSIParams o = baseParams(DSIAlgorithm::EcaB, uCpi, uTaps, uWindow);
    o.eBackend = DSIBackend::Gpu;

    std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaB,
                                                       DSIBackend::Gpu);
    assert(p && p->init(o));

    std::vector<ComplexF> vcFirst(static_cast<size_t>(uCpi));
    std::vector<ComplexF> vcAgain(static_cast<size_t>(uCpi));
    DSIStats oStats;
    assert(p->cancel(vcRef.data(), vcSur.data(), vcFirst.data(), oStats));
    for (int i = 0; i < 3; ++i) {
        assert(p->cancel(vcRef.data(), vcSur.data(), vcAgain.data(), oStats));
        for (uint64_t n = 0; n < uCpi; ++n) {
            assert(vcAgain[n].real() == vcFirst[n].real());
            assert(vcAgain[n].imag() == vcFirst[n].imag());
        }
    }

    std::printf("  GPU is bit-identical across runs ................... ok\n");
}

#endif // PBR_WITH_CUDA

} // namespace

int main()
{
    std::printf("test_eca\n");

    testRngIsUsable();
    testRejectsWhatIsNotImplemented();
    testRejectsBadBuffers();
    testEstimationWindowClamp();
    testCancelsKnownClutter(DSIAlgorithm::EcaM, 4096, 4096, "ECA-m");
    testCancelsKnownClutter(DSIAlgorithm::EcaB, 512,  512,  "ECA-B");
    testCancelsKnownClutter(DSIAlgorithm::EcaS, 1024, 256,  "ECA-S");
    testPreservesTargetBeyondTaps();
    testBatchingBeatsSingleSolveOnVaryingClutter();
    testSingleWindowBatchMatchesEcaM();
    testSlidingWithCpiLongEstimateMatchesEcaM();
    testConfigMapping();

#if defined(PBR_WITH_CUDA)
    testGpuMatchesCpuWithoutAmplification();
    testGpuCancelsKnownClutter();
    testGpuIsDeterministic();
#endif

    std::printf("test_eca: all tests passed\n");
    return 0;
}
