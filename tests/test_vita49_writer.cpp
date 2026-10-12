// Unit tests for pbr::Vita49Writer. Plain main() + asserts, no framework.
//
// The writer is the mirror of the parser, so most tests build a packet and
// parse it back. Two compare byte for byte against real FERS datagrams, which
// pins the layout against reality rather than against our own reading of it.
//
// NDEBUG is undefined deliberately and before <cassert>: the project builds
// Release, which defines NDEBUG and would compile every assert below out.
#undef NDEBUG
#include <cassert>

#include "pbr/ChannelIngest.hpp"
#include "pbr/Vita49Packet.hpp"
#include "pbr/Vita49Writer.hpp"

#include "test_vita49_writer_vectors.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pbr;

namespace {

constexpr uint64_t kFs = 200000;   // the N210 rate the chain will use
constexpr uint64_t kPacket = 342;
constexpr uint32_t kRefId = 1;
constexpr uint32_t kSurId = 2;
const char* const kRefName = "ArmasuisseRefRxClean";
const char* const kSurName = "ArmasuisseSurRxClean";

// ChannelIngest::absoluteIndex(), reproduced: it is private there, and the
// point of this test is that makeTime() is its exact inverse.
uint64_t absoluteIndex(uint64_t uSec, uint64_t uPs, uint64_t uFs)
{
    return uSec * uFs + (uPs * uFs) / 1000000000000ULL;
}

int16_t be16Signed(const uint8_t* p)
{
    return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

Vita49StreamInfo makeInfo(uint32_t uStreamId, const char* pName, unsigned uChannel)
{
    Vita49StreamInfo oInfo;
    oInfo.uStreamId      = uStreamId;
    oInfo.uReceiverId    = uStreamId;
    oInfo.sReceiverName  = pName;
    oInfo.dSampleRateHz  = double(kFs);
    oInfo.dCentreFreqHz  = 89e6;
    oInfo.dBandwidthHz   = double(kFs);
    oInfo.dAdcFullScale  = 1.0;
    oInfo.dGainDb        = 0.0;
    oInfo.uAdcBits       = 14;
    oInfo.uChannel       = uChannel;
    oInfo.bExtRef        = false;
    oInfo.sDevice        = "USRP N210 BasicRX";
    oInfo.sDriverVersion = "3.10.3";
    oInfo.sSessionId     = "cafe1234";
    return oInfo;
}

// A distinct non-zero code for every absolute index, well inside int16, so a
// misplaced or missing sample is visible. Q is the negative of I.
int16_t codeAt(uint64_t uIndex)
{
    return int16_t(1 + (uIndex % 30000));
}

void fillSamples(std::vector<int16_t>& v, uint64_t uIndex, uint64_t uCount)
{
    v.resize(size_t(2 * uCount));
    for (uint64_t u = 0; u < uCount; ++u) {
        v[size_t(2 * u)]     = codeAt(uIndex + u);
        v[size_t(2 * u + 1)] = int16_t(-codeAt(uIndex + u));
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// The design note's 6.2 argument, executed: for every rate the N210 might run
// at, and for the FERS rate, makeTime() followed by absoluteIndex() is the
// identity, including at r = 0 and r = fs - 1 where floor would fail.
void testMakeTimeIsExactInverse()
{
    const uint64_t aFs[] = { 200000, 204800, 250000, 160000, 1000000, 10000000 };
    uint64_t uState = 0x2545F4914F6CDD1DULL;

    for (size_t i = 0; i < sizeof(aFs) / sizeof(aFs[0]); ++i) {
        const uint64_t uFs   = aFs[i];
        const uint64_t uBase = 1789572208ULL * uFs;   // a 2026 epoch second

        const uint64_t aIdx[] = { 0, 1, uFs - 1, uFs, uFs + 1,
                                  uBase, uBase + 1, uBase + uFs - 1, uBase + uFs };
        for (size_t k = 0; k < sizeof(aIdx) / sizeof(aIdx[0]); ++k) {
            const Vita49Time oT = makeTime(aIdx[k], uFs);
            assert(oT.uPicoseconds < 1000000000000ULL);
            assert(absoluteIndex(oT.uSeconds, oT.uPicoseconds, uFs) == aIdx[k]);
        }

        for (int k = 0; k < 5000; ++k) {
            uState = uState * 6364136223846793005ULL + 1442695040888963407ULL;
            const uint64_t uIdx = uBase + (uState >> 11) % (1000 * uFs);
            const Vita49Time oT = makeTime(uIdx, uFs);
            assert(oT.uPicoseconds < 1000000000000ULL);
            assert(absoluteIndex(oT.uSeconds, oT.uPicoseconds, uFs) == uIdx);
        }
    }

    // FERS rounds its picoseconds differently (llround of a long double), so
    // its field is not bit-identical to ours, but both must decode to the same
    // index. The captured packet: 0x6AAAB470 s + 0x5E2ECB4800 ps at 204800 Hz.
    const uint64_t uFersIndex = absoluteIndex(0x6AAAB470u, 0x5E2ECB4800ULL, 204800);
    const Vita49Time oOurs = makeTime(uFersIndex, 204800);
    assert(oOurs.uSeconds == 0x6AAAB470u);
    assert(absoluteIndex(oOurs.uSeconds, oOurs.uPicoseconds, 204800) == uFersIndex);
    assert(uFersIndex % 204800 == 82844);

    // Degenerate rate.
    const Vita49Time oZero = makeTime(12345, 0);
    assert(oZero.uSeconds == 0 && oZero.uPicoseconds == 0);
}

void testDataPacketRoundTrip()
{
    const uint64_t uIndex = 1789572208ULL * kFs + 777;
    std::vector<int16_t> vSamples;
    fillSamples(vSamples, uIndex, kPacket);
    // Include the extremes, which a forgotten sign extension would mangle.
    vSamples[0] = -32768;
    vSamples[1] =  32767;

    std::vector<uint8_t> vOut(2048, 0xAA);
    size_t uLength = 0;
    assert(writeVita49Data(vOut.data(), vOut.size(), kSurId, 7, makeTime(uIndex, kFs),
                           vSamples.data(), kPacket, false, true, uLength));
    assert(uLength == 1400);   // FERS's datagram size for 342 samples

    Vita49Packet oPacket;
    assert(parseVita49Packet(vOut.data(), uLength, oPacket));
    assert(oPacket.eType          == Vita49Type::Data);
    assert(oPacket.uStreamId      == kSurId);
    assert(oPacket.uClassId       == kVita49ClassId);
    assert(oPacket.uPacketCount   == 7u);
    assert(oPacket.uTsi           == 1u);
    assert(oPacket.uTsf           == 2u);
    assert(oPacket.uDeclaredWords == 350u);
    assert(oPacket.uSampleCount   == kPacket);
    assert(absoluteIndex(oPacket.uTimestampSec, oPacket.uTimestampPs, kFs) == uIndex);

    // Not locked, sample loss: every enable bit, valid data, loss.
    assert(oPacket.uTrailerRaw == 0xF8004800u);

    for (uint64_t u = 0; u < kPacket; ++u) {
        assert(be16Signed(oPacket.pPayload + 4 * u)     == vSamples[size_t(2 * u)]);
        assert(be16Signed(oPacket.pPayload + 4 * u + 2) == vSamples[size_t(2 * u + 1)]);
    }

    // Locked, no loss: FERS's usual trailer.
    assert(writeVita49Data(vOut.data(), vOut.size(), kRefId, 0, makeTime(uIndex, kFs),
                           vSamples.data(), 4, true, false, uLength));
    assert(uLength == 48);
    assert(be32(vOut.data() + 44) == 0xF800E000u);

    // Zero samples is legal: header plus trailer.
    assert(writeVita49Data(vOut.data(), vOut.size(), kRefId, 0, makeTime(uIndex, kFs),
                           nullptr, 0, true, false, uLength));
    assert(uLength == 32);
}

// Build the captured FERS data packet from its own decoded samples and
// compare all 1400 bytes. Nonzero samples throughout, so a missing byte swap
// cannot hide.
void testDataPacketMatchesFers()
{
    const uint8_t* pFers = fers_vectors::kFersDataPacket;
    const size_t   uFersLength = sizeof(fers_vectors::kFersDataPacket);
    assert(uFersLength == 1400);

    Vita49Packet oFers;
    assert(parseVita49Packet(pFers, uFersLength, oFers));
    assert(oFers.eType == Vita49Type::Data);
    assert(oFers.uSampleCount == kPacket);
    assert(oFers.uTrailerRaw == 0xF800E000u);

    std::vector<int16_t> vSamples(2 * kPacket);
    size_t uNonZero = 0;
    for (uint64_t u = 0; u < kPacket; ++u) {
        vSamples[size_t(2 * u)]     = be16Signed(oFers.pPayload + 4 * u);
        vSamples[size_t(2 * u + 1)] = be16Signed(oFers.pPayload + 4 * u + 2);
        if (vSamples[size_t(2 * u)] != 0 || vSamples[size_t(2 * u + 1)] != 0)
            ++uNonZero;
    }
    assert(uNonZero > 250);

    Vita49Time oTime;
    oTime.uSeconds     = uint32_t(oFers.uTimestampSec);
    oTime.uPicoseconds = oFers.uTimestampPs;

    std::vector<uint8_t> vOut(2048, 0);
    size_t uLength = 0;
    assert(writeVita49Data(vOut.data(), vOut.size(), oFers.uStreamId,
                           uint8_t(oFers.uPacketCount), oTime,
                           vSamples.data(), kPacket, true, false, uLength));
    assert(uLength == uFersLength);
    assert(std::memcmp(vOut.data(), pFers, uFersLength) == 0);
}

// The fixed 92 bytes of the captured context packet, given FERS's inputs.
// The size field (bytes 2 and 3) depends on the JSON length and is skipped.
void testContextPacketMatchesFers()
{
    const uint8_t* pFers = fers_vectors::kFersContextHead;
    assert(sizeof(fers_vectors::kFersContextHead) == 92);

    Vita49StreamInfo oInfo = makeInfo(0x53F11BABu, kSurName, 1);
    oInfo.uReceiverId   = 844424930131982ULL;
    oInfo.dSampleRateHz = 204800.0;
    oInfo.dCentreFreqHz = 89e6;
    oInfo.dBandwidthHz  = 102400.0;
    oInfo.dAdcFullScale = 0.005;

    Vita49Time oTime;
    oTime.uSeconds     = 0x6AAAB470u;
    oTime.uPicoseconds = 0x5E2ECB4800ULL;

    std::vector<uint8_t> vOut(2048, 0);
    size_t uLength = 0;
    assert(writeVita49Context(vOut.data(), vOut.size(), oInfo, 0, oTime, true, 0, uLength));
    assert(uLength > 92 && uLength % 4 == 0);

    assert(std::memcmp(vOut.data(), pFers, 2) == 0);
    assert(std::memcmp(vOut.data() + 4, pFers + 4, 92 - 4) == 0);

    // And it parses as a context packet with the right rate.
    Vita49Packet oPacket;
    assert(parseVita49Packet(vOut.data(), uLength, oPacket));
    assert(oPacket.eType == Vita49Type::Context);
    assert(oPacket.dSampleRate == 204800.0);
}

void testContextPacketParsesAndNames()
{
    const Vita49StreamInfo oInfo = makeInfo(kRefId, kRefName, 0);
    const uint64_t uIndex = 1789572208ULL * kFs;

    std::vector<uint8_t> vOut(2048, 0xAA);
    size_t uLength = 0;
    assert(writeVita49Context(vOut.data(), vOut.size(), oInfo, 3, makeTime(uIndex, kFs),
                              false, kVita49FlagStreamOpen, uLength));

    Vita49Packet oPacket;
    assert(parseVita49Packet(vOut.data(), uLength, oPacket));
    assert(oPacket.eType        == Vita49Type::Context);
    assert(oPacket.uStreamId    == kRefId);
    assert(oPacket.uPacketCount == 3u);
    assert(oPacket.dSampleRate  == double(kFs));
    assert(absoluteIndex(oPacket.uTimestampSec, oPacket.uTimestampPs, kFs) == uIndex);
    assert(oPacket.uJsonLength > 0);
    assert(oPacket.pJson[oPacket.uJsonLength] == '\0');

    // Not locked, no loss: enables, valid data only.
    assert(be32(vOut.data() + 32) == 0xF8004000u);

    // Exactly what ChannelIngest::handleContext does.
    const size_t uReceiverAt = findJsonKey(oPacket.pJson, oPacket.uJsonLength, "receiver");
    assert(uReceiverAt < oPacket.uJsonLength);
    std::string sValue;
    assert(findJsonString(oPacket.pJson, oPacket.uJsonLength, "name", sValue, uReceiverAt));
    assert(sValue == kRefName);
    assert(findJsonString(oPacket.pJson, oPacket.uJsonLength, "mode", sValue, uReceiverAt));
    assert(sValue == "cw");
    // Unanchored too: no other "name" key may come first.
    assert(findJsonString(oPacket.pJson, oPacket.uJsonLength, "name", sValue, 0));
    assert(sValue == kRefName);

    const std::string sJson(oPacket.pJson, oPacket.uJsonLength);
    assert(sJson.find("\"context_flags\":8") != std::string::npos);
    assert(findJsonString(oPacket.pJson, oPacket.uJsonLength, "session", sValue, uReceiverAt));
    assert(sValue == "cafe1234");
    assert(sJson.find("\"schema\":\"fers-vita49-context-v1\"") != std::string::npos);

    // Sample-loss flag sets both the JSON flag and the state indicator bit.
    assert(writeVita49Context(vOut.data(), vOut.size(), oInfo, 4, makeTime(uIndex, kFs),
                              true, kVita49FlagSampleLoss | kVita49FlagStreamClose, uLength));
    assert(be32(vOut.data() + 32) == 0xF800E800u);
    assert(std::string(reinterpret_cast<const char*>(vOut.data()) + 92)
               .find("\"context_flags\":20") != std::string::npos);
}

void testRejects()
{
    const uint64_t uIndex = 1789572208ULL * kFs;
    std::vector<int16_t> vSamples;
    fillSamples(vSamples, uIndex, 513);
    std::vector<uint8_t> vOut(2048, 0);
    size_t uLength = 0;

    std::fprintf(stderr, "-- expected CHECK failures follow --\n");

    // Buffer too small for the data packet.
    assert(!writeVita49Data(vOut.data(), 1399, kRefId, 0, makeTime(uIndex, kFs),
                            vSamples.data(), 342, false, false, uLength));
    assert(uLength == 0);

    // More samples than the ingest accepts.
    assert(!writeVita49Data(vOut.data(), vOut.size(), kRefId, 0, makeTime(uIndex, kFs),
                            vSamples.data(), 513, false, false, uLength));

    // Picoseconds not below one second.
    Vita49Time oBad;
    oBad.uSeconds     = 1;
    oBad.uPicoseconds = 1000000000000ULL;
    assert(!writeVita49Data(vOut.data(), vOut.size(), kRefId, 0, oBad,
                            vSamples.data(), 4, false, false, uLength));

    // Null output.
    assert(!writeVita49Data(nullptr, 2048, kRefId, 0, makeTime(uIndex, kFs),
                            vSamples.data(), 4, false, false, uLength));

    // A name the JSON could not carry, and an empty one.
    Vita49StreamInfo oInfo = makeInfo(kRefId, "Ref\"Rx", 0);
    assert(!writeVita49Context(vOut.data(), vOut.size(), oInfo, 0, makeTime(uIndex, kFs),
                               false, 0, uLength));
    oInfo.sReceiverName = "";
    assert(!writeVita49Context(vOut.data(), vOut.size(), oInfo, 0, makeTime(uIndex, kFs),
                               false, 0, uLength));

    // Context buffer too small.
    oInfo.sReceiverName = kRefName;
    assert(!writeVita49Context(vOut.data(), 100, oInfo, 0, makeTime(uIndex, kFs),
                               false, 0, uLength));

    std::fprintf(stderr, "-- end of expected failures --\n");
}

// ---------------------------------------------------------------------------
// End to end: a two-stream sequence, exactly as the driver emits it, through
// a real ChannelIngest. Roles resolve from the context packets, the packet
// counters show no gaps, both CPIs come out at the expected start, and a
// simulated overflow (a jump of 1000 indices) reads back as zeros in place
// rather than shifting everything after it.
// ---------------------------------------------------------------------------

struct TestStream {
    uint32_t uStreamId;
    Vita49StreamInfo oInfo;
    uint8_t  uPacketCount;
    uint64_t uLastContextIndex;
};

void feed(ChannelIngest& oIngest, const uint8_t* pBytes, size_t uLength)
{
    Vita49Packet oPacket;
    assert(parseVita49Packet(pBytes, uLength, oPacket));
    assert(oIngest.handlePacket(oPacket));
}

void sendContext(ChannelIngest& oIngest, TestStream& oStream, uint64_t uIndex, uint32_t uFlags,
                 std::vector<uint8_t>& vBuf)
{
    size_t uLength = 0;
    assert(writeVita49Context(vBuf.data(), vBuf.size(), oStream.oInfo, oStream.uPacketCount,
                              makeTime(uIndex, kFs), false, uFlags, uLength));
    oStream.uPacketCount      = uint8_t((oStream.uPacketCount + 1) & 0x0F);
    oStream.uLastContextIndex = uIndex;
    feed(oIngest, vBuf.data(), uLength);
}

void sendData(ChannelIngest& oIngest, TestStream& oStream, uint64_t uIndex, uint64_t uCount,
              bool bLoss, std::vector<uint8_t>& vBuf, std::vector<int16_t>& vSamples)
{
    fillSamples(vSamples, uIndex, uCount);
    size_t uLength = 0;
    assert(writeVita49Data(vBuf.data(), vBuf.size(), oStream.uStreamId, oStream.uPacketCount,
                           makeTime(uIndex, kFs), vSamples.data(), uCount, false, bLoss, uLength));
    oStream.uPacketCount = uint8_t((oStream.uPacketCount + 1) & 0x0F);
    feed(oIngest, vBuf.data(), uLength);
}

void testIngestEndToEnd()
{
    const uint64_t kCpi = kFs;              // 1 s
    const uint64_t kCap = 3 * kFs;

    ChannelIngestConfig oConfig;
    oConfig.uSampleRateHz     = kFs;
    oConfig.dFullScale        = 32767.0;    // one code = 1.0
    oConfig.uBufferCapacity   = kCap;
    oConfig.sReferenceName    = kRefName;
    oConfig.sSurveillanceName = kSurName;
    oConfig.uCpiSamples       = kCpi;
    oConfig.uHopSamples       = kCpi;
    oConfig.dMinCpiCoverage   = 0.99;

    ChannelIngest oIngest(oConfig);
    assert(oIngest.isValid());

    TestStream aStreams[2];
    aStreams[0].uStreamId = kRefId;
    aStreams[0].oInfo     = makeInfo(kRefId, kRefName, 0);
    aStreams[1].uStreamId = kSurId;
    aStreams[1].oInfo     = makeInfo(kSurId, kSurName, 1);
    for (int i = 0; i < 2; ++i) {
        aStreams[i].uPacketCount      = 0;
        aStreams[i].uLastContextIndex = 0;
    }

    std::vector<uint8_t> vBuf(2048);
    std::vector<int16_t> vSamples;

    const uint64_t uI0       = 1789572208ULL * kFs + 12345;   // not on a second boundary
    const uint64_t uGapAt    = uI0 + kCpi + 50000;             // inside the second CPI
    const uint64_t uGapLen   = 1000;
    const uint64_t uEnd      = uI0 + 2 * kCpi + 5000;

    // Open, as the driver does: a context per stream before any data.
    for (int i = 0; i < 2; ++i)
        sendContext(oIngest, aStreams[i], uI0, kVita49FlagStreamOpen, vBuf);
    assert(oIngest.rolesResolved());
    assert(oIngest.roleOf(kRefId) == ChannelRole::Reference);
    assert(oIngest.roleOf(kSurId) == ChannelRole::Surveillance);

    uint64_t uIndex   = uI0;
    bool     bGapDone = false;
    bool     bLoss    = false;
    while (uIndex < uEnd) {
        if (!bGapDone && uIndex >= uGapAt) {
            // The overflow: the source's tick jumps ahead and the driver
            // re-anchors to it, flagging the next packets.
            uIndex  += uGapLen;
            bGapDone = true;
            bLoss    = true;
        }
        for (int i = 0; i < 2; ++i) {
            if (uIndex - aStreams[i].uLastContextIndex >= kFs)
                sendContext(oIngest, aStreams[i], uIndex,
                            bLoss ? kVita49FlagSampleLoss : 0, vBuf);
            sendData(oIngest, aStreams[i], uIndex, kPacket, bLoss, vBuf, vSamples);
        }
        bLoss   = false;
        uIndex += kPacket;
    }

    const ChannelIngest::Stats& oStats = oIngest.stats();
    assert(oStats.uPacketCountGaps == 0);
    assert(oStats.uUnresolvedData  == 0);
    assert(oStats.uRejectedWrites  == 0);
    assert(oStats.uOversizePackets == 0);
    assert(oStats.uContextPackets  >= 6);   // 2 open + 2 heartbeats per second

    std::vector<ComplexF> vcRef(static_cast<size_t>(kCpi));
    std::vector<ComplexF> vcSur(static_cast<size_t>(kCpi));
    uint64_t uCpiStart = 0;

    // First CPI: anchored at the first sample, every value in place.
    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), nullptr, kCpi, uCpiStart) == CpiStatus::Ready);
    assert(uCpiStart == uI0);
    for (uint64_t u = 0; u < kCpi; ++u) {
        const float fExpected = float(codeAt(uI0 + u));
        assert(vcRef[size_t(u)] == ComplexF(fExpected, -fExpected));
        assert(vcSur[size_t(u)] == ComplexF(fExpected, -fExpected));
    }

    // Second CPI: the gap is 0.5% of the window, under the 1% coverage limit,
    // so it is handed over; the missing stretch is zero and what follows it
    // is at the index its timestamp says, not shifted back by the gap.
    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), nullptr, kCpi, uCpiStart) == CpiStatus::Ready);
    assert(uCpiStart == uI0 + kCpi);

    // The gap begins at the first index the driver never sent: the packet
    // boundary at or after uGapAt.
    uint64_t uGapStart = uI0;
    while (uGapStart < uGapAt) uGapStart += kPacket;
    for (uint64_t u = 0; u < kCpi; ++u) {
        const uint64_t uAbs = uCpiStart + u;
        const bool bInGap = (uAbs >= uGapStart && uAbs < uGapStart + uGapLen);
        const float fExpected = bInGap ? 0.0f : float(codeAt(uAbs));
        assert(vcRef[size_t(u)] == ComplexF(fExpected, -fExpected));
        assert(vcSur[size_t(u)] == ComplexF(fExpected, -fExpected));
    }

    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), nullptr, kCpi, uCpiStart) == CpiStatus::NotYet);
    assert(oIngest.stats().uCpisEmitted == 2);
    assert(oIngest.stats().uCpisSkippedCoverage == 0);
}

} // namespace

int main()
{
    testMakeTimeIsExactInverse();
    testDataPacketRoundTrip();
    testDataPacketMatchesFers();
    testContextPacketMatchesFers();
    testContextPacketParsesAndNames();
    testRejects();
    testIngestEndToEnd();

    std::printf("test_vita49_writer: all tests passed\n");
    return 0;
}
