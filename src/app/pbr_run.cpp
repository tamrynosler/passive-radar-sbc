// pbr_run -- the whole processing chain, live or from a recorded capture.
//
//   VITA 49.2 UDP  ->  circular buffers  ->  CPI extraction
//                  ->  DSI cancellation  ->  range-Doppler map  ->  CFAR
//                  ->  detections, telemetry, ARD frames streamed and/or written
//
// Usage:
//   ./pbr_run --live --group 239.192.0.1 --seconds 30 --out /data/results/x
//   ./pbr_run --file data/fers_capture.pbrcap --config config/fers.conf
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
#include "pbr/DSICanceller.hpp"
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
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace pbr;

namespace {

const size_t   kDatagramCapacity = 2048;
const uint64_t kMaxDetections    = 8192;
const char* const kDefaultConfigPath = "config/prb.conf";

// Bins in the before-processing spectrum streamed with each frame: 512
// int16 values fit one 1400-byte datagram, and 400 Hz per bin at 204800 Hz
// is finer than the viewer's panel can draw.
const uint32_t kSpectrumBins = 512;

// A stage returning false on a CPI produces a blank frame and the run goes
// on; this many in a row means the fault is not in the data, and the run
// stops so the error does not scroll past for hours.
const uint64_t kMaxConsecutiveFailures = 10;

// Keys a live change may not touch: each one sizes or names something that
// exists only once per run. A change to one is refused by name.
const char* const kRestartKeys[] = {
    "SAMPLE_RATE_Hz", "REFERENCE_RECEIVER_NAME", "SURVEILLANCE_RECEIVER_NAME",
    "UDP_PORT", "CONTROL_PORT", "VITA49_FULLSCALE", "BUFFER_LENGTH_s",
    "DSI_BACKEND", "ARD_WRITE_FILES", "ARD_STREAM", "ARD_STREAM_DEST",
};

// Everything that describes the SIGNAL PROCESSING lives in the config file, so
// a run is fully reproducible from a tracked file. What is left here is only
// what varies per invocation: where the samples come from, how long to run,
// and where to put the results.
struct Options {
    std::string sCapturePath;       // empty means live
    std::string sConfigPath = kDefaultConfigPath;
    std::string sOutDir;            // empty means console only
    std::string sMulticastGroup;    // empty means plain unicast on UDP_PORT
    double      dSeconds    = 0.0;  // 0 means "until the source ends"
    bool        bLive       = false;
};

void printUsage(const char* pProgram)
{
    std::printf("usage: %s (--live | --file CAPTURE) [options]\n", pProgram);
    std::printf("  --live           receive VITA 49 over UDP (uhd_to_vita49 or FERS)\n");
    std::printf("  --file FILE      replay a .pbrcap capture instead\n");
    std::printf("  --seconds N      stop after N seconds (live only)\n");
    std::printf("  --group ADDR     also join this multicast group (live only), the\n");
    std::printf("                   way uhd_to_vita49 feeds several clients at once\n");
    std::printf("  --out DIR        write telemetry.csv, detections.csv and changes.log\n");
    std::printf("                   here, and ard/ frames if the config has ARD_WRITE_FILES = 1\n");
    std::printf("  --config FILE    configuration (default %s)\n", kDefaultConfigPath);
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
        else if (sArg == "--seconds" && bV)     o.dSeconds = std::atof(argv[++i]);
        else if (sArg == "--group" && bV)       o.sMulticastGroup = argv[++i];
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
    for (size_t u = 0; u < vcRef.size(); ++u) {
        const float fI = vcRef[u].real();
        const float fQ = vcRef[u].imag();
        if (!std::isfinite(fI) || !std::isfinite(fQ)) {
            pReasonOut = "non-finite sample in the reference";
            return false;
        }
        dRefPower += double(fI) * fI + double(fQ) * fQ;
    }

    double dSurPower = 0.0;
    for (size_t u = 0; u < vcSur.size(); ++u) {
        const float fI = vcSur[u].real();
        const float fQ = vcSur[u].imag();
        if (!std::isfinite(fI) || !std::isfinite(fQ)) {
            pReasonOut = "non-finite sample in the surveillance";
            return false;
        }
        dSurPower += double(fI) * fI + double(fQ) * fQ;
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

double nowSeconds(const std::chrono::steady_clock::time_point& oStart)
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - oStart).count();
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
    std::vector<Detection> vDet;
    std::vector<uint8_t>   vFrame;      // one serialised ARD frame

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
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

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
    oChain.vDet.assign(static_cast<size_t>(kMaxDetections), Detection());

    ArdFileHeader oFrameSizing;
    oFrameSizing.uRangeBins   = oChain.oArdParams.uRangeBins;
    oFrameSizing.uDopplerBins = oChain.oArdParams.uDopplerBins;
    oFrameSizing.uDetections  = kMaxDetections;
    oChain.vFrame.assign(ardFrameBytes(oFrameSizing), 0);

    // Warm up. The first CUDA call pays for context creation, and cuFFT
    // defers some plan work to its first execution. Without this the very
    // first CPI blows its budget and the first telemetry row looks like a
    // failure.
    std::fill(oChain.vcRef.begin(), oChain.vcRef.end(), ComplexF(1.0f, 0.0f));
    std::fill(oChain.vcSur.begin(), oChain.vcSur.end(), ComplexF(0.5f, 0.5f));
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
                                           kMaxDetections, oWarmArd)
              : oChain.pArd->process(oChain.vcRef.data(), oChain.vcCleaned.data(),
                                     oChain.vcMap.data(), oChain.vDet.data(),
                                     kMaxDetections, oWarmArd);
    }
    if (!bWarm) {
        std::fprintf(stderr, "pbr_run: the warm-up pass failed\n");
        return false;
    }

    dMsOut = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
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
// Live changes: the control port and the change log.
// ---------------------------------------------------------------------------

// Which keys differ between two parameter sets, as "KEY: old -> new" lines.
std::string configDiff(const PbrConfig& oOld, const PbrConfig& oNew,
                       std::vector<std::string>* pKeysOut)
{
    std::map<std::string, std::string> mOld, mNew;
    const std::string aTexts[2] = { pbrConfigToText(oOld), pbrConfigToText(oNew) };
    for (int i = 0; i < 2; ++i) {
        size_t uPos = 0;
        while (uPos < aTexts[i].size()) {
            size_t uEnd = aTexts[i].find('\n', uPos);
            if (uEnd == std::string::npos) uEnd = aTexts[i].size();
            const std::string sLine = aTexts[i].substr(uPos, uEnd - uPos);
            uPos = uEnd + 1;
            const size_t uEq = sLine.find(" = ");
            if (uEq == std::string::npos) continue;
            (i == 0 ? mOld : mNew)[sLine.substr(0, uEq)] = sLine.substr(uEq + 3);
        }
    }
    std::string sOut;
    for (std::map<std::string, std::string>::const_iterator it = mNew.begin(); it != mNew.end(); ++it) {
        if (mOld[it->first] != it->second) {
            sOut += it->first + ": " + mOld[it->first] + " -> " + it->second + "\n";
            if (pKeysOut != nullptr)
                pKeysOut->push_back(it->first);
        }
    }
    return sOut;
}

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

bool controlHandler(const std::string& sMethod, const std::string& sPath,
                    const std::string& sBody, void* pContext, HttpResponse& oResponse)
{
    Control* pCtl = static_cast<Control*>(pContext);

    if (sPath == "/api/config" && sMethod == "GET") {
        setText(oResponse, pbrConfigToText(*pCtl->pCurrent));
        return true;
    }

    if (sPath == "/api/reset" && sMethod == "POST") {
        // Back to the config file's values. If the source's sample rate made
        // the chain adapt, the file's windows are re-adapted the same way, so
        // the reset lands on what the file would have meant at this rate.
        PbrConfig   oCandidate = *pCtl->pInitial;
        std::string sReport;
        if (oCandidate.uSampleRateHz != pCtl->pCurrent->uSampleRateHz &&
            !adaptPbrConfigToSampleRate(*pCtl->pInitial, pCtl->pCurrent->uSampleRateHz,
                                        oCandidate, sReport)) {
            oResponse.iStatus = 400;
            setText(oResponse, "refused: the file's settings cannot be adapted to the "
                               "source's rate:\n" + sReport);
            return true;
        }
        std::vector<std::string> vKeys;
        const std::string sDiff = configDiff(*pCtl->pCurrent, oCandidate, &vKeys);
        if (vKeys.empty()) {
            pCtl->bPending = false;
            setText(oResponse, "already at the config file's values\n");
            return true;
        }
        pCtl->oPending     = oCandidate;
        pCtl->bPending     = true;
        pCtl->sPendingDiff = sDiff;
        setText(oResponse, "reset accepted; applies at the next CPI boundary (after CPI " +
                           std::to_string(pCtl->uCpiIndex) + "):\n" + sDiff);
        return true;
    }

    if (sPath == "/api/apply" && sMethod == "POST") {
        // Restart-only keys are refused by name before anything else, so
        // the answer says why rather than whatever the validator trips on.
        {
            size_t uPos = 0;
            while (uPos < sBody.size()) {
                size_t uEnd = sBody.find('\n', uPos);
                if (uEnd == std::string::npos) uEnd = sBody.size();
                std::string sLine = sBody.substr(uPos, uEnd - uPos);
                uPos = uEnd + 1;
                const size_t uHash = sLine.find('#');
                if (uHash != std::string::npos) sLine.erase(uHash);
                const size_t uEq = sLine.find('=');
                if (uEq == std::string::npos) continue;
                std::string sKey = sLine.substr(0, uEq);
                while (!sKey.empty() && (sKey[sKey.size() - 1] == ' ' || sKey[sKey.size() - 1] == '\t'))
                    sKey.erase(sKey.size() - 1);
                while (!sKey.empty() && (sKey[0] == ' ' || sKey[0] == '\t'))
                    sKey.erase(0, 1);
                for (size_t k = 0; k < sizeof(kRestartKeys) / sizeof(kRestartKeys[0]); ++k) {
                    if (sKey != kRestartKeys[k])
                        continue;
                    // Only a change is refused; restating the current value
                    // (the page sends the whole set back) is fine.
                    const std::string sCurrent = pbrConfigToText(*pCtl->pCurrent);
                    std::string sValue = sLine.substr(uEq + 1);
                    while (!sValue.empty() && (sValue[0] == ' ' || sValue[0] == '\t')) sValue.erase(0, 1);
                    while (!sValue.empty() && (sValue[sValue.size() - 1] == ' ' || sValue[sValue.size() - 1] == '\t' || sValue[sValue.size() - 1] == '\r'))
                        sValue.erase(sValue.size() - 1);
                    if (sCurrent.find(sKey + " = " + sValue + "\n") == std::string::npos) {
                        oResponse.iStatus = 400;
                        setText(oResponse, "refused: " + sKey +
                                           " cannot change while running; restart with it in the config\n");
                        return true;
                    }
                }
            }
        }

        PbrConfig   oCandidate;
        std::string sWhy;
        if (!applyPbrConfigLines(*pCtl->pCurrent, sBody, oCandidate, sWhy)) {
            oResponse.iStatus = 400;
            setText(oResponse, "refused:\n" + sWhy);
            return true;
        }

        std::vector<std::string> vKeys;
        const std::string sDiff = configDiff(*pCtl->pCurrent, oCandidate, &vKeys);
        if (vKeys.empty()) {
            pCtl->bPending = false;
            setText(oResponse, "no change: every value is already what it is\n");
            return true;
        }
        for (size_t u = 0; u < vKeys.size(); ++u) {
            for (size_t k = 0; k < sizeof(kRestartKeys) / sizeof(kRestartKeys[0]); ++k) {
                if (vKeys[u] == kRestartKeys[k]) {
                    oResponse.iStatus = 400;
                    setText(oResponse, "refused: " + vKeys[u] +
                                       " cannot change while running; restart with it in the config\n");
                    return true;
                }
            }
        }

        pCtl->oPending     = oCandidate;
        pCtl->bPending     = true;
        pCtl->sPendingDiff = sDiff;
        setText(oResponse, "accepted; applies at the next CPI boundary (after CPI " +
                           std::to_string(pCtl->uCpiIndex) + "):\n" + sDiff);
        return true;
    }

    return false;
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

    // Build the output paths from the one directory the caller gave. The
    // CSVs and the change log always go there; ARD frames only if the config
    // says to write them (1.5 MB per CPI), and DIR/ard/ is not even created
    // otherwise. The output flags are fixed for the run.
    const bool bWriteFrames  = oCfg.bArdWriteFiles && !oOptions.sOutDir.empty();
    const bool bStreamFrames = oCfg.bArdStream;
    std::string sArdDir, sTelemetryPath, sDetectionsPath, sChangesPath;
    if (!oOptions.sOutDir.empty()) {
        sTelemetryPath  = oOptions.sOutDir + "/telemetry.csv";
        sDetectionsPath = oOptions.sOutDir + "/detections.csv";
        sChangesPath    = oOptions.sOutDir + "/changes.log";
        if (bWriteFrames)
            sArdDir = oOptions.sOutDir + "/ard";
        // mkdir -p, so a run never fails at the first CPI for want of a
        // directory it could have made at startup.
        const std::string sCmd = "mkdir -p '" +
                                 (bWriteFrames ? sArdDir : oOptions.sOutDir) + "'";
        if (std::system(sCmd.c_str()) != 0) {
            std::fprintf(stderr, "pbr_run: cannot create %s\n", oOptions.sOutDir.c_str());
            return 1;
        }
    }
    if (oCfg.bArdWriteFiles && oOptions.sOutDir.empty())
        std::fprintf(stderr, "pbr_run: ARD_WRITE_FILES is on but no --out was given; "
                             "no frames will be written\n");

    // ---- build the chain and the ingest -------------------------------------
    Chain  oChain;
    double dBuildMs = 0.0;
    if (!buildChain(oCfg, oChain, dBuildMs))
        return 1;

    std::unique_ptr<ChannelIngest> pIngest;
    if (!buildIngest(oCfg, pIngest))
        return 1;

    std::vector<uint8_t> vDatagram(kDatagramCapacity);

    ArdStreamSender oArdSender;
    if (bStreamFrames &&
        !oArdSender.open(oCfg.sArdStreamHost.c_str(), oCfg.uArdStreamPort, nullptr))
        return 1;
    const std::string sRunName = runNameFrom(oOptions.sOutDir);

    printPbrConfig(oCfg);
    std::printf("\npbr_run\n");
    std::printf("  source        %s\n",
                oOptions.bLive ? "live UDP" : oOptions.sCapturePath.c_str());
    std::printf("  output        %s\n",
                oOptions.sOutDir.empty() ? "console only" : oOptions.sOutDir.c_str());
    std::printf("  frames        %s%s%s\n",
                bWriteFrames ? "written to disk" : "",
                (bWriteFrames && bStreamFrames) ? " and " : "",
                bStreamFrames ? "streamed" : (bWriteFrames ? "" : "neither written nor streamed"));
    std::printf("  chain         %s -> %s\n", oChain.pDsi->name(), oChain.pArd->name());
    std::printf("  coupling      %s\n",
                oChain.bDeviceCoupled ? "device-coupled: samples stay on the GPU"
                                      : "via host: the cleaned CPI round-trips");
    std::printf("  build+warm-up %.1f ms (not counted)\n", dBuildMs);
    if (oOptions.bLive && !oChain.bDeviceCoupled) {
        std::fprintf(stderr,
            "\npbr_run: refusing a live run on the CPU backend. It is a\n"
            "         correctness reference -- the direct DFT alone takes tens\n"
            "         of seconds per CPI -- so it cannot keep up with a live\n"
            "         stream and the run would only measure how far behind it\n"
            "         fell. Use DSI_BACKEND = GPU.\n");
        return 1;
    }

    // ---- the control port ---------------------------------------------------
    const PbrConfig oInitialCfg = oCfg;   // the file's set, for the Reset button
    Control    oControl;
    oControl.pCurrent = &oCfg;
    oControl.pInitial = &oInitialCfg;
    HttpServer oControlServer;
    if (oCfg.uControlPort != 0) {
        if (!oControlServer.open(oCfg.uControlPort))
            return 1;
        std::printf("  control       http://<this host>:%u  (GET /api/config, POST /api/apply, POST /api/reset)\n",
                    static_cast<unsigned>(oCfg.uControlPort));
    }
    std::printf("\n");

    // ---- open the source ---------------------------------------------------
    UdpReceiver   oReceiver;
    CaptureReader oReader;
    if (oOptions.bLive) {
        if (!oReceiver.open(oCfg.uUdpPort))
            return 1;
        if (!oOptions.sMulticastGroup.empty()) {
            if (!oReceiver.joinMulticastGroup(oOptions.sMulticastGroup.c_str()))
                return 1;
            std::printf("joined multicast group %s\n", oOptions.sMulticastGroup.c_str());
        }
        // The single-threaded design rests on this buffer absorbing what
        // arrives while a CPI is being processed, so report what the kernel
        // actually granted rather than what was asked for.
        const double dSlackSeconds =
            double(oReceiver.actualRcvBufBytes()) / (1198.0 * 1400.0);
        std::printf("socket buffer %d bytes (%.2f s of slack)\n",
                    oReceiver.actualRcvBufBytes(), dSlackSeconds);
        if (dSlackSeconds < 1.0)
            std::printf("  WARNING: under 1 s of slack. Raise "
                        "net.core.rmem_max on the board.\n");
        std::printf("listening on UDP port %u -- start uhd_to_vita49 (make drive) or fers-cli now\n\n",
                    static_cast<unsigned>(oCfg.uUdpPort));
    } else if (!oReader.open(oOptions.sCapturePath.c_str())) {
        return 1;
    }

    // ---- output files ------------------------------------------------------
    std::FILE* pDetCsv = nullptr;
    if (!sDetectionsPath.empty()) {
        pDetCsv = std::fopen(sDetectionsPath.c_str(), "w");
        if (pDetCsv == nullptr) {
            std::fprintf(stderr, "pbr_run: cannot open %s\n", sDetectionsPath.c_str());
            return 1;
        }
        std::fprintf(pDetCsv, "cpi,signalTimeS,rangeBin,dopplerBin,"
                              "rangeKm,dopplerHz,powerDb,snrDb\n");
    }

    std::FILE* pTelCsv = nullptr;
    if (!sTelemetryPath.empty()) {
        pTelCsv = std::fopen(sTelemetryPath.c_str(), "w");
        if (pTelCsv == nullptr) {
            std::fprintf(stderr, "pbr_run: cannot open %s\n", sTelemetryPath.c_str());
            return 1;
        }
        std::fprintf(pTelCsv, "cpi,cpiStartIndex,signalTimeS,wallTimeS,lagS,"
                              "dsiMs,ardMs,totalMs,suppressionDb,peakDb,"
                              "detections,borderDetections,skipped,datagrams,"
                              "waitMs,printMs,detCsvMs,ardWriteMs,status\n");
    }

    std::FILE* pChangeLog = nullptr;
    if (!sChangesPath.empty()) {
        pChangeLog = std::fopen(sChangesPath.c_str(), "w");
        if (pChangeLog == nullptr) {
            std::fprintf(stderr, "pbr_run: cannot open %s\n", sChangesPath.c_str());
            return 1;
        }
        std::fprintf(pChangeLog, "# live parameter changes; the starting set is prb.conf beside this file\n");
        std::fflush(pChangeLog);
    }

    // ---- the loop ----------------------------------------------------------
    const std::chrono::steady_clock::time_point oStart =
        std::chrono::steady_clock::now();

    uint64_t uCpiIndex     = 0;
    uint64_t uDatagrams    = 0;
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
    bool     bHaveFirst     = false;
    double   dMaxLag        = 0.0;
    double   dWorstTotalMs  = 0.0;
    std::vector<double> vdTotalMs;
    bool     bReadError     = false;
    bool     bProcessError  = false;
    double   dLastUnresolvedNote = 0.0;   // first reminder five seconds in, not at the first datagram
    uint64_t uNullCpis      = 0;   // blank frames: input not processable
    uint64_t uFailedCpis    = 0;   // blank frames: a stage returned false
    uint64_t uConsecutiveFailures = 0;
    uint64_t uLiveChanges   = 0;
    uint64_t uRateAdaptions = 0;
    uint64_t uRateRefusedHz = 0;   // a source rate that could not be adapted to, reported once

    // ---- transient diagnostics --------------------------------------------
    // Splits the rest of the loop into the places a stall could hide: writing
    // the frame, writing the CSVs, printing the progress line (which on a
    // `make run` goes through tee and an ssh pipe, and blocks if that backs
    // up), and waiting for the next CPI's packets.
    std::chrono::steady_clock::time_point oLastOutputEnd = oStart;
    bool     bHaveLastOutput = false;

    for (;;) {
        if (oOptions.dSeconds > 0.0 && nowSeconds(oStart) >= oOptions.dSeconds)
            break;

        // ---- live changes, at the boundary --------------------------------
        // Nothing is mid-CPI here: the previous iteration's drain loop ran
        // to NotYet. Serve the control port (microseconds when idle), then
        // apply whatever it accepted.
        if (oControlServer.isOpen()) {
            bool bServed = false;
            oControl.uCpiIndex = uCpiIndex;
            oControlServer.servePending(controlHandler, &oControl, bServed);
        }
        if (oControl.bPending) {
            oControl.bPending = false;
            const PbrConfig oNewCfg = oControl.oPending;
            const bool bCpiChanged = (oNewCfg.uCpiSamples != oCfg.uCpiSamples ||
                                      oNewCfg.uHopSamples != oCfg.uHopSamples);
            double dMs = 0.0;
            // buildChain releases the old stages first, so the GPU never
            // holds two sets; if the new set fails to build, the old one is
            // built again from the config that is still current.
            if (!buildChain(oNewCfg, oChain, dMs)) {
                std::fprintf(stderr, "pbr_run: the accepted change could not be built; "
                                     "rebuilding the previous set\n");
                if (!buildChain(oCfg, oChain, dMs)) {
                    bProcessError = true;
                    break;
                }
            } else if (bCpiChanged && !pIngest->setCpi(oNewCfg.uCpiSamples, oNewCfg.uHopSamples)) {
                std::fprintf(stderr, "pbr_run: the ingest refused the new CPI; keeping the previous set\n");
                if (!buildChain(oCfg, oChain, dMs)) {
                    bProcessError = true;
                    break;
                }
            } else {
                oCfg = oNewCfg;
                ++uLiveChanges;
                char achMs[64];
                std::snprintf(achMs, sizeof(achMs), "rebuilt in %.0f ms\n", dMs);
                logChange(pChangeLog, uCpiIndex, "live change applied", oControl.sPendingDiff + achMs);
            }
        }

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
        if (!pIngest->handlePacket(oPacket)) {
            bProcessError = true;
            break;
        }

        // ---- the source's sample rate ----------------------------------------
        // A context packet declared a rate the config did not expect. Adapt
        // every window to it (the author's rule: pick the nearest settings
        // that work and say so), rebuild the chain AND the ingest, since every
        // buffered index depends on the rate, and carry on. A rate that
        // cannot be adapted to is reported once and nothing is processed.
        {
            const double   dSource = pIngest->sourceSampleRateHz();
            const uint64_t uSource = static_cast<uint64_t>(std::llround(dSource));
            if (dSource > 0.0 && uSource != oCfg.uSampleRateHz && uSource != uRateRefusedHz) {
                PbrConfig   oNewCfg;
                std::string sReport;
                double      dMs = 0.0;
                const bool bInteger = std::fabs(dSource - static_cast<double>(uSource)) < 1e-6;
                if (!bInteger) {
                    sReport = "source sample rate " + std::to_string(dSource) +
                              " Hz is not a whole number of hertz; the chain cannot index it\n";
                }
                if (bInteger && adaptPbrConfigToSampleRate(oCfg, uSource, oNewCfg, sReport) &&
                    buildChain(oNewCfg, oChain, dMs) && buildIngest(oNewCfg, pIngest)) {
                    oCfg = oNewCfg;
                    ++uRateAdaptions;
                    bHaveFirst = false;   // a new timeline: lag and signal time start again
                    char achMs[64];
                    std::snprintf(achMs, sizeof(achMs), "rebuilt in %.0f ms\n", dMs);
                    logChange(pChangeLog, uCpiIndex, "sample rate adapted", sReport + achMs);
                } else {
                    uRateRefusedHz = uSource;
                    std::fprintf(stderr, "pbr_run: %s         nothing will be processed until the "
                                         "source or the config changes\n", sReport.c_str());
                    if (!oChain.pDsi && !buildChain(oCfg, oChain, dMs)) {
                        bProcessError = true;
                        break;
                    }
                }
            }
        }

        // Packets are arriving but the streams have not been matched to the
        // configured receiver names: nothing will ever be processed, and
        // without this line the only sign is a summary at the end. A
        // reminder, not a deadline; it repeats until the names agree.
        if (!pIngest->rolesResolved()) {
            const double dNow = nowSeconds(oStart);
            if (dNow - dLastUnresolvedNote >= 5.0) {
                dLastUnresolvedNote = dNow;
                std::fprintf(stderr,
                    "pbr_run: %llu datagrams received but the channels are not identified yet.\n"
                    "         seen: %s\n"
                    "         config expects \"%s\" (reference) and \"%s\" (surveillance)\n",
                    (unsigned long long)uDatagrams, pIngest->streamSummary().c_str(),
                    oCfg.sReferenceName.c_str(), oCfg.sSurveillanceName.c_str());
            }
        }

        // Drain whatever CPIs the new data completed.
        for (;;) {
            uint64_t uCpiStart = 0;
            const CpiStatus eCpi = pIngest->tryExtractCpi(
                oChain.vcRef.data(), oChain.vcSur.data(), oCfg.uCpiSamples, uCpiStart);

            if (eCpi == CpiStatus::NotYet)  break;
            if (eCpi == CpiStatus::Skipped) continue;

            if (!bHaveFirst) {
                uFirstCpiStart = uCpiStart;
                dStartupOffset = nowSeconds(oStart);
            }

            // ---- cancel ---------------------------------------------------
            const std::chrono::steady_clock::time_point t0 =
                std::chrono::steady_clock::now();

            // Everything since the previous CPI's output finished: draining
            // the socket, parsing, and spinning while the next window fills.
            const double dWaitMs = bHaveLastOutput
                ? std::chrono::duration<double, std::milli>(t0 - oLastOutputEnd).count()
                : 0.0;
            const bool bWantMap = (bWriteFrames || bStreamFrames) &&
                                  (uCpiIndex % oCfg.uArdSnapshotEvery == 0);
            ComplexF* pMap = bWantMap ? oChain.vcMap.data() : nullptr;

            // A CPI the chain cannot process is not a crash and not a stop:
            // it goes out as a BLANK frame carrying a status word, so a dead
            // front end shows as "receiving, nothing to process" downstream.
            // The input guard runs first, so the solver is never handed a
            // window that would break it; a stage that still returns false is
            // counted, and only a run of them ends the program.
            uint64_t    uStatus = kArdStatusOk;
            const char* pReason = "";
            DSIStats oDsiStats;
            ArdStats oArdStats;

            bool bCancelOk = false;
            if (!inputIsProcessable(oChain.vcRef, oChain.vcSur, pReason))
                uStatus = kArdStatusNullInput;
            else
                bCancelOk = oChain.pDsi->cancel(oChain.vcRef.data(), oChain.vcSur.data(),
                                                oChain.bDeviceCoupled ? nullptr : oChain.vcCleaned.data(),
                                                oDsiStats);

            const std::chrono::steady_clock::time_point t1 =
                std::chrono::steady_clock::now();

            if (uStatus == kArdStatusOk && !bCancelOk) {
                uStatus = kArdStatusFailed;
                pReason = "cancellation returned an error";
            }

            // ---- map and detect -------------------------------------------
            if (uStatus == kArdStatusOk) {
                const bool bArdOk = oChain.bDeviceCoupled
                    ? oChain.pArd->processDevice(oChain.pDsi->deviceReference(),
                                                 oChain.pDsi->deviceOutput(),
                                                 pMap, oChain.vDet.data(), kMaxDetections, oArdStats)
                    : oChain.pArd->process(oChain.vcRef.data(), oChain.vcCleaned.data(), pMap,
                                           oChain.vDet.data(), kMaxDetections, oArdStats);
                if (!bArdOk) {
                    uStatus = kArdStatusFailed;
                    pReason = "map formation or CFAR returned an error";
                }
            }
            const std::chrono::steady_clock::time_point t2 =
                std::chrono::steady_clock::now();

            if (uStatus != kArdStatusOk) {
                // The blank frame: zero map, no detections, zero statistics.
                if (pMap != nullptr)
                    std::fill(oChain.vcMap.begin(), oChain.vcMap.end(), ComplexF(0.0f, 0.0f));
                oDsiStats = DSIStats();
                oArdStats = ArdStats();
                if (uStatus == kArdStatusNullInput) {
                    ++uNullCpis;
                    uConsecutiveFailures = 0;
                } else {
                    ++uFailedCpis;
                    ++uConsecutiveFailures;
                    if (uConsecutiveFailures >= kMaxConsecutiveFailures) {
                        std::fprintf(stderr, "pbr_run: %llu consecutive processing "
                                             "failures; this is not the data. Stopping.\n",
                                     (unsigned long long)uConsecutiveFailures);
                        bProcessError = true;
                        break;
                    }
                }
            } else {
                uConsecutiveFailures = 0;
            }

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

            // Both clocks are zeroed at the same instant: the moment the
            // first CPI finished processing. A chain that keeps up then holds
            // the lag at zero, whatever the algorithm costs per CPI.
            if (!bHaveFirst) {
                bHaveFirst      = true;
                dWallAtFirstCpi = dWallS;
            }

            const double dLagS    = (dWallS - dWallAtFirstCpi) - dSignalS;
            if (dLagS > dMaxLag) dMaxLag = dLagS;
            if (dTotalMs > dWorstTotalMs) dWorstTotalMs = dTotalMs;
            vdTotalMs.push_back(dTotalMs);

            const ChannelIngest::Stats& oIngestStats = pIngest->stats();

            const std::chrono::steady_clock::time_point t3 =
                std::chrono::steady_clock::now();

            if (uStatus == kArdStatusOk) {
                std::printf("CPI %4llu  t=%7.3f s  dsi %6.1f  ard %5.1f  "
                            "total %6.1f ms  %5.2f dB  %3llu det  lag %6.3f s\n",
                            (unsigned long long)uCpiIndex, dSignalS,
                            dDsiMs, dArdMs, dTotalMs,
                            oDsiStats.dSuppressionDb,
                            (unsigned long long)oArdStats.uDetections, dLagS);
            } else {
                std::printf("CPI %4llu  t=%7.3f s  %s: %s -- blank frame  lag %6.3f s\n",
                            (unsigned long long)uCpiIndex, dSignalS,
                            (uStatus == kArdStatusNullInput) ? "NULL INPUT" : "FAILED",
                            pReason, dLagS);
            }
            std::fflush(stdout);

            const std::chrono::steady_clock::time_point t4 =
                std::chrono::steady_clock::now();

            if (pDetCsv != nullptr) {
                for (uint64_t u = 0; u < oArdStats.uDetections; ++u) {
                    const Detection& d = oChain.vDet[static_cast<size_t>(u)];
                    std::fprintf(pDetCsv, "%llu,%.6f,%llu,%llu,%.4f,%.4f,%.4f,%.4f\n",
                                 (unsigned long long)uCpiIndex, dSignalS,
                                 (unsigned long long)d.uRangeBin,
                                 (unsigned long long)d.uDopplerBin,
                                 d.dRangeKm, d.dDopplerHz, d.dPowerDb, d.dSnrDb);
                }
            }

            const std::chrono::steady_clock::time_point t5 =
                std::chrono::steady_clock::now();

            if (bWantMap) {
                ArdFileHeader oArdHeader;
                oArdHeader.uRangeBins         = oChain.oArdParams.uRangeBins;
                oArdHeader.uDopplerBins       = oChain.oArdParams.uDopplerBins;
                oArdHeader.uSampleRateHz      = oChain.oArdParams.uSampleRateHz;
                oArdHeader.uDopplerDecimation = oChain.oArdParams.uDopplerDecimation;
                oArdHeader.uCpiStartIndex     = uCpiStart;
                oArdHeader.uDetections        = oArdStats.uDetections;
                oArdHeader.uStatus            = uStatus;

                size_t uFrameBytes = 0;
                if (!serialiseArdFrame(oArdHeader, oChain.vcMap.data(), oChain.vDet.data(),
                                       oChain.vFrame.data(), oChain.vFrame.size(), uFrameBytes)) {
                    bProcessError = true;
                    break;
                }

                if (bWriteFrames) {
                    char achPath[512];
                    std::snprintf(achPath, sizeof(achPath), "%s/ard_%05llu.bin",
                                  sArdDir.c_str(), (unsigned long long)uCpiIndex);
                    if (!writeArdFrameBytes(achPath, oChain.vFrame.data(), uFrameBytes)) {
                        bProcessError = true;
                        break;
                    }
                }

                if (bStreamFrames) {
                    // The frame, then its status: what the viewer's header
                    // strip shows, with the telemetry column names as keys,
                    // plus the live config values and where to send changes.
                    const uint32_t uSequence = static_cast<uint32_t>(uCpiIndex);
                    oArdSender.sendFrame(oChain.vFrame.data(), uFrameBytes, uSequence);

                    // Then each channel's spectrum as it was handed to the
                    // canceller, blank frames included: a dead input is
                    // exactly what that panel is for. Timed inside ardWriteMs.
                    const ComplexF* apChannel[2] = { oChain.vcRef.data(), oChain.vcSur.data() };
                    for (uint32_t c = 0; c < 2; ++c) {
                        if (oChain.oSpectrum.compute(apChannel[c], oCfg.uCpiSamples,
                                                     oChain.vfSpectrum.data()))
                            oArdSender.sendSpectrum(oChain.vfSpectrum.data(), kSpectrumBins, c,
                                                    static_cast<uint32_t>(oCfg.uSampleRateHz),
                                                    uSequence);
                    }

                    char achStatus[1200];
                    const int iLen = std::snprintf(
                        achStatus, sizeof(achStatus),
                        "{\"runName\":\"%s\",\"cpi\":%llu,\"status\":%llu,"
                        "\"reason\":\"%s\",\"signalTimeS\":%.3f,\"lagS\":%.3f,"
                        "\"dsiMs\":%.1f,\"ardMs\":%.1f,\"totalMs\":%.1f,"
                        "\"suppressionDb\":%.2f,\"peakDb\":%.2f,"
                        "\"detections\":%llu,\"borderDetections\":%llu,\"skipped\":%llu,"
                        "\"controlPort\":%u,\"liveChanges\":%llu,"
                        "\"config\":{\"DSI_ALGORITHM\":\"%s\",\"DSI_BACKEND\":\"%s\","
                        "\"ARD_ALGORITHM\":\"%s\",\"CFAR_PFA\":\"%g\",\"CPI_LENGTH_s\":\"%g\","
                        "\"SAMPLE_RATE_Hz\":\"%llu\"}}",
                        sRunName.c_str(), (unsigned long long)uCpiIndex,
                        (unsigned long long)uStatus, pReason, dSignalS, dLagS,
                        dDsiMs, dArdMs, dTotalMs,
                        oDsiStats.dSuppressionDb, oArdStats.dPeakDb,
                        (unsigned long long)oArdStats.uDetections,
                        (unsigned long long)oArdStats.uDetectionsInBorder,
                        (unsigned long long)oIngestStats.uCpisOvertaken,
                        static_cast<unsigned>(oCfg.uControlPort),
                        (unsigned long long)uLiveChanges,
                        oCfg.sDsiAlgorithm.c_str(), oCfg.sDsiBackend.c_str(),
                        oCfg.sArdAlgorithm.c_str(), oCfg.dCfarPfa, oCfg.dCpiSeconds,
                        (unsigned long long)oCfg.uSampleRateHz);
                    if (iLen > 0 && static_cast<size_t>(iLen) < sizeof(achStatus))
                        oArdSender.sendStatus(achStatus, uSequence);
                }
            }

            const std::chrono::steady_clock::time_point t6 =
                std::chrono::steady_clock::now();

            // Written last, so the row can carry every phase of this CPI.
            // The telemetry write itself is one fprintf and falls into the
            // next CPI's waitMs, which is where it belongs.
            if (pTelCsv != nullptr) {
                std::fprintf(pTelCsv,
                    "%llu,%llu,%.6f,%.6f,%.6f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                    "%llu,%llu,%llu,%llu,%.4f,%.4f,%.4f,%.4f,%llu\n",
                    (unsigned long long)uCpiIndex,
                    (unsigned long long)uCpiStart,
                    dSignalS, dWallS, dLagS, dDsiMs, dArdMs, dTotalMs,
                    oDsiStats.dSuppressionDb, oArdStats.dPeakDb,
                    (unsigned long long)oArdStats.uDetections,
                    (unsigned long long)oArdStats.uDetectionsInBorder,
                    (unsigned long long)oIngestStats.uCpisOvertaken,
                    (unsigned long long)uDatagrams,
                    dWaitMs,
                    std::chrono::duration<double, std::milli>(t4 - t3).count(),
                    std::chrono::duration<double, std::milli>(t5 - t4).count(),
                    std::chrono::duration<double, std::milli>(t6 - t5).count(),
                    (unsigned long long)uStatus);
            }

            oLastOutputEnd   = std::chrono::steady_clock::now();
            bHaveLastOutput  = true;

            ++uCpiIndex;
        }

        if (bProcessError)
            break;
    }

    if (pDetCsv != nullptr)    std::fclose(pDetCsv);
    if (pTelCsv != nullptr)    std::fclose(pTelCsv);
    if (pChangeLog != nullptr) std::fclose(pChangeLog);

    // ---- report ------------------------------------------------------------
    const ChannelIngest::Stats& oStats = pIngest->stats();
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
    std::printf("driver session changes  %llu\n",
                (unsigned long long)oStats.uSessionChanges);
    std::printf("timeline re-anchors     %llu\n",
                (unsigned long long)oStats.uReanchors);
    std::printf("live changes applied    %llu\n", (unsigned long long)uLiveChanges);
    std::printf("sample rate adaptions   %llu\n", (unsigned long long)uRateAdaptions);
    if (bStreamFrames)
        std::printf("ARD stream send fails   %llu\n",
                    (unsigned long long)oArdSender.sendFailures());
    std::printf("\n-- CPIs\n");
    std::printf("   processed              %llu\n", (unsigned long long)uCpiIndex);
    std::printf("   skipped, overtaken     %llu\n",
                (unsigned long long)oStats.uCpisOvertaken);
    std::printf("   skipped, poor coverage %llu\n",
                (unsigned long long)oStats.uCpisSkippedCoverage);
    std::printf("   blank, null input      %llu  (channel all zeros or non-finite)\n",
                (unsigned long long)uNullCpis);
    std::printf("   blank, stage failed    %llu\n",
                (unsigned long long)uFailedCpis);
    std::printf("\n-- timing, against a %.0f ms budget per CPI\n", dBudgetMs);
    if (!vdTotalMs.empty()) {
        std::printf("   best / median / worst  %.1f / %.1f / %.1f ms\n",
                    vdTotalMs.front(), dMedianMs, vdTotalMs.back());
        std::printf("   worst as a fraction    %.3f of the budget\n",
                    dWorstTotalMs / dBudgetMs);
    }
    std::printf("   startup offset         %.3f s (before the first CPI; not a backlog)\n",
                dStartupOffset);
    std::printf("   lag growth since       %.3f s  <-- must stay near zero\n", dMaxLag);

    // Blank frames for null input are the chain doing its job on a dead
    // input and do not fail the run; a stage failure does. A file replay
    // runs as fast as it can, so lag is meaningless there.
    bool bPass = !bReadError && !bProcessError &&
                 uParseFailures == 0 &&
                 oStats.uRejectedWrites == 0 &&
                 oStats.uCpisOvertaken == 0 &&
                 uFailedCpis == 0 &&
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
