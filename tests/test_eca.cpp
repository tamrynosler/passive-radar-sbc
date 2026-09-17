// Unit tests for the CPU ECA cancellers (ECA-m and ECA-B).
//
// The golden-file comparison against MATLAB is a separate, larger check that
// needs a real captured CPI. These tests cover what can be checked with
// synthetic signals, where the right answer is known exactly:
//
//   * clutter built from a known set of taps is cancelled essentially to zero
//   * a target outside the cancelled delay range survives
//   * batching beats a single solve when the clutter changes during the CPI
//   * every parameter that is not implemented is refused, not ignored

#include "pbr/Config.hpp"
#include "pbr/DSICanceller.hpp"
#include "pbr/types.hpp"

#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using namespace pbr;

namespace {

// Deterministic generator, so a failure is always reproducible. Plain LCG --
// the statistical quality does not matter, only that it is repeatable and the
// same on every platform.
struct Lcg {
    uint64_t uState;
    explicit Lcg(uint64_t uSeed) : uState(uSeed) {}
    double next()   // uniform in [-1, 1)
    {
        uState = uState * 6364136223846793005ULL + 1442695040888963407ULL;
        const uint32_t u = static_cast<uint32_t>(uState >> 33);
        return (static_cast<double>(u) / 2147483648.0) - 1.0;
    }
};

std::vector<ComplexF> makeNoise(uint64_t uCount, uint64_t uSeed)
{
    Lcg oRng(uSeed);
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

DSIParams baseParams(DSIAlgorithm eAlgorithm, uint64_t uCpi, uint64_t uTaps,
                     uint64_t uWindow)
{
    DSIParams o;
    o.eAlgorithm       = eAlgorithm;
    o.eBackend         = DSIBackend::Cpu;
    o.uCpiSamples      = uCpi;
    o.uClutterTaps     = uTaps;
    o.uEstimateSamples = uWindow;
    o.uUpdateSamples   = uWindow;
    o.uDopplerTaps     = 0;
    o.dDiagonalLoading = 0.0;
    o.bAbsoluteLoading = false;
    return o;
}

// ---------------------------------------------------------------------------

void testRejectsWhatIsNotImplemented()
{
    // Algorithms that do not exist yet must fail at the factory, not quietly
    // become something else.
    assert(!makeDSICanceller(DSIAlgorithm::EcaS,  DSIBackend::Cpu));
    assert(!makeDSICanceller(DSIAlgorithm::EcaCd, DSIBackend::Cpu));
    assert(!makeDSICanceller(DSIAlgorithm::Cgls,  DSIBackend::Cpu));

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

    // An estimation window longer than the update window is ECA-S.
    o = baseParams(DSIAlgorithm::EcaB, 1024, 8, 256);
    o.uEstimateSamples = 512;
    assert(!p->init(o));

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

// Clutter built from a known set of taps must be cancelled almost perfectly:
// the answer is exactly in the subspace the canceller searches.
void testCancelsKnownClutter(DSIAlgorithm eAlgorithm, uint64_t uWindow,
                             const char* pLabel)
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
    assert(p->init(baseParams(eAlgorithm, uCpi, uTaps, uWindow)));

    std::vector<ComplexF> vcOut(static_cast<size_t>(uCpi));
    DSIStats oStats;
    assert(p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats));

    // Nothing should be left but float rounding in the constructed input.
    assert(oStats.dSuppressionDb > 60.0);
    assert(totalPower(vcOut) < 1e-6 * totalPower(vcSur));

    std::printf("  %s cancels known clutter (%.1f dB) %s ok\n",
                pLabel, oStats.dSuppressionDb,
                (eAlgorithm == DSIAlgorithm::EcaM) ? "........." : ".........");
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

} // namespace

int main()
{
    std::printf("test_eca\n");

    testRejectsWhatIsNotImplemented();
    testRejectsBadBuffers();
    testCancelsKnownClutter(DSIAlgorithm::EcaM, 4096, "ECA-m");
    testCancelsKnownClutter(DSIAlgorithm::EcaB, 512,  "ECA-B");
    testPreservesTargetBeyondTaps();
    testBatchingBeatsSingleSolveOnVaryingClutter();
    testSingleWindowBatchMatchesEcaM();
    testConfigMapping();

    std::printf("test_eca: all tests passed\n");
    return 0;
}
