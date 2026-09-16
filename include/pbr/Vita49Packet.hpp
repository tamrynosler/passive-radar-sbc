#ifndef PBR_VITA49_PACKET_HPP
#define PBR_VITA49_PACKET_HPP

#include <cstddef>
#include <cstdint>

namespace pbr {

// Decoder for FERS's VITA 49.2 UDP output.
//
// This is FERS's own placeholder VRT profile (class ID kVita49ClassId), not
// the full VITA 49.2 standard, and FERS marks the output "alpha and
// unverified". Treat any field not listed here as unconfirmed. Reference:
// github.com/stpaine/FERS/wiki/VITA49-Streaming-Implementation, cross-checked
// against matlab/parse_vita49_packet.m.
//
// All fields are big-endian. Byte offsets common to both packet types:
//
//    0   4   VRT header word, bit numbering from the MSB of byte 0:
//              31:28  packet type (0x1 signal data, 0x4 context)
//              27:24  indicator bits (class ID present, trailer present, ...)
//              23:22  TSI, timestamp-integer mode     -- observed 1 = UTC
//              21:20  TSF, timestamp-fractional mode  -- observed 2 = real-time ps
//              19:16  packet count, mod 16
//              15:0   declared packet size in 32-bit words
//    4   4   stream ID
//    8   8   class ID
//   16   4   UTC integer seconds
//   20   8   UTC fractional picoseconds
//
// Signal data packets then carry interleaved big-endian int16 I/Q from offset
// 28 to length-4, with a 4-byte trailer at the end (32 bytes of fixed
// overhead in total).
//
// Context packets carry an IEEE-754 double sample rate at offset 44 and
// NUL-terminated ASCII JSON from offset 92.

// FERS's placeholder VRT profile identifier.
constexpr uint64_t kVita49ClassId = 0xFA52530001000101ULL;

// Fixed byte offsets and sizes, named so the parser reads as the table above.
constexpr size_t kVita49CommonHeaderBytes  = 28;
constexpr size_t kVita49TrailerBytes       = 4;
constexpr size_t kVita49ContextHeaderBytes = 92;
constexpr size_t kVita49SampleRateOffset   = 44;

enum class Vita49Type {
    Data,      // signal data packet, header nibble 0x1
    Context,   // context packet,     header nibble 0x4
    Other      // well-formed, but a type we do not handle
};

// A decoded packet. For Data and Context packets this holds POINTERS into the
// caller's receive buffer rather than copies, so the buffer must outlive the
// Vita49Packet and must not be reused until the packet has been consumed.
struct Vita49Packet {
    Vita49Type eType;

    uint32_t uStreamId;
    uint64_t uClassId;
    uint32_t uPacketCount;     // mod-16 sequence counter from the header
    uint32_t uDeclaredWords;   // packet size in 32-bit words, per the header
    uint32_t uTsi;             // timestamp-integer mode: 1 = UTC
    uint32_t uTsf;             // timestamp-fractional mode: 2 = real-time ps
    uint64_t uTimestampSec;    // UTC integer seconds
    uint64_t uTimestampPs;     // UTC fractional picoseconds, < 1e12

    // Data packets only. Timestamp above is the time of the FIRST sample.
    const uint8_t* pPayload;   // interleaved big-endian int16 I/Q
    uint64_t       uSampleCount;   // IQ pairs = payload bytes / 4
    uint32_t       uTrailerRaw;    // indicator bits; positions unpublished,
                                   // so captured but not decoded

    // Context packets only.
    double      dSampleRate;
    const char* pJson;
    size_t      uJsonLength;   // excludes the NUL terminator
};

// Decode one UDP datagram.
//
// Returns false if the datagram is not a well-formed packet of this profile.
// A well-formed packet whose type we do not handle returns true with
// eType == Other, so callers can ignore it rather than treating it as an
// error -- the format is officially unverified, so tolerating unexpected
// types is cheap insurance.
//
// uDeclaredWords IS validated against uLength. It was recorded but not
// enforced until a 30 s live FERS run confirmed the field is populated
// correctly (18001 of 18001 packets on each channel), which makes it a free
// corruption and truncation check. Relax this if a future FERS version stops
// setting it.
bool parseVita49Packet(const uint8_t* pBytes, size_t uLength, Vita49Packet& oPacket);

} // namespace pbr

#endif // PBR_VITA49_PACKET_HPP
