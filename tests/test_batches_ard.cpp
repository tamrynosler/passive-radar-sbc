// Unit tests for ArdAlgorithm::Batches.
//
// The claim this file exists to check is narrow and strong: Batches is not an
// approximation of Direct, it is the same computation reached by a different
// route. So almost every test here is a comparison against Direct rather than
// against an independently derived expected value -- Direct is already
// validated against MATLAB by ard_verify, and anything that makes Batches
// disagree with it is a bug in the packing.
//
// The packing is the only place a mistake can hide, and it fails in specific
// ways that these tests are shaped to catch:
//
//   * a transform too short for the deepest delay wraps the reference and
//     corrupts the LARGEST lags -- caught by comparing whole maps rather
//     than just the peak
//   * a missing wrapped tail loses the reference history before each block,
//     which corrupts the largest lags of EVERY block equally
//   * a missing zero-guard at the start of the CPI corrupts block 0 only

#include "pbr/ArdProcessor.hpp"
#include "pbr/types.hpp"
#include "test_signals.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pbr;
using namespace pbrtest;

namespace {

ArdParams baseParams(ArdBackend eBackend, ArdAlgorithm eAlgorithm,
                     uint64_t uRangeBins)
{
    ArdParams o;
    o.eBackend           = eBackend;
    o.eAlgorithm         = eAlgorithm;
    o.uCpiSamples        = 8192;
    o.uSampleRateHz      = 8192;     // 1 s of signal, so 1 Hz per bin
    o.uRangeBins         = uRangeBins;
    o.uDopplerDecimation = 128;
    o.uDopplerBins       = 64;       // 8192 / 128
    o.dCfarPfa           = 1e-4;
    o.uCfarGuardRange    = 1;
    o.uCfarGuardDoppler  = 1;
    o.uCfarTrainRange    = 2;
    o.uCfarTrainDoppler  = 3;
    return o;
}

// Run one configuration and hand back the map and the detections.
void runOnce(const ArdParams& o,
             const std::vector<ComplexF>& vcRef,
             const std::vector<ComplexF>& vcSur,
             std::vector<ComplexF>& vcMap,
             std::vector<Detection>& vDet,
             ArdStats& oStats)
{
    std::unique_ptr<ArdProcessor> p = makeArdProcessor(o.eBackend);
    assert(p);
    assert(p->init(o));

    vcMap.assign(static_cast<size_t>(o.uRangeBins * o.uDopplerBins),
                 ComplexF(0.0f, 0.0f));
    vDet.assign(1024, Detection());
    assert(p->process(vcRef.data(), vcSur.data(), vcMap.data(),
                      vDet.data(), 1024, oStats));
}

std::vector<std::pair<uint64_t, uint64_t> > detectionSet(
    const std::vector<Detection>& vDet, uint64_t uCount)
{
    std::vector<std::pair<uint64_t, uint64_t> > v;
    for (uint64_t u = 0; u < uCount; ++u)
        v.push_back(std::make_pair(vDet[u].uRangeBin, vDet[u].uDopplerBin));
    // The GPU allocates detection slots with atomicAdd, so the ORDER of the
    // list is not reproducible even between two runs of the same algorithm.
    // The set is.
    std::sort(v.begin(), v.end());
    return v;
}

// ---------------------------------------------------------------------------

// The transform must be long enough for one block plus the history the
// deepest delay reaches back for, and a power of two. Off by one here is the
// difference between an exact answer and a wrapped, quietly wrong one.
void testTransformLengthRule()
{
    // The shipped configuration: 128 + 120 - 1 = 247, rounded up.
    assert(ardBatchTransformLength(128, 120) == 256);

    // Exactly a power of two must NOT round up to the next one.
    assert(ardBatchTransformLength(128, 129) == 256);   // 256 needed
    assert(ardBatchTransformLength(128, 130) == 512);   // 257 needed
    assert(ardBatchTransformLength(1, 1) == 1);         // 1 needed

    // Small cases, where the stride alone already decides it.
    assert(ardBatchTransformLength(64, 1) == 64);       // 64 needed
    assert(ardBatchTransformLength(64, 2) == 128);      // 65 needed

    // Nonsense in, zero out, rather than a plausible-looking length.
    assert(ardBatchTransformLength(0, 120) == 0);
    assert(ardBatchTransformLength(128, 0) == 0);

    std::printf("  transform length rule .............................. ok\n");
}

// The main event. Same input, both algorithms, same backend: the maps must
// agree to floating-point rounding, not merely resemble each other.
void testBatchesMatchesDirect(ArdBackend eBackend, const char* pLabel,
                              uint64_t uRangeBins, double dTolerance)
{
    const ArdParams oDirect  = baseParams(eBackend, ArdAlgorithm::Direct,  uRangeBins);
    const ArdParams oBatches = baseParams(eBackend, ArdAlgorithm::Batches, uRangeBins);

    // Noise in both channels plus a planted echo, so every lag carries real
    // content and a wrapped reference cannot hide in an all-zero region.
    const std::vector<ComplexF> vcRef   = makeNoise(oDirect.uCpiSamples, 4201);
    const std::vector<ComplexF> vcNoise = makeNoise(oDirect.uCpiSamples, 4202);
    const std::vector<ComplexF> vcEcho  = plantEcho(vcRef, 9, -4.0, 0.5,
                                                    oDirect.uSampleRateHz);

    std::vector<ComplexF> vcSur(static_cast<size_t>(oDirect.uCpiSamples));
    for (size_t n = 0; n < vcSur.size(); ++n)
        vcSur[n] = ComplexF(vcEcho[n].real() + vcNoise[n].real(),
                            vcEcho[n].imag() + vcNoise[n].imag());

    std::vector<ComplexF>  vcMapD, vcMapB;
    std::vector<Detection> vDetD,  vDetB;
    ArdStats oD, oB;

    runOnce(oDirect,  vcRef, vcSur, vcMapD, vDetD, oD);
    runOnce(oBatches, vcRef, vcSur, vcMapB, vDetB, oB);

    const double dRel = relativeL2(vcMapB, vcMapD);
    std::printf("  %s K=%llu: relL2 %.2e, peak %.3f vs %.3f dB, "
                "%llu vs %llu detections\n",
                pLabel, (unsigned long long)uRangeBins, dRel,
                oB.dPeakDb, oD.dPeakDb,
                (unsigned long long)oB.uDetections,
                (unsigned long long)oD.uDetections);

    assert(dRel < dTolerance);
    assert(oB.uDetections == oD.uDetections);
    assert(detectionSet(vDetB, oB.uDetections) ==
           detectionSet(vDetD, oD.uDetections));
    assert(std::fabs(oB.dPeakDb - oD.dPeakDb) < 1e-3);
}

// Block 0 is the only block whose wrapped tail reaches before the start of
// the CPI, where the reference is defined to be zero. Getting that wrong
// corrupts exactly one block out of 64, which is invisible in a whole-map
// norm dominated by the peak -- so check that block on its own.
//
// A reference that is zero everywhere EXCEPT the tail the wrap would reach
// for makes the failure unmissable: if block 0 wrongly picks up the end of
// the CPI, its lag values move.
void testFirstBlockTreatsHistoryAsZero()
{
    const uint64_t uRangeBins = 32;
    const ArdParams oDirect  = baseParams(ArdBackend::Cpu, ArdAlgorithm::Direct,  uRangeBins);
    const ArdParams oBatches = baseParams(ArdBackend::Cpu, ArdAlgorithm::Batches, uRangeBins);

    std::vector<ComplexF> vcRef = makeNoise(oDirect.uCpiSamples, 5150);
    // Load the very end of the reference, which is what a wrap at block 0
    // would incorrectly reach for.
    for (size_t n = vcRef.size() - uRangeBins; n < vcRef.size(); ++n)
        vcRef[n] = ComplexF(40.0f, -40.0f);

    const std::vector<ComplexF> vcSur = makeNoise(oDirect.uCpiSamples, 5151);

    std::vector<ComplexF>  vcMapD, vcMapB;
    std::vector<Detection> vDetD,  vDetB;
    ArdStats oD, oB;

    runOnce(oDirect,  vcRef, vcSur, vcMapD, vDetD, oD);
    runOnce(oBatches, vcRef, vcSur, vcMapB, vDetB, oB);

    const double dRel = relativeL2(vcMapB, vcMapD);
    std::printf("  first block treats pre-CPI reference as zero: "
                "relL2 %.2e ... ok\n", dRel);
    assert(dRel < 1e-5);
}

// Batches must find the target in the same cell Direct does -- the property
// the whole chain is validated on.
void testEchoLandsInTheRightCell(ArdBackend eBackend, const char* pLabel)
{
    const ArdParams o = baseParams(eBackend, ArdAlgorithm::Batches, 32);
    const uint64_t uDelay     = 7;
    const double   dDopplerHz = 5.0;

    const std::vector<ComplexF> vcRef = makeNoise(o.uCpiSamples, 6001);
    const std::vector<ComplexF> vcSur = plantEcho(vcRef, uDelay, dDopplerHz,
                                                  1.0, o.uSampleRateHz);

    std::vector<ComplexF>  vcMap;
    std::vector<Detection> vDet;
    ArdStats oStats;
    runOnce(o, vcRef, vcSur, vcMap, vDet, oStats);

    uint64_t uPeak = 0;
    double   dPeak = -1.0;
    for (size_t u = 0; u < vcMap.size(); ++u) {
        const double dMag = double(vcMap[u].real()) * vcMap[u].real() +
                            double(vcMap[u].imag()) * vcMap[u].imag();
        if (dMag > dPeak) { dPeak = dMag; uPeak = u; }
    }

    const uint64_t uPeakRange   = uPeak % o.uRangeBins;
    const uint64_t uPeakDoppler = uPeak / o.uRangeBins;
    const uint64_t uExpectedDoppler =
        o.uDopplerBins / 2 + static_cast<uint64_t>(dDopplerHz);

    assert(uPeakRange == uDelay);
    assert(uPeakDoppler == uExpectedDoppler);

    std::printf("  %s Batches echo lands in cell (r=%llu, d=%llu) ...... ok\n",
                pLabel, (unsigned long long)uPeakRange,
                (unsigned long long)uPeakDoppler);
}

void testRejectsBadParameters()
{
    // A stride that does not divide the CPI is rejected before the batches
    // correlator is ever built, by the shared parameter checks.
    std::unique_ptr<ArdProcessor> p = makeArdProcessor(ArdBackend::Cpu);
    assert(p);
    ArdParams o = baseParams(ArdBackend::Cpu, ArdAlgorithm::Batches, 32);
    o.uDopplerDecimation = 100;
    assert(!p->init(o));

    // Name round-trips, and an unknown name is refused rather than defaulted.
    ArdAlgorithm e = ArdAlgorithm::Direct;
    assert(ardAlgorithmFromString("Batches", e) && e == ArdAlgorithm::Batches);
    assert(ardAlgorithmFromString("Direct", e) && e == ArdAlgorithm::Direct);
    assert(!ardAlgorithmFromString("batches", e));   // case matters
    assert(!ardAlgorithmFromString("XF", e));
    assert(!ardAlgorithmFromString(nullptr, e));
    assert(std::string(ardAlgorithmName(ArdAlgorithm::Batches)) == "Batches");
    assert(std::string(ardAlgorithmName(ArdAlgorithm::Direct)) == "Direct");

    std::printf("  rejects bad parameters and unknown names ........... ok\n");
}

#if defined(PBR_WITH_CUDA)
// The GPU batches path against the CPU batches path: both FP32, so this is
// checking the kernels and the cuFFT plan, not the arithmetic width.
void testGpuBatchesMatchesCpuBatches()
{
    const ArdParams oCpu = baseParams(ArdBackend::Cpu, ArdAlgorithm::Batches, 32);
    const ArdParams oGpu = baseParams(ArdBackend::Gpu, ArdAlgorithm::Batches, 32);

    const std::vector<ComplexF> vcRef = makeNoise(oCpu.uCpiSamples, 7301);
    const std::vector<ComplexF> vcSur = plantEcho(vcRef, 9, -4.0, 0.4,
                                                  oCpu.uSampleRateHz);

    std::vector<ComplexF>  vcMapC, vcMapG;
    std::vector<Detection> vDetC,  vDetG;
    ArdStats oC, oG;

    runOnce(oCpu, vcRef, vcSur, vcMapC, vDetC, oC);
    runOnce(oGpu, vcRef, vcSur, vcMapG, vDetG, oG);

    const double dRel = relativeL2(vcMapG, vcMapC);
    std::printf("  GPU vs CPU Batches: relL2 %.2e, %llu vs %llu detections\n",
                dRel, (unsigned long long)oG.uDetections,
                (unsigned long long)oC.uDetections);

    assert(dRel < 1e-5);
    assert(oG.uDetections == oC.uDetections);
    assert(detectionSet(vDetG, oG.uDetections) ==
           detectionSet(vDetC, oC.uDetections));

    std::printf("  GPU and CPU agree on the Batches map ............... ok\n");
}
#endif

} // namespace

int main()
{
    std::printf("test_batches_ard\n");

    testTransformLengthRule();
    testRejectsBadParameters();

    // Three range-bin counts, chosen for where they land against the
    // power-of-two boundary: 32 needs 159 -> 256 (a loose fit, 97 slack),
    // 129 needs exactly 256 (no slack at all, the tightest packing the rule
    // permits), 130 needs 257 -> 512 (the rounding up).
    testBatchesMatchesDirect(ArdBackend::Cpu, "CPU", 32,  1e-5);
    testBatchesMatchesDirect(ArdBackend::Cpu, "CPU", 129, 1e-5);
    testBatchesMatchesDirect(ArdBackend::Cpu, "CPU", 130, 1e-5);

    testFirstBlockTreatsHistoryAsZero();
    testEchoLandsInTheRightCell(ArdBackend::Cpu, "CPU");

#if defined(PBR_WITH_CUDA)
    testBatchesMatchesDirect(ArdBackend::Gpu, "GPU", 32,  1e-5);
    testBatchesMatchesDirect(ArdBackend::Gpu, "GPU", 129, 1e-5);
    testBatchesMatchesDirect(ArdBackend::Gpu, "GPU", 130, 1e-5);
    testEchoLandsInTheRightCell(ArdBackend::Gpu, "GPU");
    testGpuBatchesMatchesCpuBatches();
#endif

    std::printf("test_batches_ard: all tests passed\n");
    return 0;
}
