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
    uint64_t    uSampleRateHz;        // must match the source's declared rate
    double      dFullScale;           // must match --vita49-fullscale
    uint64_t    uBufferCapacity;      // samples per channel (bufSeconds * fs)
    std::string sReferenceName;       // must match <receiver name="..."> exactly
    std::string sSurveillanceName;

    // CPI extraction. Deliberately a narrow interface rather than taking the
    // whole PbrConfig, so ChannelIngest stays testable without a config file.
    uint64_t    uCpiSamples;          // window length handed to the DSP chain
    uint64_t    uHopSamples;          // step between consecutive CPI starts
    double      dMinCpiCoverage;      // fraction that must have come from
                                      // packets (not gap fill), e.g. 0.99
};

// Outcome of one CPI extraction attempt.
enum class CpiStatus {
    Ready,     // a window was written to the caller's buffers
    NotYet,    // not enough data has arrived; try again after more packets
    Skipped    // a window was lost or too gappy to use; the start has advanced
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

    // Try to hand over the next coherent processing interval.
    //
    // On Ready, pcRef and pcSur each hold uCount samples covering the same
    // absolute index range, starting at uCpiStartOut, and the internal start
    // pointer has advanced by one hop. On NotYet nothing was written. On
    // Skipped the buffers MAY have been written but must not be used: either
    // the window fell out of the buffer before both channels completed it, or
    // it contained too many gaps to trust.
    //
    // uCount must equal uCpiSamples from the config. The caller owns both
    // buffers. Drain in a loop until NotYet.
    CpiStatus tryExtractCpi(ComplexF* pcRef, ComplexF* pcSur, uint64_t uCount,
                            uint64_t& uCpiStartOut);

    // Change the window length and hop for every CPI from the next one on,
    // keeping the buffered samples and the current start pointer. For a live
    // parameter change at a CPI boundary. Refused (false, logged) if the
    // window would not fit the buffer or either value is zero.
    bool setCpi(uint64_t uCpiSamples, uint64_t uHopSamples);

    // True once both reference and surveillance stream IDs are known.
    bool rolesResolved() const;

    ChannelRole roleOf(uint32_t uStreamId) const;

    // Counters for pbr_run's summary and the tests.
    struct Stats {
        uint64_t uContextPackets;
        uint64_t uDataPackets;
        uint64_t uOtherPackets;
        uint64_t uSamplesWritten;
        uint64_t uRejectedWrites;     // buffer refused: too late
        uint64_t uUnresolvedData;     // data arrived before its context packet
        uint64_t uPacketCountGaps;    // mod-16 header counter skipped
        uint64_t uOversizePackets;    // more samples than kMaxSamplesPerPacket

        uint64_t uCpisEmitted;          // handed over as Ready
        uint64_t uCpisSkippedCoverage;  // too few packet-written samples
        uint64_t uCpisOvertaken;        // overwritten before both channels
                                        // completed them

        uint64_t uSessionChanges;       // a stream's driver session id changed
        uint64_t uReanchors;            // the timeline jumped backwards; buffers reset
    };
    const Stats& stats() const { return mStats; }

    // One line naming every stream seen so far, its receiver name and role,
    // for the "packets arrive but names do not match" reminder.
    std::string streamSummary() const;

    // The sample rate the source declares in its context packets, 0 until
    // one has arrived. The caller compares it with the configured rate.
    double sourceSampleRateHz() const { return mdSourceSampleRateHz; }

private:
    bool handleContext(const Vita49Packet& oPacket);
    bool handleData(const Vita49Packet& oPacket);

    // The three steps of handleData, in order.
    void decodeSamples(const Vita49Packet& oPacket, ComplexF* pcOut) const;
    void reanchorIfJumpedBack(const CircularBuffer& oBuffer, uint64_t uAbsIndex,
                              uint64_t uCount);
    void noteFirstIndex(bool bSurveillance, uint64_t uAbsIndex);

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
        std::string sName;          // receiver.name last seen, may not match either config name
        std::string sSession;       // receiver.session last seen
    };

    // Forget the timeline: both buffers reset, first-index tracking and the
    // CPI anchor cleared. Roles and packet counters are kept.
    void resetTimeline();

    StreamState* findStream(uint32_t uStreamId);
    StreamState& findOrAddStream(uint32_t uStreamId);

    ChannelIngestConfig mConfig;
    double              mdCodeToVolts;   // dFullScale / kFullScaleCode
    double              mdSourceSampleRateHz;

    CircularBuffer mRefBuffer;
    CircularBuffer mSurBuffer;

    std::vector<StreamState> mvStreams;

    // Lowest absolute index ever written on each channel. The CPI anchor needs
    // this, and it must be the MINIMUM rather than the first one written,
    // because packets can arrive out of order.
    bool     mbRefSeen;
    uint64_t muRefFirstIndex;
    bool     mbSurSeen;
    uint64_t muSurFirstIndex;

    // Absolute index of the next CPI to hand over.
    bool     mbCpiAnchored;
    uint64_t muCpiStart;

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
