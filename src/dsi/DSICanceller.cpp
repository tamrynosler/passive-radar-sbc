// Factory and config plumbing for the DSI cancellers.
//
// This file knows which implementations exist and how a config file maps onto
// DSIParams. The algorithms themselves know neither.

#include "pbr/DSICanceller.hpp"

#include <cstdio>
#include <cstring>

namespace pbr {

// Each backend's factory lives with its implementations. Declared here rather
// than in a header because nothing outside this file calls them.
std::unique_ptr<DSICanceller> makeDSICancellerCpu(DSIAlgorithm eAlgorithm);

#if defined(PBR_WITH_CUDA)
std::unique_ptr<DSICanceller> makeDSICancellerGpu(DSIAlgorithm eAlgorithm);
#endif

namespace {

// One table drives naming in both directions, so a name can never parse to
// one thing and print as another.
const struct { DSIAlgorithm eValue; const char* pName; } kAlgorithms[] = {
    { DSIAlgorithm::EcaM,  "ECA-m"  },
    { DSIAlgorithm::EcaB,  "ECA-B"  },
    { DSIAlgorithm::EcaS,  "ECA-S"  },
    { DSIAlgorithm::EcaCd, "ECA-CD" },
    { DSIAlgorithm::Cgls,  "CGLS"   },
};

const struct { DSIBackend eValue; const char* pName; } kBackends[] = {
    { DSIBackend::Cpu, "CPU" },
    { DSIBackend::Gpu, "GPU" },
};

} // namespace

bool dsiAlgorithmFromString(const char* pName, DSIAlgorithm& eAlgorithm)
{
    if (pName == nullptr) {
        std::fprintf(stderr, "dsi: null algorithm name\n");
        return false;
    }
    for (size_t u = 0; u < sizeof(kAlgorithms) / sizeof(kAlgorithms[0]); ++u) {
        if (std::strcmp(pName, kAlgorithms[u].pName) == 0) {
            eAlgorithm = kAlgorithms[u].eValue;
            return true;
        }
    }
    std::fprintf(stderr, "dsi: unknown algorithm \"%s\"; expected one of "
                         "ECA-m ECA-B ECA-S ECA-CD CGLS\n", pName);
    return false;
}

bool dsiBackendFromString(const char* pName, DSIBackend& eBackend)
{
    if (pName == nullptr) {
        std::fprintf(stderr, "dsi: null backend name\n");
        return false;
    }
    for (size_t u = 0; u < sizeof(kBackends) / sizeof(kBackends[0]); ++u) {
        if (std::strcmp(pName, kBackends[u].pName) == 0) {
            eBackend = kBackends[u].eValue;
            return true;
        }
    }
    std::fprintf(stderr, "dsi: unknown backend \"%s\"; expected CPU or GPU\n", pName);
    return false;
}

const char* dsiAlgorithmName(DSIAlgorithm eAlgorithm)
{
    for (size_t u = 0; u < sizeof(kAlgorithms) / sizeof(kAlgorithms[0]); ++u) {
        if (kAlgorithms[u].eValue == eAlgorithm)
            return kAlgorithms[u].pName;
    }
    return "unknown";
}

const char* dsiBackendName(DSIBackend eBackend)
{
    for (size_t u = 0; u < sizeof(kBackends) / sizeof(kBackends[0]); ++u) {
        if (kBackends[u].eValue == eBackend)
            return kBackends[u].pName;
    }
    return "unknown";
}

std::unique_ptr<DSICanceller> makeDSICanceller(DSIAlgorithm eAlgorithm,
                                               DSIBackend   eBackend)
{
    if (eBackend == DSIBackend::Cpu)
        return makeDSICancellerCpu(eAlgorithm);

#if defined(PBR_WITH_CUDA)
    return makeDSICancellerGpu(eAlgorithm);
#else
    // Never silently fall back to the CPU. A benchmark row labelled GPU that
    // was actually measured on the CPU is worse than no row at all, and
    // nothing downstream could tell the difference.
    std::fprintf(stderr,
                 "dsi: the GPU backend is not available -- this build has no "
                 "CUDA (PBR_WITH_CUDA is not defined). Build on the Jetson via "
                 "CMake, or select DSI_BACKEND = CPU.\n");
    return std::unique_ptr<DSICanceller>();
#endif
}

bool dsiParamsFromConfig(const PbrConfig& oConfig, DSIParams& oParams)
{
    oParams = DSIParams();

    if (!dsiAlgorithmFromString(oConfig.sDsiAlgorithm.c_str(), oParams.eAlgorithm))
        return false;
    if (!dsiBackendFromString(oConfig.sDsiBackend.c_str(), oParams.eBackend))
        return false;

    oParams.uCpiSamples      = oConfig.uCpiSamples;
    oParams.uClutterTaps     = oConfig.uClutterTaps;
    oParams.uDopplerTaps     = oConfig.uEcaDopplerTaps;
    oParams.dDiagonalLoading = oConfig.dEcaDiagLoading;
    oParams.uMaxIterations   = oConfig.uCglsIterations;
    oParams.dTolerance       = oConfig.dCglsTolerance;

    if (oParams.eAlgorithm == DSIAlgorithm::EcaM) {
        // ECA-m is defined as "what the MATLAB reference chain does", so it
        // ignores the configured windows and inherits the reference's quirks:
        // one solve over the whole CPI, and an ABSOLUTE 1e-3 on the diagonal
        // rather than a relative fraction. Reproducing it exactly is the whole
        // point of having it -- every other algorithm gets scale-invariant
        // loading instead.
        oParams.uEstimateSamples = oConfig.uCpiSamples;
        oParams.uUpdateSamples   = oConfig.uCpiSamples;
        oParams.dDiagonalLoading = 1e-3;
        oParams.bAbsoluteLoading = true;
    } else {
        oParams.uEstimateSamples = oConfig.uEcaEstimateSamples;
        oParams.uUpdateSamples   = oConfig.uEcaUpdateSamples;
        oParams.bAbsoluteLoading = false;
    }

    return true;
}

} // namespace pbr
