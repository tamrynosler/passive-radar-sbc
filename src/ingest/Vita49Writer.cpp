#include "pbr/Vita49Writer.hpp"

#include "pbr/ChannelIngest.hpp"   // kMaxSamplesPerPacket
#include "pbr/Vita49Packet.hpp"    // kVita49ClassId, offsets
#include "pbr/check.hpp"

#include <cstdio>
#include <cstring>

namespace pbr {

namespace {

constexpr uint64_t kPicosecondsPerSecond = 1000000000000ULL;

// Big-endian field writers, byte by byte: the inverse of readBe* in
// Vita49Packet.cpp, and for the same reasons (no alignment assumptions, no
// dependence on the host byte order).

void writeBe16(uint8_t* p, uint16_t uValue)
{
    p[0] = static_cast<uint8_t>(uValue >> 8);
    p[1] = static_cast<uint8_t>(uValue);
}

void writeBe32(uint8_t* p, uint32_t uValue)
{
    p[0] = static_cast<uint8_t>(uValue >> 24);
    p[1] = static_cast<uint8_t>(uValue >> 16);
    p[2] = static_cast<uint8_t>(uValue >>  8);
    p[3] = static_cast<uint8_t>(uValue);
}

void writeBe64(uint8_t* p, uint64_t uValue)
{
    for (size_t u = 0; u < 8; ++u)
        p[u] = static_cast<uint8_t>(uValue >> (56 - 8 * u));
}

// IEEE-754 double as a big-endian bit pattern; memcpy rather than a cast so
// there is no aliasing violation.
void writeBeDouble(uint8_t* p, double dValue)
{
    uint64_t uBits = 0;
    std::memcpy(&uBits, &dValue, sizeof(uBits));
    writeBe64(p, uBits);
}

// The 28-byte common header. Indicator bits: class ID present (27), trailer
// present (26, data packets only) and FERS's "VITA 49.2 format" bit (25).
// TSI = 1 (UTC), TSF = 2 (real-time picoseconds), as the parser enforces.
void writeCommonHeader(uint8_t* p, uint32_t uType, bool bTrailer,
                       uint8_t uPacketCount, size_t uTotalBytes,
                       uint32_t uStreamId, const Vita49Time& oTime)
{
    p[0] = static_cast<uint8_t>((uType << 4) | 0x8u | (bTrailer ? 0x4u : 0x0u) | 0x2u);
    p[1] = static_cast<uint8_t>((1u << 6) | (2u << 4) | (uPacketCount & 0x0Fu));
    writeBe16(p + 2, static_cast<uint16_t>(uTotalBytes / 4));
    writeBe32(p + 4, uStreamId);
    writeBe64(p + 8, kVita49ClassId);
    writeBe32(p + 16, oTime.uSeconds);
    writeBe64(p + 20, oTime.uPicoseconds);
}

// Trailer and context state indicators share one layout: bits 31:27 enable
// the five indicators, bits 15:11 carry their values, same order. FERS sets
// every enable bit; so do we.
uint32_t makeIndicators(bool bRefLocked, bool bSampleLoss)
{
    uint32_t uWord = 0xF8000000u;
    uWord |= (1u << 14);                       // valid data
    if (bRefLocked)
        uWord |= (1u << 15) | (1u << 13);      // calibrated time, reference lock
    // over-range (bit 12) stays clear: not reported by the N210
    if (bSampleLoss)
        uWord |= (1u << 11);
    return uWord;
}

// A receiver name goes into the JSON unescaped, so it must be plain ASCII
// without the two characters that would break the string.
bool isSafeJsonString(const std::string& s)
{
    for (size_t u = 0; u < s.size(); ++u) {
        const unsigned char ch = static_cast<unsigned char>(s[u]);
        if (ch < 0x20 || ch > 0x7E || ch == '"' || ch == '\\')
            return false;
    }
    return true;
}

} // namespace

Vita49Time makeTime(uint64_t uSampleIndex, uint64_t uSampleRateHz)
{
    Vita49Time oTime;
    oTime.uSeconds     = 0;
    oTime.uPicoseconds = 0;
    if (uSampleRateHz == 0)
        return oTime;

    const uint64_t uRemainder = uSampleIndex % uSampleRateHz;

    // r < fs <= 1e7, so r * 1e12 < 1e19 < 2^64: no overflow for any rate the
    // driver accepts. Ceiling division, see the header.
    oTime.uSeconds     = static_cast<uint32_t>(uSampleIndex / uSampleRateHz);
    oTime.uPicoseconds = (uRemainder * kPicosecondsPerSecond + uSampleRateHz - 1)
                         / uSampleRateHz;
    return oTime;
}

bool writeVita49Data(uint8_t* pOut, size_t uCapacity,
                     uint32_t uStreamId, uint8_t uPacketCount,
                     const Vita49Time& oTime,
                     const int16_t* pSamples, uint64_t uCount,
                     bool bRefLocked, bool bSampleLoss,
                     size_t& uLengthOut)
{
    uLengthOut = 0;

    CHECK(pOut != nullptr, "writeVita49Data with a null output buffer");
    CHECK(pSamples != nullptr || uCount == 0, "writeVita49Data with null samples");
    CHECK(uCount <= kMaxSamplesPerPacket, "more samples than the ingest accepts per packet");
    CHECK(oTime.uPicoseconds < kPicosecondsPerSecond, "picoseconds not below one second");

    const size_t uTotal = kVita49DataOverheadBytes + static_cast<size_t>(uCount) * 4;
    CHECK(uTotal <= uCapacity, "output buffer too small for the data packet");

    writeCommonHeader(pOut, 0x1, true, uPacketCount, uTotal, uStreamId, oTime);

    // sc16 from UHD is host-order int16 I, int16 Q per sample; VITA wants each
    // int16 big-endian. Same order, swapped bytes.
    uint8_t* p = pOut + kVita49CommonHeaderBytes;
    for (uint64_t u = 0; u < uCount; ++u) {
        writeBe16(p,     static_cast<uint16_t>(pSamples[2 * u]));
        writeBe16(p + 2, static_cast<uint16_t>(pSamples[2 * u + 1]));
        p += 4;
    }

    writeBe32(p, makeIndicators(bRefLocked, bSampleLoss));

    uLengthOut = uTotal;
    return true;
}

bool writeVita49Context(uint8_t* pOut, size_t uCapacity,
                        const Vita49StreamInfo& oInfo, uint8_t uPacketCount,
                        const Vita49Time& oTime,
                        bool bRefLocked, uint32_t uContextFlags,
                        size_t& uLengthOut)
{
    uLengthOut = 0;

    CHECK(pOut != nullptr, "writeVita49Context with a null output buffer");
    CHECK(oTime.uPicoseconds < kPicosecondsPerSecond, "picoseconds not below one second");
    CHECK(!oInfo.sReceiverName.empty(), "receiver name is empty");
    CHECK(isSafeJsonString(oInfo.sReceiverName), "receiver name is not plain ASCII");
    CHECK(isSafeJsonString(oInfo.sDevice), "device string is not plain ASCII");
    CHECK(isSafeJsonString(oInfo.sDriverVersion), "driver version is not plain ASCII");
    CHECK(isSafeJsonString(oInfo.sSessionId), "session id is not plain ASCII");

    // The JSON the ingest reads: "receiver" first, then "name" and "mode"
    // inside it, before any other key called name or mode. Everything else is
    // for a human or a FERS-aware tool.
    char achJson[512];
    const int iJson = std::snprintf(
        achJson, sizeof(achJson),
        "{\"schema\":\"fers-vita49-context-v1\","
        "\"simulation_name\":\"uhd_to_vita49\","
        "\"receiver\":{\"id\":%llu,\"name\":\"%s\",\"mode\":\"cw\","
        "\"session\":\"%s\",\"adc_bits\":%u,\"context_flags\":%u},"
        "\"source\":{\"device\":\"%s\",\"channel\":%u,\"fc_hz\":%.1f,"
        "\"gain_db\":%.2f,\"ext_ref\":%s,\"uhd\":\"%s\"}}",
        static_cast<unsigned long long>(oInfo.uReceiverId),
        oInfo.sReceiverName.c_str(),
        oInfo.sSessionId.c_str(),
        oInfo.uAdcBits,
        static_cast<unsigned>(uContextFlags),
        oInfo.sDevice.c_str(),
        oInfo.uChannel,
        oInfo.dCentreFreqHz,
        oInfo.dGainDb,
        oInfo.bExtRef ? "true" : "false",
        oInfo.sDriverVersion.c_str());
    CHECK(iJson > 0 && static_cast<size_t>(iJson) < sizeof(achJson),
          "context JSON does not fit its buffer");

    // NUL-terminated, then zero-padded to a 32-bit boundary, as FERS does.
    const size_t uJsonBytes  = static_cast<size_t>(iJson) + 1;
    const size_t uJsonPadded = (uJsonBytes + 3) & ~static_cast<size_t>(3);
    const size_t uTotal      = kVita49ContextHeaderBytes + uJsonPadded;
    CHECK(uTotal <= uCapacity, "output buffer too small for the context packet");

    writeCommonHeader(pOut, 0x4, false, uPacketCount, uTotal, oInfo.uStreamId, oTime);

    const bool bSampleLoss = (uContextFlags & kVita49FlagSampleLoss) != 0;

    writeBe32    (pOut + 28, 0xFF800000u);                    // CIF0: the nine fields below
    writeBe32    (pOut + 32, makeIndicators(bRefLocked, bSampleLoss));
    writeBe64    (pOut + 36, 0x2110100200010001ULL);          // complex Cartesian int16 pairs
    writeBeDouble(pOut + kVita49SampleRateOffset, oInfo.dSampleRateHz);
    writeBeDouble(pOut + 52, oInfo.dCentreFreqHz);
    writeBeDouble(pOut + 60, 0.0);                            // IF offset
    writeBeDouble(pOut + 68, oInfo.dBandwidthHz);
    writeBeDouble(pOut + 76, oInfo.dAdcFullScale);
    writeBe64    (pOut + 84, oInfo.uReceiverId);

    std::memset(pOut + kVita49ContextHeaderBytes, 0, uJsonPadded);
    std::memcpy(pOut + kVita49ContextHeaderBytes, achJson, static_cast<size_t>(iJson));

    uLengthOut = uTotal;
    return true;
}

} // namespace pbr
