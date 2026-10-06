#ifndef PBR_ARD_STREAM_HPP
#define PBR_ARD_STREAM_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pbr/UdpSender.hpp"

namespace pbr {

// The ARD stream: a serialised PBRARD01 frame (ArdFile.hpp) cut into UDP
// datagrams, plus one status datagram per frame. Both halves live here so
// the format has one home and one test. Design: docs/ard-stream-design.md.
//
// Little-endian throughout, like the frame itself. Two datagram types, each
// at most 1400 bytes so nothing fragments on a 1500-byte MTU:
//
//   frame chunk, magic "PBRARDS1"
//     offset  size  field
//          0     8  magic
//          8     4  uint32 frame sequence (CPI index mod 2^32)
//         12     4  uint32 chunk index, 0-based
//         16     4  uint32 chunk count
//         20     4  uint32 frame bytes in total
//         24     4  uint32 payload bytes in this chunk
//         28     4  reserved, 0
//         32  <=1368 payload: bytes [index * 1368, ...) of the frame
//
//   status, magic "PBRARDT1"
//     offset  size  field
//          0     8  magic
//          8     4  uint32 frame sequence it describes
//         12     4  uint32 JSON bytes, NUL included
//         16  <=1384 ASCII JSON, NUL-terminated
//
//   spectrum, magic "PBRARDP1" (design note 4.4, added 2026-09-30)
//     offset  size  field
//          0     8  magic
//          8     4  uint32 frame sequence it belongs to
//         12     4  uint32 channel: 0 reference, 1 surveillance
//         16     4  uint32 bins N
//         20     4  uint32 sample rate, Hz
//         24    2N  int16 dB x 100 per bin, bin 0 = -fs/2, bin N/2 = carrier
//
// A 120 x 1600 frame is 1123 chunks. The spectra and then the status are
// sent after the frame's last chunk, so a receiver holding a complete frame
// and its status has a consistent set.

constexpr size_t kArdChunkHeaderBytes   = 32;
constexpr size_t kArdChunkPayloadBytes  = 1368;
constexpr size_t kArdDatagramBytes      = 1400;   // header + full payload
constexpr size_t kArdStatusHeaderBytes  = 16;
constexpr size_t kArdStatusMaxJsonBytes = kArdDatagramBytes - kArdStatusHeaderBytes;
constexpr size_t kArdSpectrumHeaderBytes = 24;
constexpr size_t kArdSpectrumMaxBins     = (kArdDatagramBytes - kArdSpectrumHeaderBytes) / 2;   // 688
constexpr uint32_t kArdSpectrumChannels  = 2;

// The sending half, used by pbr_run. Owns a UdpSender; the destination may
// be unicast, multicast or broadcast as that class allows.
class ArdStreamSender {
public:
    bool open(const char* pHost, uint16_t uPort, const char* pInterfaceIp);
    bool isOpen() const { return moSender.isOpen(); }

    // Cut a serialised frame into chunks and send them in order. A send
    // failure is counted by the sender and does not stop the frame.
    bool sendFrame(const uint8_t* pFrame, size_t uLength, uint32_t uSequence);

    // pJson must be NUL-terminated ASCII of fewer than
    // kArdStatusMaxJsonBytes bytes including the NUL.
    bool sendStatus(const char* pJson, uint32_t uSequence);

    // One channel's spectrum in dB (SpectrumEstimator's output), sent as
    // int16 hundredths. uBins at most kArdSpectrumMaxBins, uChannel below
    // kArdSpectrumChannels.
    bool sendSpectrum(const float* pDb, uint32_t uBins, uint32_t uChannel,
                      uint32_t uSampleRateHz, uint32_t uSequence);

    uint64_t sendFailures() const { return moSender.sendFailures(); }

private:
    UdpSender moSender;
    uint8_t   mauDatagram[kArdDatagramBytes];
};

// What one datagram did to the assembler's state.
enum class ArdFeedResult {
    Ignored,          // not ours, malformed, or for a sequence we are not holding
    ChunkStored,      // a chunk of the frame being assembled
    FrameComplete,    // frame() and frameSequence() are now valid
    StatusAttached,   // statusJson() now describes frame()
    SpectrumAttached  // spectrum(channel) now belongs to frame()
};

// The receiving half, used by ard_view. Reassembles one frame at a time and
// keeps the newest complete one. Two buffers, allocated when the first chunk
// says how big a frame is and reallocated only if that changes; no
// per-datagram allocation.
//
// Rules (design note 4.3): a chunk for a new sequence starts a new frame and
// drops whatever was incomplete, counted in framesDropped(); duplicates are
// ignored; order does not matter; a completed frame must pass
// parseArdFrameHeader() or it is dropped too. A status datagram attaches to
// the complete frame of the same sequence, or waits for the frame being
// assembled to complete, or is ignored.
class ArdFrameAssembler {
public:
    ArdFrameAssembler();

    ArdFeedResult feed(const uint8_t* pDatagram, size_t uLength);

    // Valid after the first FrameComplete.
    bool haveFrame() const { return mbHaveComplete; }
    const std::vector<uint8_t>& frame() const { return mvuComplete; }
    uint32_t frameSequence() const { return muCompleteSequence; }

    // The status JSON for frame(), or empty if none has arrived for it.
    const std::string& statusJson() const { return msStatus; }

    // The spectrum datagram (header included) of one channel for frame(),
    // or empty if none has arrived for it. Attaches like the status does.
    const std::vector<uint8_t>& spectrum(uint32_t uChannel) const;

    uint64_t chunksReceived()  const { return muChunksReceived; }
    uint64_t chunksRejected()  const { return muChunksRejected; }
    uint64_t framesCompleted() const { return muFramesCompleted; }
    uint64_t framesDropped()   const { return muFramesDropped; }

private:
    ArdFeedResult feedChunk(const uint8_t* pDatagram, size_t uLength);
    ArdFeedResult feedStatus(const uint8_t* pDatagram, size_t uLength);
    ArdFeedResult feedSpectrum(const uint8_t* pDatagram, size_t uLength);
    ArdFeedResult publishFrame();   // the working frame is complete
    void dropWorking();

    // The frame being assembled.
    bool                 mbAssembling;
    uint32_t             muSequence;
    uint32_t             muChunkCount;
    uint32_t             muChunksGot;
    uint32_t             muFrameBytes;
    std::vector<uint8_t> mvuWorking;
    std::vector<uint8_t> mvuChunkSeen;    // one flag per chunk index

    // The newest complete frame and its status.
    bool                 mbHaveComplete;
    uint32_t             muCompleteSequence;
    std::vector<uint8_t> mvuComplete;
    std::string          msStatus;

    // A status that arrived before its frame completed.
    bool        mbHavePendingStatus;
    uint32_t    muPendingSequence;
    std::string msPendingStatus;

    // The spectra for the complete frame, and any that arrived early.
    std::vector<uint8_t> mavuSpectrum[kArdSpectrumChannels];
    std::vector<uint8_t> mavuPendingSpectrum[kArdSpectrumChannels];
    uint32_t             mauPendingSpectrumSequence[kArdSpectrumChannels];
    std::vector<uint8_t> mvuNoSpectrum;  // what spectrum() returns when there is none

    uint64_t muChunksReceived;
    uint64_t muChunksRejected;
    uint64_t muFramesCompleted;
    uint64_t muFramesDropped;
};

} // namespace pbr

#endif // PBR_ARD_STREAM_HPP
