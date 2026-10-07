// Unit tests for the ECA-LD canceller.
//
// The MATLAB golden check is dsi_verify against matlab/eca_ld_golden.m. These
// are the synthetic checks:
//
//   * ECA-LD solves ECA-B's problem with a Toeplitz Gram matrix, so on white
//     noise it must land close to ECA-B, closer as the batch grows
//   * clutter built from known taps is cancelled (the Toeplitz approximation
//     limits the depth)
//   * a target past the last tap survives
//   * the canceller is stateless and deterministic
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

DSIParams ecaLdParams(uint64_t uCpi, uint64_t uTaps, uint64_t uBatch)
{
    DSIParams o;
    o.eAlgorithm         = DSIAlgorithm::EcaLd;
    o.eBackend           = DSIBackend::Cpu;
    o.uCpiSamples        = uCpi;
    o.uClutterTaps       = uTaps;
    o.uEcaLdBatchSamples = uBatch;
    o.uDopplerTaps       = 0;
    o.dDiagonalLoading   = 0.0;
    o.bFloat32Math       = true;
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
    std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaLd, DSIBackend::Cpu);
    assert(p);

    DSIParams o = ecaLdParams(1024, 8, 300);
    assert(!p->init(o));               // batch does not divide the CPI

    o = ecaLdParams(1024, 256, 256);
    assert(!p->init(o));               // batch not above the taps

    o = ecaLdParams(1024, 8, 256);
    o.uDopplerTaps = 1;                // delay-only subspace, as ECA
    assert(!p->init(o));

    o = ecaLdParams(1024, 8, 256);
    o.dDiagonalLoading = -1e-3;
    assert(!p->init(o));

    o = ecaLdParams(1024, 8, 256);
    assert(p->init(o));
    assert(!p->init(o));               // twice

    std::vector<ComplexF> vcRef = makeNoise(1024, 1), vcSur = makeNoise(1024, 2);
    std::vector<ComplexF> vcOut(1024);
    DSIStats oStats;
    assert(!p->cancel(nullptr, vcSur.data(), vcOut.data(), oStats));
    assert(!p->cancel(vcRef.data(), vcSur.data(), vcSur.data(), oStats));
    assert(p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats));
    assert(oStats.uIterations == 0);

    std::printf("  rejects bad parameters and buffers ................ ok\n");
}

// The only difference from ECA-B is the Toeplitz Gram matrix, whose error
// scales as K / L. Loading is zero on both.
void testMatchesEcaBOnNoise()
{
    const uint64_t uCpi = 8192, uTaps = 64;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 101);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 202);

    double adRel[2];
    const uint64_t auBatch[2] = {1024, 4096};
    for (int i = 0; i < 2; ++i) {
        std::vector<ComplexF> vcEca, vcLd;
        DSIStats oEca, oLd;
        assert(runOnce(ecaBParams(uCpi, uTaps, auBatch[i]), vcRef, vcSur, vcEca, oEca));
        assert(runOnce(ecaLdParams(uCpi, uTaps, auBatch[i]), vcRef, vcSur, vcLd, oLd));
        adRel[i] = relativeL2(vcLd, vcEca);
        std::printf("  ECA-LD vs ECA-B, batch %llu, FP32: relL2 %.2e, "
                    "suppression %.3f / %.3f dB\n",
                    static_cast<unsigned long long>(auBatch[i]),
                    adRel[i], oLd.dSuppressionDb, oEca.dSuppressionDb);
    }
    assert(adRel[0] < 5e-2);
    assert(adRel[1] < adRel[0]);
}

void testCancelsKnownClutter()
{
    const uint64_t uCpi = 8192, uTaps = 16, uBatch = 1024;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 11);
    const std::vector<ComplexF> vcSur = makeClutter(vcRef, 8);

    std::vector<ComplexF> vcOut;
    DSIStats oStats;
    assert(runOnce(ecaLdParams(uCpi, uTaps, uBatch), vcRef, vcSur, vcOut, oStats));

    std::printf("  cancels known clutter: %.1f dB\n", oStats.dSuppressionDb);
    assert(oStats.dSuppressionDb > 39.0);   // measured 49.2 dB; the Toeplitz approximation limits the depth
}

void testPreservesTargetBeyondTaps()
{
    const uint64_t uCpi = 8192, uTaps = 16, uBatch = 1024;
    const std::vector<ComplexF> vcRef    = makeNoise(uCpi, 23);
    const std::vector<ComplexF> vcTarget = makeTarget(vcRef, 40, 0.01, 0.02);
    const std::vector<ComplexF> vcSur    = add(makeClutter(vcRef, 8), vcTarget);

    std::vector<ComplexF> vcOut;
    DSIStats oStats;
    assert(runOnce(ecaLdParams(uCpi, uTaps, uBatch), vcRef, vcSur, vcOut, oStats));

    const double dFraction = targetResidualFraction(vcOut, vcTarget);
    std::printf("  preserves a target past the taps: %.1f dB, %.1f%% off\n",
                oStats.dSuppressionDb, 100.0 * dFraction);
    assert(oStats.dSuppressionDb > 26.0);   // measured 36.1 dB
    assert(dFraction < 0.15);
}

void testStatelessAndDeterministic()
{
    const uint64_t uCpi = 8192, uTaps = 32, uBatch = 1024;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 31);
    const std::vector<ComplexF> vcSur = makeClutter(vcRef, 8);

    std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaLd, DSIBackend::Cpu);
    assert(p && p->init(ecaLdParams(uCpi, uTaps, uBatch)));
    std::vector<ComplexF> vcFirst(uCpi), vcSecond(uCpi);
    DSIStats oStats;
    assert(p->cancel(vcRef.data(), vcSur.data(), vcFirst.data(), oStats));
    assert(p->cancel(vcRef.data(), vcSur.data(), vcSecond.data(), oStats));
    assert(identical(vcFirst, vcSecond));

    std::printf("  stateless and deterministic ....................... ok\n");
}

void testFp32MatchesFp64()
{
    const uint64_t uCpi = 8192, uTaps = 64, uBatch = 1024;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 101);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 202);

    std::vector<ComplexF> vc32, vc64;
    DSIStats o32, o64;
    DSIParams o = ecaLdParams(uCpi, uTaps, uBatch);
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
    const uint64_t uCpi = 8192, uTaps = 64, uBatch = 1024;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 101);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 202);

    std::vector<ComplexF> vcCpu, vcGpu;
    DSIStats oCpu, oGpu;
    DSIParams o = ecaLdParams(uCpi, uTaps, uBatch);
    assert(runOnce(o, vcRef, vcSur, vcCpu, oCpu));
    o.eBackend = DSIBackend::Gpu;
    assert(runOnce(o, vcRef, vcSur, vcGpu, oGpu));

    const double dRel = relativeL2(vcGpu, vcCpu);
    std::printf("  GPU vs CPU-FP32, no amplification: relL2 %.2e\n", dRel);
    assert(dRel < 1e-3);
}

void testGpuCancelsKnownClutter()
{
    const uint64_t uCpi = 8192, uTaps = 16, uBatch = 1024;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 11);
    const std::vector<ComplexF> vcSur = makeClutter(vcRef, 8);

    std::vector<ComplexF> vcOut;
    DSIStats oStats;
    DSIParams o = ecaLdParams(uCpi, uTaps, uBatch);
    o.eBackend = DSIBackend::Gpu;
    assert(runOnce(o, vcRef, vcSur, vcOut, oStats));
    std::printf("  GPU cancels known clutter: %.1f dB\n", oStats.dSuppressionDb);
    assert(oStats.dSuppressionDb > 39.0);
}

void testGpuIsDeterministic()
{
    const uint64_t uCpi = 8192, uTaps = 32, uBatch = 1024;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 404);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 505);

    DSIParams o = ecaLdParams(uCpi, uTaps, uBatch);
    o.eBackend = DSIBackend::Gpu;
    std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaLd, DSIBackend::Gpu);
    assert(p && p->init(o));

    std::vector<ComplexF> vcFirst(uCpi), vcAgain(uCpi);
    DSIStats oStats;
    assert(p->cancel(vcRef.data(), vcSur.data(), vcFirst.data(), oStats));
    for (int i = 0; i < 3; ++i) {
        assert(p->cancel(vcRef.data(), vcSur.data(), vcAgain.data(), oStats));
        assert(identical(vcFirst, vcAgain));
    }
    std::printf("  GPU is bit-identical across runs ................... ok\n");
}

#endif // PBR_WITH_CUDA

} // namespace

int main()
{
    std::printf("test_ecald\n");

    testRejectsBadParameters();
    testMatchesEcaBOnNoise();
    testCancelsKnownClutter();
    testPreservesTargetBeyondTaps();
    testStatelessAndDeterministic();
    testFp32MatchesFp64();

#if defined(PBR_WITH_CUDA)
    testGpuMatchesCpu();
    testGpuCancelsKnownClutter();
    testGpuIsDeterministic();
#endif

    std::printf("test_ecald: all tests passed\n");
    return 0;
}
