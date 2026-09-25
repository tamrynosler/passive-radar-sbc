// ard_view -- the range-Doppler viewer's sidecar, in C++.
//
//   pbr_run --(ARD stream, UDP)--> ard_view --(HTTP)--> a browser
//                    <--(control port, HTTP)--
//
// Joins the ARD multicast group (or listens on a port), reassembles each
// frame in memory, keeps only the newest complete one, and serves the page
// in src/display/ard_viewer.html plus these endpoints:
//
//   GET  /                   the page
//   GET  /api/status         JSON: latest CPI, counters, and the status JSON
//                            pbr_run sent with the frame, verbatim
//   GET  /api/frame/latest   the newest complete frame, PBRARD01 bytes
//   GET  /api/config         the chain's live parameter set (forwarded)
//   POST /api/apply          KEY = VALUE lines for the chain (forwarded);
//                            the chain's answer comes back as the body
//   POST /api/reset          back to the chain's config file values (forwarded)
//
// The last two are forwarded to pbr_run's control port. Its address is
// learnt, not configured: the host is whoever sends the frames, the port is
// in the status JSON. So any viewer that sees the stream can change the
// chain, and a viewer that has seen no stream says so.
//
// Nothing is written to disk. Two threads: one blocks on the UDP socket and
// feeds the assembler, the other accepts HTTP connections. They share the
// newest frame, its status and the counters under one mutex; an HTTP
// request copies the frame out (1.5 MB, well under a millisecond) so the
// receiver is never held up by a slow browser.
//
// Runs on the board (make view) or on any machine on the switch that joins
// the group (make host builds it without CUDA). Design:
// docs/ard-stream-design.md. Replaces scripts/ard_viewer.py.
//
// Usage:
//   ./ard_view                                   # 239.192.0.2:4992, http 8080
//   ./ard_view --group 239.192.0.2 --iface <mac-ip> --http 8081
//   ./ard_view --file data/ard/ard_00000.bin     # serve one recorded frame

#include "pbr/ArdFile.hpp"
#include "pbr/ArdStream.hpp"
#include "pbr/HttpServer.hpp"
#include "pbr/UdpReceiver.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>

using namespace pbr;

namespace {

const char* const kDefaultGroup = "239.192.0.2";
const uint16_t    kDefaultPort  = 4992;
const uint16_t    kDefaultHttp  = 8080;
const char* const kDefaultPage  = "src/display/ard_viewer.html";
const size_t      kDatagramCapacity = 2048;
const int         kRcvBufBytes  = 8 * 1024 * 1024;   // a frame arrives as a 1.5 MB burst
const double      kControlTimeoutSeconds = 3.0;

struct Options {
    std::string sGroup    = kDefaultGroup;   // empty: plain unicast on the port
    std::string sIface;                      // interface to join on, for a multi-homed host
    uint16_t    uPort     = kDefaultPort;
    uint16_t    uHttp     = kDefaultHttp;
    std::string sPagePath = kDefaultPage;
    std::string sFilePath;                   // serve one recorded frame instead
};

void printUsage(const char* pProgram)
{
    std::printf("usage: %s [options]\n", pProgram);
    std::printf("  --group ADDR   multicast group to join (default %s; \"\" for unicast)\n", kDefaultGroup);
    std::printf("  --iface IPV4   join on this interface (a laptop with Wi-Fi and a wired port)\n");
    std::printf("  --port N       UDP port of the ARD stream (default %u)\n", static_cast<unsigned>(kDefaultPort));
    std::printf("  --http N       HTTP port to serve the page on (default %u)\n", static_cast<unsigned>(kDefaultHttp));
    std::printf("  --page FILE    the page to serve (default %s)\n", kDefaultPage);
    std::printf("  --file FRAME   serve one recorded .bin instead of a stream\n");
}

bool parseArgs(int argc, char* argv[], Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string sArg = argv[i];
        const bool bV = (i + 1 < argc);
        if (sArg == "--help" || sArg == "-h")   return false;
        else if (sArg == "--group" && bV)       o.sGroup = argv[++i];
        else if (sArg == "--iface" && bV)       o.sIface = argv[++i];
        else if (sArg == "--port" && bV)        o.uPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (sArg == "--http" && bV)        o.uHttp = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (sArg == "--page" && bV)        o.sPagePath = argv[++i];
        else if (sArg == "--file" && bV)        o.sFilePath = argv[++i];
        else {
            std::fprintf(stderr, "unrecognised or incomplete argument: %s\n", sArg.c_str());
            return false;
        }
    }
    return true;
}

volatile std::sig_atomic_t g_iStop = 0;

void onSignal(int)
{
    g_iStop = 1;
}

// What the two threads share. Everything here is read and written only with
// the mutex held.
struct Shared {
    std::mutex           oMutex;
    bool                 bHaveFrame = false;
    uint32_t             uCpi = 0;
    std::vector<uint8_t> vFrame;
    std::string          sStatus;            // pbr_run's status JSON, or empty
    std::string          sChainHost;         // who sent the frames
    uint16_t             uChainControlPort = 0;
    uint64_t             uFramesCompleted = 0;
    uint64_t             uFramesDropped = 0;
    uint64_t             uChunksReceived = 0;
};

struct Context {
    Shared*     pShared;
    std::string sPagePath;
};

// "controlPort":4993 out of the status JSON, 0 if absent.
uint16_t controlPortFrom(const std::string& sJson)
{
    const size_t uAt = sJson.find("\"controlPort\":");
    if (uAt == std::string::npos)
        return 0;
    const long lPort = std::strtol(sJson.c_str() + uAt + 14, nullptr, 10);
    return (lPort > 0 && lPort <= 65535) ? static_cast<uint16_t>(lPort) : 0;
}

// The receive thread: block on the socket, feed the assembler, publish.
void receiveLoop(UdpReceiver* pReceiver, Shared* pShared)
{
    ArdFrameAssembler    oAssembler;
    std::vector<uint8_t> vDatagram(kDatagramCapacity);
    std::string          sSender;

    while (!g_iStop) {
        size_t uLength = 0;
        const ReceiveResult eResult =
            pReceiver->receive(vDatagram.data(), vDatagram.size(), uLength, &sSender);
        if (eResult == ReceiveResult::Error)
            break;
        if (eResult == ReceiveResult::WouldBlock)
            continue;   // the one-second wake-up; check the stop flag

        const ArdFeedResult eFed = oAssembler.feed(vDatagram.data(), uLength);
        if (eFed == ArdFeedResult::FrameComplete || eFed == ArdFeedResult::StatusAttached) {
            std::lock_guard<std::mutex> oLock(pShared->oMutex);
            if (eFed == ArdFeedResult::FrameComplete) {
                pShared->vFrame     = oAssembler.frame();
                pShared->uCpi       = oAssembler.frameSequence();
                pShared->bHaveFrame = true;
                pShared->sChainHost = sSender;
            }
            pShared->sStatus = oAssembler.statusJson();
            if (!pShared->sStatus.empty())
                pShared->uChainControlPort = controlPortFrom(pShared->sStatus);
            pShared->uFramesCompleted = oAssembler.framesCompleted();
            pShared->uFramesDropped   = oAssembler.framesDropped();
            pShared->uChunksReceived  = oAssembler.chunksReceived();
        }
    }
}

bool readWholeFile(const std::string& sPath, std::vector<uint8_t>& vOut)
{
    std::FILE* pFile = std::fopen(sPath.c_str(), "rb");
    if (pFile == nullptr)
        return false;
    vOut.clear();
    uint8_t auChunk[65536];
    for (;;) {
        const size_t uGot = std::fread(auChunk, 1, sizeof(auChunk), pFile);
        if (uGot == 0)
            break;
        vOut.insert(vOut.end(), auChunk, auChunk + uGot);
    }
    std::fclose(pFile);
    return true;
}

void setBody(HttpResponse& oResponse, const std::string& sText)
{
    oResponse.vBody.assign(sText.begin(), sText.end());
}

// Forward a request to the chain's control port and relay its answer.
void forwardToChain(Shared* pShared, const char* pMethod, const char* pPath,
                    const std::string& sBody, HttpResponse& oResponse)
{
    std::string sHost;
    uint16_t    uPort = 0;
    {
        std::lock_guard<std::mutex> oLock(pShared->oMutex);
        sHost = pShared->sChainHost;
        uPort = pShared->uChainControlPort;
    }
    if (sHost.empty() || uPort == 0) {
        oResponse.iStatus = 500;
        setBody(oResponse, "no chain known yet: the control address is learnt from the "
                           "frames, and none has arrived (or the chain has CONTROL_PORT = 0)\n");
        return;
    }

    int         iStatus = 0;
    std::string sAnswer;
    if (!httpRequest(sHost.c_str(), uPort, pMethod, pPath, sBody,
                     kControlTimeoutSeconds, iStatus, sAnswer)) {
        oResponse.iStatus = 500;
        setBody(oResponse, "chain at " + sHost + ":" + std::to_string(uPort) +
                           " did not answer: " + sAnswer + "\n");
        return;
    }
    oResponse.iStatus = (iStatus == 200 || iStatus == 400) ? iStatus : 500;
    setBody(oResponse, sAnswer);
}

bool handleRequest(const std::string& sMethod, const std::string& sPath,
                   const std::string& sBody, void* pContext, HttpResponse& oResponse)
{
    Context* pCtx    = static_cast<Context*>(pContext);
    Shared*  pShared = pCtx->pShared;

    if (sPath == "/" && sMethod == "GET") {
        // Read on every request, so editing the page needs no restart.
        if (!readWholeFile(pCtx->sPagePath, oResponse.vBody)) {
            oResponse.iStatus = 500;
            setBody(oResponse, "page not found: " + pCtx->sPagePath + "\n");
            return true;
        }
        oResponse.sContentType = "text/html; charset=utf-8";
        return true;
    }

    if (sPath == "/api/status" && sMethod == "GET") {
        const double dNow = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string sJson;
        {
            std::lock_guard<std::mutex> oLock(pShared->oMutex);
            char achHead[320];
            if (pShared->bHaveFrame) {
                std::snprintf(achHead, sizeof(achHead),
                              "{\"latestCpi\":%u,\"framesCompleted\":%llu,"
                              "\"framesDropped\":%llu,\"chunksReceived\":%llu,"
                              "\"chainHost\":\"%s\",\"chainControlPort\":%u,"
                              "\"serverTime\":%.3f,\"stream\":",
                              pShared->uCpi,
                              (unsigned long long)pShared->uFramesCompleted,
                              (unsigned long long)pShared->uFramesDropped,
                              (unsigned long long)pShared->uChunksReceived,
                              pShared->sChainHost.c_str(),
                              static_cast<unsigned>(pShared->uChainControlPort),
                              dNow);
            } else {
                std::snprintf(achHead, sizeof(achHead),
                              "{\"latestCpi\":null,\"framesCompleted\":%llu,"
                              "\"framesDropped\":%llu,\"chunksReceived\":%llu,"
                              "\"chainHost\":\"\",\"chainControlPort\":0,"
                              "\"serverTime\":%.3f,\"stream\":",
                              (unsigned long long)pShared->uFramesCompleted,
                              (unsigned long long)pShared->uFramesDropped,
                              (unsigned long long)pShared->uChunksReceived,
                              dNow);
            }
            sJson  = achHead;
            sJson += pShared->sStatus.empty() ? "null" : pShared->sStatus;
            sJson += "}";
        }
        oResponse.sContentType = "application/json";
        setBody(oResponse, sJson);
        return true;
    }

    if (sPath == "/api/frame/latest" && sMethod == "GET") {
        std::lock_guard<std::mutex> oLock(pShared->oMutex);
        if (!pShared->bHaveFrame)
            return false;
        oResponse.vBody        = pShared->vFrame;
        oResponse.sContentType = "application/octet-stream";
        oResponse.sExtraHeader = "X-Pbr-Cpi: " + std::to_string(pShared->uCpi);
        return true;
    }

    if (sPath == "/api/config" && sMethod == "GET") {
        forwardToChain(pShared, "GET", "/api/config", "", oResponse);
        return true;
    }

    if (sPath == "/api/apply" && sMethod == "POST") {
        forwardToChain(pShared, "POST", "/api/apply", sBody, oResponse);
        return true;
    }

    if (sPath == "/api/reset" && sMethod == "POST") {
        forwardToChain(pShared, "POST", "/api/reset", "", oResponse);
        return true;
    }

    return false;
}

} // namespace

int main(int argc, char* argv[])
{
    Options o;
    if (!parseArgs(argc, argv, o)) {
        printUsage(argv[0]);
        return 1;
    }

    Shared  oShared;
    Context oContext;
    oContext.pShared   = &oShared;
    oContext.sPagePath = o.sPagePath;

    UdpReceiver oReceiver;
    std::thread oReceiveThread;

    if (!o.sFilePath.empty()) {
        // One recorded frame, for working on the page without a stream.
        std::vector<uint8_t> vFrame;
        ArdFileHeader oHeader;
        if (!readWholeFile(o.sFilePath, vFrame) ||
            !parseArdFrameHeader(vFrame.data(), vFrame.size(), oHeader)) {
            std::fprintf(stderr, "ard_view: cannot read %s as an ARD frame\n", o.sFilePath.c_str());
            return 1;
        }
        char achStatus[256];
        std::snprintf(achStatus, sizeof(achStatus),
                      "{\"runName\":\"file\",\"cpi\":0,\"status\":%llu,\"reason\":\"\","
                      "\"detections\":%llu}",
                      (unsigned long long)oHeader.uStatus,
                      (unsigned long long)oHeader.uDetections);
        oShared.bHaveFrame       = true;
        oShared.uCpi             = 0;
        oShared.vFrame           = vFrame;
        oShared.sStatus          = achStatus;
        oShared.uFramesCompleted = 1;
        std::printf("ard_view: serving %s (%llu x %llu, status %llu)\n", o.sFilePath.c_str(),
                    (unsigned long long)oHeader.uRangeBins,
                    (unsigned long long)oHeader.uDopplerBins,
                    (unsigned long long)oHeader.uStatus);
    } else {
        if (!oReceiver.open(o.uPort, kRcvBufBytes, true))
            return 1;
        if (!o.sGroup.empty() && !oReceiver.joinMulticastGroup(o.sGroup.c_str(), o.sIface.c_str()))
            return 1;
        std::printf("ard_view: ARD stream on UDP %u%s%s, receive buffer %d bytes\n",
                    static_cast<unsigned>(o.uPort),
                    o.sGroup.empty() ? "" : ", group ", o.sGroup.c_str(),
                    oReceiver.actualRcvBufBytes());
        if (oReceiver.actualRcvBufBytes() < 2 * 1024 * 1024)
            std::printf("  WARNING: under 2 MB of receive buffer; a 1.5 MB frame burst "
                        "may drop. Raise net.core.rmem_max.\n");
        oReceiveThread = std::thread(receiveLoop, &oReceiver, &oShared);
    }

    HttpServer oHttp;
    if (!oHttp.open(o.uHttp)) {
        g_iStop = 1;
        if (oReceiveThread.joinable())
            oReceiveThread.join();
        return 1;
    }
    std::printf("ard_view: open http://localhost:%u  (page %s)\n",
                static_cast<unsigned>(o.uHttp), o.sPagePath.c_str());
    std::fflush(stdout);

    // sigaction without SA_RESTART, not std::signal: glibc's signal() sets
    // SA_RESTART, which makes the blocking accept() below resume after the
    // handler instead of returning EINTR, so the stop flag was only noticed
    // at the next HTTP request. With a stalled browser that was never, and
    // `timeout`, `pkill` and Ctrl-C all failed to end the program.
    struct sigaction oAction;
    std::memset(&oAction, 0, sizeof(oAction));
    oAction.sa_handler = onSignal;
    sigemptyset(&oAction.sa_mask);
    oAction.sa_flags = 0;
    sigaction(SIGINT,  &oAction, nullptr);
    sigaction(SIGTERM, &oAction, nullptr);

    while (!g_iStop) {
        if (!oHttp.serveOne(handleRequest, &oContext))
            break;
    }

    g_iStop = 1;
    if (oReceiveThread.joinable())
        oReceiveThread.join();

    std::lock_guard<std::mutex> oLock(oShared.oMutex);
    std::printf("\nard_view: %llu frames complete, %llu dropped, %llu chunks\n",
                (unsigned long long)oShared.uFramesCompleted,
                (unsigned long long)oShared.uFramesDropped,
                (unsigned long long)oShared.uChunksReceived);
    return 0;
}
