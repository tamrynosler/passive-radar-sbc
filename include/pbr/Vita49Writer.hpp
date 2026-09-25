#ifndef PBR_VITA49_WRITER_HPP
#define PBR_VITA49_WRITER_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace pbr {

// Encoder for FERS's VITA 49.2 profile: the mirror image of Vita49Packet.hpp,
// which documents the byte layout. Everything parseVita49Packet() and
// ChannelIngest enforce is produced here, so a stream built with these two
// functions is indistinguishable from FERS to the ingest.
//
// Pure functions that fill a caller-owned buffer. Nothing is allocated and no
// state is kept: the per-stream packet counter belongs to the caller, because
// it must advance on EVERY packet of a stream, context packets included.

// What a context packet says about its stream. Filled once at start-up from
// the source's ACTUAL settings (read back from UHD), never from the values
// that were asked for.
struct Vita49StreamInfo {
    uint32_t    uStreamId;         // 1 = reference, 2 = surveillance
    uint64_t    uReceiverId;       // same values, in the 64-bit device field
    std::string sReceiverName;     // must equal the prb.conf name exactly
    double      dSampleRateHz;     // actual rate; the ingest checks it
    double      dCentreFreqHz;
    double      dBandwidthHz;      // reported only; the ingest ignores it
    double      dAdcFullScale;     // 1.0 for raw codes; equals VITA49_FULLSCALE
    double      dGainDb;
    unsigned    uAdcBits;          // 14 on the N210
    unsigned    uChannel;          // 0 = RX-A, 1 = RX-B
    bool        bExtRef;
    std::string sDevice;           // "USRP N210 BasicRX", "synth"
    std::string sDriverVersion;    // UHD version string, or "none"
};

struct Vita49Time {
    uint32_t uSeconds;             // UTC integer seconds
    uint64_t uPicoseconds;         // < 1e12, guaranteed by makeTime()
};

// context_flags in the receiver JSON object, mirroring FERS's ContextFlags.
constexpr uint32_t kVita49FlagSampleLoss  = 0x04;   // first context after a gap
constexpr uint32_t kVita49FlagStreamOpen  = 0x08;   // first context packet
constexpr uint32_t kVita49FlagStreamClose = 0x10;   // last context packet

// Fixed bytes around a data payload: 28-byte header plus 4-byte trailer.
constexpr size_t kVita49DataOverheadBytes = 32;

// Exact conversion of an absolute sample index to a VRT timestamp such that
// ChannelIngest::absoluteIndex() recovers the same index:
//
//     seconds     = index / fs
//     picoseconds = ceil((index mod fs) * 1e12 / fs)
//
// Ceiling, not floor: the ingest truncates (ps * fs) / 1e12, and with the
// ceiling the product lands in [r * 1e12, r * 1e12 + fs), which truncates to
// exactly r. Floor could give r - 1. Valid for any integer fs up to 1e7.
Vita49Time makeTime(uint64_t uSampleIndex, uint64_t uSampleRateHz);

// Build one signal data packet.
//
// pSamples points at uCount host-order int16 I/Q pairs (2 * uCount values);
// they are written big-endian. The trailer carries valid-data set, calibrated
// time and reference lock equal to bRefLocked, over-range clear (the N210
// does not report clipping through rx_metadata_t), and sample loss as given.
//
// Returns false, having logged the reason, if the buffer is too small, uCount
// exceeds kMaxSamplesPerPacket, or the time is malformed.
bool writeVita49Data(uint8_t* pOut, size_t uCapacity,
                     uint32_t uStreamId, uint8_t uPacketCount,
                     const Vita49Time& oTime,
                     const int16_t* pSamples, uint64_t uCount,
                     bool bRefLocked, bool bSampleLoss,
                     size_t& uLengthOut);

// Build one context packet. uContextFlags is a combination of the
// kVita49Flag* values; the state-indicator word's sample-loss bit follows
// kVita49FlagSampleLoss.
bool writeVita49Context(uint8_t* pOut, size_t uCapacity,
                        const Vita49StreamInfo& oInfo, uint8_t uPacketCount,
                        const Vita49Time& oTime,
                        bool bRefLocked, uint32_t uContextFlags,
                        size_t& uLengthOut);

} // namespace pbr

#endif // PBR_VITA49_WRITER_HPP
