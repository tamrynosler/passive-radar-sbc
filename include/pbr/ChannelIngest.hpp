#ifndef PBR_CHANNEL_INGEST_HPP
#define PBR_CHANNEL_INGEST_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "pbr/CircularBuffer.hpp"
#include "pbr/Vita49Packet.hpp"

namespace pbr {

enum class ChannelRole {
    Unknown,        // stream seen, but no context packet has named it yet
    Reference,
    Surveillance
};

struct ChannelIngestConfig {
    uint64_t    uSampleRateHz;        // 204800, must match the .fersxml <rate>
    double      dFullScale;           // must match --vita49-fullscale
    uint64_t    uBufferCapacity;      // samples per channel (bufSeconds * fs)
    std::string sReferenceName;       // must match <receiver name="..."> exactly
    std::string sSurveillanceName;
};

// Largest packet we will accept. FERS's default packetisation is 342 samples
// (a 1400-byte datagram); this leaves generous headroom. Raising
// --vita49-max-udp-payload requires raising this and the receive buffer in
// the caller to match.
constexpr uint64_t kMaxSamplesPerPacket = 512;

// Turns decoded VITA 49 packets into samples in the right channel's buffer.
//
// Responsibilities:
//   * learn which stream ID is reference and which is surveillance, from the
//     receiver name in context-packet JSON
//   * convert big-endian int16 ADC codes to physical units
//   * work out each packet's absolute sample index from its timestamp
//   * write into the matching CircularBuffer
//
// Not thread-safe. Single-threaded, like everything else in the ingest path.
class ChannelIngest {
public:
    explicit ChannelIngest(const ChannelIngestConfig& oConfig);

    ChannelIngest(const ChannelIngest&) = delete;
    ChannelIngest& operator=(const ChannelIngest&) = delete;

    // False if either buffer failed to allocate, or the config is unusable.
    bool isValid() const { return mbValid; }

    // Dispatch one decoded packet. Returns false only on a genuine fault;
    // ordinary conditions (a packet for an unresolved stream, a stale write)
    // return true and are recorded in stats().
    bool handlePacket(const Vita49Packet& oPacket);

    // True once both reference and surveillance stream IDs are known.
    bool rolesResolved() const;

    ChannelRole roleOf(uint32_t uStreamId) const;

    const CircularBuffer& buffer(ChannelRole eRole) const;

    // Counters for the checkpoint program. Cheap, and every one of these
    // answers a question the checkpoint has to answer.
    struct Stats {
        uint64_t uContextPackets;
        uint64_t uDataPackets;
        uint64_t uOtherPackets;
        uint64_t uSamplesWritten;
        uint64_t uRejectedWrites;     // buffer refused: too late
        uint64_t uUnresolvedData;     // data arrived before its context packet
        uint64_t uPacketCountGaps;    // mod-16 header counter skipped
        uint64_t uOversizePackets;    // more samples than kMaxSamplesPerPacket
    };
    const Stats& stats() const { return mStats; }

private:
    bool handleContext(const Vita49Packet& oPacket);
    bool handleData(const Vita49Packet& oPacket);

    // Absolute sample index of a packet's first sample, from its VRT
    // timestamp. See docs/design_decisions.md.
    uint64_t absoluteIndex(uint64_t uSeconds, uint64_t uPicoseconds) const;

    // Per-stream state. Only ever a couple of entries, so a vector with a
    // linear scan is the right structure.
    struct StreamState {
        uint32_t    uStreamId;
        ChannelRole eRole;
        bool        bHavePacketCount;
        uint32_t    uLastPacketCount;
        bool        bModeReported;
    };

    StreamState* findStream(uint32_t uStreamId);
    StreamState& findOrAddStream(uint32_t uStreamId);

    ChannelIngestConfig mConfig;
    double              mdCodeToVolts;   // dFullScale / 32767

    CircularBuffer mRefBuffer;
    CircularBuffer mSurBuffer;

    std::vector<StreamState> mvStreams;

    Stats mStats;
    bool  mbValid;
};

// Extract a string value from flat ASCII JSON.
//
// Deliberately not a JSON parser. It finds "<key>" and returns the quoted
// string that follows the next colon. That is enough for the two fields we
// need (receiver.name and receiver.mode) and keeps the project free of an
// external dependency.
//
// Limitations, all acceptable for FERS's metadata but worth knowing:
//   * no escape-sequence handling, so a value containing \" is truncated
//   * no nesting awareness beyond the uSearchFrom hint, so a matching key in
//     an unrelated object would win if it appeared first
//   * no number, array or boolean support
//
// If FERS's metadata ever grows structure, replace this one function with a
// real parser; nothing else in the ingest path looks at JSON.
bool findJsonString(const char* pJson, size_t uLength, const char* pKey,
                    std::string& sValueOut, size_t uSearchFrom = 0);

// Offset just past "<key>" in the JSON, or uLength if absent. Used to anchor
// a findJsonString search inside a particular object.
size_t findJsonKey(const char* pJson, size_t uLength, const char* pKey);

} // namespace pbr

#endif // PBR_CHANNEL_INGEST_HPP
