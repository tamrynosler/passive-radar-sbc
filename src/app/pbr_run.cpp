// pbr_run -- the whole processing chain, live or from a recorded capture.
//
//   VITA 49.2 UDP  ->  circular buffers  ->  CPI extraction
//                  ->  DSI cancellation  ->  range-Doppler map  ->  CFAR
//                  ->  detections, telemetry, optional ARD snapshots
//
// Usage:
//   ./pbr_run --live --seconds 30 --ard-dir data/ard --detections data/det.csv
//   ./pbr_run --file data/fers_capture.pbrcap --fullscale 0.005
//
// Both sources run the same loop, so a recorded run exercises the live path.
//
// ---------------------------------------------------------------------------
// Why this is single-threaded
// ---------------------------------------------------------------------------
//
// Processing costs about 162 ms per CPI on the Jetson (142 ms cancelling,
// 20 ms mapping) and a CPI arrives once a second. While processing runs, the
// socket is not being drained, so datagrams accumulate: 1198 per second at
// 1400 bytes is 1.68 MB/s, and 162 ms of that is 272 KB against a socket
// buffer of 8 MB -- about 3% of it, or a 6x margin on the CPI budget.
//
// A receive thread would buy nothing at those numbers and would introduce
// shared state between a producer and a consumer, which is where the bugs
// that only appear after an hour live. The telemetry below reports the margin
// every run, so the day it stops being true is visible rather than inferred.
//
// ---------------------------------------------------------------------------
// How "real time" is judged
// ---------------------------------------------------------------------------
//
// Three signals, of which the third is the one that matters:
//
//   * CPIs skipped -- a window overwritten before we reached it. Must be 0.
//   * packet continuity -- every sample lands where its timestamp says.
//   * LAG = wall time elapsed - signal time elapsed. If the chain keeps up
//     this stays flat; if it falls behind it climbs monotonically, and it
//     starts climbing long before anything is actually dropped.

#include "pbr/ArdFile.hpp"
#include "pbr/ArdProcessor.hpp"
#include "pbr/CaptureFile.hpp"
#include "pbr/ChannelIngest.hpp"
#include "pbr/Config.hpp"
#include "pbr/DSICanceller.hpp"
#include "pbr/UdpReceiver.hpp"
#include "pbr/Vita49Packet.hpp"
#include "pbr/types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <string>
#include <vector>

using namespace pbr;

namespace {

const size_t   kDatagramCapacity = 2048;
const uint64_t kMaxDetections    = 8192;
const char* const kDefaultConfigPath = "config/prb.conf";

// Everything that describes the SIGNAL PROCESSING lives in the config file, so
// a run is fully reproducible from a tracked file. What is left here is only
// what varies per invocation: where the samples come from, how long to run,
// and where to put the results.
struct Options {
    std::string sCapturePath;       // empty means live
    std::string sConfigPath = kDefaultConfigPath;
    std::string sOutDir;            // empty means console only
    double      dSeconds    = 0.0;  // 0 means "until the source ends"
    bool        bLive       = false;
};

void printUsage(const char* pProgram)
{
    std::printf("usage: %s (--live | --file CAPTURE) [options]\n", pProgram);
    std::printf("  --live           receive from FERS over UDP\n");
    std::printf("  --file FILE      replay a .pbrcap capture instead\n");
    std::printf("  --seconds N      stop after N seconds (live only)\n");
    std::printf("  --out DIR        write results here: ard/ snapshots,\n");
    std::printf("                   telemetry.csv and detections.csv\n");
    std::printf("  --config FILE    configuration (default %s)\n", kDefaultConfigPath);
    std::printf("\n");
    std::printf("Everything else -- algorithm, backend, full scale, CFAR and ECA\n");
    std::printf("settings, snapshot rate -- comes from the config file, so a run\n");
    std::printf("is described entirely by a tracked file rather than by whatever\n");
    std::printf("flags happened to be typed.\n");
}

bool parseArgs(int argc, char* argv[], Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string sArg = argv[i];
        const bool bV = (i + 1 < argc);

        if (sArg == "--help" || sArg == "-h")   return false;
        else if (sArg == "--live")              o.bLive = true;
        else if (sArg == "--file" && bV)        o.sCapturePath = argv[++i];
        else if (sArg == "--config" && bV)      o.sConfigPath = argv[++i];
        else if (sArg == "--out" && bV)         o.sOutDir = argv[++i];
        else if (sArg == "--seconds" && bV)     o.dSeconds = std::atof(argv[++i]);
        else {
            std::fprintf(stderr, "unrecognised or incomplete argument: %s\n", sArg.c_str());
            return false;
        }
    }

    // Valid: --live with no file, or --file with no --live. Anything else is
    // ambiguous about where the samples should come from.
    if (o.bLive != o.sCapturePath.empty()) {
        std::fprintf(stderr, "give exactly one of --live or --file CAPTURE\n");
        return false;
    }
    return true;
}

double nowSeconds(const std::chrono::steady_clock::time_point& oStart)
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - oStart).count();
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

    const double dFullScale = oCfg.dFullScale;

    // Build the output paths from the one directory the caller gave.
    std::string sArdDir, sTelemetryPath, sDetectionsPath;
    if (!oOptions.sOutDir.empty()) {
        sArdDir         = oOptions.sOutDir + "/ard";
        sTelemetryPath  = oOptions.sOutDir + "/telemetry.csv";
        sDetectionsPath = oOptions.sOutDir + "/detections.csv";
        // mkdir -p, so a run never fails at the first CPI for want of a
        // directory it could have made at startup.
        const std::string sCmd = "mkdir -p '" + sArdDir + "'";
        if (std::system(sCmd.c_str()) != 0) {
            std::fprintf(stderr, "pbr_run: cannot create %s\n", sArdDir.c_str());
            return 1;
        }
    }

    // ---- build the chain ---------------------------------------------------
    DSIParams oDsiParams;
    ArdParams oArdParams;
    if (!dsiParamsFromConfig(oCfg, oDsiParams) ||
        !ardParamsFromConfig(oCfg, oArdParams))
        return 1;

    std::unique_ptr<DSICanceller> pDsi =
        makeDSICanceller(oDsiParams.eAlgorithm, oDsiParams.eBackend);
    if (!pDsi || !pDsi->init(oDsiParams))
        return 1;

    std::unique_ptr<ArdProcessor> pArd = makeArdProcessor(oArdParams.eBackend);
    if (!pArd || !pArd->init(oArdParams))
        return 1;

    // Chain the two stages through device memory when both are on the GPU, so
    // the cleaned samples never move through the host. Falls back cleanly
    // otherwise, and says which it did -- a GPU run must never quietly be a
    // host run.
    const bool bDeviceCoupled = (pDsi->deviceOutput() != nullptr);
    pDsi->setHostOutputEnabled(!bDeviceCoupled);

    ChannelIngestConfig oIngestConfig;
    oIngestConfig.uSampleRateHz     = oCfg.uSampleRateHz;
    oIngestConfig.dFullScale        = dFullScale;
    oIngestConfig.uBufferCapacity   = oCfg.uBufferCapacity;
    oIngestConfig.sReferenceName    = oCfg.sReferenceName;
    oIngestConfig.sSurveillanceName = oCfg.sSurveillanceName;
    oIngestConfig.uCpiSamples       = oCfg.uCpiSamples;
    oIngestConfig.uHopSamples       = oCfg.uHopSamples;
    oIngestConfig.dMinCpiCoverage   = oCfg.dMinCpiCoverage;

    ChannelIngest oIngest(oIngestConfig);
    if (!oIngest.isValid()) {
        std::fprintf(stderr, "pbr_run: ChannelIngest failed to initialise\n");
        return 1;
    }

    // ---- buffers, all allocated up front -----------------------------------
    const uint64_t uCells = oArdParams.uRangeBins * oArdParams.uDopplerBins;
    std::vector<ComplexF>  vcRef(static_cast<size_t>(oCfg.uCpiSamples));
    std::vector<ComplexF>  vcSur(static_cast<size_t>(oCfg.uCpiSamples));
    std::vector<ComplexF>  vcCleaned(bDeviceCoupled ? 0
                                     : static_cast<size_t>(oCfg.uCpiSamples));
    std::vector<ComplexF>  vcMap(static_cast<size_t>(uCells));
    std::vector<Detection> vDet(static_cast<size_t>(kMaxDetections));
    std::vector<uint8_t>   vDatagram(kDatagramCapacity);

    printPbrConfig(oCfg);
    std::printf("\npbr_run\n");
    std::printf("  source        %s\n",
                oOptions.bLive ? "live UDP" : oOptions.sCapturePath.c_str());
    std::printf("  output        %s\n",
                oOptions.sOutDir.empty() ? "console only" : oOptions.sOutDir.c_str());
    std::printf("  chain         %s -> %s\n", pDsi->name(), pArd->name());
    std::printf("  coupling      %s\n",
                bDeviceCoupled ? "device-coupled: samples stay on the GPU"
                               : "via host: the cleaned CPI round-trips");
    if (oOptions.bLive && !bDeviceCoupled) {
        std::fprintf(stderr,
            "\npbr_run: refusing a live run on the CPU backend. It is a\n"
            "         correctness reference -- the direct DFT alone takes tens\n"
            "         of seconds per CPI -- so it cannot keep up with a live\n"
            "         stream and the run would only measure how far behind it\n"
            "         fell. Use --backend GPU.\n");
        return 1;
    }

    // ---- warm up -----------------------------------------------------------
    // The first CUDA call pays for context creation, and cuFFT defers some
    // plan work to its first execution. Without this the very first CPI blows
    // its budget and every run's first telemetry row looks like a failure.
    {
        std::fill(vcRef.begin(), vcRef.end(), ComplexF(1.0f, 0.0f));
        std::fill(vcSur.begin(), vcSur.end(), ComplexF(0.5f, 0.5f));
        DSIStats oWarmDsi;
        ArdStats oWarmArd;
        const std::chrono::steady_clock::time_point t0 =
            std::chrono::steady_clock::now();

        bool bWarm = pDsi->cancel(vcRef.data(), vcSur.data(),
                                  bDeviceCoupled ? nullptr : vcCleaned.data(),
                                  oWarmDsi);
        if (bWarm) {
            bWarm = bDeviceCoupled
                  ? pArd->processDevice(pDsi->deviceReference(), pDsi->deviceOutput(),
                                        vcMap.data(), vDet.data(), kMaxDetections,
                                        oWarmArd)
                  : pArd->process(vcRef.data(), vcCleaned.data(), vcMap.data(),
                                  vDet.data(), kMaxDetections, oWarmArd);
        }
        if (!bWarm) {
            std::fprintf(stderr, "pbr_run: the warm-up pass failed\n");
            return 1;
        }
        std::printf("  warm-up       %.1f ms (not counted)\n",
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count());
    }
    std::printf("\n");

    // ---- open the source ---------------------------------------------------
    UdpReceiver   oReceiver;
    CaptureReader oReader;
    if (oOptions.bLive) {
        if (!oReceiver.open(oCfg.uUdpPort))
            return 1;
        // The single-threaded design rests on this buffer absorbing the
        // ~272 KB that arrives while a CPI is being processed, so report what
        // the kernel actually granted rather than what was asked for.
        const double dSlackSeconds =
            double(oReceiver.actualRcvBufBytes()) / (1198.0 * 1400.0);
        std::printf("socket buffer %d bytes (%.2f s of slack)\n",
                    oReceiver.actualRcvBufBytes(), dSlackSeconds);
        if (dSlackSeconds < 1.0)
            std::printf("  WARNING: under 1 s of slack. Raise "
                        "net.core.rmem_max on the board.\n");
        std::printf("listening on UDP port %u -- start fers-cli now\n\n",
                    static_cast<unsigned>(oCfg.uUdpPort));
    } else if (!oReader.open(oOptions.sCapturePath.c_str())) {
        return 1;
    }

    // ---- output files ------------------------------------------------------
    std::FILE* pDetCsv = nullptr;
    if (!sDetectionsPath.empty()) {
        pDetCsv = std::fopen(sDetectionsPath.c_str(), "w");
        if (pDetCsv == nullptr) {
            std::fprintf(stderr, "pbr_run: cannot open %s\n",
                         sDetectionsPath.c_str());
            return 1;
        }
        std::fprintf(pDetCsv, "cpi,signalTimeS,rangeBin,dopplerBin,"
                              "rangeKm,dopplerHz,powerDb,snrDb\n");
    }

    std::FILE* pTelCsv = nullptr;
    if (!sTelemetryPath.empty()) {
        pTelCsv = std::fopen(sTelemetryPath.c_str(), "w");
        if (pTelCsv == nullptr) {
            std::fprintf(stderr, "pbr_run: cannot open %s\n",
                         sTelemetryPath.c_str());
            return 1;
        }
        std::fprintf(pTelCsv, "cpi,cpiStartIndex,signalTimeS,wallTimeS,lagS,"
                              "dsiMs,ardMs,totalMs,suppressionDb,peakDb,"
                              "detections,borderDetections,skipped,datagrams\n");
    }

    // ---- the loop ----------------------------------------------------------
    const std::chrono::steady_clock::time_point oStart =
        std::chrono::steady_clock::now();

    uint64_t uCpiIndex     = 0;
    uint64_t uDatagrams    = 0;
    uint64_t uParseFailures = 0;
    uint64_t uFirstCpiStart = 0;
    double   dWallAtFirstCpi = 0.0;
    bool     bHaveFirst     = false;
    double   dMaxLag        = 0.0;
    double   dWorstTotalMs  = 0.0;
    std::vector<double> vdTotalMs;
    bool     bReadError     = false;
    bool     bProcessError  = false;

    for (;;) {
        if (oOptions.dSeconds > 0.0 && nowSeconds(oStart) >= oOptions.dSeconds)
            break;

        size_t uLength = 0;
        bool   bGot    = false;

        if (oOptions.bLive) {
            const ReceiveResult eResult =
                oReceiver.receive(vDatagram.data(), vDatagram.size(), uLength);
            if (eResult == ReceiveResult::WouldBlock)
                continue;   // nothing waiting; the socket is non-blocking
            if (eResult == ReceiveResult::Error) {
                bReadError = true;
                break;
            }
            bGot = true;
        } else {
            bool bEof = false;
            if (!oReader.read(vDatagram.data(), vDatagram.size(), uLength, bEof)) {
                if (!bEof)
                    bReadError = true;
                break;
            }
            bGot = true;
        }
        if (!bGot)
            continue;

        ++uDatagrams;

        Vita49Packet oPacket;
        if (!parseVita49Packet(vDatagram.data(), uLength, oPacket)) {
            ++uParseFailures;
            continue;
        }
        if (!oIngest.handlePacket(oPacket)) {
            bProcessError = true;
            break;
        }

        // Drain whatever CPIs the new data completed.
        for (;;) {
            uint64_t uCpiStart = 0;
            const CpiStatus eCpi = oIngest.tryExtractCpi(
                vcRef.data(), vcSur.data(), oCfg.uCpiSamples, uCpiStart);

            if (eCpi == CpiStatus::NotYet)  break;
            if (eCpi == CpiStatus::Skipped) continue;

            if (!bHaveFirst) {
                bHaveFirst      = true;
                uFirstCpiStart  = uCpiStart;
                dWallAtFirstCpi = nowSeconds(oStart);
            }

            // ---- cancel ---------------------------------------------------
            const std::chrono::steady_clock::time_point t0 =
                std::chrono::steady_clock::now();
            DSIStats oDsiStats;
            if (!pDsi->cancel(vcRef.data(), vcSur.data(),
                              bDeviceCoupled ? nullptr : vcCleaned.data(),
                              oDsiStats)) {
                bProcessError = true;
                break;
            }

            // ---- map and detect -------------------------------------------
            const std::chrono::steady_clock::time_point t1 =
                std::chrono::steady_clock::now();
            ArdStats oArdStats;
            const bool bWantMap = !sArdDir.empty() &&
                                  (uCpiIndex % oCfg.uArdSnapshotEvery == 0);
            ComplexF* pMap = bWantMap ? vcMap.data() : nullptr;

            const bool bArdOk = bDeviceCoupled
                ? pArd->processDevice(pDsi->deviceReference(), pDsi->deviceOutput(),
                                      pMap, vDet.data(), kMaxDetections, oArdStats)
                : pArd->process(vcRef.data(), vcCleaned.data(), pMap,
                                vDet.data(), kMaxDetections, oArdStats);
            if (!bArdOk) {
                bProcessError = true;
                break;
            }
            const std::chrono::steady_clock::time_point t2 =
                std::chrono::steady_clock::now();

            const double dDsiMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            const double dArdMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
            const double dTotalMs = dDsiMs + dArdMs;

            // Signal time is where this CPI sits in the stream; wall time is
            // how long we have been running. What matters is not the gap
            // between them but whether that gap GROWS.
            //
            // Both clocks are zeroed at the first CPI. Anything before it --
            // waiting for the transmitter to start, the socket filling, the
            // first window completing -- is a fixed startup offset, not a
            // backlog, and measuring from program start would bake it in
            // permanently and make a chain that is keeping up perfectly look
            // like one that is seconds behind.
            const double dSignalS = double(uCpiStart - uFirstCpiStart) /
                                    double(oCfg.uSampleRateHz);
            const double dWallS   = nowSeconds(oStart);
            const double dLagS    = (dWallS - dWallAtFirstCpi) - dSignalS;
            if (dLagS > dMaxLag) dMaxLag = dLagS;
            if (dTotalMs > dWorstTotalMs) dWorstTotalMs = dTotalMs;
            vdTotalMs.push_back(dTotalMs);

            const ChannelIngest::Stats& oIngestStats = oIngest.stats();

            std::printf("CPI %4llu  t=%7.3f s  dsi %6.1f  ard %5.1f  "
                        "total %6.1f ms  %5.2f dB  %3llu det  lag %6.3f s\n",
                        (unsigned long long)uCpiIndex, dSignalS,
                        dDsiMs, dArdMs, dTotalMs,
                        oDsiStats.dSuppressionDb,
                        (unsigned long long)oArdStats.uDetections, dLagS);
            std::fflush(stdout);

            if (pTelCsv != nullptr) {
                std::fprintf(pTelCsv,
                    "%llu,%llu,%.6f,%.6f,%.6f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                    "%llu,%llu,%llu,%llu\n",
                    (unsigned long long)uCpiIndex,
                    (unsigned long long)uCpiStart,
                    dSignalS, dWallS, dLagS, dDsiMs, dArdMs, dTotalMs,
                    oDsiStats.dSuppressionDb, oArdStats.dPeakDb,
                    (unsigned long long)oArdStats.uDetections,
                    (unsigned long long)oArdStats.uDetectionsInBorder,
                    (unsigned long long)oIngestStats.uCpisOvertaken,
                    (unsigned long long)uDatagrams);
            }

            if (pDetCsv != nullptr) {
                for (uint64_t u = 0; u < oArdStats.uDetections; ++u) {
                    const Detection& d = vDet[static_cast<size_t>(u)];
                    std::fprintf(pDetCsv, "%llu,%.6f,%llu,%llu,%.4f,%.4f,%.4f,%.4f\n",
                                 (unsigned long long)uCpiIndex, dSignalS,
                                 (unsigned long long)d.uRangeBin,
                                 (unsigned long long)d.uDopplerBin,
                                 d.dRangeKm, d.dDopplerHz, d.dPowerDb, d.dSnrDb);
                }
            }

            if (bWantMap) {
                char achPath[512];
                std::snprintf(achPath, sizeof(achPath), "%s/ard_%05llu.bin",
                              sArdDir.c_str(),
                              (unsigned long long)uCpiIndex);
                ArdFileHeader oArdHeader;
                oArdHeader.uRangeBins         = oArdParams.uRangeBins;
                oArdHeader.uDopplerBins       = oArdParams.uDopplerBins;
                oArdHeader.uSampleRateHz      = oArdParams.uSampleRateHz;
                oArdHeader.uDopplerDecimation = oArdParams.uDopplerDecimation;
                oArdHeader.uCpiStartIndex     = uCpiStart;
                oArdHeader.uDetections        = oArdStats.uDetections;
                if (!writeArdFile(achPath, oArdHeader, vcMap.data(), vDet.data())) {
                    bProcessError = true;
                    break;
                }
            }

            ++uCpiIndex;
        }

        if (bProcessError)
            break;
    }

    if (pDetCsv != nullptr) std::fclose(pDetCsv);
    if (pTelCsv != nullptr) std::fclose(pTelCsv);

    // ---- report ------------------------------------------------------------
    const ChannelIngest::Stats& oStats = oIngest.stats();
    const double dElapsed = nowSeconds(oStart);

    std::sort(vdTotalMs.begin(), vdTotalMs.end());
    const double dMedianMs = vdTotalMs.empty() ? 0.0
                           : vdTotalMs[vdTotalMs.size() / 2];
    const double dBudgetMs = 1000.0 * double(oCfg.uHopSamples) /
                             double(oCfg.uSampleRateHz);

    std::printf("\n================ pbr_run summary ================\n");
    std::printf("elapsed                 %.2f s\n", dElapsed);
    std::printf("datagrams               %llu  (%llu parse failures)\n",
                (unsigned long long)uDatagrams,
                (unsigned long long)uParseFailures);
    std::printf("writes rejected (late)  %llu\n",
                (unsigned long long)oStats.uRejectedWrites);
    std::printf("packet-count gaps       %llu\n",
                (unsigned long long)oStats.uPacketCountGaps);
    std::printf("\n-- CPIs\n");
    std::printf("   processed              %llu\n", (unsigned long long)uCpiIndex);
    std::printf("   skipped, overtaken     %llu\n",
                (unsigned long long)oStats.uCpisOvertaken);
    std::printf("   skipped, poor coverage %llu\n",
                (unsigned long long)oStats.uCpisSkippedCoverage);
    std::printf("\n-- timing, against a %.0f ms budget per CPI\n", dBudgetMs);
    if (!vdTotalMs.empty()) {
        std::printf("   best / median / worst  %.1f / %.1f / %.1f ms\n",
                    vdTotalMs.front(), dMedianMs, vdTotalMs.back());
        std::printf("   worst as a fraction    %.3f of the budget\n",
                    dWorstTotalMs / dBudgetMs);
    }
    std::printf("   startup offset         %.3f s (before the first CPI; not a backlog)\n",
                dWallAtFirstCpi);
    std::printf("   lag growth since       %.3f s  <-- must stay near zero\n", dMaxLag);

    // A file replay runs as fast as it can, so lag is meaningless there.
    bool bPass = !bReadError && !bProcessError &&
                 uParseFailures == 0 &&
                 oStats.uRejectedWrites == 0 &&
                 oStats.uCpisOvertaken == 0 &&
                 uCpiIndex > 0;
    if (oOptions.bLive) {
        if (dWorstTotalMs > dBudgetMs) {
            std::printf("\n   WORST CPI EXCEEDED THE BUDGET\n");
            bPass = false;
        }
        // Measured from the first CPI, so this is drift, not offset. A chain
        // that keeps up holds this near zero however long it runs; one that
        // falls behind grows it without bound.
        const double dLagLimit = dBudgetMs / 1000.0;
        if (dMaxLag > dLagLimit) {
            std::printf("\n   LAG GREW BY %.3f s -- the chain is falling behind\n",
                        dMaxLag);
            bPass = false;
        }
    }

    std::printf("\n=================================================\n");
    // Only a live run can be said to have "kept up": a file replay processes
    // as fast as it can, so its lag is meaningless and saying otherwise would
    // put a reassuring word next to a number that does not support it.
    if (!bPass)
        std::printf("FAIL -- see the counters above.\n");
    else if (oOptions.bLive)
        std::printf("PASS -- the chain kept up in real time, every check clean.\n");
    else
        std::printf("PASS -- every check clean. This was a file replay, so the\n"
                    "        timing above is throughput, NOT real-time behaviour.\n");
    return bPass ? 0 : 1;
}
