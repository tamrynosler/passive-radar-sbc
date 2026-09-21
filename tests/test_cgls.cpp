// Unit tests for the CGLS canceller.
//
// There is no MATLAB golden file for CGLS. The anchor is instead ECA-B: both
// minimise the same least-squares problem per window, so CGLS run long
// enough on a well-conditioned problem must land on ECA-B's answer. Beyond
// that, the same synthetic checks the ECA tests use:
//
//   * clutter built from known taps is cancelled almost to nothing
//   * a target past the last tap survives
//   * the iteration count is what was asked for, and priming helps
//
// All CPU runs are FP32, so every figure is comparable with the GPU.

#include "pbr/DSICanceller.hpp"
#include "pbr/types.hpp"
#include "test_signals.hpp"

#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

using namespace pbr;
using namespace pbrtest;

namespace {

DSIParams cglsParams(uint64_t uCpi, uint64_t uTaps, uint64_t uWindow,
                     uint64_t uIterations, bool bPrime)
{
    DSIParams o;
    o.eAlgorithm       = DSIAlgorithm::Cgls;
    o.eBackend         = DSIBackend::Cpu;
    o.uCpiSamples      = uCpi;
    o.uClutterTaps     = uTaps;
    o.uEstimateSamples = uWindow;
    o.uUpdateSamples   = uWindow;
    o.uDopplerTaps     = 0;
    o.uMaxIterations   = uIterations;
    o.bCglsPrime       = bPrime;
    o.bFloat32Math     = true;
    return o;
}

DSIParams ecaBParams(uint64_t uCpi, uint64_t uTaps, uint64_t uWindow)
{
    DSIParams o;
    o.eAlgorithm       = DSIAlgorithm::EcaB;
    o.eBackend         = DSIBackend::Cpu;
    o.uCpiSamples      = uCpi;
    o.uClutterTaps     = uTaps;
    o.uEstimateSamples = uWindow;
    o.uUpdateSamples   = uWindow;
    o.uDopplerTaps     = 0;
    o.dDiagonalLoading = 0.0;
    o.bFloat32Math     = true;
    return o;
}

bool run(const DSIParams& oParams, const std::vector<ComplexF>& vcRef,
         const std::vector<ComplexF>& vcSur, std::vector<ComplexF>& vcOut,
         DSIStats& oStats)
{
    std::unique_ptr<DSICanceller> p = makeDSICanceller(oParams.eAlgorithm, oParams.eBackend);
    if (!p || !p->init(oParams))
        return false;
    vcOut.assign(vcRef.size(), ComplexF());
    return p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats);
}

void testRejectsBadParameters()
{
    std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::Cgls, DSIBackend::Cpu);
    assert(p);

    DSIParams o = cglsParams(1024, 8, 256, 30, false);
    o.uEstimateSamples = 512;          // sliding windows are not defined for CGLS
    assert(!p->init(o));

    o = cglsParams(1024, 8, 256, 30, false);
    o.uDopplerTaps = 1;                // delay-only subspace, as ECA
    assert(!p->init(o));

    o = cglsParams(1024, 8, 256, 0, false);
    assert(!p->init(o));               // zero iterations does nothing

    o = cglsParams(1024, 300, 256, 30, false);
    assert(!p->init(o));               // underdetermined

    o = cglsParams(1024, 8, 256, 30, false);
    assert(p->init(o));
    assert(!p->init(o));               // twice

    std::vector<ComplexF> vcRef = makeNoise(1024, 1), vcSur = makeNoise(1024, 2);
    std::vector<ComplexF> vcOut(1024);
    DSIStats oStats;
    assert(!p->cancel(nullptr, vcSur.data(), vcOut.data(), oStats));
    assert(!p->cancel(vcRef.data(), vcSur.data(), vcSur.data(), oStats));
    assert(p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats));
    assert(oStats.uIterations == 30);

    std::printf("  rejects bad parameters and buffers ................ ok\n");
}

// Eight unknowns, thirty iterations: in exact arithmetic CG would be done in
// eight. What is left is float rounding.
void testCancelsKnownClutter()
{
    const uint64_t uCpi = 4096, uTaps = 8, uWindow = 512;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 11);
    const std::vector<ComplexF> vcSur = makeClutter(vcRef, uTaps);

    std::vector<ComplexF> vcOut;
    DSIStats oStats;
    assert(run(cglsParams(uCpi, uTaps, uWindow, 30, false), vcRef, vcSur, vcOut, oStats));

    assert(oStats.dSuppressionDb > 60.0);
    assert(totalPower(vcOut) < 1e-6 * totalPower(vcSur));

    std::printf("  cancels known clutter (%.1f dB) .................... ok\n",
                oStats.dSuppressionDb);
}

// The anchor. Uncorrelated signals, so nothing is deeply cancelled and the
// FP32 rounding is not amplified; the two solvers must then agree closely.
// 64 taps of white noise over 1024 samples is well conditioned, so CGLS
// converges long before 200 iterations.
void testConvergesToEcaBAnswer()
{
    const uint64_t uCpi = 8192, uTaps = 64, uWindow = 1024;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 101);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 202);

    std::vector<ComplexF> vcEca, vcCgls;
    DSIStats oEca, oCgls;
    assert(run(ecaBParams(uCpi, uTaps, uWindow), vcRef, vcSur, vcEca, oEca));
    assert(run(cglsParams(uCpi, uTaps, uWindow, 200, false), vcRef, vcSur, vcCgls, oCgls));

    const double dRel = relativeL2(vcCgls, vcEca);
    std::printf("  CGLS(200) vs ECA-B, FP32: relL2 %.2e, "
                "suppression %.3f / %.3f dB\n",
                dRel, oCgls.dSuppressionDb, oEca.dSuppressionDb);
    assert(dRel < 1e-4);

    // And how far a realistic count gets. Reported, not asserted: this is
    // the semiconvergence curve's first point, and a thesis measurement.
    for (uint64_t uIt = 5; uIt <= 30; uIt += 5) {
        assert(run(cglsParams(uCpi, uTaps, uWindow, uIt, false), vcRef, vcSur, vcCgls, oCgls));
        std::printf("      %2llu iterations: relL2 vs ECA-B %.2e\n",
                    static_cast<unsigned long long>(uIt), relativeL2(vcCgls, vcEca));
    }
}

void testPreservesTargetBeyondTaps()
{
    const uint64_t uCpi = 4096, uTaps = 8, uWindow = 1024;
    const std::vector<ComplexF> vcRef    = makeNoise(uCpi, 23);
    const std::vector<ComplexF> vcTarget = makeTarget(vcRef, 40, 0.01, 0.02);
    const std::vector<ComplexF> vcSur    = add(makeClutter(vcRef, uTaps), vcTarget);

    std::vector<ComplexF> vcOut;
    DSIStats oStats;
    assert(run(cglsParams(uCpi, uTaps, uWindow, 30, false), vcRef, vcSur, vcOut, oStats));

    const double dFraction = targetResidualFraction(vcOut, vcTarget);
    assert(oStats.dSuppressionDb > 20.0);
    assert(dFraction < 0.05);

    std::printf("  preserves a target past the taps (%.1f dB, %.1f%% off)  ok\n",
                oStats.dSuppressionDb, 100.0 * dFraction);
}

// Priming starts from the previous CPI's weights. On a stationary scene the
// second CPI then needs far fewer iterations to get to the same place; and a
// cold canceller, given the same input twice, must give the same answer
// twice.
void testPrimingHelpsAndColdIsStateless()
{
    const uint64_t uCpi = 4096, uTaps = 8, uWindow = 512;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 31);
    const std::vector<ComplexF> vcSur = makeClutter(vcRef, uTaps);

    std::vector<ComplexF> vcFirst(uCpi), vcSecond(uCpi);
    DSIStats oFirst, oSecond;

    // Two iterations is not enough to cancel eight taps from a cold start.
    {
        std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::Cgls, DSIBackend::Cpu);
        assert(p && p->init(cglsParams(uCpi, uTaps, uWindow, 2, true)));
        assert(p->cancel(vcRef.data(), vcSur.data(), vcFirst.data(), oFirst));
        assert(p->cancel(vcRef.data(), vcSur.data(), vcSecond.data(), oSecond));
    }
    assert(oSecond.dSuppressionDb > oFirst.dSuppressionDb + 10.0);
    std::printf("  priming: 2 iterations cold %.1f dB, primed %.1f dB ... ok\n",
                oFirst.dSuppressionDb, oSecond.dSuppressionDb);

    {
        std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::Cgls, DSIBackend::Cpu);
        assert(p && p->init(cglsParams(uCpi, uTaps, uWindow, 2, false)));
        assert(p->cancel(vcRef.data(), vcSur.data(), vcFirst.data(), oFirst));
        assert(p->cancel(vcRef.data(), vcSur.data(), vcSecond.data(), oSecond));
    }
    for (uint64_t n = 0; n < uCpi; ++n) {
        assert(vcFirst[n].real() == vcSecond[n].real());
        assert(vcFirst[n].imag() == vcSecond[n].imag());
    }
    std::printf("  cold start is stateless ........................... ok\n");
}

#if defined(PBR_WITH_CUDA)

void testGpuMatchesCpu()
{
    const uint64_t uCpi = 8192, uTaps = 64, uWindow = 1024;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 101);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 202);

    std::vector<ComplexF> vcCpu, vcGpu;
    DSIStats oCpu, oGpu;
    DSIParams o = cglsParams(uCpi, uTaps, uWindow, 30, false);
    assert(run(o, vcRef, vcSur, vcCpu, oCpu));
    o.eBackend = DSIBackend::Gpu;
    assert(run(o, vcRef, vcSur, vcGpu, oGpu));

    // The per-window norms are reduced in a different order on the GPU, so
    // alpha and beta differ in the last bit each iteration; with nothing
    // deeply cancelled that stays small.
    const double dRel = relativeL2(vcGpu, vcCpu);
    std::printf("  GPU vs CPU-FP32, no amplification: relL2 %.2e\n", dRel);
    assert(dRel < 1e-5);
    assert(oGpu.uIterations == 30);
}

void testGpuCancelsKnownClutter()
{
    const uint64_t uCpi = 8192, uTaps = 8, uWindow = 1024;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 303);
    const std::vector<ComplexF> vcSur = makeClutter(vcRef, uTaps);

    std::vector<ComplexF> vcOut;
    DSIStats oStats;
    DSIParams o = cglsParams(uCpi, uTaps, uWindow, 30, false);
    o.eBackend = DSIBackend::Gpu;
    assert(run(o, vcRef, vcSur, vcOut, oStats));
    std::printf("  GPU cancels known clutter: %.1f dB\n", oStats.dSuppressionDb);
    assert(oStats.dSuppressionDb > 60.0);
}

void testGpuIsDeterministic()
{
    const uint64_t uCpi = 8192, uTaps = 32, uWindow = 2048;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 404);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 505);

    DSIParams o = cglsParams(uCpi, uTaps, uWindow, 30, false);
    o.eBackend = DSIBackend::Gpu;
    std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::Cgls, DSIBackend::Gpu);
    assert(p && p->init(o));

    std::vector<ComplexF> vcFirst(uCpi), vcAgain(uCpi);
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
    std::printf("test_cgls\n");

    testRejectsBadParameters();
    testCancelsKnownClutter();
    testConvergesToEcaBAnswer();
    testPreservesTargetBeyondTaps();
    testPrimingHelpsAndColdIsStateless();

#if defined(PBR_WITH_CUDA)
    testGpuMatchesCpu();
    testGpuCancelsKnownClutter();
    testGpuIsDeterministic();
#endif

    std::printf("test_cgls: all tests passed\n");
    return 0;
}
