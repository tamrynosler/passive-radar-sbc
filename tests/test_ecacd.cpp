// Unit tests for the ECA-CD canceller.
//
// ECA-CD is exact for clutter at zero delay -- a scaled copy of the reference
// is a scaled copy in every batch's spectrum too -- and approximate for
// delayed clutter, where the first (delay) samples of every batch belong to
// the previous batch's spectrum. So the checks are:
//
//   * zero-delay clutter is cancelled almost to nothing
//   * delayed clutter is cancelled to a depth that improves with batch length
//   * a Doppler-shifted target survives; slowly-moving clutter within the
//     Doppler columns is cancelled with P = 1 and not with P = 0
//
// All CPU runs are FP32, so every figure is comparable with the GPU.

#include "pbr/DSICanceller.hpp"
#include "pbr/types.hpp"
#include "test_signals.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace pbr;
using namespace pbrtest;

namespace {

DSIParams cdParams(uint64_t uCpi, uint64_t uBatch, uint64_t uDopplerTaps)
{
    DSIParams o;
    o.eAlgorithm      = DSIAlgorithm::EcaCd;
    o.eBackend        = DSIBackend::Cpu;
    o.uCpiSamples     = uCpi;
    o.uCdBatchSamples = uBatch;
    o.uCdDopplerTaps  = uDopplerTaps;
    o.bFloat32Math    = true;
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

// Clutter whose single weight rotates slowly across the CPI: a scatterer
// drifting at dCyclesPerCpi Doppler bins. At zero delay, so exact for the
// batch model; what is being tested is the Doppler columns.
std::vector<ComplexF> makeDriftingClutter(const std::vector<ComplexF>& vcRef,
                                          double dCyclesPerCpi)
{
    std::vector<ComplexF> vc(vcRef.size());
    for (uint64_t n = 0; n < vcRef.size(); ++n) {
        const double dPhase = 2.0 * 3.14159265358979323846 * dCyclesPerCpi *
                              double(n) / double(vcRef.size());
        const std::complex<double> c =
            std::complex<double>(std::cos(dPhase), std::sin(dPhase)) *
            std::complex<double>(vcRef[n].real(), vcRef[n].imag());
        vc[n] = ComplexF(static_cast<float>(c.real()), static_cast<float>(c.imag()));
    }
    return vc;
}

void testRejectsBadParameters()
{
    std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaCd, DSIBackend::Cpu);
    assert(p);

    assert(!p->init(cdParams(32768, 300, 1)));    // not a power of two
    assert(!p->init(cdParams(32768, 0, 1)));
    assert(!p->init(cdParams(30000, 256, 1)));    // does not tile the CPI
    assert(!p->init(cdParams(32768, 256, 4)));    // P capped at 3
    assert(!p->init(cdParams(1024, 512, 1)));     // 2 batches, 3 columns
    assert(p->init(cdParams(32768, 256, 1)));
    assert(!p->init(cdParams(32768, 256, 1)));    // twice

    std::vector<ComplexF> vcRef = makeNoise(32768, 1), vcSur = makeNoise(32768, 2);
    std::vector<ComplexF> vcOut(32768);
    DSIStats oStats;
    assert(!p->cancel(vcRef.data(), nullptr, vcOut.data(), oStats));
    assert(!p->cancel(vcRef.data(), vcSur.data(), vcRef.data(), oStats));
    assert(p->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats));

    std::printf("  rejects bad parameters and buffers ................ ok\n");
}

// A single tap at zero delay is exactly one complex weight per carrier, so
// the projection removes it completely.
void testCancelsZeroDelayClutter()
{
    const uint64_t uCpi = 32768, uBatch = 256;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 11);
    const std::vector<ComplexF> vcSur = makeClutter(vcRef, 1);

    std::vector<ComplexF> vcOut;
    DSIStats oStats;
    assert(run(cdParams(uCpi, uBatch, 1), vcRef, vcSur, vcOut, oStats));
    assert(oStats.dSuppressionDb > 60.0);

    // P = 0 is enough for stationary clutter too.
    DSIStats oStats0;
    assert(run(cdParams(uCpi, uBatch, 0), vcRef, vcSur, vcOut, oStats0));
    assert(oStats0.dSuppressionDb > 60.0);

    std::printf("  cancels zero-delay clutter (P=1 %.1f dB, P=0 %.1f dB)  ok\n",
                oStats.dSuppressionDb, oStats0.dSuppressionDb);
}

// Delayed clutter breaks the circular model in the first few samples of
// each batch. The leak scales with (delay spread / batch), so a longer batch
// must cancel deeper. The numbers are the measurement the file header
// promises; the assertions are only that the trend is right and the depth
// useful.
void testDelayedClutterDepthGrowsWithBatch()
{
    const uint64_t uCpi = 32768, uTaps = 8;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 17);
    const std::vector<ComplexF> vcSur = makeClutter(vcRef, uTaps);

    double adDb[3] = { 0.0, 0.0, 0.0 };
    const uint64_t auBatch[3] = { 256, 1024, 4096 };
    for (int i = 0; i < 3; ++i) {
        std::vector<ComplexF> vcOut;
        DSIStats oStats;
        assert(run(cdParams(uCpi, auBatch[i], 1), vcRef, vcSur, vcOut, oStats));
        adDb[i] = oStats.dSuppressionDb;
    }

    std::printf("  8-tap clutter: batch 256 %.1f dB, 1024 %.1f dB, 4096 %.1f dB\n",
                adDb[0], adDb[1], adDb[2]);
    assert(adDb[0] > 15.0);
    assert(adDb[1] > adDb[0]);
    assert(adDb[2] > adDb[1]);
}

// A target at 0.01 cycles per sample is hundreds of Doppler bins from zero,
// far outside the P = 1 columns, so it must come through. Zero-delay clutter
// so the batch model is exact and only the target is being measured.
void testPreservesMovingTarget()
{
    const uint64_t uCpi = 32768, uBatch = 256;
    const std::vector<ComplexF> vcRef    = makeNoise(uCpi, 23);
    const std::vector<ComplexF> vcTarget = makeTarget(vcRef, 40, 0.01, 0.02);
    const std::vector<ComplexF> vcSur    = add(makeClutter(vcRef, 1), vcTarget);

    std::vector<ComplexF> vcOut;
    DSIStats oStats;
    assert(run(cdParams(uCpi, uBatch, 1), vcRef, vcSur, vcOut, oStats));

    const double dFraction = targetResidualFraction(vcOut, vcTarget);
    assert(oStats.dSuppressionDb > 20.0);
    assert(dFraction < 0.05);

    std::printf("  preserves a moving target (%.1f dB, %.1f%% off) ..... ok\n",
                oStats.dSuppressionDb, 100.0 * dFraction);
}

// Clutter drifting by exactly one Doppler bin over the CPI sits on the p = 1
// column: P = 1 removes it, P = 0 cannot. This is the whole point of the
// "D" in ECA-CD.
//
// Not exact, though. The column treats the drift as one phase per batch, but
// the phase actually turns through 2 pi / M WITHIN each batch too, and that
// part is not in the subspace. Measured 37 dB at M = 128; the batches CAF
// downstream makes the same approximation. P = 0 measures exactly 0 dB
// because one full turn across the batches is orthogonal to the stationary
// column.
void testDopplerColumnsCatchDriftingClutter()
{
    const uint64_t uCpi = 32768, uBatch = 256;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 29);
    const std::vector<ComplexF> vcSur = makeDriftingClutter(vcRef, 1.0);

    std::vector<ComplexF> vcOut;
    DSIStats oP0, oP1;
    assert(run(cdParams(uCpi, uBatch, 0), vcRef, vcSur, vcOut, oP0));
    assert(run(cdParams(uCpi, uBatch, 1), vcRef, vcSur, vcOut, oP1));

    std::printf("  drifting clutter: P=0 %.1f dB, P=1 %.1f dB ......... ok\n",
                oP0.dSuppressionDb, oP1.dSuppressionDb);
    assert(oP1.dSuppressionDb > 30.0);
    assert(oP0.dSuppressionDb < 10.0);
}

#if defined(PBR_WITH_CUDA)

// cuFFT and the CPU's radix-2 order their butterflies differently, so the
// two cannot agree to the last bit; with nothing deeply cancelled the
// difference stays at FFT rounding level.
void testGpuMatchesCpu()
{
    const uint64_t uCpi = 32768, uBatch = 256;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 101);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 202);

    std::vector<ComplexF> vcCpu, vcGpu;
    DSIStats oCpu, oGpu;
    DSIParams o = cdParams(uCpi, uBatch, 1);
    assert(run(o, vcRef, vcSur, vcCpu, oCpu));
    o.eBackend = DSIBackend::Gpu;
    assert(run(o, vcRef, vcSur, vcGpu, oGpu));

    const double dRel = relativeL2(vcGpu, vcCpu);
    std::printf("  GPU vs CPU-FP32, no amplification: relL2 %.2e\n", dRel);
    assert(dRel < 1e-5);
}

void testGpuCancelsZeroDelayClutter()
{
    const uint64_t uCpi = 32768, uBatch = 256;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 303);
    const std::vector<ComplexF> vcSur = makeClutter(vcRef, 1);

    std::vector<ComplexF> vcOut;
    DSIStats oStats;
    DSIParams o = cdParams(uCpi, uBatch, 1);
    o.eBackend = DSIBackend::Gpu;
    assert(run(o, vcRef, vcSur, vcOut, oStats));
    std::printf("  GPU cancels zero-delay clutter: %.1f dB\n", oStats.dSuppressionDb);
    assert(oStats.dSuppressionDb > 60.0);
}

void testGpuIsDeterministic()
{
    const uint64_t uCpi = 32768, uBatch = 512;
    const std::vector<ComplexF> vcRef = makeNoise(uCpi, 404);
    const std::vector<ComplexF> vcSur = makeNoise(uCpi, 505);

    DSIParams o = cdParams(uCpi, uBatch, 1);
    o.eBackend = DSIBackend::Gpu;
    std::unique_ptr<DSICanceller> p = makeDSICanceller(DSIAlgorithm::EcaCd, DSIBackend::Gpu);
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
    std::printf("test_ecacd\n");

    testRejectsBadParameters();
    testCancelsZeroDelayClutter();
    testDelayedClutterDepthGrowsWithBatch();
    testPreservesMovingTarget();
    testDopplerColumnsCatchDriftingClutter();

#if defined(PBR_WITH_CUDA)
    testGpuMatchesCpu();
    testGpuCancelsZeroDelayClutter();
    testGpuIsDeterministic();
#endif

    std::printf("test_ecacd: all tests passed\n");
    return 0;
}
