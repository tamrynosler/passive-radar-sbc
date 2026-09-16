#include "pbr/ChannelIngest.hpp"

#include "pbr/check.hpp"

#include <cassert>
#include <cstring>

namespace pbr {

namespace {

// Read a big-endian int16 from two bytes, preserving sign.
int32_t readBe16Signed(const uint8_t* p)
{
    const uint16_t uRaw = static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
    return static_cast<int32_t>(static_cast<int16_t>(uRaw));
}

bool isJsonSpace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

} // namespace

// ---------------------------------------------------------------------------
// Minimal JSON field extraction. See the header for why this is not a parser
// and what would replace it.
// ---------------------------------------------------------------------------

size_t findJsonKey(const char* pJson, size_t uLength, const char* pKey)
{
    if (pJson == nullptr || pKey == nullptr)
        return uLength;

    const size_t uKeyLength = std::strlen(pKey);
    if (uKeyLength == 0 || uLength < uKeyLength + 2)
        return uLength;

    // Look for the key including its surrounding quotes, so "name" does not
    // match a value that happens to contain the word.
    for (size_t u = 0; u + uKeyLength + 2 <= uLength; ++u) {
        if (pJson[u] != '"')
            continue;
        if (std::memcmp(pJson + u + 1, pKey, uKeyLength) != 0)
            continue;
        if (pJson[u + 1 + uKeyLength] != '"')
            continue;
        return u + uKeyLength + 2;
    }

    return uLength;
}

bool findJsonString(const char* pJson, size_t uLength, const char* pKey,
                    std::string& sValueOut, size_t uSearchFrom)
{
    sValueOut.clear();

    CHECK(pJson != nullptr, "findJsonString with null JSON");
    CHECK(pKey != nullptr, "findJsonString with null key");
    CHECK(uSearchFrom <= uLength, "findJsonString search offset past the end");

    const size_t uAfterKey =
        findJsonKey(pJson + uSearchFrom, uLength - uSearchFrom, pKey);
    if (uAfterKey >= uLength - uSearchFrom)
        return false;   // key absent; not an error worth logging

    size_t u = uSearchFrom + uAfterKey;

    while (u < uLength && isJsonSpace(pJson[u]))
        ++u;
    if (u >= uLength || pJson[u] != ':')
        return false;
    ++u;
    while (u < uLength && isJsonSpace(pJson[u]))
        ++u;
    if (u >= uLength || pJson[u] != '"')
        return false;
    ++u;

    const size_t uValueStart = u;
    while (u < uLength && pJson[u] != '"')
        ++u;
    if (u >= uLength)
        return false;   // unterminated string

    sValueOut.assign(pJson + uValueStart, u - uValueStart);
    return true;
}

// ---------------------------------------------------------------------------
// ChannelIngest
// ---------------------------------------------------------------------------

ChannelIngest::ChannelIngest(const ChannelIngestConfig& oConfig)
    : mConfig(oConfig),
      mdCodeToVolts(0.0),
      mRefBuffer(oConfig.uBufferCapacity),
      mSurBuffer(oConfig.uBufferCapacity),
      mbValid(false)
{
    std::memset(&mStats, 0, sizeof(mStats));

    if (!mRefBuffer.isValid() || !mSurBuffer.isValid()) {
        std::fprintf(stderr, "ChannelIngest: channel buffer allocation failed\n");
        return;
    }
    if (oConfig.uSampleRateHz == 0) {
        std::fprintf(stderr, "ChannelIngest: sample rate must be non-zero\n");
        return;
    }
    if (oConfig.sReferenceName.empty() || oConfig.sSurveillanceName.empty()) {
        std::fprintf(stderr, "ChannelIngest: receiver names must be configured\n");
        return;
    }
    if (oConfig.sReferenceName == oConfig.sSurveillanceName) {
        std::fprintf(stderr, "ChannelIngest: reference and surveillance names are identical\n");
        return;
    }

    // Raw int16 codes map to physical units as code / 32767 * fullscale.
    mdCodeToVolts = oConfig.dFullScale / 32767.0;

    mbValid = true;
}

ChannelIngest::StreamState* ChannelIngest::findStream(uint32_t uStreamId)
{
    for (size_t u = 0; u < mvStreams.size(); ++u) {
        if (mvStreams[u].uStreamId == uStreamId)
            return &mvStreams[u];
    }
    return nullptr;
}

ChannelIngest::StreamState& ChannelIngest::findOrAddStream(uint32_t uStreamId)
{
    StreamState* pExisting = findStream(uStreamId);
    if (pExisting != nullptr)
        return *pExisting;

    StreamState oNew;
    oNew.uStreamId        = uStreamId;
    oNew.eRole            = ChannelRole::Unknown;
    oNew.bHavePacketCount = false;
    oNew.uLastPacketCount = 0;
    oNew.bModeReported    = false;
    mvStreams.push_back(oNew);

    return mvStreams.back();
}

ChannelRole ChannelIngest::roleOf(uint32_t uStreamId) const
{
    for (size_t u = 0; u < mvStreams.size(); ++u) {
        if (mvStreams[u].uStreamId == uStreamId)
            return mvStreams[u].eRole;
    }
    return ChannelRole::Unknown;
}

bool ChannelIngest::rolesResolved() const
{
    bool bHaveRef = false;
    bool bHaveSur = false;
    for (size_t u = 0; u < mvStreams.size(); ++u) {
        if (mvStreams[u].eRole == ChannelRole::Reference)    bHaveRef = true;
        if (mvStreams[u].eRole == ChannelRole::Surveillance) bHaveSur = true;
    }
    return bHaveRef && bHaveSur;
}

const CircularBuffer& ChannelIngest::buffer(ChannelRole eRole) const
{
    return (eRole == ChannelRole::Surveillance) ? mSurBuffer : mRefBuffer;
}

uint64_t ChannelIngest::absoluteIndex(uint64_t uSeconds, uint64_t uPicoseconds) const
{
    // Absolute sample index of a packet's first sample:
    //
    //     index = seconds * fs  +  (picoseconds * fs) / 1e12
    //
    // Integer throughout, so the result is exact. Two details matter.
    //
    // Multiply before dividing. Integer division would make picoseconds / 1e12
    // zero every time, discarding the whole fractional part.
    //
    // Headroom. seconds * fs is about 3.7e14 at current epoch values, and
    // picoseconds * fs peaks just under 2.05e17 given the parser guarantees
    // picoseconds < 1e12. Both sit far inside uint64_t's 1.8e19 ceiling.
    //
    // Division truncates rather than rounds, so a timestamp landing mid-sample
    // resolves to the sample already in progress; the error is bounded by one
    // sample period. FERS timestamps land on sample boundaries in cw_mode, and
    // the checkpoint catches it if they do not: consecutive packets from one
    // stream must be exactly uSampleCount apart, and truncation drift would
    // show up as an occasional off-by-one in that spacing.
    constexpr uint64_t kPicosecondsPerSecond = 1000000000000ULL;

    // Guaranteed by parseVita49Packet; restated here because the overflow
    // headroom above depends on it.
    assert(uPicoseconds < kPicosecondsPerSecond);

    const uint64_t uFs = mConfig.uSampleRateHz;

    return uSeconds * uFs + (uPicoseconds * uFs) / kPicosecondsPerSecond;
}

bool ChannelIngest::handlePacket(const Vita49Packet& oPacket)
{
    CHECK(mbValid, "handlePacket on an invalid ChannelIngest");

    // The mod-16 packet counter advances for EVERY packet in a stream, context
    // packets included, so it has to be tracked here rather than in
    // handleData(). Counting only data packets reported a spurious gap each
    // time a context heartbeat consumed a counter value, which at FERS's 1 Hz
    // heartbeat looked exactly like one dropped packet per second.
    {
        StreamState& oStream = findOrAddStream(oPacket.uStreamId);
        if (oStream.bHavePacketCount) {
            const uint32_t uExpected = (oStream.uLastPacketCount + 1u) & 0x0Fu;
            if (oPacket.uPacketCount != uExpected)
                ++mStats.uPacketCountGaps;
        }
        oStream.uLastPacketCount = oPacket.uPacketCount;
        oStream.bHavePacketCount = true;
    }

    switch (oPacket.eType) {
    case Vita49Type::Context:
        ++mStats.uContextPackets;
        return handleContext(oPacket);

    case Vita49Type::Data:
        ++mStats.uDataPackets;
        return handleData(oPacket);

    case Vita49Type::Other:
    default:
        ++mStats.uOtherPackets;
        return true;
    }
}

bool ChannelIngest::handleContext(const Vita49Packet& oPacket)
{
    StreamState& oStream = findOrAddStream(oPacket.uStreamId);

    if (oPacket.pJson == nullptr || oPacket.uJsonLength == 0)
        return true;   // heartbeat with no metadata; nothing to learn

    // Anchor the search inside the "receiver" object where possible, so a
    // "name" belonging to some other object cannot win.
    const size_t uReceiverAt =
        findJsonKey(oPacket.pJson, oPacket.uJsonLength, "receiver");
    const size_t uSearchFrom =
        (uReceiverAt < oPacket.uJsonLength) ? uReceiverAt : 0;

    std::string sName;
    if (findJsonString(oPacket.pJson, oPacket.uJsonLength, "name", sName, uSearchFrom)) {
        ChannelRole eRole = ChannelRole::Unknown;
        if (sName == mConfig.sReferenceName)
            eRole = ChannelRole::Reference;
        else if (sName == mConfig.sSurveillanceName)
            eRole = ChannelRole::Surveillance;

        if (eRole != ChannelRole::Unknown && oStream.eRole != eRole) {
            oStream.eRole = eRole;
            std::fprintf(stderr, "ChannelIngest: stream 0x%08X is \"%s\" (%s)\n",
                         oPacket.uStreamId, sName.c_str(),
                         (eRole == ChannelRole::Reference) ? "reference" : "surveillance");
        }
    }

    // Report each stream's mode once. A receiver in pulsed_mode does not
    // stream incrementally -- it renders everything and dumps it in one
    // burst -- so seeing anything other than "cw" here explains a stream that
    // appears dead and then floods.
    if (!oStream.bModeReported) {
        std::string sMode;
        if (findJsonString(oPacket.pJson, oPacket.uJsonLength, "mode", sMode, uSearchFrom)) {
            oStream.bModeReported = true;
            std::fprintf(stderr, "ChannelIngest: stream 0x%08X mode is \"%s\"%s\n",
                         oPacket.uStreamId, sMode.c_str(),
                         (sMode == "cw") ? "" : "  <-- expected \"cw\" for live streaming");
        }
    }

    // The context packet also carries the sample rate; disagreeing with the
    // configured rate would corrupt every absolute index we compute.
    if (oPacket.dSampleRate > 0.0) {
        const double dConfigured = static_cast<double>(mConfig.uSampleRateHz);
        if (oPacket.dSampleRate != dConfigured) {
            std::fprintf(stderr,
                         "ChannelIngest: stream 0x%08X reports fs = %.3f Hz but %.3f Hz is configured\n",
                         oPacket.uStreamId, oPacket.dSampleRate, dConfigured);
        }
    }

    return true;
}

bool ChannelIngest::handleData(const Vita49Packet& oPacket)
{
    CHECK(oPacket.pPayload != nullptr, "data packet with a null payload");

    StreamState& oStream = findOrAddStream(oPacket.uStreamId);

    if (oStream.eRole == ChannelRole::Unknown) {
        // No context packet has named this stream yet, so we do not know which
        // buffer the samples belong in and have to drop them.
        //
        // The MATLAB reference instead creates a buffer per stream ID on first
        // sight and attaches the role later. That costs a buffer per unknown
        // stream for a window that is normally milliseconds, since FERS emits
        // a context packet when each stream opens. If this counter is ever
        // more than a handful, revisit that decision.
        ++mStats.uUnresolvedData;
        return true;
    }

    if (oPacket.uSampleCount > kMaxSamplesPerPacket) {
        ++mStats.uOversizePackets;
        std::fprintf(stderr,
                     "ChannelIngest: packet carries %llu samples, over the %llu limit; dropped\n",
                     static_cast<unsigned long long>(oPacket.uSampleCount),
                     static_cast<unsigned long long>(kMaxSamplesPerPacket));
        return true;
    }

    if (oPacket.uSampleCount == 0)
        return true;

    const bool bIsSurveillance = (oStream.eRole == ChannelRole::Surveillance);

    // Convert big-endian int16 pairs to physical units, scaling by
    // fullscale / 32767 as they go.
    //
    // Nothing else happens in this loop on purpose. ADC code-range tracking
    // used to live here, but a running min/max is per-sample work in the
    // production ingest path and would show up in the latency benchmarks.
    // ingest_probe converts the same payload independently, so the diagnostic
    // belongs there instead.
    ComplexF acStaging[kMaxSamplesPerPacket];

    for (uint64_t u = 0; u < oPacket.uSampleCount; ++u) {
        const uint8_t* pPair = oPacket.pPayload + u * 4;
        const int32_t iI = readBe16Signed(pPair);
        const int32_t iQ = readBe16Signed(pPair + 2);

        acStaging[u] = ComplexF(static_cast<float>(iI * mdCodeToVolts),
                                static_cast<float>(iQ * mdCodeToVolts));
    }

    const uint64_t uAbsIndex =
        absoluteIndex(oPacket.uTimestampSec, oPacket.uTimestampPs);

    CircularBuffer& oBuffer = bIsSurveillance ? mSurBuffer : mRefBuffer;

    if (!oBuffer.write(uAbsIndex, acStaging, oPacket.uSampleCount)) {
        ++mStats.uRejectedWrites;
        return true;   // already logged by the buffer; not a fault here
    }

    mStats.uSamplesWritten += oPacket.uSampleCount;
    return true;
}

} // namespace pbr
