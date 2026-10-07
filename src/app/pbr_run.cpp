// pbr_run -- the whole processing chain, live or from a recorded capture.
//
//   VITA 49.2 UDP  ->  circular buffers  ->  CPI extraction
//                  ->  DSI cancellation  ->  range-Doppler map  ->  CFAR
//                  ->  detections, telemetry, ARD frames streamed and/or written
//
// Usage:
//   ./pbr_run --live --group 239.192.0.1 --seconds 30 --out /data/results/x
//   ./pbr_run --file data/S0_clean/capture.pbrcap --config config/fers.conf
//   ./pbr_run --live --config config/fers.conf --capture data/S1/capture.pbrcap \
//             --dump-cpi data/S1/cpi_0000.bin
//
// Both sources run the same loop, so a recorded run exercises the live path.
//
// ---------------------------------------------------------------------------
// Why this is single-threaded
// ---------------------------------------------------------------------------
//
// Processing costs 25 to 230 ms per CPI on the Jetson depending on the
// clocks, and a CPI arrives once a second. While processing runs, the socket
// is not being drained, so datagrams accumulate: 1198 per second at 1400
// bytes is 1.68 MB/s, a fraction of the 8 MB socket buffer.
//
// A receive thread would buy nothing at those numbers and would introduce
// shared state between a producer and a consumer, which is where the bugs
// that only appear after an hour live. The telemetry reports the margin
// every run, so the day it stops being true is visible rather than inferred.
//
// ---------------------------------------------------------------------------
// Live parameters
// ---------------------------------------------------------------------------
//
// The config file seeds one in-memory parameter set at startup and is never
// written. Two things can replace that set while the run goes on, both only
// at a CPI boundary, both logged to DIR/changes.log with the CPI number:
//
//   * the viewer's Apply button, arriving as KEY = VALUE lines on the HTTP
//     control port (CONTROL_PORT), validated as a whole set with the same
//     rules as the file, refused with the reason otherwise;
//   * the source declaring a sample rate the config did not expect, which
//     re-expresses every window at the real rate (adaptPbrConfigToSampleRate)
//     and prints what moved.
//
// Either one rebuilds the stages that depend on what changed: the cancellers
// and map former always (a few hundred milliseconds, inside one hop), the
// ingest only for a rate change, since every buffered index depends on it.
// Keys that cannot change without a restart (ports, names, full scale,
// buffer length, the backend) are refused by name.
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
#include "pbr/ArdStream.hpp"
#include "pbr/CaptureFile.hpp"
#include "pbr/ChannelIngest.hpp"
#include "pbr/Config.hpp"
#include "pbr/CpiFile.hpp"
#include "pbr/DSICanceller.hpp"
#include "pbr/DetectionGrouper.hpp"
#include "pbr/HttpServer.hpp"
#include "pbr/Spectrum.hpp"
#include "pbr/UdpReceiver.hpp"
#include "pbr/Vita49Packet.hpp"
#include "pbr/types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace pbr;

namespace {

typedef std::chrono::steady_clock::time_point TimePoint;

const size_t   kDatagramCapacity = 2048;

// Bins in the before-processing spectrum streamed with each frame: 512
// int16 values fit one 1400-byte datagram, and 400 Hz per bin at 204800 Hz
// is finer than the viewer's panel can draw.
const uint32_t kSpectrumBins = 512;

// A stage returning false on a CPI produces a blank frame and the run goes
// on; this many in a row means the fault is not in the data, and the run
// stops so the error does not scroll past for hours.
const uint64_t kMaxConsecutiveFailures = 10;

// Everything that describes the SIGNAL PROCESSING lives in the config file, so
// a run is fully reproducible from a tracked file. What is left here is only
// what varies per invocation: where the samples come from, how long to run,
// and where to put the results.
struct Options {
    std::string sCapturePath;       // empty means live
    std::string sConfigPath = kDefaultConfigPath;
    std::string sOutDir;            // empty means console only
    std::string sMulticastGroup;    // empty means plain unicast on UDP_PORT
    std::string sSaveCapturePath;   // empty means no capture is recorded
    std::string sDumpCpiPath;       // empty means no CPI is written
    double      dSeconds    = 0.0;  // 0 means "until the source ends"
    bool        bLive       = false;
    bool        bSaveConfig = false;
};

void printUsage(const char* pProgram)
{
    std::printf("usage: %s (--live | --file CAPTURE) [options]\n", pProgram);
    std::printf("  --live           receive VITA 49 over UDP (uhd_to_vita49 or FERS)\n");
    std::printf("  --file FILE      replay a .pbrcap capture instead\n");
    std::printf("  --seconds N      stop after N seconds\n");
    std::printf("  --group ADDR     also join this multicast group (live only), the\n");
    std::printf("                   way uhd_to_vita49 feeds several clients at once\n");
    std::printf("  --out DIR        write telemetry.csv, detections.csv and changes.log\n");
    std::printf("                   here, and ard/ frames if the config has ARD_WRITE_FILES = 1\n");
    std::printf("  --save-config    with --out: copy the config file to DIR/config.conf\n");
    std::printf("  --config FILE    configuration (default %s)\n", kDefaultConfigPath);
    std::printf("  --capture FILE   also record every datagram to FILE (.pbrcap), for\n");
    std::printf("                   replaying the same stream later with --file\n");
    std::printf("  --dump-cpi FILE  also write the first CPI's input, both channels, to\n");
    std::printf("                   FILE: the fixed input of dsi_verify, dsi_bench and\n");
    std::printf("                   matlab/eca_golden.m\n");
    std::printf("\n");
    std::printf("Everything else -- algorithm, backend, full scale, CFAR and ECA\n");
    std::printf("settings, outputs -- comes from the config file, so a run is\n");
    std::printf("described entirely by a tracked file plus its changes.log.\n");
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
        else if (sArg == "--save-config")       o.bSaveConfig = true;
        else if (sArg == "--seconds" && bV)     o.dSeconds = std::atof(argv[++i]);
        else if (sArg == "--group" && bV)       o.sMulticastGroup = argv[++i];
        else if (sArg == "--capture" && bV)     o.sSaveCapturePath = argv[++i];
        else if (sArg == "--dump-cpi" && bV)    o.sDumpCpiPath = argv[++i];
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
    if (o.bSaveConfig && o.sOutDir.empty()) {
        std::fprintf(stderr, "--save-config needs --out DIR to save it in\n");
        return false;
    }
    return true;
}

// The energy in one channel, or false if a sample is not finite.
bool channelPower(const std::vector<ComplexF>& vc, double& dPowerOut)
{
    dPowerOut = 0.0;
    for (size_t u = 0; u < vc.size(); ++u) {
        const float fI = vc[u].real();
        const float fQ = vc[u].imag();
        if (!std::isfinite(fI) || !std::isfinite(fQ))
            return false;
        dPowerOut += double(fI) * fI + double(fQ) * fQ;
    }
    return true;
}

// The two conditions under which the chain cannot produce a meaningful map
// and would fail or produce NaNs if asked: a channel with no energy at all
// (the Gram matrix is singular and the relative diagonal loading is zero) and
// a non-finite sample. Anything else, however quiet, is processed as it is.
// The typical trigger is a front end with nothing connected.
bool inputIsProcessable(const std::vector<ComplexF>& vcRef,
                        const std::vector<ComplexF>& vcSur,
                        const char*& pReasonOut)
{
    double dRefPower = 0.0;
    double dSurPower = 0.0;
    if (!channelPower(vcRef, dRefPower)) {
        pReasonOut = "non-finite sample in the reference";
        return false;
    }
    if (!channelPower(vcSur, dSurPower)) {
        pReasonOut = "non-finite sample in the surveillance";
        return false;
    }
    if (dRefPower == 0.0) {
        pReasonOut = "reference channel is all zeros";
        return false;
    }
    if (dSurPower == 0.0) {
        pReasonOut = "surveillance channel is all zeros";
        return false;
    }
    return true;
}

// The last path component, with anything that would break a JSON string
// replaced, for the status datagram's run name.
std::string runNameFrom(const std::string& sOutDir)
{
    if (sOutDir.empty())
        return "live";
    std::string sName = sOutDir;
    while (!sName.empty() && sName[sName.size() - 1] == '/')
        sName.erase(sName.size() - 1);
    const size_t uSlash = sName.rfind('/');
    if (uSlash != std::string::npos)
        sName = sName.substr(uSlash + 1);
    for (size_t u = 0; u < sName.size(); ++u) {
        const unsigned char ch = static_cast<unsigned char>(sName[u]);
        if (ch < 0x20 || ch > 0x7E || ch == '"' || ch == '\\')
            sName[u] = '_';
    }
    return sName.empty() ? "live" : sName;
}

double nowSeconds(const TimePoint& oStart)
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - oStart).count();
}

double msBetween(const TimePoint& oFrom, const TimePoint& oTo)
{
    return std::chrono::duration<double, std::milli>(oTo - oFrom).count();
}

// ---------------------------------------------------------------------------
// The chain: every stage and buffer that depends on the parameter set, built
// as one unit from a config so it can be built again from another.
// ---------------------------------------------------------------------------
struct Chain {
    DSIParams oDsiParams;
    ArdParams oArdParams;
    std::unique_ptr<DSICanceller> pDsi;
    std::unique_ptr<ArdProcessor> pArd;
    bool bDeviceCoupled = false;

    std::vector<ComplexF>  vcRef;
    std::vector<ComplexF>  vcSur;
    std::vector<ComplexF>  vcCleaned;
    std::vector<ComplexF>  vcMap;
    std::vector<Detection> vDet;        // the CFAR's raw detections
    std::vector<uint8_t>   vFrame;      // one serialised ARD frame

    // The detections grouped into one plot per target; the frame carries
    // the plots, as Detection records, rather than the raw detections.
    DetectionGrouper       oGrouper;
    std::vector<Plot>      vPlots;
    std::vector<Detection> vPlotRecords;

    // The two channels' spectra before any processing, streamed with each
    // frame for the viewer's before-processing spectrogram.
    SpectrumEstimator      oSpectrum;
    std::vector<float>     vfSpectrum;  // kSpectrumBins dB values, reused per channel
};

// Build (or rebuild) the chain for a config, warm-up included. Releases the
// stages it held first: the Nano's 4 GB is shared with the GPU and two sets
// of stages would not fit side by side. On failure oChain must not be run.
bool buildChain(const PbrConfig& oCfg, Chain& oChain, double& dMsOut)
{
    const TimePoint tStart = std::chrono::steady_clock::now();

    oChain.pDsi.reset();
    oChain.pArd.reset();

    if (!dsiParamsFromConfig(oCfg, oChain.oDsiParams) ||
        !ardParamsFromConfig(oCfg, oChain.oArdParams))
        return false;

    oChain.pDsi = makeDSICanceller(oChain.oDsiParams.eAlgorithm, oChain.oDsiParams.eBackend);
    if (!oChain.pDsi || !oChain.pDsi->init(oChain.oDsiParams))
        return false;

    oChain.pArd = makeArdProcessor(oChain.oArdParams.eBackend);
    if (!oChain.pArd || !oChain.pArd->init(oChain.oArdParams))
        return false;

    // Chain the two stages through device memory when both are on the GPU, so
    // the cleaned samples never move through the host. Falls back cleanly
    // otherwise, and says which it did -- a GPU run must never quietly be a
    // host run.
    oChain.bDeviceCoupled = (oChain.pDsi->deviceOutput() != nullptr);
    oChain.pDsi->setHostOutputEnabled(!oChain.bDeviceCoupled);

    const uint64_t uCells = oChain.oArdParams.uRangeBins * oChain.oArdParams.uDopplerBins;
    oChain.vcRef.assign(static_cast<size_t>(oCfg.uCpiSamples), ComplexF(0.0f, 0.0f));
    oChain.vcSur.assign(static_cast<size_t>(oCfg.uCpiSamples), ComplexF(0.0f, 0.0f));
    if (!oChain.oSpectrum.init(kSpectrumBins))
        return false;
    oChain.vfSpectrum.assign(kSpectrumBins, 0.0f);
    oChain.vcCleaned.assign(oChain.bDeviceCoupled ? 0 : static_cast<size_t>(oCfg.uCpiSamples),
                            ComplexF(0.0f, 0.0f));
    oChain.vcMap.assign(static_cast<size_t>(uCells), ComplexF(0.0f, 0.0f));
    oChain.vDet.assign(static_cast<size_t>(kMaxArdDetections), Detection());
    if (!oChain.oGrouper.init(oChain.oArdParams.uRangeBins, oChain.oArdParams.uDopplerBins))
        return false;
    oChain.vPlots.assign(static_cast<size_t>(kMaxArdDetections), Plot());
    oChain.vPlotRecords.assign(static_cast<size_t>(kMaxArdDetections), Detection());

    ArdFileHeader oFrameSizing;
    oFrameSizing.uRangeBins   = oChain.oArdParams.uRangeBins;
    oFrameSizing.uDopplerBins = oChain.oArdParams.uDopplerBins;
    oFrameSizing.uDetections  = kMaxArdDetections;
    oChain.vFrame.assign(ardFrameBytes(oFrameSizing), 0);

    // Warm up. The first CUDA call pays for context creation, and cuFFT
    // defers some plan work to its first execution. Without this the very
    // first CPI blows its budget and the first telemetry row looks like a
    // failure.
    //
    // The warm-up signal must look like signal. A constant reference makes
    // every delayed column of the clutter matrix identical, so the Gram is
    // singular to FP32 and ECA-m's whole-CPI Cholesky fails before any real
    // CPI arrives (2026-10-02; the batched algorithms survived only because
    // their windows are short). A fixed pseudo-random sequence (a plain LCG,
    // the same every run) is well conditioned; the output is discarded.
    uint32_t uLcg = 12345u;
    for (size_t n = 0; n < oChain.vcRef.size(); ++n) {
        uLcg = uLcg * 1664525u + 1013904223u;
        const float fI = static_cast<float>(uLcg >> 8) / 16777216.0f - 0.5f;
        uLcg = uLcg * 1664525u + 1013904223u;
        const float fQ = static_cast<float>(uLcg >> 8) / 16777216.0f - 0.5f;
        oChain.vcRef[n] = ComplexF(fI, fQ);
        oChain.vcSur[n] = ComplexF(0.5f * fI, 0.5f * fQ);
    }
    DSIStats oWarmDsi;
    ArdStats oWarmArd;
    bool bWarm = oChain.pDsi->cancel(oChain.vcRef.data(), oChain.vcSur.data(),
                                     oChain.bDeviceCoupled ? nullptr : oChain.vcCleaned.data(),
                                     oWarmDsi);
    if (bWarm) {
        bWarm = oChain.bDeviceCoupled
              ? oChain.pArd->processDevice(oChain.pDsi->deviceReference(),
                                           oChain.pDsi->deviceOutput(),
                                           oChain.vcMap.data(), oChain.vDet.data(),
                                           kMaxArdDetections, oWarmArd)
              : oChain.pArd->process(oChain.vcRef.data(), oChain.vcCleaned.data(),
                                     oChain.vcMap.data(), oChain.vDet.data(),
                                     kMaxArdDetections, oWarmArd);
    }
    if (!bWarm) {
        std::fprintf(stderr, "pbr_run: the warm-up pass failed\n");
        return false;
    }
    // A stateful canceller would otherwise start the first real CPI from the
    // weights it fitted to the warm-up signal.
    if (!oChain.pDsi->resetState())
        return false;

    dMsOut = msBetween(tStart, std::chrono::steady_clock::now());
    return true;
}

bool buildIngest(const PbrConfig& oCfg, std::unique_ptr<ChannelIngest>& pIngest)
{
    ChannelIngestConfig oIngestConfig;
    oIngestConfig.uSampleRateHz     = oCfg.uSampleRateHz;
    oIngestConfig.dFullScale        = oCfg.dFullScale;
    oIngestConfig.uBufferCapacity   = oCfg.uBufferCapacity;
    oIngestConfig.sReferenceName    = oCfg.sReferenceName;
    oIngestConfig.sSurveillanceName = oCfg.sSurveillanceName;
    oIngestConfig.uCpiSamples       = oCfg.uCpiSamples;
    oIngestConfig.uHopSamples       = oCfg.uHopSamples;
    oIngestConfig.dMinCpiCoverage   = oCfg.dMinCpiCoverage;

    pIngest.reset();
    pIngest.reset(new ChannelIngest(oIngestConfig));
    if (!pIngest->isValid()) {
        std::fprintf(stderr, "pbr_run: ChannelIngest failed to initialise\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Live changes: the control port.
// ---------------------------------------------------------------------------

// What the control port's handler needs to see and set. The handler runs in
// the main loop's thread (servePending is polled), so no locking.
struct Control {
    const PbrConfig* pCurrent = nullptr;
    const PbrConfig* pInitial = nullptr;   // the file's set, as loaded at startup
    PbrConfig        oPending;
    bool             bPending = false;
    std::string      sPendingDiff;
    uint64_t         uCpiIndex = 0;
};

void setText(HttpResponse& oResponse, const std::string& sText)
{
    oResponse.vBody.assign(sText.begin(), sText.end());
}

void refuse(HttpResponse& oResponse, const std::string& sText)
{
    oResponse.iStatus = 400;
    setText(oResponse, sText);
}

// Queue oCandidate for the next CPI boundary, or say there is nothing to do.
void queueChange(Control& oCtl, const PbrConfig& oCandidate, const char* pAccepted,
                 const char* pNoChange, HttpResponse& oResponse)
{
    const std::string sDiff = pbrConfigDiff(*oCtl.pCurrent, oCandidate);
    if (sDiff.empty()) {
        oCtl.bPending = false;
        setText(oResponse, pNoChange);
        return;
    }
    oCtl.oPending     = oCandidate;
    oCtl.bPending     = true;
    oCtl.sPendingDiff = sDiff;
    setText(oResponse, std::string(pAccepted) + "; applies at the next CPI boundary (after CPI " +
                       std::to_string(oCtl.uCpiIndex) + "):\n" + sDiff);
}

bool controlHandler(const std::string& sMethod, const std::string& sPath,
                    const std::string& sBody, void* pContext, HttpResponse& oResponse)
{
    Control& oCtl = *static_cast<Control*>(pContext);

    if (sPath == "/api/config" && sMethod == "GET") {
        setText(oResponse, pbrConfigToText(*oCtl.pCurrent));
        return true;
    }

    if (sPath == "/api/reset" && sMethod == "POST") {
        // Back to the config file's values. If the source's sample rate made
        // the chain adapt, the file's windows are re-adapted the same way, so
        // the reset lands on what the file would have meant at this rate.
        PbrConfig   oCandidate = *oCtl.pInitial;
        std::string sReport;
        if (oCandidate.uSampleRateHz != oCtl.pCurrent->uSampleRateHz &&
            !adaptPbrConfigToSampleRate(*oCtl.pInitial, oCtl.pCurrent->uSampleRateHz,
                                        oCandidate, sReport)) {
            refuse(oResponse, "refused: the file's settings cannot be adapted to the "
                              "source's rate:\n" + sReport);
            return true;
        }
        queueChange(oCtl, oCandidate, "reset accepted",
                    "already at the config file's values\n", oResponse);
        return true;
    }

    if (sPath == "/api/apply" && sMethod == "POST") {
        const std::string sKey = restartKeyChanged(*oCtl.pCurrent, sBody);
        if (!sKey.empty()) {
            refuse(oResponse, "refused: " + sKey +
                              " cannot change while running; restart with it in the config\n");
            return true;
        }
        PbrConfig   oCandidate;
        std::string sWhy;
        if (!applyPbrConfigLines(*oCtl.pCurrent, sBody, oCandidate, sWhy)) {
            refuse(oResponse, "refused:\n" + sWhy);
            return true;
        }
        queueChange(oCtl, oCandidate, "accepted",
                    "no change: every value is already what it is\n", oResponse);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Outputs.
// ---------------------------------------------------------------------------

struct Outputs {
    bool        bWriteFrames  = false;   // ARD frames to DIR/ard/
    bool        bStreamFrames = false;   // ARD frames over UDP
    std::string sArdDir;
    std::FILE*  pDetCsv    = nullptr;
    std::FILE*  pPlotCsv   = nullptr;
    std::FILE*  pTelCsv    = nullptr;
    std::FILE*  pChangeLog = nullptr;
    ArdStreamSender oSender;
    std::string sRunName;
};

// Decide what is written and streamed, and make the output directory. The
// CSVs and the change log always go to DIR; ARD frames only if the config
// says to write them (1.5 MB per CPI), and DIR/ard/ is not even created
// otherwise. The output flags are fixed for the run.
bool makeOutputDir(const Options& oOptions, const PbrConfig& oCfg, Outputs& oOut)
{
    oOut.bWriteFrames  = oCfg.bArdWriteFiles && !oOptions.sOutDir.empty();
    oOut.bStreamFrames = oCfg.bArdStream;
    oOut.sRunName      = runNameFrom(oOptions.sOutDir);

    if (!oOptions.sOutDir.empty()) {
        if (oOut.bWriteFrames)
            oOut.sArdDir = oOptions.sOutDir + "/ard";
        // mkdir -p, so a run never fails at the first CPI for want of a
        // directory it could have made at startup.
        const std::string sCmd = "mkdir -p '" +
                                 (oOut.bWriteFrames ? oOut.sArdDir : oOptions.sOutDir) + "'";
        if (std::system(sCmd.c_str()) != 0) {
            std::fprintf(stderr, "pbr_run: cannot create %s\n", oOptions.sOutDir.c_str());
            return false;
        }
    }
    if (oCfg.bArdWriteFiles && oOptions.sOutDir.empty())
        std::fprintf(stderr, "pbr_run: ARD_WRITE_FILES is on but no --out was given; "
                             "no frames will be written\n");
    return true;
}

std::FILE* openForWriting(const std::string& sPath)
{
    std::FILE* pFile = std::fopen(sPath.c_str(), "w");
    if (pFile == nullptr)
        std::fprintf(stderr, "pbr_run: cannot open %s\n", sPath.c_str());
    return pFile;
}

bool copyFile(const std::string& sFrom, const std::string& sTo)
{
    std::FILE* pIn = std::fopen(sFrom.c_str(), "rb");
    if (pIn == nullptr) {
        std::fprintf(stderr, "pbr_run: cannot read %s\n", sFrom.c_str());
        return false;
    }
    std::FILE* pOut = openForWriting(sTo);
    bool bOk = (pOut != nullptr);
    char achChunk[4096];
    while (bOk) {
        const size_t uGot = std::fread(achChunk, 1, sizeof(achChunk), pIn);
        if (uGot == 0)
            break;
        bOk = std::fwrite(achChunk, 1, uGot, pOut) == uGot;
    }
    std::fclose(pIn);
    if (pOut != nullptr && std::fclose(pOut) != 0)
        bOk = false;
    if (!bOk)
        std::fprintf(stderr, "pbr_run: could not copy %s to %s\n", sFrom.c_str(), sTo.c_str());
    return bOk;
}

// The three files in DIR, each with its header line, and the config copy if
// it was asked for. Nothing to do without --out.
bool openOutputFiles(const Options& oOptions, Outputs& oOut)
{
    if (oOptions.sOutDir.empty())
        return true;

    oOut.pDetCsv = openForWriting(oOptions.sOutDir + "/detections.csv");
    if (oOut.pDetCsv == nullptr)
        return false;
    std::fprintf(oOut.pDetCsv, "cpi,signalTimeS,rangeBin,dopplerBin,"
                               "rangeKm,dopplerHz,powerDb,snrDb\n");

    oOut.pPlotCsv = openForWriting(oOptions.sOutDir + "/plots.csv");
    if (oOut.pPlotCsv == nullptr)
        return false;
    std::fprintf(oOut.pPlotCsv, "cpi,signalTimeS,rangeKm,dopplerHz,peakRangeBin,"
                                "peakDopplerBin,peakPowerDb,peakSnrDb,cells\n");

    oOut.pTelCsv = openForWriting(oOptions.sOutDir + "/telemetry.csv");
    if (oOut.pTelCsv == nullptr)
        return false;
    std::fprintf(oOut.pTelCsv, "cpi,cpiStartIndex,signalTimeS,wallTimeS,lagS,"
                               "dsiMs,ardMs,totalMs,suppressionDb,peakDb,"
                               "detections,borderDetections,skipped,datagrams,"
                               "waitMs,printMs,detCsvMs,ardWriteMs,status,plots\n");

    if (oOptions.bSaveConfig &&
        !copyFile(oOptions.sConfigPath, oOptions.sOutDir + "/config.conf"))
        return false;

    oOut.pChangeLog = openForWriting(oOptions.sOutDir + "/changes.log");
    if (oOut.pChangeLog == nullptr)
        return false;
    std::fprintf(oOut.pChangeLog, "# live parameter changes; the starting set is %s%s\n",
                 oOptions.sConfigPath.c_str(),
                 oOptions.bSaveConfig ? ", copied to config.conf beside this file" : "");
    std::fflush(oOut.pChangeLog);
    return true;
}

void closeOutputFiles(Outputs& oOut)
{
    if (oOut.pDetCsv != nullptr)    std::fclose(oOut.pDetCsv);
    if (oOut.pPlotCsv != nullptr)   std::fclose(oOut.pPlotCsv);
    if (oOut.pTelCsv != nullptr)    std::fclose(oOut.pTelCsv);
    if (oOut.pChangeLog != nullptr) std::fclose(oOut.pChangeLog);
    oOut.pDetCsv = oOut.pPlotCsv = oOut.pTelCsv = oOut.pChangeLog = nullptr;
}

// Append to DIR/changes.log (and echo to the console).
void logChange(std::FILE* pLog, uint64_t uCpiIndex, const char* pWhat, const std::string& sDetail)
{
    std::printf("\n-- %s at CPI %llu\n%s", pWhat, (unsigned long long)uCpiIndex, sDetail.c_str());
    if (pLog != nullptr) {
        std::fprintf(pLog, "CPI %llu  %s\n%s", (unsigned long long)uCpiIndex, pWhat, sDetail.c_str());
        std::fflush(pLog);
    }
}

std::string rebuiltIn(double dMs)
{
    char achMs[64];
    std::snprintf(achMs, sizeof(achMs), "rebuilt in %.0f ms\n", dMs);
    return achMs;
}

// ---------------------------------------------------------------------------
// The run: everything the loop reads and updates, in one place.
// ---------------------------------------------------------------------------
struct Run {
    Options   oOptions;
    PbrConfig oCfg;
    Chain     oChain;
    std::unique_ptr<ChannelIngest> pIngest;
    Outputs   oOut;
    Control   oControl;
    TimePoint oStart;

    uint64_t uCpiIndex      = 0;
    uint64_t uDatagrams     = 0;
    uint64_t uParseFailures = 0;
    uint64_t uFirstCpiStart = 0;
    // Two different instants, and conflating them was a bug worth naming.
    //
    // dStartupOffset is when the FIRST CPI became available -- the wait for
    // the transmitter plus one full second of signal. It is reported as a
    // fixed offset.
    //
    // dWallAtFirstCpi is when that first CPI FINISHED being processed, and it
    // is what the lag is measured from. Zeroing the lag at the earlier instant
    // instead meant every CPI's lag carried one CPI's processing time as a
    // permanent offset, so a healthy chain read 0.25 s rather than 0 and the
    // slower algorithms sat closest to the failure threshold for no reason.
    double   dStartupOffset  = 0.0;
    double   dWallAtFirstCpi = 0.0;
    bool     bHaveFirst      = false;
    double   dMaxLag         = 0.0;
    std::vector<double> vdTotalMs;
    bool     bCpiDumped      = false;
    double   dLastUnresolvedNote = 0.0;   // first reminder five seconds in, not at the first datagram
    uint64_t uNullCpis       = 0;   // blank frames: input not processable
    uint64_t uFailedCpis     = 0;   // blank frames: a stage returned false
    uint64_t uConsecutiveFailures = 0;
    uint64_t uLiveChanges    = 0;
    uint64_t uRateAdaptions  = 0;
    uint64_t uRateRefusedHz  = 0;   // a source rate that could not be adapted to, reported once

    // When the previous CPI's output finished, so the next CPI's waitMs can
    // say how long the loop spent draining the socket and waiting for data.
    TimePoint oLastOutputEnd;
    bool      bHaveLastOutput = false;
};

// Apply a change the control port accepted, at the boundary. False only if
// neither the new set nor the previous one could be built.
bool applyPendingChange(Run& oRun)
{
    Control& oCtl = oRun.oControl;
    oCtl.bPending = false;
    const PbrConfig oNewCfg = oCtl.oPending;
    const bool bCpiChanged = (oNewCfg.uCpiSamples != oRun.oCfg.uCpiSamples ||
                              oNewCfg.uHopSamples != oRun.oCfg.uHopSamples);
    double dMs = 0.0;
    // buildChain releases the old stages first, so the GPU never holds two
    // sets; if the new set fails to build, the old one is built again from
    // the config that is still current.
    if (!buildChain(oNewCfg, oRun.oChain, dMs)) {
        std::fprintf(stderr, "pbr_run: the accepted change could not be built; "
                             "rebuilding the previous set\n");
        return buildChain(oRun.oCfg, oRun.oChain, dMs);
    }
    if (bCpiChanged && !oRun.pIngest->setCpi(oNewCfg.uCpiSamples, oNewCfg.uHopSamples)) {
        std::fprintf(stderr, "pbr_run: the ingest refused the new CPI; keeping the previous set\n");
        return buildChain(oRun.oCfg, oRun.oChain, dMs);
    }
    oRun.oCfg = oNewCfg;
    ++oRun.uLiveChanges;
    logChange(oRun.oOut.pChangeLog, oRun.uCpiIndex, "live change applied",
              oCtl.sPendingDiff + rebuiltIn(dMs));
    return true;
}

// A context packet declared a rate the config did not expect. Adapt every
// window to it (the author's rule: pick the nearest settings that work and
// say so), rebuild the chain AND the ingest, since every buffered index
// depends on the rate, and carry on. A rate that cannot be adapted to is
// reported once and nothing is processed. False only if the previous set
// could not be restored after a failed rebuild.
bool followSourceRate(Run& oRun)
{
    const double   dSource = oRun.pIngest->sourceSampleRateHz();
    const uint64_t uSource = static_cast<uint64_t>(std::llround(dSource));
    if (dSource <= 0.0 || uSource == oRun.oCfg.uSampleRateHz || uSource == oRun.uRateRefusedHz)
        return true;

    PbrConfig   oNewCfg;
    std::string sReport;
    double      dMs = 0.0;
    const bool bInteger = std::fabs(dSource - static_cast<double>(uSource)) < 1e-6;
    if (!bInteger) {
        sReport = "source sample rate " + std::to_string(dSource) +
                  " Hz is not a whole number of hertz; the chain cannot index it\n";
    }
    const bool bAdapted = bInteger &&
                          adaptPbrConfigToSampleRate(oRun.oCfg, uSource, oNewCfg, sReport);
    const bool bChainOk  = bAdapted && buildChain(oNewCfg, oRun.oChain, dMs);
    const bool bIngestOk = bChainOk && buildIngest(oNewCfg, oRun.pIngest);
    if (bIngestOk) {
        oRun.oCfg = oNewCfg;
        ++oRun.uRateAdaptions;
        oRun.bHaveFirst = false;   // a new timeline: lag and signal time start again
        logChange(oRun.oOut.pChangeLog, oRun.uCpiIndex, "sample rate adapted",
                  sReport + rebuiltIn(dMs));
        return true;
    }

    oRun.uRateRefusedHz = uSource;
    std::fprintf(stderr, "pbr_run: %s         nothing will be processed until the "
                         "source or the config changes\n", sReport.c_str());
    // A build that was attempted and failed may have left the chain or the
    // ingest half made for the new rate: put back the current set's.
    if (bAdapted && !buildChain(oRun.oCfg, oRun.oChain, dMs))
        return false;
    if (bChainOk && !buildIngest(oRun.oCfg, oRun.pIngest))
        return false;
    return true;
}

// Packets are arriving but the streams have not been matched to the
// configured receiver names: nothing will ever be processed, and without
// this line the only sign is a summary at the end. A reminder, not a
// deadline; it repeats until the names agree.
void noteUnresolvedRoles(Run& oRun)
{
    if (oRun.pIngest->rolesResolved())
        return;
    const double dNow = nowSeconds(oRun.oStart);
    if (dNow - oRun.dLastUnresolvedNote < 5.0)
        return;
    oRun.dLastUnresolvedNote = dNow;
    std::fprintf(stderr,
        "pbr_run: %llu datagrams received but the channels are not identified yet.\n"
        "         seen: %s\n"
        "         config expects \"%s\" (reference) and \"%s\" (surveillance)\n",
        (unsigned long long)oRun.uDatagrams, oRun.pIngest->streamSummary().c_str(),
        oRun.oCfg.sReferenceName.c_str(), oRun.oCfg.sSurveillanceName.c_str());
}

// ---------------------------------------------------------------------------
// One CPI: process it, then publish the results.
// ---------------------------------------------------------------------------

struct CpiResult {
    uint64_t    uCpiStart = 0;
    uint64_t    uStatus   = kArdStatusOk;
    const char* pReason   = "";
    DSIStats    oDsiStats;
    ArdStats    oArdStats;
    uint64_t    uPlots    = 0;
    double      dDsiMs    = 0.0;
    double      dArdMs    = 0.0;
    double      dWaitMs   = 0.0;
    double      dSignalS  = 0.0;
    double      dWallS    = 0.0;
    double      dLagS     = 0.0;
};

// Cancel, then map and detect. A CPI the chain cannot process is not a crash
// and not a stop: it goes out as a BLANK frame carrying a status word, so a
// dead front end shows as "receiving, nothing to process" downstream. The
// input guard runs first, so the solver is never handed a window that would
// break it.
void processCpi(Chain& oChain, ComplexF* pMap, CpiResult& oResult)
{
    const TimePoint tStart = std::chrono::steady_clock::now();

    bool bCancelOk = false;
    if (!inputIsProcessable(oChain.vcRef, oChain.vcSur, oResult.pReason))
        oResult.uStatus = kArdStatusNullInput;
    else
        bCancelOk = oChain.pDsi->cancel(oChain.vcRef.data(), oChain.vcSur.data(),
                                        oChain.bDeviceCoupled ? nullptr : oChain.vcCleaned.data(),
                                        oResult.oDsiStats);

    const TimePoint tCancelled = std::chrono::steady_clock::now();

    if (oResult.uStatus == kArdStatusOk && !bCancelOk) {
        oResult.uStatus = kArdStatusFailed;
        oResult.pReason = "cancellation returned an error";
    }

    if (oResult.uStatus == kArdStatusOk) {
        const bool bArdOk = oChain.bDeviceCoupled
            ? oChain.pArd->processDevice(oChain.pDsi->deviceReference(),
                                         oChain.pDsi->deviceOutput(),
                                         pMap, oChain.vDet.data(), kMaxArdDetections,
                                         oResult.oArdStats)
            : oChain.pArd->process(oChain.vcRef.data(), oChain.vcCleaned.data(), pMap,
                                   oChain.vDet.data(), kMaxArdDetections, oResult.oArdStats);
        if (!bArdOk) {
            oResult.uStatus = kArdStatusFailed;
            oResult.pReason = "map formation or CFAR returned an error";
        }
    }
    const TimePoint tMapped = std::chrono::steady_clock::now();

    if (oResult.uStatus != kArdStatusOk) {
        // The blank frame: zero map, no detections, zero statistics.
        if (pMap != nullptr)
            std::fill(oChain.vcMap.begin(), oChain.vcMap.end(), ComplexF(0.0f, 0.0f));
        oResult.oDsiStats = DSIStats();
        oResult.oArdStats = ArdStats();
    } else {
        // One plot per group of touching detections. Microseconds, and
        // deliberately outside ardMs, which times the processor alone. No
        // plot can be dropped: vPlots holds one per detection.
        uint64_t uDropped = 0;
        oResult.uPlots = oChain.oGrouper.group(oChain.vDet.data(), oResult.oArdStats.uDetections,
                                               oChain.vPlots.data(), oChain.vPlots.size(),
                                               uDropped);
        for (uint64_t u = 0; u < oResult.uPlots; ++u)
            oChain.vPlotRecords[static_cast<size_t>(u)] =
                plotAsDetection(oChain.vPlots[static_cast<size_t>(u)]);
    }

    oResult.dDsiMs = msBetween(tStart, tCancelled);
    oResult.dArdMs = msBetween(tCancelled, tMapped);
}

// Count a blank frame. False when stage failures have run on long enough
// that the fault is not in the data.
bool countStatus(Run& oRun, uint64_t uStatus)
{
    if (uStatus == kArdStatusOk) {
        oRun.uConsecutiveFailures = 0;
    } else if (uStatus == kArdStatusNullInput) {
        ++oRun.uNullCpis;
        oRun.uConsecutiveFailures = 0;
    } else {
        ++oRun.uFailedCpis;
        ++oRun.uConsecutiveFailures;
        if (oRun.uConsecutiveFailures >= kMaxConsecutiveFailures) {
            std::fprintf(stderr, "pbr_run: %llu consecutive processing "
                                 "failures; this is not the data. Stopping.\n",
                         (unsigned long long)oRun.uConsecutiveFailures);
            return false;
        }
    }
    return true;
}

// Signal time is where this CPI sits in the stream; wall time is how long we
// have been running. What matters is not the gap between them but whether
// that gap GROWS.
//
// Both clocks are zeroed at the same instant: the moment the first CPI
// finished processing. Anything before it -- waiting for the transmitter to
// start, the socket filling, the first window completing -- is a fixed
// startup offset, not a backlog, and measuring from program start would bake
// it in permanently and make a chain that is keeping up perfectly look like
// one that is seconds behind. A chain that keeps up then holds the lag at
// zero, whatever the algorithm costs per CPI.
void updateTimeline(Run& oRun, CpiResult& oResult)
{
    oResult.dSignalS = double(oResult.uCpiStart - oRun.uFirstCpiStart) /
                       double(oRun.oCfg.uSampleRateHz);
    oResult.dWallS   = nowSeconds(oRun.oStart);
    if (!oRun.bHaveFirst) {
        oRun.bHaveFirst      = true;
        oRun.dWallAtFirstCpi = oResult.dWallS;
    }
    oResult.dLagS = (oResult.dWallS - oRun.dWallAtFirstCpi) - oResult.dSignalS;
    if (oResult.dLagS > oRun.dMaxLag)
        oRun.dMaxLag = oResult.dLagS;
    oRun.vdTotalMs.push_back(oResult.dDsiMs + oResult.dArdMs);
}

void printCpiLine(uint64_t uCpiIndex, const CpiResult& oResult)
{
    if (oResult.uStatus == kArdStatusOk) {
        std::printf("CPI %4llu  t=%7.3f s  dsi %6.1f  ard %5.1f  "
                    "total %6.1f ms  %5.2f dB  %3llu det  %3llu plots  lag %6.3f s\n",
                    (unsigned long long)uCpiIndex, oResult.dSignalS,
                    oResult.dDsiMs, oResult.dArdMs, oResult.dDsiMs + oResult.dArdMs,
                    oResult.oDsiStats.dSuppressionDb,
                    (unsigned long long)oResult.oArdStats.uDetections,
                    (unsigned long long)oResult.uPlots, oResult.dLagS);
    } else {
        std::printf("CPI %4llu  t=%7.3f s  %s: %s -- blank frame  lag %6.3f s\n",
                    (unsigned long long)uCpiIndex, oResult.dSignalS,
                    (oResult.uStatus == kArdStatusNullInput) ? "NULL INPUT" : "FAILED",
                    oResult.pReason, oResult.dLagS);
    }
    std::fflush(stdout);
}

// The raw CFAR detections, for checking the detector, and the plots made
// from them.
void writeDetections(const Outputs& oOut, uint64_t uCpiIndex, const Chain& oChain,
                     const CpiResult& oResult)
{
    if (oOut.pDetCsv != nullptr) {
        for (uint64_t u = 0; u < oResult.oArdStats.uDetections; ++u) {
            const Detection& oDet = oChain.vDet[static_cast<size_t>(u)];
            std::fprintf(oOut.pDetCsv, "%llu,%.6f,%llu,%llu,%.4f,%.4f,%.4f,%.4f\n",
                         (unsigned long long)uCpiIndex, oResult.dSignalS,
                         (unsigned long long)oDet.uRangeBin,
                         (unsigned long long)oDet.uDopplerBin,
                         oDet.dRangeKm, oDet.dDopplerHz, oDet.dPowerDb, oDet.dSnrDb);
        }
    }
    if (oOut.pPlotCsv != nullptr) {
        for (uint64_t u = 0; u < oResult.uPlots; ++u) {
            const Plot& oPlot = oChain.vPlots[static_cast<size_t>(u)];
            std::fprintf(oOut.pPlotCsv, "%llu,%.6f,%.4f,%.4f,%llu,%llu,%.4f,%.4f,%llu\n",
                         (unsigned long long)uCpiIndex, oResult.dSignalS,
                         oPlot.dRangeKm, oPlot.dDopplerHz,
                         (unsigned long long)oPlot.uPeakRangeBin,
                         (unsigned long long)oPlot.uPeakDopplerBin,
                         oPlot.dPeakPowerDb, oPlot.dPeakSnrDb,
                         (unsigned long long)oPlot.uCells);
        }
    }
}

// The frame, then each channel's spectrum as it was handed to the canceller
// (blank frames included: a dead input is exactly what that panel is for),
// then the status: what the viewer's header strip shows, with the telemetry
// column names as keys, plus the live config values and where to send
// changes.
void streamFrame(Run& oRun, size_t uFrameBytes, const CpiResult& oResult)
{
    Chain&           oChain  = oRun.oChain;
    const PbrConfig& oCfg    = oRun.oCfg;
    ArdStreamSender& oSender = oRun.oOut.oSender;
    const uint32_t uSequence = static_cast<uint32_t>(oRun.uCpiIndex);

    oSender.sendFrame(oChain.vFrame.data(), uFrameBytes, uSequence);

    const ComplexF* apChannel[2] = { oChain.vcRef.data(), oChain.vcSur.data() };
    for (uint32_t c = 0; c < 2; ++c) {
        if (oChain.oSpectrum.compute(apChannel[c], oCfg.uCpiSamples, oChain.vfSpectrum.data()))
            oSender.sendSpectrum(oChain.vfSpectrum.data(), kSpectrumBins, c,
                                 static_cast<uint32_t>(oCfg.uSampleRateHz), uSequence);
    }

    char achStatus[1200];
    const int iLen = std::snprintf(
        achStatus, sizeof(achStatus),
        "{\"runName\":\"%s\",\"cpi\":%llu,\"status\":%llu,"
        "\"reason\":\"%s\",\"signalTimeS\":%.3f,\"lagS\":%.3f,"
        "\"dsiMs\":%.1f,\"ardMs\":%.1f,\"totalMs\":%.1f,"
        "\"suppressionDb\":%.2f,\"peakDb\":%.2f,"
        "\"detections\":%llu,\"plots\":%llu,\"borderDetections\":%llu,\"skipped\":%llu,"
        "\"controlPort\":%u,\"liveChanges\":%llu,"
        "\"config\":{\"DSI_ALGORITHM\":\"%s\",\"DSI_BACKEND\":\"%s\","
        "\"ARD_ALGORITHM\":\"%s\",\"CFAR_PFA\":\"%g\",\"CPI_LENGTH_s\":\"%g\","
        "\"SAMPLE_RATE_Hz\":\"%llu\"}}",
        oRun.oOut.sRunName.c_str(), (unsigned long long)oRun.uCpiIndex,
        (unsigned long long)oResult.uStatus, oResult.pReason, oResult.dSignalS, oResult.dLagS,
        oResult.dDsiMs, oResult.dArdMs, oResult.dDsiMs + oResult.dArdMs,
        oResult.oDsiStats.dSuppressionDb, oResult.oArdStats.dPeakDb,
        (unsigned long long)oResult.oArdStats.uDetections,
        (unsigned long long)oResult.uPlots,
        (unsigned long long)oResult.oArdStats.uDetectionsInBorder,
        (unsigned long long)oRun.pIngest->stats().uCpisOvertaken,
        static_cast<unsigned>(oCfg.uControlPort),
        (unsigned long long)oRun.uLiveChanges,
        oCfg.sDsiAlgorithm.c_str(), oCfg.sDsiBackend.c_str(),
        oCfg.sArdAlgorithm.c_str(), oCfg.dCfarPfa, oCfg.dCpiSeconds,
        (unsigned long long)oCfg.uSampleRateHz);
    if (iLen > 0 && static_cast<size_t>(iLen) < sizeof(achStatus))
        oSender.sendStatus(achStatus, uSequence);
}

// Serialise the map and the plots once, then write it and/or stream it.
bool publishFrame(Run& oRun, const CpiResult& oResult)
{
    const Chain& oChain = oRun.oChain;
    ArdFileHeader oHeader;
    oHeader.uRangeBins         = oChain.oArdParams.uRangeBins;
    oHeader.uDopplerBins       = oChain.oArdParams.uDopplerBins;
    oHeader.uSampleRateHz      = oChain.oArdParams.uSampleRateHz;
    oHeader.uDopplerDecimation = oChain.oArdParams.uDopplerDecimation;
    oHeader.uCpiStartIndex     = oResult.uCpiStart;
    oHeader.uDetections        = oResult.uPlots;
    oHeader.uStatus            = oResult.uStatus;

    size_t uFrameBytes = 0;
    if (!serialiseArdFrame(oHeader, oChain.vcMap.data(), oChain.vPlotRecords.data(),
                           oRun.oChain.vFrame.data(), oChain.vFrame.size(), uFrameBytes))
        return false;

    if (oRun.oOut.bWriteFrames) {
        char achPath[512];
        std::snprintf(achPath, sizeof(achPath), "%s/ard_%05llu.bin",
                      oRun.oOut.sArdDir.c_str(), (unsigned long long)oRun.uCpiIndex);
        if (!writeArdFrameBytes(achPath, oChain.vFrame.data(), uFrameBytes))
            return false;
    }
    if (oRun.oOut.bStreamFrames)
        streamFrame(oRun, uFrameBytes, oResult);
    return true;
}

void writeTelemetry(const Run& oRun, const CpiResult& oResult,
                    double dPrintMs, double dDetCsvMs, double dArdWriteMs)
{
    if (oRun.oOut.pTelCsv == nullptr)
        return;
    std::fprintf(oRun.oOut.pTelCsv,
        "%llu,%llu,%.6f,%.6f,%.6f,%.4f,%.4f,%.4f,%.4f,%.4f,"
        "%llu,%llu,%llu,%llu,%.4f,%.4f,%.4f,%.4f,%llu,%llu\n",
        (unsigned long long)oRun.uCpiIndex,
        (unsigned long long)oResult.uCpiStart,
        oResult.dSignalS, oResult.dWallS, oResult.dLagS,
        oResult.dDsiMs, oResult.dArdMs, oResult.dDsiMs + oResult.dArdMs,
        oResult.oDsiStats.dSuppressionDb, oResult.oArdStats.dPeakDb,
        (unsigned long long)oResult.oArdStats.uDetections,
        (unsigned long long)oResult.oArdStats.uDetectionsInBorder,
        (unsigned long long)oRun.pIngest->stats().uCpisOvertaken,
        (unsigned long long)oRun.uDatagrams,
        oResult.dWaitMs, dPrintMs, dDetCsvMs, dArdWriteMs,
        (unsigned long long)oResult.uStatus, (unsigned long long)oResult.uPlots);
}

// The first CPI's input, once, if --dump-cpi asked for it.
bool dumpFirstCpi(Run& oRun, uint64_t uCpiStart)
{
    if (oRun.oOptions.sDumpCpiPath.empty() || oRun.bCpiDumped)
        return true;
    CpiFileHeader oHeader;
    oHeader.uSampleCount  = oRun.oCfg.uCpiSamples;
    oHeader.uSampleRateHz = oRun.oCfg.uSampleRateHz;
    oHeader.uStartIndex   = uCpiStart;
    if (!writeCpiFile(oRun.oOptions.sDumpCpiPath.c_str(), oHeader,
                      oRun.oChain.vcRef.data(), oRun.oChain.vcSur.data()))
        return false;
    oRun.bCpiDumped = true;
    std::printf("wrote the first CPI (start index %llu) to %s\n",
                (unsigned long long)uCpiStart, oRun.oOptions.sDumpCpiPath.c_str());
    return true;
}

// One extracted CPI, start to finish. False on an error that ends the run.
bool handleCpi(Run& oRun, uint64_t uCpiStart)
{
    if (!dumpFirstCpi(oRun, uCpiStart))
        return false;

    if (!oRun.bHaveFirst) {
        oRun.uFirstCpiStart = uCpiStart;
        oRun.dStartupOffset = nowSeconds(oRun.oStart);
    }

    CpiResult oResult;
    oResult.uCpiStart = uCpiStart;
    // Everything since the previous CPI's output finished: draining the
    // socket, parsing, and spinning while the next window fills.
    oResult.dWaitMs = oRun.bHaveLastOutput
                    ? msBetween(oRun.oLastOutputEnd, std::chrono::steady_clock::now())
                    : 0.0;

    const bool bWantMap = (oRun.oOut.bWriteFrames || oRun.oOut.bStreamFrames) &&
                          (oRun.uCpiIndex % oRun.oCfg.uArdSnapshotEvery == 0);
    processCpi(oRun.oChain, bWantMap ? oRun.oChain.vcMap.data() : nullptr, oResult);
    if (!countStatus(oRun, oResult.uStatus))
        return false;
    updateTimeline(oRun, oResult);

    // The rest is split into the places a stall could hide: printing the
    // progress line (which on a `make run` goes through tee and an ssh pipe,
    // and blocks if that backs up), writing the CSV, and writing or
    // streaming the frame.
    const TimePoint tPrintStart = std::chrono::steady_clock::now();
    printCpiLine(oRun.uCpiIndex, oResult);
    const TimePoint tPrinted = std::chrono::steady_clock::now();
    writeDetections(oRun.oOut, oRun.uCpiIndex, oRun.oChain, oResult);
    const TimePoint tDetectionsWritten = std::chrono::steady_clock::now();
    if (bWantMap && !publishFrame(oRun, oResult))
        return false;
    const TimePoint tFramePublished = std::chrono::steady_clock::now();

    // Written last, so the row can carry every phase of this CPI. The
    // telemetry write itself is one fprintf and falls into the next CPI's
    // waitMs, which is where it belongs.
    writeTelemetry(oRun, oResult, msBetween(tPrintStart, tPrinted),
                   msBetween(tPrinted, tDetectionsWritten),
                   msBetween(tDetectionsWritten, tFramePublished));

    oRun.oLastOutputEnd  = std::chrono::steady_clock::now();
    oRun.bHaveLastOutput = true;
    ++oRun.uCpiIndex;
    return true;
}

// ---------------------------------------------------------------------------
// Startup and the summary.
// ---------------------------------------------------------------------------

void printStartup(const Run& oRun, double dBuildMs)
{
    const Outputs& oOut = oRun.oOut;
    printPbrConfig(oRun.oCfg);
    std::printf("\npbr_run\n");
    std::printf("  source        %s\n",
                oRun.oOptions.bLive ? "live UDP" : oRun.oOptions.sCapturePath.c_str());
    std::printf("  output        %s\n",
                oRun.oOptions.sOutDir.empty() ? "console only" : oRun.oOptions.sOutDir.c_str());
    std::printf("  frames        %s%s%s\n",
                oOut.bWriteFrames ? "written to disk" : "",
                (oOut.bWriteFrames && oOut.bStreamFrames) ? " and " : "",
                oOut.bStreamFrames ? "streamed" : (oOut.bWriteFrames ? "" : "neither written nor streamed"));
    std::printf("  chain         %s -> %s\n", oRun.oChain.pDsi->name(), oRun.oChain.pArd->name());
    std::printf("  coupling      %s\n",
                oRun.oChain.bDeviceCoupled ? "device-coupled: samples stay on the GPU"
                                           : "via host: the cleaned CPI round-trips");
    std::printf("  build+warm-up %.1f ms (not counted)\n", dBuildMs);
    if (oRun.oOptions.bLive && !oRun.oChain.bDeviceCoupled)
        std::fprintf(stderr,
            "\npbr_run: WARNING: a live run on the CPU backend. It is a correctness\n"
            "         reference and may not keep up with a live stream; the summary's\n"
            "         budget and lag checks will say if it fell behind.\n");
}

// Open the live socket or the capture file.
bool openSource(const Run& oRun, UdpReceiver& oReceiver, CaptureReader& oReader)
{
    const PbrConfig& oCfg = oRun.oCfg;
    if (!oRun.oOptions.bLive)
        return oReader.open(oRun.oOptions.sCapturePath.c_str());

    if (!oReceiver.open(oCfg.uUdpPort))
        return false;
    if (!oRun.oOptions.sMulticastGroup.empty()) {
        if (!oReceiver.joinMulticastGroup(oRun.oOptions.sMulticastGroup.c_str()))
            return false;
        std::printf("joined multicast group %s\n", oRun.oOptions.sMulticastGroup.c_str());
    }
    // The single-threaded design rests on this buffer absorbing what arrives
    // while a CPI is being processed, so report what the kernel actually
    // granted rather than what was asked for. Two channels of int16 I and Q;
    // the packet headers add about 2% and are left out.
    const double dBytesPerSecond = 2.0 * 4.0 * double(oCfg.uSampleRateHz);
    const double dSlackSeconds   = double(oReceiver.actualRcvBufBytes()) / dBytesPerSecond;
    std::printf("socket buffer %d bytes (%.2f s of slack)\n",
                oReceiver.actualRcvBufBytes(), dSlackSeconds);
    if (dSlackSeconds < 1.0)
        std::printf("  WARNING: under 1 s of slack. Raise "
                    "net.core.rmem_max on the board.\n");
    std::printf("listening on UDP port %u -- start uhd_to_vita49 (make drive) or fers-cli now\n\n",
                static_cast<unsigned>(oCfg.uUdpPort));
    return true;
}

double medianOf(std::vector<double> vd)   // by value: sorted here
{
    if (vd.empty())
        return 0.0;
    std::sort(vd.begin(), vd.end());
    const size_t uMid = vd.size() / 2;
    return (vd.size() % 2 == 0) ? 0.5 * (vd[uMid - 1] + vd[uMid]) : vd[uMid];
}

// The counters, the timing and the verdict. True for PASS.
bool printSummary(const Run& oRun, const CaptureWriter& oCapture, bool bCaptureClosed,
                  bool bError)
{
    const ChannelIngest::Stats& oStats = oRun.pIngest->stats();
    const double dElapsed  = nowSeconds(oRun.oStart);
    const double dBudgetMs = 1000.0 * double(oRun.oCfg.uHopSamples) /
                             double(oRun.oCfg.uSampleRateHz);
    double dBestMs = 0.0, dWorstMs = 0.0;
    if (!oRun.vdTotalMs.empty()) {
        dBestMs  = *std::min_element(oRun.vdTotalMs.begin(), oRun.vdTotalMs.end());
        dWorstMs = *std::max_element(oRun.vdTotalMs.begin(), oRun.vdTotalMs.end());
    }

    std::printf("\n================ pbr_run summary ================\n");
    std::printf("elapsed                 %.2f s\n", dElapsed);
    std::printf("datagrams               %llu  (%llu parse failures)\n",
                (unsigned long long)oRun.uDatagrams,
                (unsigned long long)oRun.uParseFailures);
    std::printf("writes rejected (late)  %llu\n",
                (unsigned long long)oStats.uRejectedWrites);
    std::printf("packet-count gaps       %llu\n",
                (unsigned long long)oStats.uPacketCountGaps);
    std::printf("driver session changes  %llu\n",
                (unsigned long long)oStats.uSessionChanges);
    std::printf("timeline re-anchors     %llu\n",
                (unsigned long long)oStats.uReanchors);
    std::printf("live changes applied    %llu\n", (unsigned long long)oRun.uLiveChanges);
    std::printf("sample rate adaptions   %llu\n", (unsigned long long)oRun.uRateAdaptions);
    if (!oRun.oOptions.sSaveCapturePath.empty())
        std::printf("captured                %llu datagrams, %.1f MB to %s\n",
                    (unsigned long long)oCapture.recordCount(), oCapture.byteCount() / 1e6,
                    oRun.oOptions.sSaveCapturePath.c_str());
    if (oRun.oOut.bStreamFrames)
        std::printf("ARD stream send fails   %llu\n",
                    (unsigned long long)oRun.oOut.oSender.sendFailures());
    std::printf("\n-- CPIs\n");
    std::printf("   processed              %llu\n", (unsigned long long)oRun.uCpiIndex);
    std::printf("   skipped, overtaken     %llu\n",
                (unsigned long long)oStats.uCpisOvertaken);
    std::printf("   skipped, poor coverage %llu\n",
                (unsigned long long)oStats.uCpisSkippedCoverage);
    std::printf("   blank, null input      %llu  (channel all zeros or non-finite)\n",
                (unsigned long long)oRun.uNullCpis);
    std::printf("   blank, stage failed    %llu\n",
                (unsigned long long)oRun.uFailedCpis);
    std::printf("\n-- timing, against a %.0f ms budget per CPI\n", dBudgetMs);
    if (!oRun.vdTotalMs.empty()) {
        std::printf("   best / median / worst  %.1f / %.1f / %.1f ms\n",
                    dBestMs, medianOf(oRun.vdTotalMs), dWorstMs);
        std::printf("   worst as a fraction    %.3f of the budget\n", dWorstMs / dBudgetMs);
    }
    std::printf("   startup offset         %.3f s (before the first CPI; not a backlog)\n",
                oRun.dStartupOffset);
    std::printf("   lag growth since       %.3f s  <-- must stay near zero\n", oRun.dMaxLag);

    // Blank frames for null input are the chain doing its job on a dead
    // input and do not fail the run; a stage failure does. A file replay
    // runs as fast as it can, so lag is meaningless there.
    bool bPass = !bError && bCaptureClosed &&
                 oRun.uParseFailures == 0 &&
                 oStats.uRejectedWrites == 0 &&
                 oStats.uCpisOvertaken == 0 &&
                 oRun.uFailedCpis == 0 &&
                 oRun.uCpiIndex > 0;
    if (!oRun.oOptions.sDumpCpiPath.empty() && !oRun.bCpiDumped) {
        std::printf("\n   NO CPI WAS EXTRACTED, so %s was not written\n",
                    oRun.oOptions.sDumpCpiPath.c_str());
        bPass = false;
    }
    if (oRun.oOptions.bLive) {
        if (dWorstMs > dBudgetMs) {
            std::printf("\n   WORST CPI EXCEEDED THE BUDGET\n");
            bPass = false;
        }
        // Measured from the first CPI, so this is drift, not offset. A chain
        // that keeps up holds this near zero however long it runs; one that
        // falls behind grows it without bound.
        const double dLagLimit = dBudgetMs / 1000.0;
        if (oRun.dMaxLag > dLagLimit) {
            std::printf("\n   LAG GREW BY %.3f s -- the chain is falling behind\n",
                        oRun.dMaxLag);
            bPass = false;
        }
    }

    std::printf("\n=================================================\n");
    // Only a live run can be said to have "kept up": a file replay processes
    // as fast as it can, so its lag is meaningless and saying otherwise would
    // put a reassuring word next to a number that does not support it.
    if (!bPass)
        std::printf("FAIL -- see the counters above.\n");
    else if (oRun.oOptions.bLive)
        std::printf("PASS -- the chain kept up in real time, every check clean.\n");
    else
        std::printf("PASS -- every check clean. This was a file replay, so the\n"
                    "        timing above is throughput, NOT real-time behaviour.\n");
    return bPass;
}

} // namespace

int main(int argc, char* argv[])
{
    Run oRun;
    if (!parseArgs(argc, argv, oRun.oOptions)) {
        printUsage(argv[0]);
        return 1;
    }
    const Options& oOptions = oRun.oOptions;

    if (!loadPbrConfig(oOptions.sConfigPath.c_str(), oRun.oCfg))
        return 1;
    if (!makeOutputDir(oOptions, oRun.oCfg, oRun.oOut))
        return 1;

    // ---- build the chain and the ingest -------------------------------------
    double dBuildMs = 0.0;
    if (!buildChain(oRun.oCfg, oRun.oChain, dBuildMs) || !buildIngest(oRun.oCfg, oRun.pIngest))
        return 1;
    if (oRun.oOut.bStreamFrames &&
        !oRun.oOut.oSender.open(oRun.oCfg.sArdStreamHost.c_str(), oRun.oCfg.uArdStreamPort, nullptr))
        return 1;
    printStartup(oRun, dBuildMs);

    // ---- the control port ---------------------------------------------------
    const PbrConfig oInitialCfg = oRun.oCfg;   // the file's set, for the Reset button
    oRun.oControl.pCurrent = &oRun.oCfg;
    oRun.oControl.pInitial = &oInitialCfg;
    HttpServer oControlServer;
    if (oRun.oCfg.uControlPort != 0) {
        if (!oControlServer.open(oRun.oCfg.uControlPort))
            return 1;
        std::printf("  control       http://<this host>:%u  (GET /api/config, POST /api/apply, POST /api/reset)\n",
                    static_cast<unsigned>(oRun.oCfg.uControlPort));
    }
    std::printf("\n");

    // ---- the source and the outputs -----------------------------------------
    UdpReceiver   oReceiver;
    CaptureReader oReader;
    if (!openSource(oRun, oReceiver, oReader))
        return 1;

    CaptureWriter oCapture;
    if (!oOptions.sSaveCapturePath.empty()) {
        if (!oCapture.open(oOptions.sSaveCapturePath.c_str()))
            return 1;
        std::printf("recording every datagram to %s\n", oOptions.sSaveCapturePath.c_str());
    }
    if (!openOutputFiles(oOptions, oRun.oOut))
        return 1;

    // ---- the loop ----------------------------------------------------------
    std::vector<uint8_t> vDatagram(kDatagramCapacity);
    oRun.oStart = std::chrono::steady_clock::now();
    bool bError = false;

    while (!bError) {
        if (oOptions.dSeconds > 0.0 && nowSeconds(oRun.oStart) >= oOptions.dSeconds)
            break;

        // Live changes, at the boundary: nothing is mid-CPI here, since the
        // previous iteration's drain loop ran to NotYet. Serve the control
        // port (microseconds when idle), then apply whatever it accepted.
        if (oControlServer.isOpen()) {
            oRun.oControl.uCpiIndex = oRun.uCpiIndex;
            oControlServer.servePending(controlHandler, &oRun.oControl);
        }
        if (oRun.oControl.bPending && !applyPendingChange(oRun)) {
            bError = true;
            break;
        }

        size_t uLength = 0;
        if (oOptions.bLive) {
            const ReceiveResult eResult =
                oReceiver.receive(vDatagram.data(), vDatagram.size(), uLength);
            if (eResult == ReceiveResult::WouldBlock)
                continue;   // nothing waiting; the socket is non-blocking
            if (eResult == ReceiveResult::Error) {
                bError = true;
                break;
            }
        } else {
            bool bEof = false;
            if (!oReader.read(vDatagram.data(), vDatagram.size(), uLength, bEof)) {
                bError = !bEof;
                break;
            }
        }
        ++oRun.uDatagrams;

        if (oCapture.isOpen() && !oCapture.write(vDatagram.data(), uLength)) {
            bError = true;
            break;
        }

        Vita49Packet oPacket;
        if (!parseVita49Packet(vDatagram.data(), uLength, oPacket)) {
            ++oRun.uParseFailures;
            continue;
        }
        if (!oRun.pIngest->handlePacket(oPacket) || !followSourceRate(oRun)) {
            bError = true;
            break;
        }
        noteUnresolvedRoles(oRun);

        // Drain whatever CPIs the new data completed.
        for (;;) {
            uint64_t uCpiStart = 0;
            const CpiStatus eCpi = oRun.pIngest->tryExtractCpi(
                oRun.oChain.vcRef.data(), oRun.oChain.vcSur.data(), oRun.oCfg.uCpiSamples, uCpiStart);
            if (eCpi == CpiStatus::NotYet)
                break;
            if (eCpi == CpiStatus::Skipped)
                continue;
            if (!handleCpi(oRun, uCpiStart)) {
                bError = true;
                break;
            }
        }
    }

    closeOutputFiles(oRun.oOut);
    const bool bCaptureClosed = !oCapture.isOpen() || oCapture.close();
    return printSummary(oRun, oCapture, bCaptureClosed, bError) ? 0 : 1;
}
