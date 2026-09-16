// Unit tests for pbr::parseVita49Packet. Plain main() + asserts, no framework.
//
// NDEBUG is undefined deliberately and before <cassert>: the project builds
// Release, which defines NDEBUG and would compile every assert below out.
#undef NDEBUG
#include <cassert>

#include "pbr/ChannelIngest.hpp"   // findJsonString / findJsonKey
#include "pbr/Vita49Packet.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pbr;

namespace {

// ---------------------------------------------------------------------------
// A real FERS packet.
//
// Synthetic packets only prove the parser agrees with our own reading of the
// format. This one was captured from a live fers-cli 0.1.0 run and pins the
// field offsets against reality.
//
// It is abridged: the original datagram was 1400 bytes carrying 342 samples.
// This keeps the real 28-byte header, the first 24 real samples and the real
// 4-byte trailer, with the declared size field corrected from 350 to 32 words
// to match. Every other byte is exactly as FERS sent it.
//
// Header word 0x1E620020 decodes as: packet type 1 (signal data), indicator
// bits 0xE (class ID present, trailer present), TSI 1 (UTC), TSF 2 (real-time
// picoseconds), packet count 2, size 32 words.
// ---------------------------------------------------------------------------
const uint8_t kRealDataPacket[] = {
    0x1E, 0x62, 0x00, 0x20, 0x28, 0x34, 0xDD, 0x42, 0xFA, 0x52, 0x53, 0x00,
    0x01, 0x00, 0x01, 0x01, 0x6A, 0xAA, 0xB4, 0x70, 0x00, 0x00, 0x00, 0x5E,
    0x92, 0x54, 0x44, 0x53, 0x19, 0x51, 0x07, 0x86, 0x17, 0xB8, 0xFC, 0x94,
    0x19, 0x4D, 0xFB, 0x1C, 0x09, 0xD7, 0xE8, 0xA8, 0xEA, 0xD4, 0xFC, 0xBF,
    0x04, 0x93, 0x19, 0xE5, 0x1C, 0x36, 0x03, 0x21, 0x13, 0xFD, 0xF0, 0xD1,
    0x01, 0xB2, 0xE7, 0x18, 0xE6, 0xB7, 0xFA, 0xFE, 0xFA, 0x56, 0x1B, 0xB4,
    0x1A, 0x5D, 0x0E, 0x7B, 0x1C, 0x3E, 0xF8, 0x72, 0x0A, 0x6A, 0xE4, 0xB5,
    0xE7, 0xCE, 0xF3, 0x96, 0xFB, 0xB7, 0x18, 0x5E, 0x1A, 0x51, 0xFE, 0x10,
    0x02, 0x3C, 0xE4, 0x9D, 0xE6, 0xDA, 0xF1, 0xD5, 0xEA, 0x22, 0x12, 0x3F,
    0x10, 0x5F, 0x13, 0xEF, 0x13, 0x7F, 0xF1, 0x4C, 0xFC, 0xF4, 0xE7, 0xEE,
    0xEE, 0x0B, 0xED, 0x12, 0xF8, 0x00, 0xE0, 0x00,
};

// The metadata JSON out of a real FERS context packet, verbatim. Embedded as
// a string rather than hex because it is ASCII and the point of this test is
// the nested structure: note platform_name, waveform_name and simulation_name,
// any of which a naive search for "name" could latch onto instead of
// receiver.name.
const char kRealContextJson[] =
    "{\"coordinate_frame\":{\"frame\":\"ENU\",\"origin\":{\"altitude\":111.01,"
    "\"latitude\":-33.957652,\"longitude\":18.4611991},\"utm_north_hemisphere\":true,"
    "\"utm_zone\":0},\"cw\":{\"carrier_hz\":89000000.0,\"power_w\":16400.0,"
    "\"present\":true,\"waveform_id\":1688849860263937,\"waveform_name\":\"TxWaveform\"},"
    "\"initial_platform_state\":{\"platform_id\":281474976710669,"
    "\"platform_name\":\"ArmasuisseSurRxPlatform\",\"position_m\":{\"x\":287946.01,"
    "\"y\":6297267.09,\"z\":241.0},\"rotation_rad\":{\"azimuth\":-0.6108652381980155,"
    "\"elevation\":0.17453292519943295},\"velocity_mps\":{\"x\":0.0,\"y\":0.0,\"z\":0.0}},"
    "\"receiver\":{\"adc_bits\":16,\"context_flags\":72,\"id\":844424930131982,"
    "\"mode\":\"cw\",\"name\":\"ArmasuisseSurRxClean\"},"
    "\"schema\":\"fers-vita49-context-v1\",\"simulation_name\":\"Basic1Target\","
    "\"waveform\":{\"kind\":\"cw\",\"metadata_ref\":\"cw\"}}";

// ---------------------------------------------------------------------------
// Synthetic packet builder, for cases a real capture cannot supply.
// ---------------------------------------------------------------------------

void putBe16(uint8_t* p, uint16_t uValue)
{
    p[0] = static_cast<uint8_t>(uValue >> 8);
    p[1] = static_cast<uint8_t>(uValue);
}

void putBe32(uint8_t* p, uint32_t uValue)
{
    p[0] = static_cast<uint8_t>(uValue >> 24);
    p[1] = static_cast<uint8_t>(uValue >> 16);
    p[2] = static_cast<uint8_t>(uValue >> 8);
    p[3] = static_cast<uint8_t>(uValue);
}

void putBe64(uint8_t* p, uint64_t uValue)
{
    for (size_t u = 0; u < 8; ++u)
        p[u] = static_cast<uint8_t>(uValue >> (56 - 8 * u));
}

// Build the 28-byte common header. TSI = 1 (UTC), TSF = 2 (real-time ps).
void buildHeader(std::vector<uint8_t>& v, uint32_t uType, uint32_t uCount,
                 uint32_t uStreamId, uint64_t uClassId,
                 uint64_t uSec, uint64_t uPs)
{
    v[0] = static_cast<uint8_t>((uType << 4) | 0x0E);
    v[1] = static_cast<uint8_t>((1u << 6) | (2u << 4) | (uCount & 0x0Fu));
    putBe16(&v[2], static_cast<uint16_t>(v.size() / 4));
    putBe32(&v[4], uStreamId);
    putBe64(&v[8], uClassId);
    putBe32(&v[16], static_cast<uint32_t>(uSec));
    putBe64(&v[20], uPs);
}

std::vector<uint8_t> makeDataPacket(uint64_t uSampleCount, uint32_t uCount = 0,
                                    uint32_t uStreamId = 0x11223344,
                                    uint64_t uSec = 1789572208,
                                    uint64_t uPs = 500000000000ULL)
{
    std::vector<uint8_t> v(28 + uSampleCount * 4 + 4, 0);
    buildHeader(v, 0x1, uCount, uStreamId, kVita49ClassId, uSec, uPs);
    for (uint64_t u = 0; u < uSampleCount; ++u) {
        putBe16(&v[28 + u * 4],     static_cast<uint16_t>(static_cast<int16_t>(100 + u)));
        putBe16(&v[28 + u * 4 + 2], static_cast<uint16_t>(static_cast<int16_t>(-200 - static_cast<int>(u))));
    }
    putBe32(&v[v.size() - 4], 0xDEADBEEF);
    return v;
}

std::vector<uint8_t> makeContextPacket(const char* pJson)
{
    const size_t uJsonLength = (pJson != nullptr) ? std::strlen(pJson) + 1 : 0;
    const size_t uPadded     = (uJsonLength + 3) & ~size_t(3);
    std::vector<uint8_t> v(92 + uPadded, 0);
    buildHeader(v, 0x4, 0, 0x55667788, kVita49ClassId, 1789572208, 0);

    const double dRate = 204800.0;
    uint64_t uBits = 0;
    std::memcpy(&uBits, &dRate, sizeof(uBits));
    putBe64(&v[44], uBits);

    if (pJson != nullptr)
        std::memcpy(&v[92], pJson, std::strlen(pJson));
    return v;
}

int32_t be16Signed(const uint8_t* p)
{
    return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testRealDataPacket()
{
    Vita49Packet oPacket;
    assert(parseVita49Packet(kRealDataPacket, sizeof(kRealDataPacket), oPacket));

    assert(oPacket.eType          == Vita49Type::Data);
    assert(oPacket.uStreamId      == 0x2834DD42u);
    assert(oPacket.uClassId       == kVita49ClassId);
    assert(oPacket.uPacketCount   == 2u);
    assert(oPacket.uDeclaredWords == 32u);
    assert(oPacket.uTsi           == 1u);   // UTC
    assert(oPacket.uTsf           == 2u);   // real-time picoseconds
    assert(oPacket.uTimestampSec  == 1789572208ull);
    assert(oPacket.uTimestampPs   == 406181921875ull);
    assert(oPacket.uSampleCount   == 24u);
    assert(oPacket.uTrailerRaw    == 0xF800E000u);

    // The payload must be a view into the caller's buffer, never a copy.
    assert(oPacket.pPayload == kRealDataPacket + 28);

    // Real sample values, I then Q, big-endian int16.
    assert(be16Signed(oPacket.pPayload)      ==  6481);
    assert(be16Signed(oPacket.pPayload + 2)  ==  1926);
    assert(be16Signed(oPacket.pPayload + 4)  ==  6072);
    assert(be16Signed(oPacket.pPayload + 6)  ==  -876);
    assert(be16Signed(oPacket.pPayload + 8)  ==  6477);
    assert(be16Signed(oPacket.pPayload + 10) == -1252);
}

void testSyntheticDataPacket()
{
    const std::vector<uint8_t> v = makeDataPacket(4, 7, 0xAABBCCDD, 1000, 250000000000ULL);

    Vita49Packet oPacket;
    assert(parseVita49Packet(v.data(), v.size(), oPacket));

    assert(oPacket.eType         == Vita49Type::Data);
    assert(oPacket.uStreamId     == 0xAABBCCDDu);
    assert(oPacket.uPacketCount  == 7u);
    assert(oPacket.uTimestampSec == 1000ull);
    assert(oPacket.uTimestampPs  == 250000000000ull);
    assert(oPacket.uSampleCount  == 4u);
    assert(oPacket.uTrailerRaw   == 0xDEADBEEFu);
    assert(be16Signed(oPacket.pPayload)     ==  100);
    assert(be16Signed(oPacket.pPayload + 2) == -200);
}

void testEmptyDataPacket()
{
    // 28-byte header plus trailer and no samples at all is still well formed.
    const std::vector<uint8_t> v = makeDataPacket(0);

    Vita49Packet oPacket;
    assert(parseVita49Packet(v.data(), v.size(), oPacket));
    assert(oPacket.eType        == Vita49Type::Data);
    assert(oPacket.uSampleCount == 0u);
}

void testContextPacket()
{
    const std::vector<uint8_t> v = makeContextPacket(kRealContextJson);

    Vita49Packet oPacket;
    assert(parseVita49Packet(v.data(), v.size(), oPacket));

    assert(oPacket.eType       == Vita49Type::Context);
    assert(oPacket.dSampleRate == 204800.0);
    assert(oPacket.pJson       == reinterpret_cast<const char*>(v.data()) + 92);
    assert(oPacket.uJsonLength == std::strlen(kRealContextJson));
}

void testContextPacketWithoutJson()
{
    // Exactly the fixed header, no metadata. Legal, and must not run off the
    // end looking for a NUL.
    const std::vector<uint8_t> v = makeContextPacket(nullptr);

    Vita49Packet oPacket;
    assert(parseVita49Packet(v.data(), v.size(), oPacket));
    assert(oPacket.eType       == Vita49Type::Context);
    assert(oPacket.uJsonLength == 0u);
}

void testUnknownTypeIsTolerated()
{
    // A well-formed packet of a type we do not handle must be reported as
    // Other rather than rejected: the FERS format is officially unverified,
    // so unexpected types are tolerated by design.
    std::vector<uint8_t> v = makeDataPacket(4);
    v[0] = static_cast<uint8_t>((0x5u << 4) | 0x0E);

    Vita49Packet oPacket;
    assert(parseVita49Packet(v.data(), v.size(), oPacket));
    assert(oPacket.eType == Vita49Type::Other);
}

void testMalformedRejected()
{
    Vita49Packet oPacket;
    const std::vector<uint8_t> vGood = makeDataPacket(8);

    // Null buffer.
    assert(!parseVita49Packet(nullptr, 64, oPacket));

    // Shorter than the common header.
    assert(!parseVita49Packet(vGood.data(), 27, oPacket));

    // Length not a whole number of 32-bit words.
    {
        std::vector<uint8_t> v = vGood;
        v.push_back(0);
        assert(!parseVita49Packet(v.data(), v.size(), oPacket));
    }

    // Wrong class ID.
    {
        std::vector<uint8_t> v = vGood;
        v[8] ^= 0xFF;
        assert(!parseVita49Packet(v.data(), v.size(), oPacket));
    }

    // Declared size disagrees with the datagram length.
    {
        std::vector<uint8_t> v = vGood;
        putBe16(&v[2], static_cast<uint16_t>(v.size() / 4 + 1));
        assert(!parseVita49Packet(v.data(), v.size(), oPacket));
    }

    // Fractional timestamp of one second or more.
    {
        std::vector<uint8_t> v = vGood;
        putBe64(&v[20], 1000000000000ULL);
        assert(!parseVita49Packet(v.data(), v.size(), oPacket));
    }

    // TSF mode 1 is a sample count, not picoseconds; absoluteIndex() would be
    // silently wrong, so it must be refused.
    {
        std::vector<uint8_t> v = vGood;
        v[1] = static_cast<uint8_t>((1u << 6) | (1u << 4));
        assert(!parseVita49Packet(v.data(), v.size(), oPacket));
    }

    // Context packet shorter than its fixed 92-byte header.
    {
        std::vector<uint8_t> v(88, 0);
        buildHeader(v, 0x4, 0, 1, kVita49ClassId, 1, 0);
        assert(!parseVita49Packet(v.data(), v.size(), oPacket));
    }
}

// The JSON scanner is not a parser, so its behaviour on real nested metadata
// is worth pinning down explicitly.
void testJsonExtraction()
{
    const size_t uLength = std::strlen(kRealContextJson);
    std::string sValue;

    // Anchoring inside the receiver object, exactly as handleContext does.
    const size_t uReceiverAt = findJsonKey(kRealContextJson, uLength, "receiver");
    assert(uReceiverAt < uLength);

    assert(findJsonString(kRealContextJson, uLength, "name", sValue, uReceiverAt));
    assert(sValue == "ArmasuisseSurRxClean");

    assert(findJsonString(kRealContextJson, uLength, "mode", sValue, uReceiverAt));
    assert(sValue == "cw");

    // Unanchored, "name" must still not match platform_name or waveform_name,
    // because the key is matched with its surrounding quotes.
    assert(findJsonString(kRealContextJson, uLength, "name", sValue, 0));
    assert(sValue == "ArmasuisseSurRxClean");

    // A key that is not present.
    assert(!findJsonString(kRealContextJson, uLength, "no_such_key", sValue, 0));

    // A key whose value is not a string is not extracted.
    assert(!findJsonString(kRealContextJson, uLength, "adc_bits", sValue, 0));

    // Degenerate inputs.
    assert(!findJsonString("", 0, "name", sValue, 0));
    assert(!findJsonString("{\"name\":", 8, "name", sValue, 0));
    assert(!findJsonString("{\"name\":\"unterminated", 21, "name", sValue, 0));
}

} // namespace

int main()
{
    testRealDataPacket();
    testSyntheticDataPacket();
    testEmptyDataPacket();
    testContextPacket();
    testContextPacketWithoutJson();
    testUnknownTypeIsTolerated();
    testMalformedRejected();
    testJsonExtraction();

    std::printf("test_vita49_packet: all tests passed\n");
    return 0;
}
