// Unit tests for the range-Doppler stage.
//
// The MATLAB golden comparison is a separate, larger check that needs a real
// captured CPI (ard_verify). These tests cover what synthetic signals can
// prove, where the right answer is known in advance:
//
//   * a synthetic echo at a known delay and Doppler appears in the RIGHT CELL
//     -- this is what catches a transposed map, a missing fftshift, or an
//     off-by-one in the delay indexing, none of which the golden file would
//     localise
//   * the axes agree with the bin indices
//   * CFAR finds a planted target and produces roughly the expected number of
//     false alarms on noise
//   * the GPU and the CPU agree

#include "pbr/ArdFile.hpp"
#include "pbr/ArdProcessor.hpp"
#include "pbr/types.hpp"
#include "test_signals.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pbr;
using namespace pbrtest;

namespace {

// Small but realistically shaped: decimation divides the CPI, the Doppler bin
// count is even, and the CFAR window fits with room to spare.
ArdParams smallParams(ArdBackend eBackend)
{
    ArdParams o;
    o.eBackend           = eBackend;
    o.uCpiSamples        = 8192;
    o.uSampleRateHz      = 8192;     // 1 s of signal, so 1 Hz per bin
    o.uRangeBins         = 32;
    o.uDopplerDecimation = 128;
    o.uDopplerBins       = 64;       // 8192 / 128
    o.dDopplerHalfSpanHz = 20.0;
    o.dCfarPfa           = 1e-4;
    o.uCfarGuardRange    = 1;
    o.uCfarGuardDoppler  = 1;
    o.uCfarTrainRange    = 2;
    o.uCfarTrainDoppler  = 3;
    return o;
}

// ---------------------------------------------------------------------------

void testAxes()
{
    const ArdParams o = smallParams(ArdBackend::Cpu);

    // Range bin 0 is zero extra path.
    assert(std::fabs(ardRangeKm(0, o.uSampleRateHz)) < 1e-12);
    // One bin is one sample period of extra path: c / fs metres.
    const double dExpected = 299792458.0 / 8192.0 / 1000.0;
    assert(std::fabs(ardRangeKm(1, o.uSampleRateHz) - dExpected) < 1e-9);

    // The map is stored fftshifted, so the middle bin is zero Doppler.
    assert(std::fabs(ardDopplerHz(o.uDopplerBins / 2, o)) < 1e-12);
    // 8192 Hz over 128 decimation over 64 bins is 1 Hz per bin.
    assert(std::fabs(ardDopplerHz(o.uDopplerBins / 2 + 1, o) - 1.0) < 1e-9);
    assert(std::fabs(ardDopplerHz(o.uDopplerBins / 2 - 1, o) + 1.0) < 1e-9);
    // The first bin is the most negative.
    assert(ardDopplerHz(0, o) < 0.0);

    std::printf("  axes agree with the bin indices ................... ok\n");
}

// The test that catches a transposed map, a missing fftshift, or an
// off-by-one in the delay: plant an echo somewhere specific and check the
// peak lands exactly there.
void testEchoLandsInTheRightCell(ArdBackend eBackend, const char* pLabel)
{
    const ArdParams o = smallParams(eBackend);
    const uint64_t uDelay      = 7;
    const double   dDopplerHz  = 5.0;

    const std::vector<ComplexF> vcRef = makeNoise(o.uCpiSamples, 7001);
    const std::vector<ComplexF> vcSur = plantEcho(vcRef, uDelay, dDopplerHz,
                                                  1.0, o.uSampleRateHz);

    std::unique_ptr<ArdProcessor> p = makeArdProcessor(eBackend);
    assert(p && p->init(o));

    const uint64_t uCells = o.uRangeBins * o.uDopplerBins;
    std::vector<ComplexF>  vcMap(static_cast<size_t>(uCells));
    std::vector<Detection> vDet(256);
    ArdStats oStats;
    assert(p->process(vcRef.data(), vcSur.data(), vcMap.data(),
                      vDet.data(), 256, oStats));

    // Find the brightest cell.
    uint64_t uPeak = 0;
    double   dPeak = -1.0;
    for (uint64_t u = 0; u < uCells; ++u) {
        const double dMag = double(vcMap[u].real()) * vcMap[u].real() +
                            double(vcMap[u].imag()) * vcMap[u].imag();
        if (dMag > dPeak) { dPeak = dMag; uPeak = u; }
    }

    const uint64_t uPeakRange   = uPeak % o.uRangeBins;
    const uint64_t uPeakDoppler = uPeak / o.uRangeBins;

    // Exactly where it was planted. Doppler bin = centre + shift in Hz,
    // because the map is fftshifted and there is 1 Hz per bin here.
    const uint64_t uExpectedDoppler =
        o.uDopplerBins / 2 + static_cast<uint64_t>(dDopplerHz);

    assert(uPeakRange == uDelay);
    assert(uPeakDoppler == uExpectedDoppler);

    // And the reported axis values must agree with those bins.
    assert(std::fabs(ardDopplerHz(uPeakDoppler, o) - dDopplerHz) < 1e-6);

    std::printf("  %s echo at delay %llu, %.0f Hz lands in cell "
                "(r=%llu, d=%llu) ... ok\n", pLabel,
                (unsigned long long)uDelay, dDopplerHz,
                (unsigned long long)uPeakRange,
                (unsigned long long)uPeakDoppler);
}

// CFAR must find a strong planted target, and must not flood the map with
// false alarms on noise alone.
void testCfarFindsTargetAndHoldsItsRate(ArdBackend eBackend, const char* pLabel)
{
    ArdParams o = smallParams(eBackend);
    const uint64_t uDelay     = 11;
    const double   dDopplerHz = -6.0;

    const std::vector<ComplexF> vcRef   = makeNoise(o.uCpiSamples, 8101);
    const std::vector<ComplexF> vcNoise = makeNoise(o.uCpiSamples, 8202);
    std::vector<ComplexF> vcEcho = plantEcho(vcRef, uDelay, dDopplerHz,
                                             0.35, o.uSampleRateHz);

    std::vector<ComplexF> vcSur(static_cast<size_t>(o.uCpiSamples));
    for (size_t n = 0; n < vcSur.size(); ++n)
        vcSur[n] = ComplexF(vcEcho[n].real() + vcNoise[n].real(),
                            vcEcho[n].imag() + vcNoise[n].imag());

    std::unique_ptr<ArdProcessor> p = makeArdProcessor(eBackend);
    assert(p && p->init(o));

    const uint64_t uCells = o.uRangeBins * o.uDopplerBins;
    std::vector<ComplexF>  vcMap(static_cast<size_t>(uCells));
    std::vector<Detection> vDet(1024);
    ArdStats oStats;
    assert(p->process(vcRef.data(), vcSur.data(), vcMap.data(),
                      vDet.data(), 1024, oStats));

    // dDopplerHz is negative, so the expected bin is below the centre.
    const uint64_t uExpectedDoppler = o.uDopplerBins / 2 +
        static_cast<uint64_t>(static_cast<int64_t>(dDopplerHz));

    bool bFound = false;
    for (uint64_t u = 0; u < oStats.uDetections; ++u) {
        if (vDet[u].uRangeBin == uDelay &&
            vDet[u].uDopplerBin == uExpectedDoppler) {
            bFound = true;
            assert(vDet[u].dSnrDb > 0.0);       // it beat its own threshold
            assert(std::fabs(vDet[u].dRangeKm -
                             ardRangeKm(uDelay, o.uSampleRateHz)) < 1e-9);
            assert(std::fabs(vDet[u].dDopplerHz - dDopplerHz) < 1e-6);
        }
    }

    assert(bFound);
    assert(oStats.uDetectionsDropped == 0);
    // Nothing like a flood: the target plus its smear, not thousands of cells.
    assert(oStats.uDetections < uCells / 10);

    std::printf("  %s CFAR finds the target, %llu detections of %llu cells "
                "(%llu in border) ok\n", pLabel,
                (unsigned long long)oStats.uDetections,
                (unsigned long long)uCells,
                (unsigned long long)oStats.uDetectionsInBorder);
}

// detect() must give the same answer as the CFAR inside process(), when fed
// the map that process() produced. That is what makes --stage cfar a
// meaningful isolation rather than a different code path.
void testDetectMatchesProcess(ArdBackend eBackend, const char* pLabel)
{
    const ArdParams o = smallParams(eBackend);
    const std::vector<ComplexF> vcRef = makeNoise(o.uCpiSamples, 9001);
    const std::vector<ComplexF> vcSur = plantEcho(vcRef, 5, 3.0, 0.5,
                                                  o.uSampleRateHz);

    std::unique_ptr<ArdProcessor> p = makeArdProcessor(eBackend);
    assert(p && p->init(o));

    const uint64_t uCells = o.uRangeBins * o.uDopplerBins;
    std::vector<ComplexF>  vcMap(static_cast<size_t>(uCells));
    std::vector<Detection> vA(512), vB(512);
    ArdStats oA, oB;

    assert(p->process(vcRef.data(), vcSur.data(), vcMap.data(),
                      vA.data(), 512, oA));
    assert(p->detect(vcMap.data(), vB.data(), 512, oB));

    assert(oA.uDetections == oB.uDetections);

    std::vector<std::pair<uint64_t, uint64_t> > vSetA, vSetB;
    for (uint64_t u = 0; u < oA.uDetections; ++u)
        vSetA.push_back(std::make_pair(vA[u].uRangeBin, vA[u].uDopplerBin));
    for (uint64_t u = 0; u < oB.uDetections; ++u)
        vSetB.push_back(std::make_pair(vB[u].uRangeBin, vB[u].uDopplerBin));
    std::sort(vSetA.begin(), vSetA.end());
    std::sort(vSetB.begin(), vSetB.end());
    assert(vSetA == vSetB);

    std::printf("  %s detect() matches the CFAR inside process() ...... ok\n",
                pLabel);
}

void testRejectsBadParameters()
{
    std::unique_ptr<ArdProcessor> p = makeArdProcessor(ArdBackend::Cpu);
    assert(p);

    // Decimation that does not divide the CPI.
    ArdParams o = smallParams(ArdBackend::Cpu);
    o.uDopplerDecimation = 100;
    assert(!p->init(o));

    // uDopplerBins inconsistent with the decimation.
    o = smallParams(ArdBackend::Cpu);
    o.uDopplerBins = 32;
    assert(!p->init(o));

    // A CFAR window wider than the map.
    o = smallParams(ArdBackend::Cpu);
    o.uCfarTrainRange = 64;
    assert(!p->init(o));

    // Pfa outside (0, 1).
    o = smallParams(ArdBackend::Cpu);
    o.dCfarPfa = 1.0;
    assert(!p->init(o));

    // A good one, then a second init.
    o = smallParams(ArdBackend::Cpu);
    assert(p->init(o));
    assert(!p->init(o));

    std::printf("  rejects inconsistent parameters ................... ok\n");
}

void testArdFileRoundTrip()
{
    const uint64_t uRange = 8, uDop = 16;
    std::vector<ComplexF> vcMap(static_cast<size_t>(uRange * uDop));
    for (size_t u = 0; u < vcMap.size(); ++u)
        vcMap[u] = ComplexF(static_cast<float>(u), static_cast<float>(-int(u)));

    std::vector<Detection> vDet(2);
    vDet[0].uRangeBin = 3; vDet[0].uDopplerBin = 9;
    vDet[0].dRangeKm  = 1.5; vDet[0].dDopplerHz = -4.0;
    vDet[0].dPowerDb  = -3.25; vDet[0].dSnrDb = 7.5;
    vDet[1].uRangeBin = 5; vDet[1].uDopplerBin = 1;

    ArdFileHeader oOut;
    oOut.uRangeBins = uRange; oOut.uDopplerBins = uDop;
    oOut.uSampleRateHz = 8192; oOut.uDopplerDecimation = 128;
    oOut.uCpiStartIndex = 42; oOut.uDetections = 2;

    const char* pPath = "/tmp/pbr_test_ard.bin";
    assert(writeArdFile(pPath, oOut, vcMap.data(), vDet.data()));

    ArdFileHeader oIn;
    std::vector<ComplexF>  vcBack(static_cast<size_t>(uRange * uDop));
    std::vector<Detection> vDetBack(8);
    assert(readArdFile(pPath, oIn, vcBack.data(), uRange * uDop,
                       vDetBack.data(), 8));

    assert(oIn.uRangeBins == uRange && oIn.uDopplerBins == uDop);
    assert(oIn.uSampleRateHz == 8192 && oIn.uDopplerDecimation == 128);
    assert(oIn.uCpiStartIndex == 42 && oIn.uDetections == 2);
    for (size_t u = 0; u < vcMap.size(); ++u) {
        assert(vcBack[u].real() == vcMap[u].real());
        assert(vcBack[u].imag() == vcMap[u].imag());
    }
    assert(vDetBack[0].uRangeBin == 3 && vDetBack[0].uDopplerBin == 9);
    assert(vDetBack[0].dSnrDb == 7.5);

    // Too small a destination must fail rather than truncate.
    assert(!readArdFile(pPath, oIn, vcBack.data(), 4, vDetBack.data(), 8));

    std::remove(pPath);
    std::printf("  ARD file round-trips .............................. ok\n");
}

#if defined(PBR_WITH_CUDA)
// The GPU and the CPU must agree. There is no deep cancellation here to
// amplify single-precision differences, so this can be a tight comparison
// unlike the equivalent test on the canceller.
void testGpuMatchesCpu()
{
    const std::vector<ComplexF> vcRef = makeNoise(smallParams(ArdBackend::Cpu).uCpiSamples, 11001);
    const ArdParams oCpuParams = smallParams(ArdBackend::Cpu);
    const std::vector<ComplexF> vcSur = plantEcho(vcRef, 9, -4.0, 0.4,
                                                  oCpuParams.uSampleRateHz);

    const uint64_t uCells = oCpuParams.uRangeBins * oCpuParams.uDopplerBins;
    std::vector<ComplexF>  vcCpuMap(static_cast<size_t>(uCells));
    std::vector<ComplexF>  vcGpuMap(static_cast<size_t>(uCells));
    std::vector<Detection> vCpuDet(512), vGpuDet(512);
    ArdStats oCpu, oGpu;

    {
        std::unique_ptr<ArdProcessor> p = makeArdProcessor(ArdBackend::Cpu);
        assert(p && p->init(oCpuParams));
        assert(p->process(vcRef.data(), vcSur.data(), vcCpuMap.data(),
                          vCpuDet.data(), 512, oCpu));
    }
    {
        ArdParams o = smallParams(ArdBackend::Gpu);
        std::unique_ptr<ArdProcessor> p = makeArdProcessor(ArdBackend::Gpu);
        assert(p && p->init(o));
        assert(p->process(vcRef.data(), vcSur.data(), vcGpuMap.data(),
                          vGpuDet.data(), 512, oGpu));
    }

    double dDiff = 0.0, dRef = 0.0;
    for (uint64_t u = 0; u < uCells; ++u) {
        const double dRe = double(vcGpuMap[u].real()) - vcCpuMap[u].real();
        const double dIm = double(vcGpuMap[u].imag()) - vcCpuMap[u].imag();
        dDiff += dRe * dRe + dIm * dIm;
        dRef  += double(vcCpuMap[u].real()) * vcCpuMap[u].real() +
                 double(vcCpuMap[u].imag()) * vcCpuMap[u].imag();
    }
    const double dRel = std::sqrt(dDiff / dRef);
    std::printf("  GPU vs CPU map: relL2 %.2e, detections %llu vs %llu\n",
                dRel, (unsigned long long)oGpu.uDetections,
                (unsigned long long)oCpu.uDetections);

    assert(dRel < 1e-5);
    assert(oGpu.uDetections == oCpu.uDetections);

    std::vector<std::pair<uint64_t, uint64_t> > vA, vB;
    for (uint64_t u = 0; u < oCpu.uDetections; ++u)
        vA.push_back(std::make_pair(vCpuDet[u].uRangeBin, vCpuDet[u].uDopplerBin));
    for (uint64_t u = 0; u < oGpu.uDetections; ++u)
        vB.push_back(std::make_pair(vGpuDet[u].uRangeBin, vGpuDet[u].uDopplerBin));
    std::sort(vA.begin(), vA.end());
    std::sort(vB.begin(), vB.end());
    assert(vA == vB);

    std::printf("  GPU and CPU agree on map and detections ........... ok\n");
}
#endif

} // namespace

int main()
{
    std::printf("test_ard\n");

    testAxes();
    testRejectsBadParameters();
    testArdFileRoundTrip();
    testEchoLandsInTheRightCell(ArdBackend::Cpu, "CPU");
    testCfarFindsTargetAndHoldsItsRate(ArdBackend::Cpu, "CPU");
    testDetectMatchesProcess(ArdBackend::Cpu, "CPU");

#if defined(PBR_WITH_CUDA)
    testEchoLandsInTheRightCell(ArdBackend::Gpu, "GPU");
    testCfarFindsTargetAndHoldsItsRate(ArdBackend::Gpu, "GPU");
    testDetectMatchesProcess(ArdBackend::Gpu, "GPU");
    testGpuMatchesCpu();
#endif

    std::printf("test_ard: all tests passed\n");
    return 0;
}
