// Factory, axis arithmetic and config plumbing for the ARD stage.
//
// This file knows which backends exist and how a config file maps onto
// ArdParams. The implementations know neither.

#include "pbr/ArdProcessor.hpp"

#include <cstdio>
#include <cstring>

namespace pbr {

std::unique_ptr<ArdProcessor> makeArdProcessorCpu();

#if defined(PBR_WITH_CUDA)
std::unique_ptr<ArdProcessor> makeArdProcessorGpu();
#endif

namespace {

const double kSpeedOfLight = 299792458.0;

const struct { ArdBackend eValue; const char* pName; } kBackends[] = {
    { ArdBackend::Cpu, "CPU" },
    { ArdBackend::Gpu, "GPU" },
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

    if (!ardBackendFromString(oConfig.sDsiBackend.c_str(), oParams.eBackend))
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
