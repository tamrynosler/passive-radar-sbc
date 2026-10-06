// Factory, axis arithmetic and config plumbing for the ARD stage.
//
// This file knows which backends exist and how a config file maps onto
// ArdParams. The implementations know neither.

#include "pbr/ArdProcessor.hpp"
#include "pbr/check.hpp"
#include "ArdInternal.hpp"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace pbr {

namespace {

const struct { ArdBackend eValue; const char* pName; } kBackends[] = {
    { ArdBackend::Cpu, "CPU" },
    { ArdBackend::Gpu, "GPU" },
};

const struct { ArdAlgorithm eValue; const char* pName; } kAlgorithms[] = {
    { ArdAlgorithm::Direct,  "Direct"  },
    { ArdAlgorithm::Batches, "Batches" },
};

} // namespace

bool ardBackendFromString(const char* pName, ArdBackend& eBackend)
{
    if (pName == nullptr) {
        std::fprintf(stderr, "ard: null backend name\n");
        return false;
    }
    for (size_t u = 0; u < sizeof(kBackends) / sizeof(kBackends[0]); ++u) {
        if (std::strcmp(pName, kBackends[u].pName) == 0) {
            eBackend = kBackends[u].eValue;
            return true;
        }
    }
    std::fprintf(stderr, "ard: unknown backend \"%s\"; expected CPU or GPU\n", pName);
    return false;
}

const char* ardBackendName(ArdBackend eBackend)
{
    for (size_t u = 0; u < sizeof(kBackends) / sizeof(kBackends[0]); ++u) {
        if (kBackends[u].eValue == eBackend)
            return kBackends[u].pName;
    }
    return "unknown";
}

bool ardAlgorithmFromString(const char* pName, ArdAlgorithm& eAlgorithm)
{
    if (pName == nullptr) {
        std::fprintf(stderr, "ard: null algorithm name\n");
        return false;
    }
    for (size_t u = 0; u < sizeof(kAlgorithms) / sizeof(kAlgorithms[0]); ++u) {
        if (std::strcmp(pName, kAlgorithms[u].pName) == 0) {
            eAlgorithm = kAlgorithms[u].eValue;
            return true;
        }
    }
    std::fprintf(stderr,
                 "ard: unknown algorithm \"%s\"; expected Direct or Batches\n",
                 pName);
    return false;
}

const char* ardAlgorithmName(ArdAlgorithm eAlgorithm)
{
    for (size_t u = 0; u < sizeof(kAlgorithms) / sizeof(kAlgorithms[0]); ++u) {
        if (kAlgorithms[u].eValue == eAlgorithm)
            return kAlgorithms[u].pName;
    }
    return "unknown";
}

uint64_t ardBatchTransformLength(uint64_t uStride, uint64_t uRangeBins)
{
    if (uStride == 0 || uRangeBins == 0)
        return 0;

    // The wrapped tail of the reference block must not run into the head, so
    // the transform has to hold one block plus the K-1 samples of history the
    // deepest delay reaches back for. See ArdAlgorithm::Batches.
    const uint64_t uNeeded = uStride + uRangeBins - 1;

    uint64_t uL = 1;
    while (uL < uNeeded) {
        // Refuse to wrap rather than return a nonsense length.
        if (uL > (~uint64_t(0)) / 2)
            return 0;
        uL <<= 1;
    }
    return uL;
}

std::unique_ptr<ArdProcessor> makeArdProcessor(ArdBackend eBackend)
{
    if (eBackend == ArdBackend::Cpu)
        return makeArdProcessorCpu();

#if defined(PBR_WITH_CUDA)
    return makeArdProcessorGpu();
#else
    // Never silently substitute the CPU. A benchmark row labelled GPU that was
    // measured on the CPU is worse than no row at all.
    std::fprintf(stderr,
                 "ard: the GPU backend is not available -- this build has no "
                 "CUDA (PBR_WITH_CUDA is not defined). Build on the Jetson via "
                 "CMake, or select the CPU backend.\n");
    return std::unique_ptr<ArdProcessor>();
#endif
}

bool ardParamsFromConfig(const PbrConfig& oConfig, ArdParams& oParams)
{
    oParams = ArdParams();

    // The map former runs on the same backend as the canceller: a GPU
    // canceller hands its output to a GPU map former on the device, with no
    // copy (DSICanceller::deviceOutput()), and a CPU chain stays on the host.
    if (!ardBackendFromString(oConfig.sDsiBackend.c_str(), oParams.eBackend))
        return false;
    if (!ardAlgorithmFromString(oConfig.sArdAlgorithm.c_str(), oParams.eAlgorithm))
        return false;

    oParams.uCpiSamples        = oConfig.uCpiSamples;
    oParams.uSampleRateHz      = oConfig.uSampleRateHz;
    oParams.uRangeBins         = oConfig.uRangeBins;
    oParams.uDopplerDecimation = oConfig.uDopplerDecimation;
    oParams.uDopplerBins       = oConfig.uDopplerBins;
    oParams.dDopplerHalfSpanHz = oConfig.dDopplerHalfSpanHz;

    oParams.dCfarPfa          = oConfig.dCfarPfa;
    oParams.uCfarGuardRange   = oConfig.uCfarGuardRange;
    oParams.uCfarGuardDoppler = oConfig.uCfarGuardDoppler;
    oParams.uCfarTrainRange   = oConfig.uCfarTrainRange;
    oParams.uCfarTrainDoppler = oConfig.uCfarTrainDoppler;

    return true;
}

bool checkArdParams(const ArdParams& oParams, float& fAlphaOverTrainOut)
{
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uSampleRateHz > 0, "sample rate must be non-zero");
    CHECK(oParams.uRangeBins > 0, "range bin count must be non-zero");
    CHECK(oParams.uDopplerDecimation > 0, "Doppler decimation must be non-zero");
    CHECK(oParams.uCpiSamples % oParams.uDopplerDecimation == 0,
          "the decimation factor must divide the CPI exactly, or the last "
          "block is short and the Doppler axis ends up subtly wrong");
    CHECK(oParams.uDopplerBins == oParams.uCpiSamples / oParams.uDopplerDecimation,
          "uDopplerBins must equal uCpiSamples / uDopplerDecimation");
    CHECK(oParams.uDopplerBins % 2 == 0,
          "an odd Doppler bin count has no exact centre to put zero Doppler at");
    CHECK(oParams.uRangeBins <= oParams.uCpiSamples,
          "more range bins than samples in a CPI");
    // cuFFT and cuBLAS take int, and the GPU kernels index every sample and
    // every cell with uint32_t. Both factors are at most uCpiSamples, so the
    // product cannot overflow uint64_t.
    CHECK(oParams.uCpiSamples <= INT_MAX &&
          oParams.uRangeBins * oParams.uDopplerBins <= INT_MAX,
          "the CPI or the map is larger than cuFFT, cuBLAS and the kernels can index");
    CHECK(oParams.dCfarPfa > 0.0 && oParams.dCfarPfa < 1.0,
          "CFAR Pfa must be strictly between 0 and 1");

    // The CFAR window must fit inside the map, or every cell is a border cell
    // and the background estimate means nothing.
    const uint64_t uHalfRange   = oParams.uCfarGuardRange + oParams.uCfarTrainRange;
    const uint64_t uHalfDoppler = oParams.uCfarGuardDoppler + oParams.uCfarTrainDoppler;
    CHECK(2 * uHalfRange + 1 <= oParams.uRangeBins,
          "the CFAR window is wider than the map in range");
    CHECK(2 * uHalfDoppler + 1 <= oParams.uDopplerBins,
          "the CFAR window is taller than the map in Doppler");

    // Training cells: the whole window minus the guard block (which includes
    // the cell under test).
    const uint64_t uWindow = (2 * uHalfRange + 1) * (2 * uHalfDoppler + 1);
    const uint64_t uGuard  = (2 * oParams.uCfarGuardRange + 1) *
                             (2 * oParams.uCfarGuardDoppler + 1);
    const uint64_t uTrainCount = uWindow - uGuard;
    CHECK(uTrainCount > 0, "the CFAR window has no training cells");

    // The scale factor that turns "average of N training cells" into a
    // threshold achieving the requested false alarm rate, for exponentially
    // distributed noise power.
    const double dN     = static_cast<double>(uTrainCount);
    const double dAlpha = dN * (std::pow(oParams.dCfarPfa, -1.0 / dN) - 1.0);
    fAlphaOverTrainOut  = static_cast<float>(dAlpha / dN);
    return true;
}

double ardRangeKm(uint64_t uRangeBin, uint64_t uSampleRateHz)
{
    if (uSampleRateHz == 0)
        return 0.0;
    // One delay bin is one sample period of extra path: c / fs metres.
    return static_cast<double>(uRangeBin) * kSpeedOfLight /
           static_cast<double>(uSampleRateHz) / 1000.0;
}

double ardDopplerHz(uint64_t uDopplerBin, const ArdParams& oParams)
{
    if (oParams.uDopplerBins == 0 || oParams.uDopplerDecimation == 0)
        return 0.0;

    // After decimation the effective sample rate is fs / dec, spread over
    // uDopplerBins bins. Bin uDopplerBins/2 is zero Doppler, because the map
    // is stored fftshifted.
    const double dBinHz = static_cast<double>(oParams.uSampleRateHz) /
                          (static_cast<double>(oParams.uDopplerDecimation) *
                           static_cast<double>(oParams.uDopplerBins));
    const double dCentre = static_cast<double>(oParams.uDopplerBins) / 2.0;
    return (static_cast<double>(uDopplerBin) - dCentre) * dBinHz;
}

} // namespace pbr
