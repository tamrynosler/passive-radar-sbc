// Factory and config plumbing for the DSI cancellers, and what the
// implementations share.
//
// This file knows which implementations exist and how a config file maps onto
// DSIParams. The algorithms themselves know neither.

#include "pbr/DSICanceller.hpp"
#include "pbr/check.hpp"
#include "DsiInternal.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace pbr {

namespace {

// ECA-m reproduces the MATLAB reference chain, which adds this ABSOLUTE
// value to the Gram diagonal.
const double kMatlabAbsoluteLoading = 1e-3;

// One table drives naming in both directions, so a name can never parse to
// one thing and print as another.
const struct { DSIAlgorithm eValue; const char* pName; } kAlgorithms[] = {
    { DSIAlgorithm::EcaM,  "ECA-m"  },
    { DSIAlgorithm::EcaB,  "ECA-B"  },
    { DSIAlgorithm::EcaS,  "ECA-S"  },
    { DSIAlgorithm::EcaCd, "ECA-CD" },
    { DSIAlgorithm::Cgls,  "CGLS"   },
    { DSIAlgorithm::Fblms, "FBLMS"  },
    { DSIAlgorithm::EcaLd, "ECA-LD" },
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
                         "ECA-m ECA-B ECA-S ECA-CD CGLS FBLMS ECA-LD\n", pName);
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
    const bool bEca = (eAlgorithm == DSIAlgorithm::EcaM ||
                       eAlgorithm == DSIAlgorithm::EcaB ||
                       eAlgorithm == DSIAlgorithm::EcaS);

    if (eBackend == DSIBackend::Cpu) {
        if (bEca)                              return makeEcaCancellerCpu();
        if (eAlgorithm == DSIAlgorithm::EcaCd) return makeEcaCdCancellerCpu();
        if (eAlgorithm == DSIAlgorithm::Cgls)  return makeCglsCancellerCpu();
        if (eAlgorithm == DSIAlgorithm::Fblms) return makeFblmsCancellerCpu();
        if (eAlgorithm == DSIAlgorithm::EcaLd) return makeEcaLdCancellerCpu();
    } else {
#if defined(PBR_WITH_CUDA)
        if (bEca)                              return makeEcaCancellerGpu();
        if (eAlgorithm == DSIAlgorithm::EcaCd) return makeEcaCdCancellerGpu();
        if (eAlgorithm == DSIAlgorithm::Cgls)  return makeCglsCancellerGpu();
        if (eAlgorithm == DSIAlgorithm::Fblms) return makeFblmsCancellerGpu();
        if (eAlgorithm == DSIAlgorithm::EcaLd) return makeEcaLdCancellerGpu();
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

    std::fprintf(stderr, "dsi: unknown algorithm %d\n", static_cast<int>(eAlgorithm));
    return std::unique_ptr<DSICanceller>();
}

bool dsiParamsFromConfig(const PbrConfig& oConfig, DSIParams& oParams)
{
    oParams = DSIParams();

    if (!dsiAlgorithmFromString(oConfig.sDsiAlgorithm.c_str(), oParams.eAlgorithm))
        return false;
    if (!dsiBackendFromString(oConfig.sDsiBackend.c_str(), oParams.eBackend))
        return false;

    // The GPU has no double-precision option worth using, so it ignores
    // this. On the CPU it is a real choice.
    oParams.bFloat32Math     = (oConfig.sDsiMathPrecision == "FP32");

    oParams.uCpiSamples      = oConfig.uCpiSamples;
    oParams.uClutterTaps     = oConfig.uClutterTaps;
    oParams.uDopplerTaps     = oConfig.uEcaDopplerTaps;
    oParams.dDiagonalLoading = oConfig.dEcaDiagLoading;
    oParams.uMaxIterations   = oConfig.uCglsIterations;
    oParams.uCglsBatchSamples= oConfig.uCglsBatchSamples;
    oParams.bCglsPrime       = oConfig.bCglsPrime;
    oParams.uFblmsTaps       = oConfig.uFblmsTaps;
    oParams.dFblmsStep       = oConfig.dFblmsStep;
    oParams.dFblmsForget     = oConfig.dFblmsForget;
    oParams.uEcaLdBatchSamples = oConfig.uEcaLdBatchSamples;
    oParams.uCdBatchSamples  = oConfig.uCdBatchSamples;
    oParams.uCdDopplerTaps   = oConfig.uCdDopplerTaps;

    if (oParams.eAlgorithm == DSIAlgorithm::EcaM) {
        // ECA-m is defined as "what the MATLAB reference chain does", so it
        // ignores the configured windows and inherits the reference's quirks:
        // one solve over the whole CPI, and an ABSOLUTE 1e-3 on the diagonal
        // rather than a relative fraction. Reproducing it exactly is the whole
        // point of having it -- every other algorithm gets scale-invariant
        // loading instead.
        oParams.uEstimateSamples = oConfig.uCpiSamples;
        oParams.uUpdateSamples   = oConfig.uCpiSamples;
        oParams.dDiagonalLoading = kMatlabAbsoluteLoading;
        oParams.bAbsoluteLoading = true;
    } else {
        oParams.uEstimateSamples = oConfig.uEcaEstimateSamples;
        oParams.uUpdateSamples   = oConfig.uEcaUpdateSamples;
    }

    return true;
}

uint64_t dsiEstimationStart(uint64_t uUpdateStart, uint64_t uUpdateSamples,
                            uint64_t uEstimateSamples, uint64_t uCpiSamples)
{
    // Everything in units of the update window, which is also the chunk the
    // GPU shares its partial Grams at. Working in chunks rather than samples
    // is what makes the result land on a chunk boundary by construction.
    const uint64_t uWindow = uUpdateStart / uUpdateSamples;     // this update window
    const uint64_t uSpan   = uEstimateSamples / uUpdateSamples; // M, chunks per estimate
    const uint64_t uChunks = uCpiSamples / uUpdateSamples;      // chunks per CPI

    // Step back half the estimation window, rounding towards the start so an
    // even M leans early rather than late. Unsigned, so the subtraction is
    // guarded rather than allowed to wrap.
    const uint64_t uBack = (uSpan - 1) / 2;
    uint64_t uFirstChunk = (uWindow > uBack) ? (uWindow - uBack) : 0;
    if (uFirstChunk + uSpan > uChunks)
        uFirstChunk = uChunks - uSpan;

    return uFirstChunk * uUpdateSamples;
}

bool ecaParamsOk(const DSIParams& oParams)
{
    CHECK(oParams.eAlgorithm == DSIAlgorithm::EcaM ||
          oParams.eAlgorithm == DSIAlgorithm::EcaB ||
          oParams.eAlgorithm == DSIAlgorithm::EcaS,
          "the ECA canceller handles ECA-m, ECA-B and ECA-S only");
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uClutterTaps > 0, "clutter taps must be non-zero");
    CHECK(oParams.uEstimateSamples > 0 && oParams.uUpdateSamples > 0,
          "estimation and update windows must be non-zero");

    // A non-zero Doppler tap count is a real algorithm, not a tuning knob.
    // Refuse it rather than silently cancelling only stationary clutter while
    // the config file claims otherwise.
    CHECK(oParams.uDopplerTaps == 0,
          "ECA_DOPPLER_TAPS > 0 is not implemented; the clutter subspace is "
          "delay-only for now");

    // The window rule is what separates the names. ECA-m and ECA-B estimate
    // from the window they apply to; ECA-S estimates from a longer one.
    // Neither is allowed to masquerade as the other, or a benchmark row would
    // carry the wrong label.
    if (oParams.eAlgorithm == DSIAlgorithm::EcaS) {
        CHECK(oParams.uEstimateSamples > oParams.uUpdateSamples,
              "ECA-S needs an estimation window longer than the update window; "
              "equal windows are ECA-B");
    } else {
        CHECK(oParams.uEstimateSamples == oParams.uUpdateSamples,
              "an estimation window longer than the update window is ECA-S, "
              "not ECA-B");
    }
    CHECK(oParams.uEstimateSamples <= oParams.uCpiSamples,
          "the estimation window cannot be longer than the CPI");
    CHECK(oParams.uCpiSamples % oParams.uUpdateSamples == 0,
          "the update window must divide the CPI exactly");
    // Keeps every estimation window on an update-window boundary, which is
    // what lets the GPU share partial Grams; enforced on both backends so
    // they place their windows identically.
    CHECK(oParams.uEstimateSamples % oParams.uUpdateSamples == 0,
          "the update window must divide the estimation window exactly");
    CHECK(oParams.uEstimateSamples > oParams.uClutterTaps,
          "the estimation window must hold more samples than there are taps "
          "to solve for");
    CHECK(oParams.dDiagonalLoading >= 0.0, "diagonal loading must not be negative");
    return true;
}

bool cglsParamsOk(const DSIParams& oParams)
{
    CHECK(oParams.eAlgorithm == DSIAlgorithm::Cgls, "the CGLS canceller handles CGLS only");
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uClutterTaps > 0, "clutter taps must be non-zero");
    CHECK(oParams.uCglsBatchSamples > 0, "the CGLS batch must be non-zero");
    CHECK(oParams.uMaxIterations > 0, "CGLS needs at least one iteration");
    CHECK(oParams.uDopplerTaps == 0,
          "ECA_DOPPLER_TAPS > 0 is not implemented; the clutter subspace is "
          "delay-only for now");
    // CGLS is batched like ECA-B: the weights are estimated from the window
    // they are applied to, on its own window length.
    CHECK(oParams.uCpiSamples % oParams.uCglsBatchSamples == 0,
          "the CGLS batch must divide the CPI exactly");
    CHECK(oParams.uCglsBatchSamples > oParams.uClutterTaps,
          "the CGLS batch must hold more samples than there are taps to solve for");
    return true;
}

bool fblmsParamsOk(const DSIParams& oParams)
{
    CHECK(oParams.eAlgorithm == DSIAlgorithm::Fblms, "the FBLMS canceller handles FBLMS only");
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uFblmsTaps > 0, "FBLMS_TAPS must be non-zero");
    CHECK((oParams.uFblmsTaps & (oParams.uFblmsTaps - 1)) == 0,
          "FBLMS_TAPS must be a power of two (radix-2 FFT)");
    CHECK(oParams.uCpiSamples % oParams.uFblmsTaps == 0,
          "FBLMS_TAPS must divide the CPI exactly");
    CHECK(oParams.dFblmsStep > 0.0 && oParams.dFblmsStep <= 1.0,
          "FBLMS_STEP must be in (0, 1]");
    CHECK(oParams.dFblmsForget >= 0.0 && oParams.dFblmsForget < 1.0,
          "FBLMS_FORGET must be in [0, 1)");
    return true;
}

bool ecaLdParamsOk(const DSIParams& oParams)
{
    CHECK(oParams.eAlgorithm == DSIAlgorithm::EcaLd, "the ECA-LD canceller handles ECA-LD only");
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uClutterTaps > 0, "clutter taps must be non-zero");
    CHECK(oParams.uEcaLdBatchSamples > 0, "the ECA-LD batch must be non-zero");
    CHECK(oParams.uDopplerTaps == 0,
          "ECA_DOPPLER_TAPS > 0 is not implemented; the clutter subspace is "
          "delay-only for now");
    CHECK(oParams.uCpiSamples % oParams.uEcaLdBatchSamples == 0,
          "the ECA-LD batch must divide the CPI exactly");
    CHECK(oParams.uEcaLdBatchSamples > oParams.uClutterTaps,
          "the ECA-LD batch must hold more samples than there are taps to solve for");
    CHECK(oParams.dDiagonalLoading >= 0.0, "the diagonal loading must not be negative");
    return true;
}

bool ecaCdParamsOk(const DSIParams& oParams)
{
    CHECK(oParams.eAlgorithm == DSIAlgorithm::EcaCd, "the ECA-CD canceller handles ECA-CD only");
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uCdBatchSamples > 0, "the ECA-CD batch must be non-zero");
    CHECK((oParams.uCdBatchSamples & (oParams.uCdBatchSamples - 1)) == 0,
          "the ECA-CD batch length must be a power of two (radix-2 FFT)");
    CHECK(oParams.uCpiSamples % oParams.uCdBatchSamples == 0,
          "the ECA-CD batch must divide the CPI exactly");
    CHECK(oParams.uCdDopplerTaps <= kMaxCdDopplerTaps,
          "ECA_CD_DOPPLER_TAPS is over kMaxCdDopplerTaps");
    // The Doppler columns are resolved across the batches, so there have to
    // be more batches than columns for the projection to mean anything.
    CHECK(oParams.uCpiSamples / oParams.uCdBatchSamples > 2 * oParams.uCdDopplerTaps + 1,
          "too few ECA-CD batches for the Doppler columns; shorten the batch "
          "or reduce ECA_CD_DOPPLER_TAPS");
    return true;
}

std::vector<std::complex<double>> batchTwiddles(uint64_t uBatches)
{
    std::vector<std::complex<double>> vcTable(static_cast<size_t>(uBatches));
    for (uint64_t i = 0; i < uBatches; ++i) {
        const double dAngle = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(uBatches);
        vcTable[static_cast<size_t>(i)] = std::complex<double>(std::cos(dAngle), std::sin(dAngle));
    }
    return vcTable;
}

double hostSuppressionDb(const ComplexF* pcSur, const ComplexF* pcOut, uint64_t uSamples)
{
    // How much total power the cancellation removed. A headline number, and on
    // its own a misleading one -- an algorithm scores well here whether it
    // removed clutter or the targets. Target attenuation has to be measured
    // from the range-Doppler map.
    double dInput  = 0.0;
    double dOutput = 0.0;
    for (uint64_t n = 0; n < uSamples; ++n) {
        const double dSr = pcSur[n].real(), dSi = pcSur[n].imag();
        const double dOr = pcOut[n].real(), dOi = pcOut[n].imag();
        dInput  += dSr * dSr + dSi * dSi;
        dOutput += dOr * dOr + dOi * dOi;
    }
    return (dInput > 0.0 && dOutput > 0.0) ? 10.0 * std::log10(dInput / dOutput) : 0.0;
}

} // namespace pbr
