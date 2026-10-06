#include "pbr/Vita49Packet.hpp"

#include "pbr/check.hpp"

#include <cstring>

namespace pbr {

namespace {

// Big-endian field readers. These assemble values byte by byte rather than
// casting the buffer to a wider type, which keeps them correct on any host
// byte order and avoids unaligned reads -- a UDP payload offers no alignment
// guarantee at offsets 20 or 44. (The legacy PXGF reader gets this wrong: it
// reads its timestamp as *(int64_t*)&data[0], which is undefined behaviour.)

uint32_t readBe16(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 8) | static_cast<uint32_t>(p[1]);
}

uint32_t readBe32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

uint64_t readBe64(const uint8_t* p)
{
    uint64_t uValue = 0;
    for (size_t u = 0; u < 8; ++u)
        uValue = (uValue << 8) | static_cast<uint64_t>(p[u]);
    return uValue;
}

} // namespace

bool parseVita49Packet(const uint8_t* pBytes, size_t uLength, Vita49Packet& oPacket)
{
    CHECK(pBytes != nullptr, "parseVita49Packet with a null buffer");
    CHECK(uLength >= kVita49CommonHeaderBytes,
          "datagram is shorter than the common VRT header");
    CHECK((uLength & 0x3) == 0, "datagram length is not a multiple of 4 bytes");

    oPacket = Vita49Packet();

    const uint32_t uTypeNibble = static_cast<uint32_t>(pBytes[0]) >> 4;

    oPacket.uTsi           = (static_cast<uint32_t>(pBytes[1]) >> 6) & 0x3u;
    oPacket.uTsf           = (static_cast<uint32_t>(pBytes[1]) >> 4) & 0x3u;
    oPacket.uPacketCount   = static_cast<uint32_t>(pBytes[1]) & 0x0Fu;
    oPacket.uDeclaredWords = readBe16(pBytes + 2);
    oPacket.uStreamId      = readBe32(pBytes + kVita49StreamIdOffset);
    oPacket.uClassId       = readBe64(pBytes + kVita49ClassIdOffset);
    oPacket.uTimestampSec  = readBe32(pBytes + kVita49SecondsOffset);
    oPacket.uTimestampPs   = readBe64(pBytes + kVita49PicosecondsOffset);

    // The class ID is our only defence against unrelated traffic arriving on
    // the port. If FERS ever changes its profile identifier this is the first
    // thing to relax.
    CHECK(oPacket.uClassId == kVita49ClassId, "unexpected VRT class ID");

    // TSF selects what the fractional timestamp field means. Mode 2 is
    // real-time picoseconds; mode 1 would be a raw sample count, which would
    // make ChannelIngest::absoluteIndex() silently wrong rather than visibly
    // broken. Every packet in a 3610-packet capture reported TSI = 1 (UTC)
    // and TSF = 2, on both data and context packets, so this is enforced.
    CHECK(oPacket.uTsf == 2,
          "timestamp-fractional mode is not real-time picoseconds");

    // Follows from TSF = 2, and absoluteIndex()'s overflow headroom depends
    // on it.
    CHECK(oPacket.uTimestampPs < kPicosecondsPerSecond,
          "fractional timestamp is not less than one second");

    // See the header for why the declared size is enforced.
    CHECK(static_cast<size_t>(oPacket.uDeclaredWords) * 4 == uLength,
          "declared packet size disagrees with the datagram length");

    if (uTypeNibble == 0x1) {
        oPacket.eType = Vita49Type::Data;

        CHECK(uLength >= kVita49DataOverheadBytes,
              "data packet too short to hold a header and a trailer");

        // Four bytes per sample: int16 I followed by int16 Q. No separate
        // check that this divides evenly -- the datagram length is already
        // required to be a multiple of 4, and so is the overhead.
        const size_t uPayloadBytes = uLength - kVita49DataOverheadBytes;

        oPacket.pPayload     = pBytes + kVita49CommonHeaderBytes;
        oPacket.uSampleCount = uPayloadBytes / 4;
        oPacket.uTrailerRaw  = readBe32(pBytes + uLength - kVita49TrailerBytes);

    } else if (uTypeNibble == 0x4) {
        oPacket.eType = Vita49Type::Context;

        CHECK(uLength >= kVita49ContextHeaderBytes,
              "context packet shorter than its fixed header");

        // Sample rate is an IEEE-754 double stored as a big-endian 64-bit
        // pattern. Reassemble the bit pattern, then memcpy it into a double;
        // memcpy rather than a cast to avoid a strict-aliasing violation.
        const uint64_t uRateBits = readBe64(pBytes + kVita49SampleRateOffset);
        std::memcpy(&oPacket.dSampleRate, &uRateBits, sizeof(double));

        // ASCII JSON, NUL-terminated and padded to a 32-bit boundary. A
        // context packet with no JSON at all is legal here and yields length 0.
        oPacket.pJson = reinterpret_cast<const char*>(pBytes + kVita49ContextHeaderBytes);

        const size_t uMaxJson = uLength - kVita49ContextHeaderBytes;
        size_t uJsonLength = 0;
        while (uJsonLength < uMaxJson && oPacket.pJson[uJsonLength] != '\0')
            ++uJsonLength;
        oPacket.uJsonLength = uJsonLength;

    } else {
        // Well-formed but not a type we handle. Not an error.
        oPacket.eType = Vita49Type::Other;
    }

    return true;
}

} // namespace pbr
