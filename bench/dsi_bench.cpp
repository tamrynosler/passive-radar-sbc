// dsi_bench -- time DSI cancellation, and nothing else.
//
// The whole point is isolation. One CPI is frozen to a file beforehand by
//
//     pbr_run --file capture.pbrcap --config config/fers.conf --dump-cpi cpi_0000.bin
//
// so this program never opens a socket, never decodes a packet and never
// waits for FERS. It loads the samples once, calls init() once, and then
// times cancel() and only cancel(). Every algorithm and every run sees the
// same input bytes, so the comparison is fair by construction.
//
// Usage:
//   dsi_bench --cpi data/cpi_0000.bin [--algorithm ECA-B] [--backend CPU]
//             [--iters 10] [--csv out.csv]
//
// The --algorithm and --backend flags override the config file when given.
// With neither, the run is entirely described by the tracked config file.
//
// A note on what the numbers mean. Timing on the Mac proves only that the
// code runs and gives the order of magnitude; citable figures come from the
// Jetson with clocks pinned (nvpmodel -m 0 && jetson_clocks) and tegrastats
// logged alongside, per docs/benchmarking-methodology.md. Both a MINIMUM and
// a mean are reported: under DVFS the mean wanders while the minimum tracks
// the true compute time, and a mean alone has been misleading on this
// hardware before.

#include "pbr/Config.hpp"
#include "pbr/CpiFile.hpp"
#include "pbr/DSICanceller.hpp"
#include "pbr/types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pbr;

namespace {

struct Options {
    std::string sCpiPath;
    std::string sConfigPath = kDefaultConfigPath;
    std::string sAlgorithm;          // empty means "take it from the config"
    std::string sBackend;            // empty means "take it from the config"
    std::string sCsvPath;
    uint64_t    uIterations = 10;
};

void printUsage(const char* pProgram)
{
    std::printf("usage: %s --cpi FILE [options]\n", pProgram);
    std::printf("  --cpi FILE        CPI written by pbr_run --dump-cpi (required)\n");
    std::printf("  --config FILE     configuration (default %s)\n", kDefaultConfigPath);
    std::printf("  --algorithm NAME  override DSI_ALGORITHM: ECA-m ECA-B ECA-S ECA-CD CGLS\n");
    std::printf("  --backend NAME    override DSI_BACKEND: CPU or GPU\n");
    std::printf("  --iters N         timed repetitions after one warm-up (default 10)\n");
    std::printf("  --csv FILE        append one result row\n");
}

bool parseArgs(int argc, char* argv[], Options& oOptions)
{
    for (int i = 1; i < argc; ++i) {
        const std::string sArg = argv[i];
        const bool bHasValue   = (i + 1 < argc);

        if (sArg == "--help" || sArg == "-h") {
            return false;
        } else if (sArg == "--cpi" && bHasValue) {
            oOptions.sCpiPath = argv[++i];
        } else if (sArg == "--config" && bHasValue) {
            oOptions.sConfigPath = argv[++i];
        } else if (sArg == "--algorithm" && bHasValue) {
            oOptions.sAlgorithm = argv[++i];
        } else if (sArg == "--backend" && bHasValue) {
            oOptions.sBackend = argv[++i];
        } else if (sArg == "--csv" && bHasValue) {
            oOptions.sCsvPath = argv[++i];
        } else if (sArg == "--iters" && bHasValue) {
            oOptions.uIterations = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "unrecognised or incomplete argument: %s\n", sArg.c_str());
            return false;
        }
    }

    if (oOptions.sCpiPath.empty()) {
        std::fprintf(stderr, "--cpi is required\n");
        return false;
    }
    if (oOptions.uIterations == 0) {
        std::fprintf(stderr, "--iters must be at least 1\n");
        return false;
    }
    return true;
}

double meanOf(const std::vector<double>& vd)
{
    double d = 0.0;
    for (size_t u = 0; u < vd.size(); ++u)
        d += vd[u];
    return d / static_cast<double>(vd.size());
}

double stdDevOf(const std::vector<double>& vd, double dMean)
{
    if (vd.size() < 2)
        return 0.0;
    double d = 0.0;
    for (size_t u = 0; u < vd.size(); ++u)
        d += (vd[u] - dMean) * (vd[u] - dMean);
    return std::sqrt(d / static_cast<double>(vd.size() - 1));
}

double medianOf(std::vector<double> vd)   // by value: sorting a copy
{
    std::sort(vd.begin(), vd.end());
    const size_t u = vd.size() / 2;
    return (vd.size() % 2 == 0) ? 0.5 * (vd[u - 1] + vd[u]) : vd[u];
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

    // Command-line overrides are applied to the config before it is turned
    // into parameters, so there is exactly one place the algorithm and
    // backend are decided and the printout always matches what ran.
    if (!oOptions.sAlgorithm.empty())
        oCfg.sDsiAlgorithm = oOptions.sAlgorithm;
    if (!oOptions.sBackend.empty())
        oCfg.sDsiBackend = oOptions.sBackend;

    DSIParams oParams;
    if (!dsiParamsFromConfig(oCfg, oParams))
        return 1;

    // ---- load the frozen CPI ----------------------------------------------
    CpiFileHeader oHeader;
    if (!readCpiFileHeader(oOptions.sCpiPath.c_str(), oHeader))
        return 1;

    if (oHeader.uSampleCount != oCfg.uCpiSamples) {
        std::fprintf(stderr,
                     "dsi_bench: %s holds %llu samples but the config says a CPI "
                     "is %llu. The file was dumped with different settings.\n",
                     oOptions.sCpiPath.c_str(),
                     (unsigned long long)oHeader.uSampleCount,
                     (unsigned long long)oCfg.uCpiSamples);
        return 1;
    }

    std::vector<ComplexF> vcRef(static_cast<size_t>(oHeader.uSampleCount));
    std::vector<ComplexF> vcSur(static_cast<size_t>(oHeader.uSampleCount));
    if (!readCpiFile(oOptions.sCpiPath.c_str(), oHeader,
                     vcRef.data(), vcSur.data(), oHeader.uSampleCount))
        return 1;

    std::vector<ComplexF> vcOut(static_cast<size_t>(oHeader.uSampleCount));

    // ---- build and prepare the canceller ----------------------------------
    std::unique_ptr<DSICanceller> pCanceller =
        makeDSICanceller(oParams.eAlgorithm, oParams.eBackend);
    if (!pCanceller)
        return 1;

    // Everything allocates here, outside the timed region.
    if (!pCanceller->init(oParams))
        return 1;

    const double dCpiSeconds = double(oHeader.uSampleCount) /
                               double(oHeader.uSampleRateHz);

    std::printf("dsi_bench\n");
    std::printf("  cpi file      %s\n", oOptions.sCpiPath.c_str());
    std::printf("  samples       %llu  (%.3f s at %llu Hz, start index %llu)\n",
                (unsigned long long)oHeader.uSampleCount, dCpiSeconds,
                (unsigned long long)oHeader.uSampleRateHz,
                (unsigned long long)oHeader.uStartIndex);
    std::printf("  algorithm     %s\n", pCanceller->name());
    std::printf("  clutter taps  %llu\n", (unsigned long long)oParams.uClutterTaps);
    std::printf("  windows       %llu x %llu samples (%.2f ms each)\n",
                (unsigned long long)(oParams.uCpiSamples / oParams.uUpdateSamples),
                (unsigned long long)oParams.uUpdateSamples,
                1000.0 * double(oParams.uUpdateSamples) / double(oHeader.uSampleRateHz));
    std::printf("  loading       %g (%s)\n", oParams.dDiagonalLoading,
                oParams.bAbsoluteLoading ? "absolute, MATLAB reference"
                                         : "relative to the Gram diagonal");
    std::printf("  iterations    %llu after one warm-up\n",
                (unsigned long long)oOptions.uIterations);
    std::printf("\n");

    // ---- warm-up -----------------------------------------------------------
    // The first call touches pages for the first time and fills cold caches;
    // on the GPU it also pays for context creation. Timing it would report a
    // number that never recurs.
    DSIStats oStats;
    if (!pCanceller->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats))
        return 1;

    const double dSuppressionDb = oStats.dSuppressionDb;

    // ---- timed runs --------------------------------------------------------
    std::vector<double> vdMs;
    vdMs.reserve(static_cast<size_t>(oOptions.uIterations));

    for (uint64_t u = 0; u < oOptions.uIterations; ++u) {
        const std::chrono::steady_clock::time_point oStart =
            std::chrono::steady_clock::now();

        if (!pCanceller->cancel(vcRef.data(), vcSur.data(), vcOut.data(), oStats))
            return 1;

        const std::chrono::steady_clock::time_point oEnd =
            std::chrono::steady_clock::now();

        vdMs.push_back(
            std::chrono::duration<double, std::milli>(oEnd - oStart).count());

        // Every run must give the same answer; a canceller that drifts is
        // carrying state it should not.
        if (std::fabs(oStats.dSuppressionDb - dSuppressionDb) > 1e-6) {
            std::fprintf(stderr,
                         "dsi_bench: suppression changed between identical runs "
                         "(%.6f dB then %.6f dB)\n",
                         dSuppressionDb, oStats.dSuppressionDb);
            return 1;
        }
    }

    const double dMin    = *std::min_element(vdMs.begin(), vdMs.end());
    const double dMax    = *std::max_element(vdMs.begin(), vdMs.end());
    const double dMean   = meanOf(vdMs);
    const double dMedian = medianOf(vdMs);
    const double dStdDev = stdDevOf(vdMs, dMean);

    // The real-time question: one CPI of signal arrives every dCpiSeconds, so
    // anything at or above 1.0 here cannot keep up with a live stream.
    const double dBudget = (dMin / 1000.0) / dCpiSeconds;

    std::printf("================ dsi_bench results ================\n");
    std::printf("  suppression   %.2f dB\n", dSuppressionDb);
    if (oStats.uIterations > 0)
        std::printf("  solver iters  %llu\n", (unsigned long long)oStats.uIterations);
    std::printf("  time per CPI  min %.3f ms   median %.3f ms\n", dMin, dMedian);
    std::printf("                mean %.3f ms  std %.3f ms  max %.3f ms\n",
                dMean, dStdDev, dMax);
    std::printf("  real-time     %.3f x the %.3f s CPI  (%s)\n",
                dBudget, dCpiSeconds,
                (dBudget < 1.0) ? "fits" : "TOO SLOW for a live stream");
    std::printf("===================================================\n");
    std::printf("\nTiming is only citable from the Jetson with clocks pinned.\n");

    if (!oOptions.sCsvPath.empty()) {
        // Append, so a sweep over algorithms and backends accumulates into one
        // file. The header is written only when the file is new.
        const bool bExists = [&]() {
            std::FILE* p = std::fopen(oOptions.sCsvPath.c_str(), "r");
            if (p == nullptr) return false;
            std::fclose(p);
            return true;
        }();

        std::FILE* pCsv = std::fopen(oOptions.sCsvPath.c_str(), "a");
        if (pCsv == nullptr) {
            std::fprintf(stderr, "dsi_bench: cannot open %s for writing\n",
                         oOptions.sCsvPath.c_str());
            return 1;
        }
        if (!bExists) {
            std::fprintf(pCsv, "algorithm,backend,cpiSamples,sampleRateHz,"
                               "clutterTaps,updateSamples,dopplerTaps,loading,"
                               "absoluteLoading,iterations,suppressionDb,"
                               "minMs,medianMs,meanMs,stdMs,maxMs,realTimeRatio\n");
        }
        std::fprintf(pCsv,
                     "%s,%s,%llu,%llu,%llu,%llu,%llu,%g,%d,%llu,"
                     "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.6f\n",
                     dsiAlgorithmName(oParams.eAlgorithm),
                     dsiBackendName(oParams.eBackend),
                     (unsigned long long)oParams.uCpiSamples,
                     (unsigned long long)oHeader.uSampleRateHz,
                     (unsigned long long)oParams.uClutterTaps,
                     (unsigned long long)oParams.uUpdateSamples,
                     (unsigned long long)oParams.uDopplerTaps,
                     oParams.dDiagonalLoading,
                     oParams.bAbsoluteLoading ? 1 : 0,
                     (unsigned long long)oOptions.uIterations,
                     dSuppressionDb, dMin, dMedian, dMean, dStdDev, dMax, dBudget);
        std::fclose(pCsv);
        std::printf("appended a row to %s\n", oOptions.sCsvPath.c_str());
    }

    return 0;
}
