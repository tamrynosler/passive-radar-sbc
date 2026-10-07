// Unit tests for the FBLMS canceller.
//
// The MATLAB golden check is dsi_verify against matlab/fblms_golden.m. These
// are the synthetic checks:
//
//   * clutter built from known taps is cancelled once the weights have
//     adapted (the second CPI, weights carried over)
//   * a target past the last tap survives
//   * the state carries across calls, and a fresh canceller is deterministic
//   * FP32 and FP64 agree on uncorrelated noise

// NDEBUG is undefined deliberately and before <cassert>: the project builds
// Release, which defines NDEBUG and would compile every assert below out.
#undef NDEBUG

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

DSIParams fblmsParams(uint64_t uCpi, uint64_t uTaps, double dStep, double dForget)
{
    DSIParams o;
    o.eAlgorithm   = DSIAlgorithm::Fblms;
    o.eBackend     = DSIBackend::Cpu;
    o.uCpiSamples  = uCpi;
    o.uFblmsTaps   = uTaps;
    o.dFblmsStep   = dStep;
    o.dFblmsForget = dForget;
    o.bFloat32Math = true;
    return o;
}

// Two cancel() calls on the same input with one canceller.
bool runTwice(const DSIParams& oParams, const std::vector<ComplexF>& vcRef,
              const std::vector<ComplexF>& vcSur, std::vector<ComplexF>& vcFirst,
              std::vector<ComplexF>& vcSecond, DSIStats& oFirst, DSIStats& oSecond)
{
    std::unique_ptr<DSICanceller> p = makeDSICanceller(oParams.eAlgorithm, oParams.eBackend);
    if (!p || !p->init(oParams))
        return false;
    vcFirst.assign(vcRef.size(), ComplexF());
    vcSecond.assign(vcRef.size(), ComplexF());
    return p->cancel(vcRef.data(), vcSur.data(), vcFirst.data(), oFirst) &&
           p->cancel(vcRef.data(), vcSur.data(), vcSecond.data(), oSecond);
}

bool runOnce(const DSIParams& oParams, const std::vector<ComplexF>& vcRef,
             const std::vector<ComplexF>& vcSur, std::vector<ComplexF>& vcOut,
             DSIStats& oStats)
{
    std::unique_ptr<DSICanceller> p = makeDSICanceller(oParams.eAlgorithm, oParams.eBackend);
    if (!p || !p->init(oParams))
        return false;
    vcOut.assign(vcRef.size(), ComplexF());
    return p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats);
}

bool identical(const std::vector<ComplexF>& vcA, const std::vector<ComplexF>& vcB)
{
    for (size_t u = 0; u < vcA.size(); ++u)
        if (vcA[u].real() != vcB[u].real() || vcA[u].imag() != vcB[u].imag())
            return false;
    return true;
}

void testRejectsBadParameters()
{
    std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::Fblms, DSIBackend::Cpu);
    assert(p);

    assert(!p->init(fblmsParams(1024, 100, 0.05, 0.9)));    // not a power of two
    assert(!p->init(fblmsParams(1000, 256, 0.05, 0.9)));    // does not divide the CPI
    assert(!p->init(fblmsParams(1024, 16, 0.0, 0.9)));      // step 0
    assert(!p->init(fblmsParams(1024, 16, 2.0, 0.9)));      // step above 1
    assert(!p->init(fblmsParams(1024, 16, 0.05, 1.0)));     // forget 1

    const DSIParams o = fblmsParams(1024, 16, 0.05, 0.9);
    assert(p->init(o));
    assert(!p->init(o));                                    // twice

    std::vector<ComplexF> vcRef = makeNoise(1024, 1), vcSur = makeNoise(1024, 2);
    std::vector<ComplexF> vcOut(1024);
    DSIStats oStats;
    assert(!p->cancel(nullptr, vcSur.data(), vcOut.data(), oStats));
    assert(!p->cancel(vcRef.data(), vcSur.data(), vcSur.data(), oStats));
    assert(p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats));

    std::printf("  rejects bad parameters and buffers ................ ok\n");
}

void testCancelsKnownClutter()
{
    const uint64_t uCpi = 8192, uTaps = 16;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 11);
    const std::vector<ComplexF> vcSur = makeClutter(vcRef, 8);

    std::vector<ComplexF> vcFirst, vcSecond;
    DSIStats oFirst, oSecond;
    assert(runTwice(fblmsParams(uCpi, uTaps, 0.05, 0.9), vcRef, vcSur, vcFirst, vcSecond,
                    oFirst, oSecond));

    std::printf("  cancels known clutter: first CPI %.1f dB, second %.1f dB\n",
                oFirst.dSuppressionDb, oSecond.dSuppressionDb);
    assert(oFirst.dSuppressionDb > 8.0);
    assert(oSecond.dSuppressionDb > oFirst.dSuppressionDb + 10.0);
    assert(oSecond.dSuppressionDb > 37.0);
}

void testPreservesTargetBeyondTaps()
{
    const uint64_t uCpi = 8192, uTaps = 16;
    const std::vector<ComplexF> vcRef    = makeNoise(uCpi, 23);
    const std::vector<ComplexF> vcTarget = makeTarget(vcRef, 40, 0.01, 0.02);
    const std::vector<ComplexF> vcSur    = add(makeClutter(vcRef, 8), vcTarget);

    std::vector<ComplexF> vcFirst, vcSecond;
    DSIStats oFirst, oSecond;
    assert(runTwice(fblmsParams(uCpi, uTaps, 0.05, 0.9), vcRef, vcSur, vcFirst, vcSecond,
                    oFirst, oSecond));

    const double dFraction = targetResidualFraction(vcSecond, vcTarget);
    std::printf("  preserves a target past the taps: second CPI %.1f dB, "
                "%.2f%% off\n", oSecond.dSuppressionDb, 100.0 * dFraction);
    assert(oSecond.dSuppressionDb > 25.0);
    assert(dFraction < 0.2);
}

void testStatefulAndDeterministic()
{
    const uint64_t uCpi = 4096, uTaps = 16;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 31);
    const std::vector<ComplexF> vcSur = makeClutter(vcRef, 8);
    const DSIParams o = fblmsParams(uCpi, uTaps, 0.05, 0.9);

    std::vector<ComplexF> vcFirstA, vcSecondA, vcFirstB, vcSecondB;
    DSIStats oStats[4];
    assert(runTwice(o, vcRef, vcSur, vcFirstA, vcSecondA, oStats[0], oStats[1]));
    assert(runTwice(o, vcRef, vcSur, vcFirstB, vcSecondB, oStats[2], oStats[3]));

    assert(identical(vcFirstA, vcFirstB));
    assert(identical(vcSecondA, vcSecondB));
    assert(!identical(vcFirstA, vcSecondA));

    // resetState() is a cold start again: the next call matches a fresh
    // canceller's first call exactly.
    {
        std::unique_ptr<DSICanceller> p = makeDSICanceller(o.eAlgorithm, o.eBackend);
        std::vector<ComplexF> vcOut(uCpi), vcAfterReset(uCpi);
        DSIStats oStats;
        assert(p && p->init(o));
        assert(p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats));
        assert(p->resetState());
        assert(p->cancel(vcRef.data(), vcSur.data(), vcAfterReset.data(), oStats));
        assert(identical(vcAfterReset, vcFirstA));
    }

    std::printf("  stateful, deterministic, resettable ............... ok\n");
}

void testFp32MatchesFp64()
{
    const uint64_t uCpi = 8192, uTaps = 64;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 101);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 202);

    std::vector<ComplexF> vc32, vc64;
    DSIStats o32, o64;
    DSIParams o = fblmsParams(uCpi, uTaps, 0.05, 0.9);
    assert(runOnce(o, vcRef, vcSur, vc32, o32));
    o.bFloat32Math = false;
    assert(runOnce(o, vcRef, vcSur, vc64, o64));

    const double dRel = relativeL2(vc32, vc64);
    std::printf("  FP32 vs FP64 on noise: relL2 %.2e, suppression %.3f / %.3f dB\n",
                dRel, o32.dSuppressionDb, o64.dSuppressionDb);
    assert(dRel < 1e-2);
}

#if defined(PBR_WITH_CUDA)

void testGpuMatchesCpu()
{
    const uint64_t uCpi = 8192, uTaps = 64;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 101);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 202);

    std::vector<ComplexF> vcCpu, vcGpu;
    DSIStats oCpu, oGpu;
    DSIParams o = fblmsParams(uCpi, uTaps, 0.05, 0.9);
    assert(runOnce(o, vcRef, vcSur, vcCpu, oCpu));
    o.eBackend = DSIBackend::Gpu;
    assert(runOnce(o, vcRef, vcSur, vcGpu, oGpu));

    const double dRel = relativeL2(vcGpu, vcCpu);
    std::printf("  GPU vs CPU-FP32, no amplification: relL2 %.2e\n", dRel);
    assert(dRel < 1e-3);
}

void testGpuCancelsKnownClutter()
{
    const uint64_t uCpi = 8192, uTaps = 16;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 11);
    const std::vector<ComplexF> vcSur = makeClutter(vcRef, 8);

    DSIParams o = fblmsParams(uCpi, uTaps, 0.05, 0.9);
    o.eBackend = DSIBackend::Gpu;
    std::vector<ComplexF> vcFirst, vcSecond;
    DSIStats oFirst, oSecond;
    assert(runTwice(o, vcRef, vcSur, vcFirst, vcSecond, oFirst, oSecond));
    std::printf("  GPU cancels known clutter: first %.1f dB, second %.1f dB\n",
                oFirst.dSuppressionDb, oSecond.dSuppressionDb);
    assert(oSecond.dSuppressionDb > 37.0);
}

void testGpuIsDeterministic()
{
    const uint64_t uCpi = 8192, uTaps = 32;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 404);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 505);

    DSIParams o = fblmsParams(uCpi, uTaps, 0.05, 0.9);
    o.eBackend = DSIBackend::Gpu;
    std::vector<ComplexF> vcA, vcB;
    DSIStats oA, oB;
    assert(runOnce(o, vcRef, vcSur, vcA, oA));
    assert(runOnce(o, vcRef, vcSur, vcB, oB));
    assert(identical(vcA, vcB));
    std::printf("  GPU is bit-identical across fresh cancellers ...... ok\n");
}

#endif // PBR_WITH_CUDA

} // namespace

int main()
{
    std::printf("test_fblms\n");

    testRejectsBadParameters();
    testCancelsKnownClutter();
    testPreservesTargetBeyondTaps();
    testStatefulAndDeterministic();
    testFp32MatchesFp64();

#if defined(PBR_WITH_CUDA)
    testGpuMatchesCpu();
    testGpuCancelsKnownClutter();
    testGpuIsDeterministic();
#endif

    std::printf("test_fblms: all tests passed\n");
    return 0;
}
