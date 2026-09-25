#include "pbr/ArdStream.hpp"

#include "pbr/ArdFile.hpp"
#include "pbr/check.hpp"

#include <cstdio>
#include <cstring>

namespace pbr {

namespace {

const char kChunkMagic[8]  = { 'P', 'B', 'R', 'A', 'R', 'D', 'S', '1' };
const char kStatusMagic[8] = { 'P', 'B', 'R', 'A', 'R', 'D', 'T', '1' };

// Sanity bound on a corrupt chunk count: a frame of kMaxCells cells is
// under two million chunks.
const uint32_t kMaxChunks = 1u << 21;

void putU32(uint8_t* p, uint32_t uValue) { std::memcpy(p, &uValue, sizeof(uValue)); }
uint32_t getU32(const uint8_t* p)        { uint32_t u; std::memcpy(&u, p, sizeof(u)); return u; }

} // namespace

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------

bool ArdStreamSender::open(const char* pHost, uint16_t uPort, const char* pInterfaceIp)
{
    return moSender.open(pHost, uPort, pInterfaceIp);
}

bool ArdStreamSender::sendFrame(const uint8_t* pFrame, size_t uLength, uint32_t uSequence)
{
    CHECK(moSender.isOpen(), "ArdStreamSender: sendFrame before open");
    CHECK(pFrame != nullptr && uLength > 0, "ArdStreamSender: empty frame");

    const size_t uChunks = (uLength + kArdChunkPayloadBytes - 1) / kArdChunkPayloadBytes;
    CHECK(uChunks <= kMaxChunks, "ArdStreamSender: frame too large to chunk");
    CHECK(uLength <= 0xFFFFFFFFu, "ArdStreamSender: frame length does not fit 32 bits");

    std::memcpy(mauDatagram, kChunkMagic, 8);
    putU32(mauDatagram + 8,  uSequence);
    putU32(mauDatagram + 16, static_cast<uint32_t>(uChunks));
    putU32(mauDatagram + 20, static_cast<uint32_t>(uLength));
    putU32(mauDatagram + 28, 0);

    for (size_t uIndex = 0; uIndex < uChunks; ++uIndex) {
        const size_t uOffset  = uIndex * kArdChunkPayloadBytes;
        const size_t uPayload = (uLength - uOffset < kArdChunkPayloadBytes)
                              ? (uLength - uOffset) : kArdChunkPayloadBytes;
        putU32(mauDatagram + 12, static_cast<uint32_t>(uIndex));
        putU32(mauDatagram + 24, static_cast<uint32_t>(uPayload));
        std::memcpy(mauDatagram + kArdChunkHeaderBytes, pFrame + uOffset, uPayload);
        moSender.send(mauDatagram, kArdChunkHeaderBytes + uPayload);
    }
    return true;
}

bool ArdStreamSender::sendStatus(const char* pJson, uint32_t uSequence)
{
    CHECK(moSender.isOpen(), "ArdStreamSender: sendStatus before open");
    CHECK(pJson != nullptr, "ArdStreamSender: null status");

    const size_t uJsonBytes = std::strlen(pJson) + 1;   // NUL included
    CHECK(uJsonBytes <= kArdStatusMaxJsonBytes, "ArdStreamSender: status JSON too long");

    std::memcpy(mauDatagram, kStatusMagic, 8);
    putU32(mauDatagram + 8,  uSequence);
    putU32(mauDatagram + 12, static_cast<uint32_t>(uJsonBytes));
    std::memcpy(mauDatagram + kArdStatusHeaderBytes, pJson, uJsonBytes);
    moSender.send(mauDatagram, kArdStatusHeaderBytes + uJsonBytes);
    return true;
}

// ---------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------

ArdFrameAssembler::ArdFrameAssembler()
    : mbAssembling(false),
      muSequence(0),
      muChunkCount(0),
      muChunksGot(0),
      muFrameBytes(0),
      mbHaveComplete(false),
      muCompleteSequence(0),
      mbHavePendingStatus(false),
      muPendingSequence(0),
      muChunksReceived(0),
      muChunksRejected(0),
      muFramesCompleted(0),
      muFramesDropped(0)
{
}

ArdFeedResult ArdFrameAssembler::feed(const uint8_t* pDatagram, size_t uLength)
{
    if (pDatagram == nullptr || uLength < 16)
        return ArdFeedResult::Ignored;
    if (std::memcmp(pDatagram, kChunkMagic, 8) == 0)
        return feedChunk(pDatagram, uLength);
    if (std::memcmp(pDatagram, kStatusMagic, 8) == 0)
        return feedStatus(pDatagram, uLength);
    return ArdFeedResult::Ignored;
}

void ArdFrameAssembler::dropWorking()
{
    if (mbAssembling) {
        ++muFramesDropped;
        std::fprintf(stderr, "ArdFrameAssembler: frame %u dropped with %u of %u chunks\n",
                     muSequence, muChunksGot, muChunkCount);
    }
    mbAssembling = false;
    muChunksGot  = 0;
}

ArdFeedResult ArdFrameAssembler::feedChunk(const uint8_t* pDatagram, size_t uLength)
{
    if (uLength < kArdChunkHeaderBytes) {
        ++muChunksRejected;
        return ArdFeedResult::Ignored;
    }

    const uint32_t uSequence   = getU32(pDatagram + 8);
    const uint32_t uIndex      = getU32(pDatagram + 12);
    const uint32_t uCount      = getU32(pDatagram + 16);
    const uint32_t uFrameBytes = getU32(pDatagram + 20);
    const uint32_t uPayload    = getU32(pDatagram + 24);

    // Everything the header claims must be consistent with itself and with
    // the datagram, or the chunk is not trusted.
    const bool bLast = (uIndex + 1 == uCount);
    const uint32_t uExpectedPayload = bLast
        ? uFrameBytes - uIndex * static_cast<uint32_t>(kArdChunkPayloadBytes)
        : static_cast<uint32_t>(kArdChunkPayloadBytes);
    if (uCount == 0 || uCount > kMaxChunks || uIndex >= uCount ||
        uFrameBytes < kArdHeaderBytes ||
        (uFrameBytes + kArdChunkPayloadBytes - 1) / kArdChunkPayloadBytes != uCount ||
        uPayload != uExpectedPayload ||
        uLength != kArdChunkHeaderBytes + uPayload) {
        ++muChunksRejected;
        return ArdFeedResult::Ignored;
    }
    ++muChunksReceived;

    // A different sequence starts a new frame; whatever was incomplete is
    // lost, which at one frame a second a viewer tolerates.
    if (!mbAssembling || uSequence != muSequence ||
        uCount != muChunkCount || uFrameBytes != muFrameBytes) {
        if (mbAssembling && uSequence != muSequence)
            dropWorking();
        mbAssembling = true;
        muSequence   = uSequence;
        muChunkCount = uCount;
        muFrameBytes = uFrameBytes;
        muChunksGot  = 0;
        if (mvWorking.size() != uFrameBytes)
            mvWorking.assign(uFrameBytes, 0);
        mvChunkSeen.assign(uCount, 0);
    }

    if (mvChunkSeen[uIndex])
        return ArdFeedResult::ChunkStored;   // duplicate

    std::memcpy(mvWorking.data() + static_cast<size_t>(uIndex) * kArdChunkPayloadBytes,
                pDatagram + kArdChunkHeaderBytes, uPayload);
    mvChunkSeen[uIndex] = 1;
    ++muChunksGot;

    if (muChunksGot < muChunkCount)
        return ArdFeedResult::ChunkStored;

    // Complete. The same test the file reader applies, on the bytes.
    mbAssembling = false;
    ArdFileHeader oHeader;
    if (!parseArdFrameHeader(mvWorking.data(), mvWorking.size(), oHeader)) {
        ++muFramesDropped;
        return ArdFeedResult::Ignored;
    }

    // Publish by swapping buffers: no allocation, and the old complete
    // buffer becomes the next working buffer.
    mvComplete.swap(mvWorking);
    mbHaveComplete     = true;
    muCompleteSequence = muSequence;
    ++muFramesCompleted;

    msStatus.clear();
    if (mbHavePendingStatus && muPendingSequence == muSequence) {
        msStatus.swap(msPendingStatus);
        mbHavePendingStatus = false;
    }
    return ArdFeedResult::FrameComplete;
}

ArdFeedResult ArdFrameAssembler::feedStatus(const uint8_t* pDatagram, size_t uLength)
{
    const uint32_t uSequence  = getU32(pDatagram + 8);
    const uint32_t uJsonBytes = getU32(pDatagram + 12);
    if (uJsonBytes == 0 || uJsonBytes > kArdStatusMaxJsonBytes ||
        uLength != kArdStatusHeaderBytes + uJsonBytes ||
        pDatagram[kArdStatusHeaderBytes + uJsonBytes - 1] != '\0')
        return ArdFeedResult::Ignored;

    const char* pJson = reinterpret_cast<const char*>(pDatagram + kArdStatusHeaderBytes);

    if (mbHaveComplete && uSequence == muCompleteSequence) {
        msStatus.assign(pJson);
        return ArdFeedResult::StatusAttached;
    }
    if (mbAssembling && uSequence == muSequence) {
        msPendingStatus.assign(pJson);
        muPendingSequence   = uSequence;
        mbHavePendingStatus = true;
        return ArdFeedResult::ChunkStored;
    }
    return ArdFeedResult::Ignored;
}

} // namespace pbr
