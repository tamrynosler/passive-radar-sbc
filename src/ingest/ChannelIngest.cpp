#include "pbr/ChannelIngest.hpp"

#include "pbr/check.hpp"

#include <cassert>
#include <cstring>

namespace pbr {

namespace {

// The largest int16 ADC code: code / kFullScaleCode * VITA49_FULLSCALE is
// the sample in physical units.
const double kFullScaleCode = 32767.0;

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

const char* roleName(ChannelRole eRole)
{
    switch (eRole) {
    case ChannelRole::Reference:     return "reference";
    case ChannelRole::Surveillance:  return "surveillance";
    case ChannelRole::Surveillance2: return "surveillance 2";
    default:                         return "NOT ASSIGNED";
    }
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
      mdSourceSampleRateHz(0.0),
      mRefBuffer(oConfig.uBufferCapacity),
      mSurBuffer(oConfig.uBufferCapacity),
      mSur2Buffer(oConfig.uBufferCapacity),
      muChannels(oConfig.sSurveillance2Name.empty() ? 2 : 3),
      mbCpiAnchored(false),
      muCpiStart(0),
      mbValid(false)
{
    std::memset(&mStats, 0, sizeof(mStats));
    mapBuffer[0] = &mRefBuffer;
    mapBuffer[1] = &mSurBuffer;
    mapBuffer[2] = &mSur2Buffer;
    for (size_t u = 0; u < kMaxChannels; ++u) {
        mabSeen[u]       = false;
        mauFirstIndex[u] = 0;
    }

    if (!mRefBuffer.isValid() || !mSurBuffer.isValid() || !mSur2Buffer.isValid()) {
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
    if (oConfig.sReferenceName == oConfig.sSurveillanceName ||
        oConfig.sSurveillance2Name == oConfig.sReferenceName ||
        oConfig.sSurveillance2Name == oConfig.sSurveillanceName) {
        std::fprintf(stderr, "ChannelIngest: two receiver names are identical\n");
        return;
    }
    if (oConfig.uCpiSamples == 0 || oConfig.uHopSamples == 0) {
        std::fprintf(stderr, "ChannelIngest: CPI and hop lengths must be non-zero\n");
        return;
    }
    if (oConfig.uCpiSamples > oConfig.uBufferCapacity) {
        std::fprintf(stderr, "ChannelIngest: a CPI of %llu samples does not fit in a "
                             "%llu sample buffer\n",
                     static_cast<unsigned long long>(oConfig.uCpiSamples),
                     static_cast<unsigned long long>(oConfig.uBufferCapacity));
        return;
    }
    if (oConfig.dMinCpiCoverage <= 0.0 || oConfig.dMinCpiCoverage > 1.0) {
        std::fprintf(stderr, "ChannelIngest: minimum CPI coverage must be in (0, 1]\n");
        return;
    }

    mdCodeToVolts = oConfig.dFullScale / kFullScaleCode;

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

bool ChannelIngest::setCpi(uint64_t uCpiSamples, uint64_t uHopSamples)
{
    CHECK(mbValid, "setCpi on an invalid ChannelIngest");
    CHECK(uCpiSamples > 0 && uHopSamples > 0, "setCpi: CPI and hop must be non-zero");
    CHECK(uCpiSamples <= mConfig.uBufferCapacity,
          "setCpi: the new CPI does not fit the channel buffers");
    mConfig.uCpiSamples = uCpiSamples;
    mConfig.uHopSamples = uHopSamples;
    return true;
}

std::string ChannelIngest::streamSummary() const
{
    if (mvStreams.empty())
        return "no streams seen yet";
    std::string sOut;
    char achLine[160];
    for (size_t u = 0; u < mvStreams.size(); ++u) {
        const StreamState& oStream = mvStreams[u];
        std::snprintf(achLine, sizeof(achLine), "%sstream 0x%08X \"%s\" (%s)",
                      u == 0 ? "" : ", ", oStream.uStreamId,
                      oStream.sName.empty() ? "no context yet" : oStream.sName.c_str(),
                      roleName(oStream.eRole));
        sOut += achLine;
    }
    return sOut;
}

void ChannelIngest::resetTimeline()
{
    for (size_t u = 0; u < kMaxChannels; ++u) {
        mapBuffer[u]->reset();
        mabSeen[u]       = false;
        mauFirstIndex[u] = 0;
    }
    mbCpiAnchored = false;
    muCpiStart    = 0;
}

size_t ChannelIngest::channelOf(ChannelRole eRole)
{
    switch (eRole) {
    case ChannelRole::Reference:     return 0;
    case ChannelRole::Surveillance:  return 1;
    case ChannelRole::Surveillance2: return 2;
    default:                         return kMaxChannels;   // never a buffer
    }
}

bool ChannelIngest::rolesResolved() const
{
    bool abHave[kMaxChannels] = { false, false, false };
    for (size_t u = 0; u < mvStreams.size(); ++u) {
        const size_t uChannel = channelOf(mvStreams[u].eRole);
        if (uChannel < kMaxChannels)
            abHave[uChannel] = true;
    }
    for (size_t u = 0; u < muChannels; ++u) {
        if (!abHave[u])
            return false;
    }
    return true;
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
        oStream.sName = sName;
        ChannelRole eRole = ChannelRole::Unknown;
        if (sName == mConfig.sReferenceName)
            eRole = ChannelRole::Reference;
        else if (sName == mConfig.sSurveillanceName)
            eRole = ChannelRole::Surveillance;
        else if (muChannels == 3 && sName == mConfig.sSurveillance2Name)
            eRole = ChannelRole::Surveillance2;

        if (eRole != ChannelRole::Unknown && oStream.eRole != eRole) {
            oStream.eRole = eRole;
            std::fprintf(stderr, "ChannelIngest: stream 0x%08X is \"%s\" (%s)\n",
                         oPacket.uStreamId, sName.c_str(), roleName(eRole));
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

    // The driver's session id. A change means the driver restarted, or a
    // second driver is sending into the same group: either way the sample
    // indices that follow do not continue the ones before, and if two
    // drivers are running the buffers now hold an interleaving of both.
    std::string sSession;
    if (findJsonString(oPacket.pJson, oPacket.uJsonLength, "session", sSession, uSearchFrom)) {
        if (!oStream.sSession.empty() && sSession != oStream.sSession) {
            ++mStats.uSessionChanges;
            std::fprintf(stderr,
                         "ChannelIngest: WARNING stream 0x%08X changed driver session %s -> %s.\n"
                         "               A driver restarted, or TWO drivers are sending to this\n"
                         "               group. If the warning repeats, stop one of them.\n",
                         oPacket.uStreamId, oStream.sSession.c_str(), sSession.c_str());
        }
        oStream.sSession = sSession;
    }

    // The context packet also carries the sample rate; the caller compares
    // it with the configured rate and adapts or refuses (design note §7).
    if (oPacket.dSampleRate > 0.0) {
        mdSourceSampleRateHz = oPacket.dSampleRate;
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

    const size_t    uChannel = channelOf(oStream.eRole);
    CircularBuffer& oBuffer  = *mapBuffer[uChannel];

    ComplexF acStaging[kMaxSamplesPerPacket];
    decodeSamples(oPacket, acStaging);

    const uint64_t uAbsIndex =
        absoluteIndex(oPacket.uTimestampSec, oPacket.uTimestampPs);
    reanchorIfJumpedBack(oBuffer, uAbsIndex, oPacket.uSampleCount);

    if (!oBuffer.write(uAbsIndex, acStaging, oPacket.uSampleCount)) {
        ++mStats.uRejectedWrites;
        return true;   // already logged by the buffer; not a fault here
    }

    noteFirstIndex(uChannel, uAbsIndex);
    mStats.uSamplesWritten += oPacket.uSampleCount;
    return true;
}

// Big-endian int16 I/Q pairs to physical units. Nothing else happens in this
// loop on purpose: ADC code-range tracking used to live here, but per-sample
// work in the production ingest path shows up in the latency benchmarks.
void ChannelIngest::decodeSamples(const Vita49Packet& oPacket, ComplexF* pcOut) const
{
    for (uint64_t u = 0; u < oPacket.uSampleCount; ++u) {
        const uint8_t* pPair = oPacket.pPayload + u * 4;
        const int32_t iI = readBe16Signed(pPair);
        const int32_t iQ = readBe16Signed(pPair + 2);

        pcOut[u] = ComplexF(static_cast<float>(iI * mdCodeToVolts),
                            static_cast<float>(iQ * mdCodeToVolts));
    }
}

// A packet more than a whole buffer behind the write end is not a late
// packet, it is a source whose timeline went backwards: a driver restarted
// from another clock, or the clock was corrected. Waiting for wall time to
// catch up could take days, so forget the old timeline and start again here.
// Late packets within a buffer length are still rejected by the buffer.
void ChannelIngest::reanchorIfJumpedBack(const CircularBuffer& oBuffer, uint64_t uAbsIndex,
                                         uint64_t uCount)
{
    if (oBuffer.writeEnd() <= oBuffer.capacity() ||
        uAbsIndex + uCount + oBuffer.capacity() >= oBuffer.writeEnd())
        return;

    ++mStats.uReanchors;
    std::fprintf(stderr,
                 "ChannelIngest: WARNING the source timeline jumped back by %.1f s "
                 "(index %llu, write end %llu); re-anchoring, buffers cleared\n",
                 double(oBuffer.writeEnd() - uAbsIndex) / double(mConfig.uSampleRateHz),
                 (unsigned long long)uAbsIndex,
                 (unsigned long long)oBuffer.writeEnd());
    resetTimeline();
}

// Remember the lowest index seen on a channel, for the CPI anchor. Minimum
// rather than first-written, since packets can arrive reordered.
void ChannelIngest::noteFirstIndex(size_t uChannel, uint64_t uAbsIndex)
{
    if (!mabSeen[uChannel] || uAbsIndex < mauFirstIndex[uChannel]) {
        mabSeen[uChannel]       = true;
        mauFirstIndex[uChannel] = uAbsIndex;
    }
}

CpiStatus ChannelIngest::tryExtractCpi(ComplexF* pcRef, ComplexF* pcSur, ComplexF* pcSur2,
                                       uint64_t uCount, uint64_t& uCpiStartOut)
{
    uCpiStartOut = 0;

    ComplexF* apcOut[kMaxChannels] = { pcRef, pcSur, pcSur2 };
    bool bNullOut = !mbValid;
    for (size_t u = 0; u < muChannels; ++u)
        bNullOut = bNullOut || (apcOut[u] == nullptr);
    if (bNullOut) {
        std::fprintf(stderr, "tryExtractCpi: invalid ingest or null destination\n");
        return CpiStatus::NotYet;
    }
    if (uCount != mConfig.uCpiSamples) {
        std::fprintf(stderr, "tryExtractCpi: asked for %llu samples, configured "
                             "CPI is %llu\n",
                     static_cast<unsigned long long>(uCount),
                     static_cast<unsigned long long>(mConfig.uCpiSamples));
        return CpiStatus::NotYet;
    }

    // Nothing to anchor to until every channel has delivered something.
    for (size_t u = 0; u < muChannels; ++u) {
        if (!mabSeen[u])
            return CpiStatus::NotYet;
    }

    // Anchor on the earliest index every channel can cover. All live on one
    // absolute timeline, so a single start pointer serves them all.
    if (!mbCpiAnchored) {
        muCpiStart = mauFirstIndex[0];
        for (size_t u = 1; u < muChannels; ++u) {
            if (mauFirstIndex[u] > muCpiStart)
                muCpiStart = mauFirstIndex[u];
        }
        mbCpiAnchored = true;
    }

    // Has the window we wanted already been overwritten? Step forward by whole
    // hops rather than snapping, so CPI starts stay on the same lattice and an
    // index still maps predictably back to a time.
    uint64_t uOldest = 0;
    for (size_t u = 0; u < muChannels; ++u) {
        if (mapBuffer[u]->oldestValid() > uOldest)
            uOldest = mapBuffer[u]->oldestValid();
    }

    if (muCpiStart < uOldest) {
        const uint64_t uBehind = uOldest - muCpiStart;
        const uint64_t uHops   = (uBehind + mConfig.uHopSamples - 1) / mConfig.uHopSamples;
        muCpiStart += uHops * mConfig.uHopSamples;
        mStats.uCpisOvertaken += uHops;
        return CpiStatus::Skipped;
    }

    // Every channel must have written past the end of the window.
    const uint64_t uNeededEnd = muCpiStart + uCount;
    for (size_t u = 0; u < muChannels; ++u) {
        if (mapBuffer[u]->writeEnd() < uNeededEnd)
            return CpiStatus::NotYet;
    }

    for (size_t u = 0; u < muChannels; ++u) {
        if (mapBuffer[u]->read(muCpiStart, apcOut[u], uCount))
            continue;
        // The window passed the tests above, so a refusal here means the
        // buffer's view of what is resident disagrees with ours.
        std::fprintf(stderr, "tryExtractCpi: buffer refused a window at %llu that "
                             "should have been resident\n",
                     static_cast<unsigned long long>(muCpiStart));
        muCpiStart += mConfig.uHopSamples;
        return CpiStatus::Skipped;
    }

    // Coverage is the fraction of the window that arrived in packets, from
    // the buffer's per-slot validity flags. It does not look at the sample
    // values, so a quiet input (an ADC idling at a few codes, many of them
    // exactly zero) is as valid as a loud one, while a zero-filled gap is
    // not, whatever the samples around it look like. Read before the start
    // advances, on the same window the reads above used.
    bool bCovered = true;
    for (size_t u = 0; u < muChannels; ++u) {
        const double dCoverage = static_cast<double>(mapBuffer[u]->validCount(muCpiStart, uCount)) /
                                 static_cast<double>(uCount);
        if (dCoverage < mConfig.dMinCpiCoverage)
            bCovered = false;
    }

    const uint64_t uStart = muCpiStart;
    muCpiStart += mConfig.uHopSamples;

    if (!bCovered) {
        ++mStats.uCpisSkippedCoverage;
        return CpiStatus::Skipped;
    }

    ++mStats.uCpisEmitted;
    uCpiStartOut = uStart;
    return CpiStatus::Ready;
}

} // namespace pbr
