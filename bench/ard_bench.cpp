// ard_bench -- time range-Doppler map formation and CFAR, and nothing else.
//
// Same isolation as dsi_bench: one frozen CPI, loaded once, init() once, and
// the clock around process() only. Feed it the CLEANED CPI written by
// dsi_verify --dump-output, so the canceller is not in the measurement.
//
// Usage:
//   ard_bench --cleaned data/cleaned_0000.bin [--backend GPU] [--iters 20]

#include "pbr/ArdProcessor.hpp"
#include "pbr/Config.hpp"
#include "pbr/CpiFile.hpp"
#include "pbr/types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pbr;

int main(int argc, char* argv[])
{
    std::string sCleaned, sConfig = "config/prb.conf", sBackend, sCsv;
    uint64_t uIters = 20;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool bV = (i + 1 < argc);
        if (a == "--cleaned" && bV)      sCleaned = argv[++i];
        else if (a == "--config" && bV)  sConfig  = argv[++i];
        else if (a == "--backend" && bV) sBackend = argv[++i];
        else if (a == "--csv" && bV)     sCsv     = argv[++i];
        else if (a == "--iters" && bV)   uIters   = std::strtoull(argv[++i], nullptr, 10);
        else {
            std::printf("usage: %s --cleaned FILE [--backend CPU|GPU] "
                        "[--iters N] [--csv FILE]\n", argv[0]);
            return 1;
        }
    }
    if (sCleaned.empty() || uIters == 0) {
        std::fprintf(stderr, "--cleaned is required and --iters must be >= 1\n");
        return 1;
    }

    PbrConfig oCfg;
    if (!loadPbrConfig(sConfig.c_str(), oCfg))
        return 1;
    if (!sBackend.empty())
        oCfg.sDsiBackend = sBackend;

    ArdParams oParams;
    if (!ardParamsFromConfig(oCfg, oParams))
        return 1;

    CpiFileHeader oHeader;
    if (!readCpiFileHeader(sCleaned.c_str(), oHeader))
        return 1;
    if (oHeader.uSampleCount != oParams.uCpiSamples) {
        std::fprintf(stderr, "ard_bench: %s holds %llu samples, config says %llu\n",
                     sCleaned.c_str(), (unsigned long long)oHeader.uSampleCount,
                     (unsigned long long)oParams.uCpiSamples);
        return 1;
    }

    std::vector<ComplexF> vcRef(static_cast<size_t>(oHeader.uSampleCount));
    std::vector<ComplexF> vcCleaned(static_cast<size_t>(oHeader.uSampleCount));
    if (!readCpiFile(sCleaned.c_str(), oHeader, vcRef.data(), vcCleaned.data(),
                     oHeader.uSampleCount))
        return 1;

    std::unique_ptr<ArdProcessor> pArd = makeArdProcessor(oParams.eBackend);
    if (!pArd || !pArd->init(oParams))
        return 1;

    const uint64_t uCells = oParams.uRangeBins * oParams.uDopplerBins;
    std::vector<ComplexF>  vcMap(static_cast<size_t>(uCells));
    std::vector<Detection> vDet(8192);
    ArdStats oStats;

    const double dCpiSeconds = double(oHeader.uSampleCount) /
                               double(oHeader.uSampleRateHz);

    std::printf("ard_bench\n");
    std::printf("  cleaned cpi   %s\n", sCleaned.c_str());
    std::printf("  backend       %s\n", pArd->name());
    std::printf("  map           %llu range x %llu doppler\n",
                (unsigned long long)oParams.uRangeBins,
                (unsigned long long)oParams.uDopplerBins);
    std::printf("  iterations    %llu after one warm-up\n\n",
                (unsigned long long)uIters);

    // Warm-up: first call touches pages and, on the GPU, creates the context.
    if (!pArd->process(vcRef.data(), vcCleaned.data(), vcMap.data(),
                       vDet.data(), 8192, oStats))
        return 1;
    const uint64_t uFirstDetections = oStats.uDetections;

    std::vector<double> vdMs;
    vdMs.reserve(static_cast<size_t>(uIters));
    for (uint64_t u = 0; u < uIters; ++u) {
        const std::chrono::steady_clock::time_point t0 =
            std::chrono::steady_clock::now();
        if (!pArd->process(vcRef.data(), vcCleaned.data(), vcMap.data(),
                           vDet.data(), 8192, oStats))
            return 1;
        const std::chrono::steady_clock::time_point t1 =
            std::chrono::steady_clock::now();
        vdMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());

        if (oStats.uDetections != uFirstDetections) {
            std::fprintf(stderr, "ard_bench: detection count changed between "
                                 "identical runs (%llu then %llu)\n",
                         (unsigned long long)uFirstDetections,
                         (unsigned long long)oStats.uDetections);
            return 1;
        }
    }

    std::sort(vdMs.begin(), vdMs.end());
    double dMean = 0.0;
    for (size_t u = 0; u < vdMs.size(); ++u) dMean += vdMs[u];
    dMean /= double(vdMs.size());
    const double dMin = vdMs.front();
    const double dMed = vdMs[vdMs.size() / 2];

    std::printf("================ ard_bench results ================\n");
    std::printf("  detections    %llu  (%llu in the CFAR border)\n",
                (unsigned long long)oStats.uDetections,
                (unsigned long long)oStats.uDetectionsInBorder);
    std::printf("  peak          %.4f dB\n", oStats.dPeakDb);
    std::printf("  time per CPI  min %.3f ms   median %.3f ms   mean %.3f ms\n",
                dMin, dMed, dMean);
    std::printf("  real-time     %.4f x the %.3f s CPI\n",
                (dMin / 1000.0) / dCpiSeconds, dCpiSeconds);
    std::printf("===================================================\n");

    if (!sCsv.empty()) {
        const bool bExists = [&]() {
            std::FILE* p = std::fopen(sCsv.c_str(), "r");
            if (!p) return false;
            std::fclose(p); return true;
        }();
        std::FILE* pCsv = std::fopen(sCsv.c_str(), "a");
        if (!pCsv) { std::fprintf(stderr, "cannot open %s\n", sCsv.c_str()); return 1; }
        if (!bExists)
            std::fprintf(pCsv, "stage,backend,rangeBins,dopplerBins,detections,"
                               "borderDetections,peakDb,minMs,medianMs,meanMs,realTime\n");
        std::fprintf(pCsv, "ARD,%s,%llu,%llu,%llu,%llu,%.4f,%.4f,%.4f,%.4f,%.6f\n",
                     ardBackendName(oParams.eBackend),
                     (unsigned long long)oParams.uRangeBins,
                     (unsigned long long)oParams.uDopplerBins,
                     (unsigned long long)oStats.uDetections,
                     (unsigned long long)oStats.uDetectionsInBorder,
                     oStats.dPeakDb, dMin, dMed, dMean,
                     (dMin / 1000.0) / dCpiSeconds);
        std::fclose(pCsv);
        std::printf("appended a row to %s\n", sCsv.c_str());
    }
    return 0;
}
