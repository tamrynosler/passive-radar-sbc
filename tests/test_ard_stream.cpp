// Unit tests for the ARD frame serialiser and the ARD stream (chunking and
// reassembly). Plain main() + asserts, no framework.
//
// NDEBUG is undefined deliberately and before <cassert>: the project builds
// Release, which defines NDEBUG and would compile every assert below out.
#undef NDEBUG
#include <cassert>

#include "pbr/ArdFile.hpp"
#include "pbr/ArdStream.hpp"
#include "pbr/UdpReceiver.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace pbr;

namespace {

// Small so a frame is a handful of chunks: 6 x 400 cells = 19200 bytes of
// map, 64 of header, 2 detections = 19360 bytes = 15 chunks (14 full + 1).
constexpr uint64_t kR = 6;
constexpr uint64_t kD = 400;

void putU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

ArdFileHeader makeHeader(uint64_t uDetections, uint64_t uStatus)
{
    ArdFileHeader oHeader;
    oHeader.uRangeBins         = kR;
    oHeader.uDopplerBins       = kD;
    oHeader.uSampleRateHz      = 200000;
    oHeader.uDopplerDecimation = 128;
    oHeader.uCpiStartIndex     = 358065970461839ULL;
    oHeader.uDetections        = uDetections;
    oHeader.uStatus            = uStatus;
    return oHeader;
}

std::vector<ComplexF> makeMap()
{
    std::vector<ComplexF> vcMap(kR * kD);
    for (size_t u = 0; u < vcMap.size(); ++u)
        vcMap[u] = ComplexF(float(u) * 0.5f, -float(u));
    return vcMap;
}

std::vector<Detection> makeDetections()
{
    std::vector<Detection> v(2);
    v[0].uRangeBin = 3;  v[0].uDopplerBin = 210; v[0].dRangeKm = 4.5;  v[0].dDopplerHz = 37.1;  v[0].dPowerDb = -3.0; v[0].dSnrDb = 24.3;
    v[1].uRangeBin = 5;  v[1].uDopplerBin = 12;  v[1].dRangeKm = 7.49; v[1].dDopplerHz = -777.3; v[1].dPowerDb = -40.0; v[1].dSnrDb = 1.9;
    return v;
}

std::vector<uint8_t> serialise(const ArdFileHeader& oHeader)
{
    const std::vector<ComplexF>  vcMap = makeMap();
    const std::vector<Detection> vDet  = makeDetections();
    std::vector<uint8_t> v(ardFrameBytes(oHeader) + 100, 0xEE);
    size_t uLength = 0;
    assert(serialiseArdFrame(oHeader, vcMap.data(), vDet.data(), v.data(), v.size(), uLength));
    assert(uLength == ardFrameBytes(oHeader));
    v.resize(uLength);
    return v;
}

// A chunk built by hand from the documented layout, independently of the
// sender, so the two are checked against the document rather than each other.
std::vector<uint8_t> makeChunk(const std::vector<uint8_t>& vFrame, uint32_t uSeq, uint32_t uIndex)
{
    const uint32_t uCount   = uint32_t((vFrame.size() + kArdChunkPayloadBytes - 1) / kArdChunkPayloadBytes);
    const size_t   uOffset  = size_t(uIndex) * kArdChunkPayloadBytes;
    const size_t   uPayload = (vFrame.size() - uOffset < kArdChunkPayloadBytes)
                            ? vFrame.size() - uOffset : kArdChunkPayloadBytes;
    std::vector<uint8_t> v(kArdChunkHeaderBytes + uPayload, 0);
    std::memcpy(v.data(), "PBRARDS1", 8);
    putU32(&v[8], uSeq);
    putU32(&v[12], uIndex);
    putU32(&v[16], uCount);
    putU32(&v[20], uint32_t(vFrame.size()));
    putU32(&v[24], uint32_t(uPayload));
    std::memcpy(v.data() + kArdChunkHeaderBytes, vFrame.data() + uOffset, uPayload);
    return v;
}

std::vector<uint8_t> makeStatus(uint32_t uSeq, const char* pJson)
{
    const size_t uJson = std::strlen(pJson) + 1;
    std::vector<uint8_t> v(kArdStatusHeaderBytes + uJson, 0);
    std::memcpy(v.data(), "PBRARDT1", 8);
    putU32(&v[8], uSeq);
    putU32(&v[12], uint32_t(uJson));
    std::memcpy(v.data() + kArdStatusHeaderBytes, pJson, uJson);
    return v;
}

uint32_t chunkCount(const std::vector<uint8_t>& vFrame)
{
    return uint32_t((vFrame.size() + kArdChunkPayloadBytes - 1) / kArdChunkPayloadBytes);
}

// ---------------------------------------------------------------------------

// The serialiser produces exactly what writeArdFile puts on disk, and the
// header parser reads it back, status word included.
void testSerialiseMatchesFile()
{
    const ArdFileHeader oHeader = makeHeader(2, kArdStatusNullInput);
    const std::vector<uint8_t> vBytes = serialise(oHeader);
    assert(vBytes.size() == 64 + 8 * kR * kD + 48 * 2);

    char achPath[] = "/tmp/pbr_test_ard_XXXXXX";
    const int iFd = mkstemp(achPath);
    assert(iFd >= 0);
    close(iFd);

    const std::vector<ComplexF>  vcMap = makeMap();
    const std::vector<Detection> vDet  = makeDetections();
    assert(writeArdFile(achPath, oHeader, vcMap.data(), vDet.data()));

    std::vector<uint8_t> vFile;
    std::FILE* pFile = std::fopen(achPath, "rb");
    assert(pFile != nullptr);
    uint8_t auBuf[4096];
    for (;;) {
        const size_t uGot = std::fread(auBuf, 1, sizeof(auBuf), pFile);
        if (uGot == 0) break;
        vFile.insert(vFile.end(), auBuf, auBuf + uGot);
    }
    std::fclose(pFile);
    assert(vFile == vBytes);

    // And the file reader agrees with the byte parser.
    ArdFileHeader oFromFile;
    assert(readArdFileHeader(achPath, oFromFile));
    ArdFileHeader oFromBytes;
    assert(parseArdFrameHeader(vBytes.data(), vBytes.size(), oFromBytes));
    assert(oFromFile.uRangeBins == kR && oFromBytes.uRangeBins == kR);
    assert(oFromFile.uDopplerBins == kD && oFromBytes.uDopplerBins == kD);
    assert(oFromFile.uDetections == 2 && oFromBytes.uDetections == 2);
    assert(oFromFile.uStatus == kArdStatusNullInput);
    assert(oFromBytes.uStatus == kArdStatusNullInput);
    assert(oFromBytes.uCpiStartIndex == 358065970461839ULL);

    std::vector<ComplexF>  vcBack(kR * kD);
    std::vector<Detection> vDetBack(4);
    assert(readArdFile(achPath, oFromFile, vcBack.data(), vcBack.size(), vDetBack.data(), 4));
    assert(vcBack == vcMap);
    assert(vDetBack[1].uDopplerBin == 12 && vDetBack[1].dDopplerHz == -777.3);

    std::remove(achPath);

    // Wrong length is refused; a bad magic is refused.
    assert(!parseArdFrameHeader(vBytes.data(), vBytes.size() - 8, oFromBytes));
    std::vector<uint8_t> vBad = vBytes;
    vBad[0] = 'X';
    assert(!parseArdFrameHeader(vBad.data(), vBad.size(), oFromBytes));

    // Too small an output buffer is refused.
    size_t uLength = 1;
    assert(!serialiseArdFrame(oHeader, vcMap.data(), vDet.data(), auBuf, sizeof(auBuf), uLength));
    assert(uLength == 0);
}

void testReassembleInOrderAndReverse()
{
    const std::vector<uint8_t> vFrame = serialise(makeHeader(2, kArdStatusOk));
    const uint32_t uCount = chunkCount(vFrame);
    assert(uCount == 15);

    ArdFrameAssembler oAsm;
    for (uint32_t u = 0; u < uCount; ++u) {
        const std::vector<uint8_t> v = makeChunk(vFrame, 7, u);
        const ArdFeedResult e = oAsm.feed(v.data(), v.size());
        assert(e == (u + 1 == uCount ? ArdFeedResult::FrameComplete : ArdFeedResult::ChunkStored));
    }
    assert(oAsm.haveFrame());
    assert(oAsm.frameSequence() == 7);
    assert(oAsm.frame() == vFrame);
    assert(oAsm.framesCompleted() == 1 && oAsm.framesDropped() == 0);
    assert(oAsm.statusJson().empty());

    // Status after the frame attaches to it.
    const std::vector<uint8_t> vStatus = makeStatus(7, "{\"cpi\":7}");
    assert(oAsm.feed(vStatus.data(), vStatus.size()) == ArdFeedResult::StatusAttached);
    assert(oAsm.statusJson() == "{\"cpi\":7}");

    // Reverse order, with a duplicate in the middle, for the next sequence.
    for (uint32_t k = 0; k < uCount; ++k) {
        const uint32_t uIndex = uCount - 1 - k;
        std::vector<uint8_t> v = makeChunk(vFrame, 8, uIndex);
        const ArdFeedResult e = oAsm.feed(v.data(), v.size());
        assert(e == (uIndex == 0 ? ArdFeedResult::FrameComplete : ArdFeedResult::ChunkStored));
        if (uIndex == 5)
            assert(oAsm.feed(v.data(), v.size()) == ArdFeedResult::ChunkStored);   // duplicate
    }
    assert(oAsm.frameSequence() == 8);
    assert(oAsm.frame() == vFrame);
    assert(oAsm.framesCompleted() == 2 && oAsm.framesDropped() == 0);
    assert(oAsm.statusJson().empty());   // sequence 7's status does not carry over
}

void testStatusBeforeCompletionWaits()
{
    const std::vector<uint8_t> vFrame = serialise(makeHeader(0, kArdStatusOk));
    const uint32_t uCount = chunkCount(vFrame);

    ArdFrameAssembler oAsm;
    std::vector<uint8_t> v = makeChunk(vFrame, 3, 0);
    assert(oAsm.feed(v.data(), v.size()) == ArdFeedResult::ChunkStored);

    const std::vector<uint8_t> vStatus = makeStatus(3, "{\"cpi\":3}");
    assert(oAsm.feed(vStatus.data(), vStatus.size()) == ArdFeedResult::ChunkStored);   // held

    for (uint32_t u = 1; u < uCount; ++u) {
        v = makeChunk(vFrame, 3, u);
        oAsm.feed(v.data(), v.size());
    }
    assert(oAsm.haveFrame() && oAsm.frameSequence() == 3);
    assert(oAsm.statusJson() == "{\"cpi\":3}");

    // A status for a sequence nobody holds is ignored.
    const std::vector<uint8_t> vOther = makeStatus(99, "{}");
    assert(oAsm.feed(vOther.data(), vOther.size()) == ArdFeedResult::Ignored);
}

void testMissingChunkAndNewSequenceDrops()
{
    const std::vector<uint8_t> vFrame = serialise(makeHeader(1, kArdStatusOk));
    const uint32_t uCount = chunkCount(vFrame);

    ArdFrameAssembler oAsm;
    // Sequence 1 with chunk 4 missing.
    for (uint32_t u = 0; u < uCount; ++u) {
        if (u == 4) continue;
        const std::vector<uint8_t> v = makeChunk(vFrame, 1, u);
        assert(oAsm.feed(v.data(), v.size()) == ArdFeedResult::ChunkStored);
    }
    assert(!oAsm.haveFrame());

    // Sequence 2 arrives: 1 is dropped and counted; 2 completes.
    for (uint32_t u = 0; u < uCount; ++u) {
        const std::vector<uint8_t> v = makeChunk(vFrame, 2, u);
        oAsm.feed(v.data(), v.size());
    }
    assert(oAsm.haveFrame() && oAsm.frameSequence() == 2);
    assert(oAsm.framesDropped() == 1);
    assert(oAsm.framesCompleted() == 1);
    assert(oAsm.frame() == vFrame);
}

void testMalformedChunksRejected()
{
    const std::vector<uint8_t> vFrame = serialise(makeHeader(0, kArdStatusOk));
    ArdFrameAssembler oAsm;

    std::vector<uint8_t> v = makeChunk(vFrame, 1, 0);

    // Wrong magic.
    std::vector<uint8_t> vBad = v;
    vBad[7] = '9';
    assert(oAsm.feed(vBad.data(), vBad.size()) == ArdFeedResult::Ignored);

    // Index past the count.
    vBad = v;
    putU32(&vBad[12], 1000);
    assert(oAsm.feed(vBad.data(), vBad.size()) == ArdFeedResult::Ignored);

    // Payload length disagrees with the datagram length.
    vBad = v;
    vBad.pop_back();
    assert(oAsm.feed(vBad.data(), vBad.size()) == ArdFeedResult::Ignored);

    // Too short to be anything.
    assert(oAsm.feed(v.data(), 10) == ArdFeedResult::Ignored);

    // Only datagrams that claimed to be chunks and lied are "rejected"; a
    // wrong magic or a runt is simply not ours.
    assert(oAsm.chunksRejected() == 2);
    assert(oAsm.chunksReceived() == 0);

    // A frame whose bytes do not parse as an ARD frame is dropped on
    // completion rather than published.
    std::vector<uint8_t> vGarbage(vFrame.size(), 0x55);
    const uint32_t uCount = chunkCount(vGarbage);
    for (uint32_t u = 0; u < uCount; ++u) {
        const std::vector<uint8_t> c = makeChunk(vGarbage, 5, u);
        const ArdFeedResult e = oAsm.feed(c.data(), c.size());
        assert(e == (u + 1 == uCount ? ArdFeedResult::Ignored : ArdFeedResult::ChunkStored));
    }
    assert(!oAsm.haveFrame());
    assert(oAsm.framesDropped() == 1);
}

// The sender through a real loopback socket into the assembler: the whole
// path pbr_run and ard_view use, minus the network.
void testSenderToAssemblerOverLoopback()
{
    const uint16_t uPort = 45992;
    UdpReceiver oReceiver;
    assert(oReceiver.open(uPort, 4 * 1024 * 1024, true));

    ArdStreamSender oSender;
    assert(oSender.open("127.0.0.1", uPort, nullptr));

    const std::vector<uint8_t> vFrame = serialise(makeHeader(2, kArdStatusFailed));
    assert(oSender.sendFrame(vFrame.data(), vFrame.size(), 42));
    assert(oSender.sendStatus("{\"cpi\":42,\"status\":2}", 42));
    assert(oSender.sendFailures() == 0);

    ArdFrameAssembler oAsm;
    std::vector<uint8_t> vDatagram(2048);
    bool bComplete = false;
    bool bStatus   = false;
    for (int i = 0; i < 200 && !(bComplete && bStatus); ++i) {
        size_t uLength = 0;
        const ReceiveResult eResult = oReceiver.receive(vDatagram.data(), vDatagram.size(), uLength);
        if (eResult != ReceiveResult::Datagram)
            break;   // a one-second timeout means the datagrams did not arrive
        const ArdFeedResult e = oAsm.feed(vDatagram.data(), uLength);
        if (e == ArdFeedResult::FrameComplete)  bComplete = true;
        if (e == ArdFeedResult::StatusAttached) bStatus = true;
    }
    assert(bComplete && bStatus);
    assert(oAsm.frameSequence() == 42);
    assert(oAsm.frame() == vFrame);
    assert(oAsm.statusJson() == "{\"cpi\":42,\"status\":2}");
    assert(oAsm.chunksReceived() == chunkCount(vFrame));

    ArdFileHeader oHeader;
    assert(parseArdFrameHeader(oAsm.frame().data(), oAsm.frame().size(), oHeader));
    assert(oHeader.uStatus == kArdStatusFailed);
}

// Spectrum datagrams: sent after the frame they attach directly; sent
// before it they wait; a wrong sequence, channel or length is ignored; the
// values round-trip as hundredths of a dB and saturate.
void testSpectrumAttachesAndRoundTrips()
{
    const uint16_t uPort = 45993;
    UdpReceiver oReceiver;
    assert(oReceiver.open(uPort, 4 * 1024 * 1024, true));
    ArdStreamSender oSender;
    assert(oSender.open("127.0.0.1", uPort, nullptr));

    const uint32_t uBins = 16;
    float afDb[uBins];
    for (uint32_t u = 0; u < uBins; ++u) afDb[u] = -100.0f + 1.25f * static_cast<float>(u);
    afDb[3] = -500.0f;   // below the int16 range: must saturate, not wrap
    afDb[4] = 400.0f;

    const std::vector<uint8_t> vFrame = serialise(makeHeader(0, kArdStatusOk));
    assert(oSender.sendSpectrum(afDb, uBins, 1, 200000, 7));    // early: frame 7 not started yet -> ignored
    assert(oSender.sendFrame(vFrame.data(), vFrame.size(), 7));
    assert(oSender.sendSpectrum(afDb, uBins, 0, 200000, 7));    // after: attaches
    assert(oSender.sendSpectrum(afDb, uBins, 1, 200000, 8));    // wrong sequence: ignored
    assert(oSender.sendStatus("{\"cpi\":7}", 7));

    ArdFrameAssembler oAsm;
    std::vector<uint8_t> vDatagram(2048);
    int iAttached = 0;
    bool bStatus = false;
    for (int i = 0; i < 200 && !bStatus; ++i) {
        size_t uLength = 0;
        if (oReceiver.receive(vDatagram.data(), vDatagram.size(), uLength) != ReceiveResult::Datagram)
            break;
        const ArdFeedResult e = oAsm.feed(vDatagram.data(), uLength);
        if (e == ArdFeedResult::SpectrumAttached) ++iAttached;
        if (e == ArdFeedResult::StatusAttached)   bStatus = true;
    }
    assert(bStatus);
    assert(iAttached == 1);
    assert(oAsm.spectrum(1).empty());                 // the early one was for an unknown frame
    const std::vector<uint8_t>& vSpec = oAsm.spectrum(0);
    assert(vSpec.size() == kArdSpectrumHeaderBytes + 2 * uBins);
    uint32_t uField = 0;
    std::memcpy(&uField, vSpec.data() + 12, 4); assert(uField == 0);
    std::memcpy(&uField, vSpec.data() + 16, 4); assert(uField == uBins);
    std::memcpy(&uField, vSpec.data() + 20, 4); assert(uField == 200000);
    for (uint32_t u = 0; u < uBins; ++u) {
        int16_t iValue = 0;
        std::memcpy(&iValue, vSpec.data() + kArdSpectrumHeaderBytes + 2 * u, 2);
        if (u == 3)      assert(iValue == -32768);
        else if (u == 4) assert(iValue == 32767);
        else             assert(iValue == static_cast<int16_t>(afDb[u] * 100.0f));
    }
    assert(oAsm.spectrum(2).empty());                 // out-of-range channel

    // Before the frame: waits, then attaches on completion. A new frame
    // clears it.
    const std::vector<uint8_t> vFrame9 = serialise(makeHeader(1, kArdStatusOk));
    {
        size_t uLength = 0;
        // Frame 9 not started: its spectrum is ignored.
        assert(oSender.sendSpectrum(afDb, uBins, 1, 200000, 9));
        assert(oReceiver.receive(vDatagram.data(), vDatagram.size(), uLength) == ReceiveResult::Datagram);
        assert(oAsm.feed(vDatagram.data(), uLength) == ArdFeedResult::Ignored);
        // Send frame 9 and feed its first chunk, so the frame is being
        // assembled when the spectrum arrives.
        assert(oSender.sendFrame(vFrame9.data(), vFrame9.size(), 9));
        assert(oReceiver.receive(vDatagram.data(), vDatagram.size(), uLength) == ReceiveResult::Datagram);
        assert(oAsm.feed(vDatagram.data(), uLength) == ArdFeedResult::ChunkStored);
        assert(oSender.sendSpectrum(afDb, uBins, 1, 200000, 9));
        // The remaining chunks are queued ahead of the spectrum; drain them,
        // the frame completes, then the spectrum arrives and attaches.
        bool bComplete = false;
        bool bAttached = false;
        for (int i = 0; i < 200 && !bAttached; ++i) {
            if (oReceiver.receive(vDatagram.data(), vDatagram.size(), uLength) != ReceiveResult::Datagram)
                break;
            const ArdFeedResult e = oAsm.feed(vDatagram.data(), uLength);
            if (e == ArdFeedResult::FrameComplete)    bComplete = true;
            if (e == ArdFeedResult::SpectrumAttached) bAttached = true;
        }
        assert(bComplete && bAttached);
        assert(oAsm.frameSequence() == 9);
        assert(oAsm.spectrum(0).empty());             // frame 7's spectrum did not carry over
        assert(!oAsm.spectrum(1).empty());
    }

    // Malformed: a truncated spectrum datagram is ignored.
    std::vector<uint8_t> vBad(oAsm.spectrum(1));
    vBad.resize(vBad.size() - 2);
    assert(oAsm.feed(vBad.data(), vBad.size()) == ArdFeedResult::Ignored);
}

} // namespace

int main()
{
    testSerialiseMatchesFile();
    testReassembleInOrderAndReverse();
    testStatusBeforeCompletionWaits();
    testMissingChunkAndNewSequenceDrops();
    testMalformedChunksRejected();
    testSenderToAssemblerOverLoopback();
    testSpectrumAttachesAndRoundTrips();

    std::printf("test_ard_stream: all tests passed\n");
    return 0;
}
