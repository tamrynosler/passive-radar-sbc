// ingest_probe -- live checkpoint for the VITA 49.2 ingest layer.
//
// Waits for FERS to start streaming, waits for both receivers to identify
// themselves via context packets, then ingests continuously while checking
// that every sample lands at the right absolute index and survives in the
// circular buffer.
//
// This is a diagnostic tool, not a unit test: it needs a live FERS process,
// so it lives in src/app/ rather than tests/, where ctest would run it
// unattended and hang.
//
// Usage:  ingest_probe [run_seconds] [port] [fullscale]
//
// Start this first, then in another terminal:
//
//   fers-cli scenario.fersxml --out-dir=... \
//     --vita49 127.0.0.1:4991 --vita49-fullscale 0.005 -n=6
//
// Correctness cannot be checked against a golden file here, because the
// buffer is continuously overwritten. Instead three self-consistency checks
// run against the live stream:
//
//   1. INDEX CONTINUITY. The index derived from a packet's timestamp is
//      compared against one accumulated from sample counts. Two independent
//      routes to the same number; any arithmetic error breaks the agreement.
//
//   2. IMMEDIATE READ-BACK. Each packet is read straight back out of the ring
//      and compared against samples this program converted itself, without
//      going through ChannelIngest. Proves the write/read round trip and
//      cross-checks the int16 -> physical-units conversion.
//
//   3. LAG CHECK. A plain non-circular shadow copy of one CPI per channel is
//      kept and compared against the ring once the window is complete, by
//      which point hundreds of later packets have been written. This is what
//      proves data survives in a buffer that is being overwritten.

#include "pbr/CaptureFile.hpp"
#include "pbr/ChannelIngest.hpp"
#include "pbr/CircularBuffer.hpp"
#include "pbr/UdpReceiver.hpp"
#include "pbr/Vita49Packet.hpp"
#include "pbr/types.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace pbr;

namespace {

// Defaults, matching SingleTargetCleanNativeFileCw.fersxml and the MATLAB
// reference chain.
constexpr uint16_t    kDefaultPort       = 4991;
constexpr double      kDefaultFullScale  = 0.005;
constexpr uint64_t    kSampleRateHz      = 204800;
constexpr uint64_t    kBufferSeconds     = 2;
constexpr uint64_t    kCpiSamples        = kSampleRateHz;   // Tcpi = 1.0 s
constexpr int         kDefaultRunSeconds = 30;
constexpr int         kStreamWaitSeconds = 20;
constexpr size_t      kDatagramCapacity  = 2048;            // FERS sends 1400

const char* const kReferenceName    = "ArmasuisseRefRxClean";
const char* const kSurveillanceName = "ArmasuisseSurRxClean";

// Per-stream verification state.
struct StreamCheck {
    uint32_t uStreamId = 0;

    uint64_t uDataPackets = 0;
    uint64_t uSamples     = 0;

    // Check 1: index continuity.
    bool     bHaveExpected  = false;
    uint64_t uExpectedIndex = 0;
    uint64_t uIndexExact    = 0;
    uint64_t uIndexAhead    = 0;   // gap: timestamp index ran ahead
    uint64_t uIndexBehind   = 0;   // overlap or reordering
    uint64_t uFirstGapAt    = 0;

    // Check 2: immediate read-back.
    uint64_t uReadbackOk  = 0;
    uint64_t uReadbackBad = 0;

    // Check 3: lag check against a non-circular shadow copy.
    std::vector<ComplexF> vShadow;
    uint64_t uShadowBase      = 0;
    uint64_t uShadowFilled    = 0;
    bool     bShadowActive    = false;
    uint64_t uShadowOk        = 0;
    uint64_t uShadowBad       = 0;
    uint64_t uShadowAbandoned = 0;

    // Packet framing observations.
    uint64_t uDeclaredWordsOk       = 0;
    uint64_t uDeclaredWordsMismatch = 0;

    // Running raw ADC code extremes across the WHOLE run, not just the first
    // packet. This is what --vita49-fullscale has to be tuned against: codes
    // pinned near +-32767 mean clipping, codes of only a few hundred mean
    // most of the 16-bit range is being thrown away.
    bool    bCodesSeen = false;
    int32_t iCodeMin   = 0;
    int32_t iCodeMax   = 0;
};

StreamCheck& findOrAddCheck(std::vector<StreamCheck>& vChecks, uint32_t uStreamId)
{
    for (size_t u = 0; u < vChecks.size(); ++u) {
        if (vChecks[u].uStreamId == uStreamId)
            return vChecks[u];
    }
    StreamCheck oNew;
    oNew.uStreamId = uStreamId;
    vChecks.push_back(oNew);
    return vChecks.back();
}

int32_t readBe16Signed(const uint8_t* p)
{
    const uint16_t uRaw = static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
    return static_cast<int32_t>(static_cast<int16_t>(uRaw));
}

// Independent reimplementation of the index arithmetic, so check 1 is not
// just comparing ChannelIngest against itself.
uint64_t expectedAbsoluteIndex(uint64_t uSeconds, uint64_t uPicoseconds)
{
    constexpr uint64_t kPsPerSecond = 1000000000000ULL;
    return uSeconds * kSampleRateHz + (uPicoseconds * kSampleRateHz) / kPsPerSecond;
}

// Independent reimplementation of the sample conversion, for the same reason.
// Also accumulates the raw code extremes, which is why this diagnostic lives
// here rather than in ChannelIngest's production path.
void convertSamples(const Vita49Packet& oPacket, double dScale,
                    std::vector<ComplexF>& vcOut, StreamCheck& oCheck)
{
    vcOut.resize(oPacket.uSampleCount);
    for (uint64_t u = 0; u < oPacket.uSampleCount; ++u) {
        const uint8_t* pPair = oPacket.pPayload + u * 4;
        const int32_t iI = readBe16Signed(pPair);
        const int32_t iQ = readBe16Signed(pPair + 2);

        if (!oCheck.bCodesSeen) {
            oCheck.bCodesSeen = true;
            oCheck.iCodeMin   = iI;
            oCheck.iCodeMax   = iI;
        }
        if (iI < oCheck.iCodeMin) oCheck.iCodeMin = iI;
        if (iI > oCheck.iCodeMax) oCheck.iCodeMax = iI;
        if (iQ < oCheck.iCodeMin) oCheck.iCodeMin = iQ;
        if (iQ > oCheck.iCodeMax) oCheck.iCodeMax = iQ;

        vcOut[u] = ComplexF(static_cast<float>(iI * dScale),
                            static_cast<float>(iQ * dScale));
    }
}

const char* roleName(ChannelRole eRole)
{
    switch (eRole) {
    case ChannelRole::Reference:    return "reference";
    case ChannelRole::Surveillance: return "surveillance";
    default:                        return "unknown";
    }
}

struct Options {
    int         iRunSeconds = kDefaultRunSeconds;
    uint16_t    uPort       = kDefaultPort;
    double      dFullScale  = kDefaultFullScale;
    std::string sCapturePath;
};

void printUsage(const char* pProgram)
{
    std::printf("usage: %s [options]\n", pProgram);
    std::printf("  --seconds N     run checks for N seconds after both receivers\n");
    std::printf("                  identify themselves (default %d)\n", kDefaultRunSeconds);
    std::printf("  --port P        UDP port to listen on (default %u)\n",
                static_cast<unsigned>(kDefaultPort));
    std::printf("  --fullscale F   must match --vita49-fullscale (default %g)\n",
                kDefaultFullScale);
    std::printf("  --capture FILE  also record every datagram to FILE for replay\n");
}

bool parseArgs(int argc, char* argv[], Options& oOptions)
{
    for (int i = 1; i < argc; ++i) {
        const std::string sArg = argv[i];
        const bool bHasValue   = (i + 1 < argc);

        if (sArg == "--help" || sArg == "-h") {
            return false;
        } else if (sArg == "--seconds" && bHasValue) {
            oOptions.iRunSeconds = std::atoi(argv[++i]);
        } else if (sArg == "--port" && bHasValue) {
            oOptions.uPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (sArg == "--fullscale" && bHasValue) {
            oOptions.dFullScale = std::atof(argv[++i]);
        } else if (sArg == "--capture" && bHasValue) {
            oOptions.sCapturePath = argv[++i];
        } else {
            std::fprintf(stderr, "unrecognised or incomplete argument: %s\n", sArg.c_str());
            return false;
        }
    }

    if (oOptions.iRunSeconds <= 0 || oOptions.uPort == 0 || oOptions.dFullScale <= 0.0) {
        std::fprintf(stderr, "seconds, port and fullscale must all be positive\n");
        return false;
    }
    return true;
}

double secondsSince(const std::chrono::steady_clock::time_point& tStart)
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tStart).count();
}

// Compare a completed shadow window against the ring.
void verifyShadow(StreamCheck& oCheck, const CircularBuffer& oBuffer)
{
    std::vector<ComplexF> vcFromRing(kCpiSamples);

    if (!oBuffer.read(oCheck.uShadowBase, vcFromRing.data(), kCpiSamples)) {
        // The window fell out of the buffer before we could check it, which
        // means processing fell more than one CPI behind.
        ++oCheck.uShadowAbandoned;
        return;
    }

    for (uint64_t u = 0; u < kCpiSamples; ++u) {
        if (vcFromRing[u] != oCheck.vShadow[u]) {
            ++oCheck.uShadowBad;
            std::fprintf(stderr,
                         "  LAG CHECK FAILED on stream 0x%08X at absolute index %llu\n",
                         oCheck.uStreamId,
                         static_cast<unsigned long long>(oCheck.uShadowBase + u));
            return;
        }
    }
    ++oCheck.uShadowOk;
}

} // namespace

int main(int argc, char* argv[])
{
    Options oOptions;
    if (!parseArgs(argc, argv, oOptions)) {
        printUsage(argv[0]);
        return 1;
    }

    const int      iRunSeconds = oOptions.iRunSeconds;
    const uint16_t uPort       = oOptions.uPort;
    const double   dFullScale  = oOptions.dFullScale;

    std::printf("ingest_probe\n");
    std::printf("  port             %u\n", static_cast<unsigned>(uPort));
    std::printf("  sample rate      %llu Hz\n", (unsigned long long)kSampleRateHz);
    std::printf("  fullscale        %g   (must match --vita49-fullscale)\n", dFullScale);
    std::printf("  buffer           %llu s per channel\n", (unsigned long long)kBufferSeconds);
    std::printf("  run duration     %d s\n", iRunSeconds);

    CaptureWriter oCapture;
    if (!oOptions.sCapturePath.empty()) {
        if (!oCapture.open(oOptions.sCapturePath.c_str()))
            return 1;
        std::printf("  capturing to     %s\n", oOptions.sCapturePath.c_str());
    }
    std::printf("\n");

    ChannelIngestConfig oConfig;
    oConfig.uSampleRateHz     = kSampleRateHz;
    oConfig.dFullScale        = dFullScale;
    oConfig.uBufferCapacity   = kBufferSeconds * kSampleRateHz;
    oConfig.sReferenceName    = kReferenceName;
    oConfig.sSurveillanceName = kSurveillanceName;

    ChannelIngest oIngest(oConfig);
    if (!oIngest.isValid()) {
        std::fprintf(stderr, "ingest_probe: ChannelIngest failed to initialise\n");
        return 1;
    }

    UdpReceiver oReceiver;
    if (!oReceiver.open(uPort)) {
        std::fprintf(stderr, "ingest_probe: could not open UDP port %u\n",
                     static_cast<unsigned>(uPort));
        return 1;
    }

    std::printf("Socket receive buffer: %d bytes (%.2f s of stream at 1.7 MB/s)\n",
                oReceiver.actualRcvBufBytes(),
                oReceiver.actualRcvBufBytes() / 1.7e6);
    if (oReceiver.actualRcvBufBytes() < 1000000) {
        std::printf("  WARNING: small, so packet loss under load is likely.\n");
        std::printf("           Linux: sudo sysctl -w net.core.rmem_max=8388608\n");
        std::printf("           macOS: sudo sysctl -w kern.ipc.maxsockbuf=8388608\n");
    }
    std::printf("\nWaiting for FERS. Start it now:\n");
    std::printf("  fers-cli <scenario>.fersxml --vita49 <host>:%u --vita49-fullscale %g\n\n",
                static_cast<unsigned>(uPort), dFullScale);

    const double dScale = dFullScale / 32767.0;

    std::vector<uint8_t>  vDatagram(kDatagramCapacity);
    std::vector<StreamCheck> vChecks;
    std::vector<ComplexF> vcExpected;
    std::vector<ComplexF> vcReadback;

    uint64_t uDatagrams      = 0;
    uint64_t uParseFailures  = 0;
    uint64_t uOversizeSeen   = 0;

    const auto tStart = std::chrono::steady_clock::now();
    auto tLastBeat    = tStart;
    bool bSawFirst    = false;
    bool bRolesLogged = false;
    double dRunStart  = -1.0;

    while (true) {
        const double dElapsed = secondsSince(tStart);

        // Phase control: give up if FERS never appears, and stop the run
        // iRunSeconds after both roles resolve.
        if (!oIngest.rolesResolved() && dElapsed > kStreamWaitSeconds) {
            std::fprintf(stderr,
                         "\nTimed out after %d s: %s\n", kStreamWaitSeconds,
                         bSawFirst ? "packets arrived but both receivers never identified themselves"
                                   : "no packets arrived at all");
            return 1;
        }
        if (dRunStart >= 0.0 && (dElapsed - dRunStart) > iRunSeconds)
            break;

        // Drain everything currently waiting, then fall through to the
        // heartbeat. The application drives the loop, not the parser.
        size_t uBytes = 0;
        ReceiveResult eResult = oReceiver.receive(vDatagram.data(), vDatagram.size(), uBytes);

        if (eResult == ReceiveResult::Error)
            return 1;

        if (eResult == ReceiveResult::WouldBlock) {
            // Nothing waiting. Sleep briefly rather than spinning a core flat;
            // packets arrive roughly every 0.8 ms across both channels.
            std::this_thread::sleep_for(std::chrono::microseconds(200));

            // Only report "waiting" before the run starts; once it is under
            // way the heartbeat in the packet path carries the real numbers.
            if (dRunStart < 0.0 && secondsSince(tLastBeat) >= 1.0) {
                tLastBeat = std::chrono::steady_clock::now();
                std::printf("  waiting...  datagrams %llu\n",
                            (unsigned long long)uDatagrams);
                std::fflush(stdout);
            }
            continue;
        }

        ++uDatagrams;
        bSawFirst = true;

        // Record the datagram exactly as it arrived, before any parsing, so a
        // replay exercises the identical bytes.
        if (oCapture.isOpen() && !oCapture.write(vDatagram.data(), uBytes)) {
            std::fprintf(stderr, "ingest_probe: capture write failed; stopping capture\n");
            oCapture.close();
        }

        if (uBytes == vDatagram.size()) {
            ++uOversizeSeen;
            std::fprintf(stderr,
                         "ingest_probe: datagram filled the %zu-byte buffer exactly; "
                         "it may have been truncated\n", vDatagram.size());
        }

        Vita49Packet oPacket;
        if (!parseVita49Packet(vDatagram.data(), uBytes, oPacket)) {
            ++uParseFailures;
            continue;
        }

        // Let the ingest layer resolve roles and write samples.
        if (!oIngest.handlePacket(oPacket))
            return 1;

        if (oIngest.rolesResolved() && !bRolesLogged) {
            bRolesLogged = true;
            dRunStart    = secondsSince(tStart);
            std::printf("\nBoth receivers identified after %.2f s. "
                        "Running checks for %d s.\n\n", dRunStart, iRunSeconds);
        }

        if (oPacket.eType != Vita49Type::Data)
            continue;

        StreamCheck& oCheck = findOrAddCheck(vChecks, oPacket.uStreamId);
        ++oCheck.uDataPackets;
        oCheck.uSamples += oPacket.uSampleCount;

        // Does the header's declared size agree with the datagram length?
        // Recorded rather than enforced -- this run is what settles whether
        // FERS populates that field at all.
        if (static_cast<size_t>(oPacket.uDeclaredWords) * 4 == uBytes)
            ++oCheck.uDeclaredWordsOk;
        else
            ++oCheck.uDeclaredWordsMismatch;

        // The mod-16 packet counter is tracked by ChannelIngest, across all
        // packet types, and reported through stats().uPacketCountGaps.

        const uint64_t uIndex =
            expectedAbsoluteIndex(oPacket.uTimestampSec, oPacket.uTimestampPs);

        // CHECK 1: index continuity.
        if (!oCheck.bHaveExpected) {
            oCheck.bHaveExpected = true;
        } else if (uIndex == oCheck.uExpectedIndex) {
            ++oCheck.uIndexExact;
        } else if (uIndex > oCheck.uExpectedIndex) {
            if (oCheck.uIndexAhead == 0)
                oCheck.uFirstGapAt = uIndex;
            ++oCheck.uIndexAhead;
        } else {
            ++oCheck.uIndexBehind;
        }
        oCheck.uExpectedIndex = uIndex + oPacket.uSampleCount;

        const ChannelRole eRole = oIngest.roleOf(oPacket.uStreamId);
        if (eRole == ChannelRole::Unknown)
            continue;   // ChannelIngest dropped it; nothing in the ring to check

        const CircularBuffer& oBuffer = oIngest.buffer(eRole);

        // CHECK 2: immediate read-back against an independent conversion.
        convertSamples(oPacket, dScale, vcExpected, oCheck);
        vcReadback.resize(oPacket.uSampleCount);

        if (oBuffer.read(uIndex, vcReadback.data(), oPacket.uSampleCount)) {
            bool bMatch = true;
            for (uint64_t u = 0; u < oPacket.uSampleCount; ++u) {
                if (vcReadback[u] != vcExpected[u]) {
                    bMatch = false;
                    break;
                }
            }
            if (bMatch) {
                ++oCheck.uReadbackOk;
            } else {
                ++oCheck.uReadbackBad;
                std::fprintf(stderr,
                             "  READ-BACK FAILED on stream 0x%08X at index %llu\n",
                             oPacket.uStreamId, (unsigned long long)uIndex);
            }
        } else {
            ++oCheck.uReadbackBad;
        }

        // CHECK 3: accumulate into the non-circular shadow, and verify the
        // window against the ring once it is complete.
        if (!oCheck.bShadowActive) {
            oCheck.vShadow.assign(kCpiSamples, ComplexF(0.0f, 0.0f));
            oCheck.uShadowBase   = uIndex;
            oCheck.uShadowFilled = 0;
            oCheck.bShadowActive = true;
        }

        if (uIndex >= oCheck.uShadowBase &&
            uIndex < oCheck.uShadowBase + kCpiSamples) {

            // Copy only the part of the packet that lands inside the window.
            // The CPI is not a whole number of packets (204800 / 342 = 598.8),
            // so the last packet of every window straddles the boundary and
            // must be clipped rather than skipped.
            const uint64_t uOffset = uIndex - oCheck.uShadowBase;
            const uint64_t uRoom   = kCpiSamples - uOffset;
            const uint64_t uCopy   = (oPacket.uSampleCount < uRoom)
                                   ? oPacket.uSampleCount : uRoom;

            for (uint64_t u = 0; u < uCopy; ++u)
                oCheck.vShadow[uOffset + u] = vcExpected[u];
            oCheck.uShadowFilled += uCopy;

            if (oCheck.uShadowFilled >= kCpiSamples) {
                verifyShadow(oCheck, oBuffer);
                oCheck.bShadowActive = false;
            }

        } else if (uIndex >= oCheck.uShadowBase + kCpiSamples) {
            // A gap left the window incomplete and the stream has moved past
            // it. Abandon and re-anchor rather than verifying partial data.
            ++oCheck.uShadowAbandoned;
            oCheck.bShadowActive = false;
        }

        // Heartbeat.
        if (secondsSince(tLastBeat) >= 1.0) {
            tLastBeat = std::chrono::steady_clock::now();
            const ChannelIngest::Stats& oStats = oIngest.stats();

            const CircularBuffer& oRef = oIngest.buffer(ChannelRole::Reference);
            const CircularBuffer& oSur = oIngest.buffer(ChannelRole::Surveillance);
            const double dRefFill = double(oRef.writeEnd() - oRef.oldestValid()) / kSampleRateHz;
            const double dSurFill = double(oSur.writeEnd() - oSur.oldestValid()) / kSampleRateHz;

            std::printf("  t=%5.1fs  data %llu  ctx %llu  samples %llu  "
                        "fill ref %.2fs sur %.2fs  rejected %llu  gaps %llu\n",
                        secondsSince(tStart),
                        (unsigned long long)oStats.uDataPackets,
                        (unsigned long long)oStats.uContextPackets,
                        (unsigned long long)oStats.uSamplesWritten,
                        dRefFill, dSurFill,
                        (unsigned long long)oStats.uRejectedWrites,
                        (unsigned long long)oStats.uPacketCountGaps);
            std::fflush(stdout);
        }
    }

    // ---------------------------------------------------------------- report
    const ChannelIngest::Stats& oStats = oIngest.stats();
    bool bPass = true;

    std::printf("\n================ ingest_probe summary ================\n");
    if (oCapture.isOpen()) {
        std::printf("captured to             %s (%llu records, %.1f MB)\n",
                    oOptions.sCapturePath.c_str(),
                    (unsigned long long)oCapture.recordCount(),
                    oCapture.byteCount() / 1e6);
        oCapture.close();
    }
    std::printf("datagrams received      %llu\n", (unsigned long long)uDatagrams);
    std::printf("parse failures          %llu\n", (unsigned long long)uParseFailures);
    std::printf("possibly truncated      %llu\n", (unsigned long long)uOversizeSeen);
    std::printf("context packets         %llu\n", (unsigned long long)oStats.uContextPackets);
    std::printf("data packets            %llu\n", (unsigned long long)oStats.uDataPackets);
    std::printf("other packets           %llu\n", (unsigned long long)oStats.uOtherPackets);
    std::printf("samples written         %llu\n", (unsigned long long)oStats.uSamplesWritten);
    std::printf("data before context     %llu\n", (unsigned long long)oStats.uUnresolvedData);
    std::printf("writes rejected (late)  %llu\n", (unsigned long long)oStats.uRejectedWrites);
    std::printf("oversize packets        %llu\n", (unsigned long long)oStats.uOversizePackets);
    std::printf("packet-count gaps       %llu   (mod-16 header counter, all packet types)\n",
                (unsigned long long)oStats.uPacketCountGaps);

    // A packet-count gap, and the matching "ahead" in index continuity, mean
    // the network or the kernel dropped a datagram. That is an environment
    // problem, not an ingest fault, so it is reported but does not fail the
    // run -- unlike a read-back or lag-check failure, which would mean the
    // samples we did receive went to the wrong place.

    if (uParseFailures > 0 || oStats.uRejectedWrites > 0 || oStats.uOversizePackets > 0)
        bPass = false;

    for (size_t u = 0; u < vChecks.size(); ++u) {
        const StreamCheck& oCheck = vChecks[u];
        const ChannelRole eRole   = oIngest.roleOf(oCheck.uStreamId);

        std::printf("\n-- stream 0x%08X (%s)\n", oCheck.uStreamId, roleName(eRole));
        std::printf("   data packets           %llu  (%llu samples)\n",
                    (unsigned long long)oCheck.uDataPackets,
                    (unsigned long long)oCheck.uSamples);
        std::printf("   declared size matches  %llu ok / %llu mismatched\n",
                    (unsigned long long)oCheck.uDeclaredWordsOk,
                    (unsigned long long)oCheck.uDeclaredWordsMismatch);
        std::printf("   1. index continuity    %llu exact / %llu ahead / %llu behind\n",
                    (unsigned long long)oCheck.uIndexExact,
                    (unsigned long long)oCheck.uIndexAhead,
                    (unsigned long long)oCheck.uIndexBehind);
        if (oCheck.uIndexAhead > 0)
            std::printf("      first gap at index  %llu\n",
                        (unsigned long long)oCheck.uFirstGapAt);
        std::printf("   2. immediate readback  %llu ok / %llu FAILED\n",
                    (unsigned long long)oCheck.uReadbackOk,
                    (unsigned long long)oCheck.uReadbackBad);
        std::printf("   3. lag check           %llu ok / %llu FAILED / %llu abandoned\n",
                    (unsigned long long)oCheck.uShadowOk,
                    (unsigned long long)oCheck.uShadowBad,
                    (unsigned long long)oCheck.uShadowAbandoned);

        if (oCheck.bCodesSeen) {
            const int32_t iPeak = (oCheck.iCodeMax > -oCheck.iCodeMin)
                                ? oCheck.iCodeMax : -oCheck.iCodeMin;
            const double dPercent = 100.0 * iPeak / 32767.0;

            std::printf("   raw ADC codes (whole run) %d .. %d  (%.1f%% of full scale)\n",
                        oCheck.iCodeMin, oCheck.iCodeMax, dPercent);

            if (iPeak >= 32767) {
                std::printf("      WARNING: clipping. Raise --vita49-fullscale.\n");
            } else if (dPercent < 10.0) {
                // Suggest the value that would put this channel near 80%.
                std::printf("      Under-scaled: %.1f%% of the 16-bit range in use.\n",
                            dPercent);
                std::printf("      For ~80%% on this channel: --vita49-fullscale %.6f\n",
                            dFullScale * (iPeak / (0.8 * 32767.0)));
            }
        }

        if (oCheck.uReadbackBad > 0 || oCheck.uShadowBad > 0 || oCheck.uIndexBehind > 0)
            bPass = false;
        if (oCheck.uDataPackets == 0)
            bPass = false;
    }

    if (vChecks.empty())
        bPass = false;

    std::printf("\n======================================================\n");
    if (bPass) {
        std::printf("PASS -- every sample landed at the index its timestamp implies,\n");
        std::printf("        read back identically, and survived in the ring buffer.\n");
    } else {
        std::printf("FAIL -- see the counters above.\n");
    }

    return bPass ? 0 : 1;
}
