// Unit tests for ChannelIngest::tryExtractCpi. Plain main() + asserts.
//
// NDEBUG is undefined before <cassert> because the project builds Release,
// which would otherwise compile every assert out.
#undef NDEBUG
#include <cassert>

#include "pbr/ChannelIngest.hpp"
#include "pbr/Vita49Packet.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pbr;

namespace {

// Small numbers throughout, so the wrap and overwrite cases are reachable in
// a handful of packets and the arithmetic stays checkable by eye.
constexpr uint64_t kFs      = 1000;   // "sample rate"
constexpr uint64_t kCpi     = 100;    // samples per CPI
constexpr uint64_t kHop     = 100;    // no overlap
constexpr uint64_t kCap     = 400;    // buffer holds 4 CPIs
constexpr uint64_t kPacket  = 10;     // samples per packet
constexpr uint32_t kRefId   = 0x1000;
constexpr uint32_t kSurId   = 0x2000;

const char* const kRefName = "RefRx";
const char* const kSurName = "SurRx";

ChannelIngestConfig makeConfig()
{
    ChannelIngestConfig oConfig;
    oConfig.uSampleRateHz     = kFs;
    oConfig.dFullScale        = 32767.0;   // so one ADC code maps to 1.0
    oConfig.uBufferCapacity   = kCap;
    oConfig.sReferenceName    = kRefName;
    oConfig.sSurveillanceName = kSurName;
    oConfig.uCpiSamples       = kCpi;
    oConfig.uHopSamples       = kHop;
    oConfig.dMinCpiCoverage   = 0.99;
    return oConfig;
}

// ---------------------------------------------------------------------------
// Packet building
// ---------------------------------------------------------------------------

void putBe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void putBe32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);  p[3] = uint8_t(v);
}
void putBe64(uint8_t* p, uint64_t v)
{
    for (size_t u = 0; u < 8; ++u) p[u] = uint8_t(v >> (56 - 8 * u));
}

// Split an absolute sample index into (seconds, picoseconds) such that
// ChannelIngest::absoluteIndex recovers it exactly. Ceiling division is
// required; truncating loses a sample.
void indexToTimestamp(uint64_t uIndex, uint64_t& uSec, uint64_t& uPs)
{
    uSec = uIndex / kFs;
    const uint64_t uRem = uIndex % kFs;
    uPs = (uRem * 1000000000000ULL + kFs - 1) / kFs;
}

void buildHeader(std::vector<uint8_t>& v, uint32_t uType, uint32_t uStreamId,
                 uint64_t uIndex)
{
    uint64_t uSec = 0, uPs = 0;
    indexToTimestamp(uIndex, uSec, uPs);

    v[0] = uint8_t((uType << 4) | 0x0E);
    v[1] = uint8_t((1u << 6) | (2u << 4));    // TSI = UTC, TSF = real-time ps
    putBe16(&v[2], uint16_t(v.size() / 4));
    putBe32(&v[4], uStreamId);
    putBe64(&v[8], kVita49ClassId);
    putBe32(&v[16], uint32_t(uSec));
    putBe64(&v[20], uPs);
}

std::vector<uint8_t> makeContext(uint32_t uStreamId, const char* pName)
{
    const std::string sJson =
        std::string("{\"receiver\":{\"mode\":\"cw\",\"name\":\"") + pName + "\"}}";
    const size_t uPadded = (sJson.size() + 1 + 3) & ~size_t(3);

    std::vector<uint8_t> v(92 + uPadded, 0);
    buildHeader(v, 0x4, uStreamId, 0);

    const double dRate = double(kFs);
    uint64_t uBits = 0;
    std::memcpy(&uBits, &dRate, sizeof(uBits));
    putBe64(&v[44], uBits);
    std::memcpy(&v[92], sJson.c_str(), sJson.size());
    return v;
}

// Sample value for an absolute index, distinct and non-zero so a misplaced or
// missing sample is visible. Kept well inside int16 range.
int16_t codeAt(uint64_t uIndex)
{
    return int16_t(1 + (uIndex % 1000));
}

std::vector<uint8_t> makeData(uint32_t uStreamId, uint64_t uIndex, uint64_t uCount)
{
    std::vector<uint8_t> v(28 + uCount * 4 + 4, 0);
    buildHeader(v, 0x1, uStreamId, uIndex);
    for (uint64_t u = 0; u < uCount; ++u) {
        putBe16(&v[28 + u * 4],     uint16_t(codeAt(uIndex + u)));
        putBe16(&v[28 + u * 4 + 2], uint16_t(codeAt(uIndex + u)));
    }
    return v;
}

void feed(ChannelIngest& oIngest, const std::vector<uint8_t>& v)
{
    Vita49Packet oPacket;
    assert(parseVita49Packet(v.data(), v.size(), oPacket));
    assert(oIngest.handlePacket(oPacket));
}

void resolveRoles(ChannelIngest& oIngest)
{
    feed(oIngest, makeContext(kRefId, kRefName));
    feed(oIngest, makeContext(kSurId, kSurName));
    assert(oIngest.rolesResolved());
}

// Write uCount samples to both channels starting at uIndex, in kPacket chunks.
void feedRange(ChannelIngest& oIngest, uint64_t uIndex, uint64_t uCount)
{
    for (uint64_t u = 0; u < uCount; u += kPacket) {
        const uint64_t uThis = (uCount - u < kPacket) ? (uCount - u) : kPacket;
        feed(oIngest, makeData(kRefId, uIndex + u, uThis));
        feed(oIngest, makeData(kSurId, uIndex + u, uThis));
    }
}

// Every sample in the window must be the one belonging to its absolute index.
void expectWindow(const std::vector<ComplexF>& vc, uint64_t uStart)
{
    for (uint64_t u = 0; u < vc.size(); ++u) {
        const float fExpected = float(codeAt(uStart + u));
        assert(vc[u].real() == fExpected);
        assert(vc[u].imag() == fExpected);
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testNotYetBeforeData()
{
    ChannelIngest oIngest(makeConfig());
    assert(oIngest.isValid());

    std::vector<ComplexF> vcRef(kCpi), vcSur(kCpi);
    uint64_t uStart = 0;

    // No packets at all.
    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart)
           == CpiStatus::NotYet);

    // Context only: roles known, no samples.
    resolveRoles(oIngest);
    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart)
           == CpiStatus::NotYet);

    // One channel only, so there is nothing both can cover.
    feed(oIngest, makeData(kRefId, 5000, kPacket));
    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart)
           == CpiStatus::NotYet);
}

void testPartialWindowIsNotYet()
{
    ChannelIngest oIngest(makeConfig());
    resolveRoles(oIngest);

    std::vector<ComplexF> vcRef(kCpi), vcSur(kCpi);
    uint64_t uStart = 0;

    // One sample short of a whole CPI.
    feedRange(oIngest, 5000, kCpi - kPacket);
    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart)
           == CpiStatus::NotYet);

    // The packet that completes it makes the window available.
    feed(oIngest, makeData(kRefId, 5000 + kCpi - kPacket, kPacket));
    feed(oIngest, makeData(kSurId, 5000 + kCpi - kPacket, kPacket));
    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart)
           == CpiStatus::Ready);
    assert(uStart == 5000);
}

void testSequentialCpisOnTheHopLattice()
{
    ChannelIngest oIngest(makeConfig());
    resolveRoles(oIngest);

    // Three CPIs worth, starting at an index that is not a multiple of kCpi,
    // so the anchor is genuinely taken from the data.
    const uint64_t uBase = 5037;
    feedRange(oIngest, uBase, 3 * kCpi);

    std::vector<ComplexF> vcRef(kCpi), vcSur(kCpi);
    std::vector<uint64_t> vStarts;

    for (;;) {
        uint64_t uStart = 0;
        const CpiStatus eStatus =
            oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart);
        if (eStatus == CpiStatus::NotYet)
            break;
        assert(eStatus == CpiStatus::Ready);

        vStarts.push_back(uStart);
        expectWindow(vcRef, uStart);
        expectWindow(vcSur, uStart);
    }

    assert(vStarts.size() == 3);
    assert(vStarts[0] == uBase);
    for (size_t u = 1; u < vStarts.size(); ++u)
        assert(vStarts[u] - vStarts[u - 1] == kHop);   // on the hop lattice

    const ChannelIngest::Stats& oStats = oIngest.stats();
    assert(oStats.uCpisEmitted == 3);
    assert(oStats.uCpisSkippedCoverage == 0);
    assert(oStats.uCpisOvertaken == 0);
}

void testAnchorUsesTheLaterChannel()
{
    // If one channel starts later, the first CPI must begin where BOTH have
    // data, not where the earlier one does.
    ChannelIngest oIngest(makeConfig());
    resolveRoles(oIngest);

    const uint64_t uRefBase = 2000;
    const uint64_t uSurBase = 2040;

    for (uint64_t u = 0; u < kCpi + 40; u += kPacket)
        feed(oIngest, makeData(kRefId, uRefBase + u, kPacket));
    for (uint64_t u = 0; u < kCpi; u += kPacket)
        feed(oIngest, makeData(kSurId, uSurBase + u, kPacket));

    std::vector<ComplexF> vcRef(kCpi), vcSur(kCpi);
    uint64_t uStart = 0;
    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart)
           == CpiStatus::Ready);
    assert(uStart == uSurBase);
    expectWindow(vcRef, uStart);
    expectWindow(vcSur, uStart);
}

void testGapSkippedForCoverage()
{
    ChannelIngest oIngest(makeConfig());
    resolveRoles(oIngest);

    // Leave a 10-sample hole in the middle of the first window. Ten missing
    // out of a hundred is 90% coverage, below the 99% threshold.
    const uint64_t uBase = 3000;
    feedRange(oIngest, uBase, 40);
    feedRange(oIngest, uBase + 50, kCpi - 50);   // [40, 50) never written
    feedRange(oIngest, uBase + kCpi, kCpi);      // a clean second window

    std::vector<ComplexF> vcRef(kCpi), vcSur(kCpi);
    uint64_t uStart = 0;

    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart)
           == CpiStatus::Skipped);

    // The next window is intact and must come through normally.
    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart)
           == CpiStatus::Ready);
    assert(uStart == uBase + kCpi);
    expectWindow(vcRef, uStart);

    const ChannelIngest::Stats& oStats = oIngest.stats();
    assert(oStats.uCpisSkippedCoverage == 1);
    assert(oStats.uCpisEmitted == 1);
}

void testOvertakenWindowIsSkipped()
{
    ChannelIngest oIngest(makeConfig());
    resolveRoles(oIngest);

    // Anchor at 1000, then jump far enough ahead that the anchored window has
    // been overwritten before anyone asked for it.
    const uint64_t uBase = 1000;
    feedRange(oIngest, uBase, kPacket);
    feedRange(oIngest, uBase + 10 * kCpi, kCpi);

    std::vector<ComplexF> vcRef(kCpi), vcSur(kCpi);
    uint64_t uStart = 0;

    // The first attempt reports the loss rather than returning stale samples.
    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart)
           == CpiStatus::Skipped);
    assert(oIngest.stats().uCpisOvertaken > 0);

    // Drain: whatever comes back must be Ready with correct contents, never
    // a silently wrong window. Note tryExtractCpi clears the start on NotYet,
    // so the last good value has to be kept as we go.
    bool     bSawReady  = false;
    uint64_t uLastReady = 0;

    for (;;) {
        const CpiStatus eStatus =
            oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart);
        if (eStatus == CpiStatus::NotYet)
            break;
        if (eStatus == CpiStatus::Ready) {
            expectWindow(vcRef, uStart);
            expectWindow(vcSur, uStart);
            bSawReady  = true;
            uLastReady = uStart;
        }
    }

    // Recovery must land on real data, and on the original hop lattice.
    assert(bSawReady);
    assert((uLastReady - uBase) % kHop == 0);
    assert(uLastReady == uBase + 10 * kCpi);
}

void testOverlappingHop()
{
    // hop < CPI means overlapping windows. Two CPIs of data at half-CPI hop
    // should yield three windows: 0, 50, 100.
    ChannelIngestConfig oConfig = makeConfig();
    oConfig.uHopSamples = kCpi / 2;

    ChannelIngest oIngest(oConfig);
    assert(oIngest.isValid());
    resolveRoles(oIngest);

    const uint64_t uBase = 200;
    feedRange(oIngest, uBase, 2 * kCpi);

    std::vector<ComplexF> vcRef(kCpi), vcSur(kCpi);
    std::vector<uint64_t> vStarts;

    for (;;) {
        uint64_t uStart = 0;
        const CpiStatus eStatus =
            oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart);
        if (eStatus == CpiStatus::NotYet)
            break;
        assert(eStatus == CpiStatus::Ready);
        vStarts.push_back(uStart);
        expectWindow(vcRef, uStart);
    }

    assert(vStarts.size() == 3);
    assert(vStarts[0] == uBase);
    assert(vStarts[1] == uBase + kCpi / 2);
    assert(vStarts[2] == uBase + kCpi);
}

void testRejectsBadArguments()
{
    ChannelIngest oIngest(makeConfig());
    resolveRoles(oIngest);
    feedRange(oIngest, 500, kCpi);

    std::vector<ComplexF> vcRef(kCpi), vcSur(kCpi);
    uint64_t uStart = 0;

    // Wrong window length.
    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi - 1, uStart)
           == CpiStatus::NotYet);
    // Null destinations.
    assert(oIngest.tryExtractCpi(nullptr, vcSur.data(), kCpi, uStart)
           == CpiStatus::NotYet);
    assert(oIngest.tryExtractCpi(vcRef.data(), nullptr, kCpi, uStart)
           == CpiStatus::NotYet);

    // None of that may have consumed the window.
    assert(oIngest.tryExtractCpi(vcRef.data(), vcSur.data(), kCpi, uStart)
           == CpiStatus::Ready);
    assert(uStart == 500);
}

void testInvalidConfigRejected()
{
    // A CPI larger than the buffer can never be satisfied, so construction
    // must refuse rather than fail silently at extraction time.
    ChannelIngestConfig oConfig = makeConfig();
    oConfig.uCpiSamples = kCap + 1;
    ChannelIngest oTooBig(oConfig);
    assert(!oTooBig.isValid());

    oConfig = makeConfig();
    oConfig.uHopSamples = 0;
    ChannelIngest oNoHop(oConfig);
    assert(!oNoHop.isValid());
}

} // namespace

int main()
{
    testNotYetBeforeData();
    testPartialWindowIsNotYet();
    testSequentialCpisOnTheHopLattice();
    testAnchorUsesTheLaterChannel();
    testGapSkippedForCoverage();
    testOvertakenWindowIsSkipped();
    testOverlappingHop();
    testRejectsBadArguments();
    testInvalidConfigRejected();

    std::printf("test_cpi_extraction: all tests passed\n");
    return 0;
}
