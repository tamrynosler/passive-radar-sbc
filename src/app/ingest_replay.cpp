// ingest_replay -- run a recorded FERS capture through the ingest path.
//
// Two jobs:
//
//   1. File playback. Every stage must be runnable from a recorded file
//      without live FERS. This replays a .pbrcap capture through exactly the
//      same parseVita49Packet -> ChannelIngest -> CircularBuffer path the
//      live probe uses, and is deterministic, so the same file always gives
//      the same answer.
//
//   2. Parser cross-check. With --csv it writes one row per data packet.
//      matlab/compare_vita49_capture.m decodes the same capture with
//      parse_vita49_packet.m and writes the same columns, so diffing the two
//      files proves our decoder agrees with the reference implementation on
//      identical bytes. That is the one correctness dimension the live
//      probe's self-consistency checks cannot reach: if we and MATLAB both
//      unpacked I and Q the wrong way round, every live check would still
//      pass.
//
// Usage:  ingest_replay --file capture.pbrcap [--fullscale F] [--csv OUT.csv]

#include "pbr/CaptureFile.hpp"
#include "pbr/ChannelIngest.hpp"
#include "pbr/CircularBuffer.hpp"
#include "pbr/Vita49Packet.hpp"
#include "pbr/types.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace pbr;

namespace {

constexpr double   kDefaultFullScale = 0.005;
constexpr uint64_t kSampleRateHz     = 204800;
constexpr uint64_t kBufferSeconds    = 2;
constexpr size_t   kDatagramCapacity = 2048;

const char* const kReferenceName    = "ArmasuisseRefRxClean";
const char* const kSurveillanceName = "ArmasuisseSurRxClean";

struct Options {
    std::string sCapturePath;
    std::string sCsvPath;
    double      dFullScale = kDefaultFullScale;
};

void printUsage(const char* pProgram)
{
    std::printf("usage: %s --file CAPTURE [options]\n", pProgram);
    std::printf("  --file FILE      .pbrcap capture to replay (required)\n");
    std::printf("  --fullscale F    must match the --vita49-fullscale used to\n");
    std::printf("                   record the capture (default %g)\n", kDefaultFullScale);
    std::printf("  --csv FILE       write decoded data-packet fields for the\n");
    std::printf("                   MATLAB cross-check\n");
}

bool parseArgs(int argc, char* argv[], Options& oOptions)
{
    for (int i = 1; i < argc; ++i) {
        const std::string sArg = argv[i];
        const bool bHasValue   = (i + 1 < argc);

        if (sArg == "--help" || sArg == "-h") {
            return false;
        } else if (sArg == "--file" && bHasValue) {
            oOptions.sCapturePath = argv[++i];
        } else if (sArg == "--csv" && bHasValue) {
            oOptions.sCsvPath = argv[++i];
        } else if (sArg == "--fullscale" && bHasValue) {
            oOptions.dFullScale = std::atof(argv[++i]);
        } else {
            std::fprintf(stderr, "unrecognised or incomplete argument: %s\n", sArg.c_str());
            return false;
        }
    }

    if (oOptions.sCapturePath.empty()) {
        std::fprintf(stderr, "--file is required\n");
        return false;
    }
    if (oOptions.dFullScale <= 0.0) {
        std::fprintf(stderr, "fullscale must be positive\n");
        return false;
    }
    return true;
}

int32_t readBe16Signed(const uint8_t* p)
{
    const uint16_t uRaw = static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
    return static_cast<int32_t>(static_cast<int16_t>(uRaw));
}

// Same arithmetic as ChannelIngest::absoluteIndex, restated here so the CSV
// carries an index the MATLAB side can check independently.
uint64_t absoluteIndex(uint64_t uSeconds, uint64_t uPicoseconds)
{
    constexpr uint64_t kPsPerSecond = 1000000000000ULL;
    return uSeconds * kSampleRateHz + (uPicoseconds * kSampleRateHz) / kPsPerSecond;
}

// Per-stream index continuity, the same check the live probe runs.
struct StreamCheck {
    uint32_t uStreamId      = 0;
    uint64_t uDataPackets   = 0;
    bool     bHaveExpected  = false;
    uint64_t uExpectedIndex = 0;
    uint64_t uIndexExact    = 0;
    uint64_t uIndexAhead    = 0;
    uint64_t uIndexBehind   = 0;
    bool     bCodesSeen     = false;
    int32_t  iCodeMin       = 0;
    int32_t  iCodeMax       = 0;
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

} // namespace

int main(int argc, char* argv[])
{
    Options oOptions;
    if (!parseArgs(argc, argv, oOptions)) {
        printUsage(argv[0]);
        return 1;
    }

    CaptureReader oReader;
    if (!oReader.open(oOptions.sCapturePath.c_str()))
        return 1;

    ChannelIngestConfig oConfig;
    oConfig.uSampleRateHz     = kSampleRateHz;
    oConfig.dFullScale        = oOptions.dFullScale;
    oConfig.uBufferCapacity   = kBufferSeconds * kSampleRateHz;
    oConfig.sReferenceName    = kReferenceName;
    oConfig.sSurveillanceName = kSurveillanceName;

    ChannelIngest oIngest(oConfig);
    if (!oIngest.isValid()) {
        std::fprintf(stderr, "ingest_replay: ChannelIngest failed to initialise\n");
        return 1;
    }

    std::FILE* pCsv = nullptr;
    if (!oOptions.sCsvPath.empty()) {
        pCsv = std::fopen(oOptions.sCsvPath.c_str(), "w");
        if (pCsv == nullptr) {
            std::fprintf(stderr, "ingest_replay: cannot open %s for writing\n",
                         oOptions.sCsvPath.c_str());
            return 1;
        }
        std::fprintf(pCsv, "record,streamId,packetCount,declaredWords,"
                           "tsSec,tsPs,absIndex,nSamples,i0,q0,i1,q1,i2,q2\n");
    }

    std::printf("ingest_replay\n");
    std::printf("  capture     %s\n", oOptions.sCapturePath.c_str());
    std::printf("  fullscale   %g\n", oOptions.dFullScale);
    if (pCsv != nullptr)
        std::printf("  csv         %s\n", oOptions.sCsvPath.c_str());
    std::printf("\n");

    std::vector<uint8_t>     vDatagram(kDatagramCapacity);
    std::vector<StreamCheck> vChecks;

    uint64_t uRecords      = 0;
    uint64_t uParseFailures = 0;
    bool     bReadError     = false;

    for (;;) {
        size_t uLength = 0;
        bool   bEof    = false;

        if (!oReader.read(vDatagram.data(), vDatagram.size(), uLength, bEof)) {
            if (!bEof)
                bReadError = true;
            break;
        }
        ++uRecords;

        Vita49Packet oPacket;
        if (!parseVita49Packet(vDatagram.data(), uLength, oPacket)) {
            ++uParseFailures;
            continue;
        }

        if (!oIngest.handlePacket(oPacket))
            return 1;

        if (oPacket.eType != Vita49Type::Data)
            continue;

        StreamCheck& oCheck = findOrAddCheck(vChecks, oPacket.uStreamId);
        ++oCheck.uDataPackets;

        const uint64_t uIndex =
            absoluteIndex(oPacket.uTimestampSec, oPacket.uTimestampPs);

        if (!oCheck.bHaveExpected) {
            oCheck.bHaveExpected = true;
        } else if (uIndex == oCheck.uExpectedIndex) {
            ++oCheck.uIndexExact;
        } else if (uIndex > oCheck.uExpectedIndex) {
            ++oCheck.uIndexAhead;
        } else {
            ++oCheck.uIndexBehind;
        }
        oCheck.uExpectedIndex = uIndex + oPacket.uSampleCount;

        // Running raw code extremes, for fullscale calibration.
        for (uint64_t u = 0; u < oPacket.uSampleCount; ++u) {
            const int32_t iI = readBe16Signed(oPacket.pPayload + u * 4);
            const int32_t iQ = readBe16Signed(oPacket.pPayload + u * 4 + 2);
            if (!oCheck.bCodesSeen) {
                oCheck.bCodesSeen = true;
                oCheck.iCodeMin   = iI;
                oCheck.iCodeMax   = iI;
            }
            if (iI < oCheck.iCodeMin) oCheck.iCodeMin = iI;
            if (iI > oCheck.iCodeMax) oCheck.iCodeMax = iI;
            if (iQ < oCheck.iCodeMin) oCheck.iCodeMin = iQ;
            if (iQ > oCheck.iCodeMax) oCheck.iCodeMax = iQ;
        }

        if (pCsv != nullptr) {
            // First three IQ pairs are enough to expose a byte-order or
            // I/Q-orientation disagreement with MATLAB.
            int32_t aiIQ[6] = { 0, 0, 0, 0, 0, 0 };
            const uint64_t uShow = (oPacket.uSampleCount < 3) ? oPacket.uSampleCount : 3;
            for (uint64_t u = 0; u < uShow; ++u) {
                aiIQ[u * 2]     = readBe16Signed(oPacket.pPayload + u * 4);
                aiIQ[u * 2 + 1] = readBe16Signed(oPacket.pPayload + u * 4 + 2);
            }

            std::fprintf(pCsv,
                         "%llu,%u,%u,%u,%llu,%llu,%llu,%llu,%d,%d,%d,%d,%d,%d\n",
                         (unsigned long long)uRecords,
                         oPacket.uStreamId, oPacket.uPacketCount, oPacket.uDeclaredWords,
                         (unsigned long long)oPacket.uTimestampSec,
                         (unsigned long long)oPacket.uTimestampPs,
                         (unsigned long long)uIndex,
                         (unsigned long long)oPacket.uSampleCount,
                         aiIQ[0], aiIQ[1], aiIQ[2], aiIQ[3], aiIQ[4], aiIQ[5]);
        }
    }

    if (pCsv != nullptr)
        std::fclose(pCsv);

    // ---------------------------------------------------------------- report
    const ChannelIngest::Stats& oStats = oIngest.stats();
    bool bPass = !bReadError;

    std::printf("================ ingest_replay summary ================\n");
    std::printf("records replayed        %llu\n", (unsigned long long)uRecords);
    std::printf("parse failures          %llu\n", (unsigned long long)uParseFailures);
    std::printf("context packets         %llu\n", (unsigned long long)oStats.uContextPackets);
    std::printf("data packets            %llu\n", (unsigned long long)oStats.uDataPackets);
    std::printf("samples written         %llu\n", (unsigned long long)oStats.uSamplesWritten);
    std::printf("data before context     %llu\n", (unsigned long long)oStats.uUnresolvedData);
    std::printf("writes rejected (late)  %llu\n", (unsigned long long)oStats.uRejectedWrites);
    std::printf("packet-count gaps       %llu\n", (unsigned long long)oStats.uPacketCountGaps);

    if (uParseFailures > 0 || oStats.uRejectedWrites > 0)
        bPass = false;

    for (size_t u = 0; u < vChecks.size(); ++u) {
        const StreamCheck& oCheck = vChecks[u];
        const ChannelRole  eRole  = oIngest.roleOf(oCheck.uStreamId);

        const char* pRole = (eRole == ChannelRole::Reference)    ? "reference"
                          : (eRole == ChannelRole::Surveillance) ? "surveillance"
                                                                 : "unknown";

        std::printf("\n-- stream 0x%08X (%s)\n", oCheck.uStreamId, pRole);
        std::printf("   data packets        %llu\n", (unsigned long long)oCheck.uDataPackets);
        std::printf("   index continuity    %llu exact / %llu ahead / %llu behind\n",
                    (unsigned long long)oCheck.uIndexExact,
                    (unsigned long long)oCheck.uIndexAhead,
                    (unsigned long long)oCheck.uIndexBehind);
        if (oCheck.bCodesSeen) {
            const int32_t iPeak = (oCheck.iCodeMax > -oCheck.iCodeMin)
                                ? oCheck.iCodeMax : -oCheck.iCodeMin;
            std::printf("   raw ADC codes       %d .. %d  (%.1f%% of full scale)\n",
                        oCheck.iCodeMin, oCheck.iCodeMax, 100.0 * iPeak / 32767.0);
        }

        if (oCheck.uIndexBehind > 0 || oCheck.uDataPackets == 0)
            bPass = false;
    }

    if (vChecks.empty())
        bPass = false;

    std::printf("\n=======================================================\n");
    std::printf("%s\n", bPass ? "PASS -- capture replayed cleanly through the ingest path."
                              : "FAIL -- see the counters above.");
    return bPass ? 0 : 1;
}
