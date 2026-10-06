// ard_bench -- time range-Doppler map formation and CFAR, and nothing else.
//
// Same isolation as dsi_bench: one frozen CPI, loaded once, init() once, and
// the clock around process() only. Feed it the CLEANED CPI written by
// dsi_verify --dump-output, so the canceller is not in the measurement.
//
// Usage:
//   ard_bench --cleaned data/S0_clean/cleaned_0000.bin [--config FILE]
//             [--backend GPU] [--algorithm Batches] [--iters 20] [--csv FILE]

#include "pbr/ArdProcessor.hpp"
#include "pbr/Config.hpp"
#include "pbr/CpiFile.hpp"
#include "pbr/types.hpp"
#include "BenchCommon.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pbr;
using namespace pbrbench;

namespace {

struct Options {
    std::string sCleanedPath;
    std::string sConfigPath = kDefaultConfigPath;
    std::string sBackend;            // empty means "take it from the config"
    std::string sAlgorithm;          // empty means "take it from the config"
    std::string sCsvPath;
    uint64_t    uIterations = 20;
};

void printUsage(const char* pProgram)
{
    std::printf("usage: %s --cleaned FILE [options]\n", pProgram);
    std::printf("  --cleaned FILE    (reference, cleaned) CPI from dsi_verify --dump-output\n");
    std::printf("  --config FILE     configuration (default %s)\n", kDefaultConfigPath);
    std::printf("  --backend NAME    override the backend: CPU or GPU\n");
    std::printf("  --algorithm NAME  override ARD_ALGORITHM: Direct or Batches\n");
    std::printf("  --iters N         timed repetitions after one warm-up (default 20)\n");
    std::printf("  --csv FILE        append one result row\n");
}

bool parseArgs(int argc, char* argv[], Options& oOptions)
{
    for (int i = 1; i < argc; ++i) {
        const std::string sArg = argv[i];
        const bool bHasValue   = (i + 1 < argc);

        if (sArg == "--help" || sArg == "-h") {
            return false;
        } else if (sArg == "--cleaned" && bHasValue) {
            oOptions.sCleanedPath = argv[++i];
        } else if (sArg == "--config" && bHasValue) {
            oOptions.sConfigPath = argv[++i];
        } else if (sArg == "--backend" && bHasValue) {
            oOptions.sBackend = argv[++i];
        } else if (sArg == "--algorithm" && bHasValue) {
            oOptions.sAlgorithm = argv[++i];
        } else if (sArg == "--csv" && bHasValue) {
            oOptions.sCsvPath = argv[++i];
        } else if (sArg == "--iters" && bHasValue) {
            oOptions.uIterations = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "unrecognised or incomplete argument: %s\n", sArg.c_str());
            return false;
        }
    }

    if (oOptions.sCleanedPath.empty() || oOptions.uIterations == 0) {
        std::fprintf(stderr, "--cleaned is required and --iters must be at least 1\n");
        return false;
    }
    return true;
}

bool appendCsvRow(const char* pPath, const ArdParams& oParams, const ArdStats& oStats,
                  const TimingSummary& oTiming, double dRealTime)
{
    std::FILE* pCsv = openCsvForAppend(pPath,
        "stage,algorithm,backend,rangeBins,dopplerBins,detections,borderDetections,"
        "peakDb,minMs,medianMs,meanMs,realTime");
    if (pCsv == nullptr)
        return false;
    std::fprintf(pCsv, "ARD,%s,%s,%llu,%llu,%llu,%llu,%.4f,%.4f,%.4f,%.4f,%.6f\n",
                 ardAlgorithmName(oParams.eAlgorithm),
                 ardBackendName(oParams.eBackend),
                 (unsigned long long)oParams.uRangeBins,
                 (unsigned long long)oParams.uDopplerBins,
                 (unsigned long long)oStats.uDetections,
                 (unsigned long long)oStats.uDetectionsInBorder,
                 oStats.dPeakDb, oTiming.dMinMs, oTiming.dMedianMs, oTiming.dMeanMs,
                 dRealTime);
    std::fclose(pCsv);
    std::printf("appended a row to %s\n", pPath);
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    Options oOptions;
    if (!parseArgs(argc, argv, oOptions)) {
        printUsage(argv[0]);
        return 1;
    }

    PbrConfig oCfg;
    if (!loadPbrConfig(oOptions.sConfigPath.c_str(), oCfg))
        return 1;
    if (!oOptions.sBackend.empty())
        oCfg.sDsiBackend = oOptions.sBackend;
    if (!oOptions.sAlgorithm.empty())
        oCfg.sArdAlgorithm = oOptions.sAlgorithm;

    ArdParams oParams;
    if (!ardParamsFromConfig(oCfg, oParams))
        return 1;

    // ---- load the cleaned CPI ----------------------------------------------
    CpiFileHeader oHeader;
    if (!readCpiFileHeader(oOptions.sCleanedPath.c_str(), oHeader))
        return 1;
    if (oHeader.uSampleCount != oParams.uCpiSamples) {
        std::fprintf(stderr, "ard_bench: %s holds %llu samples, config says %llu\n",
                     oOptions.sCleanedPath.c_str(), (unsigned long long)oHeader.uSampleCount,
                     (unsigned long long)oParams.uCpiSamples);
        return 1;
    }
    std::vector<ComplexF> vcRef(static_cast<size_t>(oHeader.uSampleCount));
    std::vector<ComplexF> vcCleaned(static_cast<size_t>(oHeader.uSampleCount));
    if (!readCpiFile(oOptions.sCleanedPath.c_str(), oHeader, vcRef.data(), vcCleaned.data(),
                     oHeader.uSampleCount))
        return 1;

    // ---- build and prepare: everything allocates here -----------------------
    std::unique_ptr<ArdProcessor> pArd = makeArdProcessor(oParams.eBackend);
    if (!pArd || !pArd->init(oParams))
        return 1;

    std::vector<ComplexF>  vcMap(static_cast<size_t>(oParams.uRangeBins * oParams.uDopplerBins));
    std::vector<Detection> vDet(static_cast<size_t>(kMaxArdDetections));
    ArdStats oStats;

    std::printf("ard_bench\n");
    std::printf("  cleaned cpi   %s\n", oOptions.sCleanedPath.c_str());
    std::printf("  processor     %s\n", pArd->name());
    std::printf("  map           %llu range x %llu doppler\n",
                (unsigned long long)oParams.uRangeBins,
                (unsigned long long)oParams.uDopplerBins);
    std::printf("  iterations    %llu after one warm-up\n\n",
                (unsigned long long)oOptions.uIterations);

    // Warm-up: first call touches pages and, on the GPU, creates the context.
    if (!pArd->process(vcRef.data(), vcCleaned.data(), vcMap.data(),
                       vDet.data(), kMaxArdDetections, oStats))
        return 1;
    const uint64_t uFirstDetections = oStats.uDetections;

    std::vector<double> vdMs;
    vdMs.reserve(static_cast<size_t>(oOptions.uIterations));
    for (uint64_t u = 0; u < oOptions.uIterations; ++u) {
        const std::chrono::steady_clock::time_point oStart = std::chrono::steady_clock::now();
        if (!pArd->process(vcRef.data(), vcCleaned.data(), vcMap.data(),
                           vDet.data(), kMaxArdDetections, oStats))
            return 1;
        const std::chrono::steady_clock::time_point oEnd = std::chrono::steady_clock::now();
        vdMs.push_back(std::chrono::duration<double, std::milli>(oEnd - oStart).count());

        if (oStats.uDetections != uFirstDetections) {
            std::fprintf(stderr, "ard_bench: detection count changed between "
                                 "identical runs (%llu then %llu)\n",
                         (unsigned long long)uFirstDetections,
                         (unsigned long long)oStats.uDetections);
            return 1;
        }
    }

    const TimingSummary oTiming = summarise(vdMs);
    const double dCpiSeconds = static_cast<double>(oHeader.uSampleCount) /
                               static_cast<double>(oHeader.uSampleRateHz);
    const double dRealTime   = (oTiming.dMinMs / 1000.0) / dCpiSeconds;

    std::printf("================ ard_bench results ================\n");
    std::printf("  detections    %llu  (%llu in the CFAR border)\n",
                (unsigned long long)oStats.uDetections,
                (unsigned long long)oStats.uDetectionsInBorder);
    std::printf("  peak          %.4f dB\n", oStats.dPeakDb);
    std::printf("  time per CPI  min %.3f ms   median %.3f ms   mean %.3f ms\n",
                oTiming.dMinMs, oTiming.dMedianMs, oTiming.dMeanMs);
    std::printf("  real-time     %.4f x the %.3f s CPI\n", dRealTime, dCpiSeconds);
    std::printf("===================================================\n");

    if (!oOptions.sCsvPath.empty() &&
        !appendCsvRow(oOptions.sCsvPath.c_str(), oParams, oStats, oTiming, dRealTime))
        return 1;
    return 0;
}
