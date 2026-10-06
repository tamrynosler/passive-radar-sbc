// Unit tests for pbr::CircularBuffer. Plain main() + asserts, no framework.
//
// NDEBUG is undefined deliberately and before <cassert>: the project builds
// Release, which defines NDEBUG and would compile every assert below out,
// leaving a test that passes unconditionally.
#undef NDEBUG
#include <cassert>

#include "pbr/CircularBuffer.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

using pbr::CircularBuffer;
using pbr::ComplexF;

namespace {

// A distinct, cheaply checkable value for each absolute sample index.
// Index 0 maps to 0 + 0i, which is also what a gap looks like, so tests that
// care about gaps use non-zero indices.
ComplexF sampleAt(uint64_t uIndex)
{
    return ComplexF(static_cast<float>(uIndex), -static_cast<float>(uIndex));
}

std::vector<ComplexF> makeRun(uint64_t uStart, uint64_t uCount)
{
    std::vector<ComplexF> vcRun(uCount);
    for (uint64_t u = 0; u < uCount; ++u)
        vcRun[u] = sampleAt(uStart + u);
    return vcRun;
}

// Read back [uStart, uStart + uCount) and require it to equal sampleAt().
void expectRun(const CircularBuffer& oBuf, uint64_t uStart, uint64_t uCount)
{
    std::vector<ComplexF> vcGot(uCount);
    assert(oBuf.read(uStart, vcGot.data(), uCount));
    for (uint64_t u = 0; u < uCount; ++u)
        assert(vcGot[u] == sampleAt(uStart + u));
}

void testConstruction()
{
    CircularBuffer oBuf(16);
    assert(oBuf.isValid());
    assert(oBuf.capacity() == 16);
    assert(oBuf.writeEnd() == 0);
    assert(oBuf.oldestValid() == 0);

    // Zero capacity must fail cleanly rather than crash.
    CircularBuffer oBad(0);
    assert(!oBad.isValid());
}

void testEmptyOperations()
{
    CircularBuffer oBuf(16);

    // Zero-length operations are successful no-ops.
    assert(oBuf.write(0, nullptr, 0));
    assert(oBuf.read(0, nullptr, 0));

    // Nothing has been written, so any real read must fail.
    ComplexF cDst;
    assert(!oBuf.read(0, &cDst, 1));
    assert(oBuf.writeEnd() == 0);
}

void testInOrderRoundTrip()
{
    CircularBuffer oBuf(16);

    const std::vector<ComplexF> vcRun = makeRun(0, 8);
    assert(oBuf.write(0, vcRun.data(), 8));
    assert(oBuf.writeEnd() == 8);
    assert(oBuf.oldestValid() == 0);

    expectRun(oBuf, 0, 8);
    expectRun(oBuf, 3, 4);   // interior sub-window
}

void testWriteStraddlingWrap()
{
    CircularBuffer oBuf(16);

    // Fill to index 12, then write 8 more so the write crosses the wrap
    // point at 16 and is split into two copies.
    const std::vector<ComplexF> vcHead = makeRun(0, 12);
    assert(oBuf.write(0, vcHead.data(), 12));

    const std::vector<ComplexF> vcTail = makeRun(12, 8);
    assert(oBuf.write(12, vcTail.data(), 8));
    assert(oBuf.writeEnd() == 20);
    assert(oBuf.oldestValid() == 4);

    expectRun(oBuf, 4, 16);   // whole window, itself straddling the wrap
    expectRun(oBuf, 14, 4);   // short read across the wrap
}

void testOutOfOrderWrite()
{
    CircularBuffer oBuf(16);

    // Later packet arrives first, earlier one second. Both must land
    // correctly, and the late arrival must not rewind writeEnd.
    const std::vector<ComplexF> vcLate  = makeRun(4, 4);
    const std::vector<ComplexF> vcEarly = makeRun(0, 4);

    assert(oBuf.write(4, vcLate.data(), 4));
    assert(oBuf.writeEnd() == 8);

    assert(oBuf.write(0, vcEarly.data(), 4));
    assert(oBuf.writeEnd() == 8);

    expectRun(oBuf, 0, 8);
}

void testStaleWriteRejected()
{
    CircularBuffer oBuf(16);

    const std::vector<ComplexF> vcFill = makeRun(0, 16);
    assert(oBuf.write(0, vcFill.data(), 16));

    const std::vector<ComplexF> vcNext = makeRun(16, 8);
    assert(oBuf.write(16, vcNext.data(), 8));
    assert(oBuf.writeEnd() == 24);
    assert(oBuf.oldestValid() == 8);

    // Entirely below the window: rejected, and the window is unchanged.
    const std::vector<ComplexF> vcStale = makeRun(0, 4);
    assert(!oBuf.write(0, vcStale.data(), 4));
    assert(oBuf.writeEnd() == 24);
    assert(oBuf.oldestValid() == 8);
}

void testPartiallyStaleWriteTrimmed()
{
    CircularBuffer oBuf(16);

    const std::vector<ComplexF> vcFill = makeRun(0, 16);
    assert(oBuf.write(0, vcFill.data(), 16));
    const std::vector<ComplexF> vcNext = makeRun(16, 8);
    assert(oBuf.write(16, vcNext.data(), 8));
    assert(oBuf.oldestValid() == 8);

    // Spans [6, 12): the first two samples are stale and must be trimmed,
    // the remaining four written at 8..11.
    const std::vector<ComplexF> vcStraddle = makeRun(6, 6);
    assert(oBuf.write(6, vcStraddle.data(), 6));

    expectRun(oBuf, 8, 4);
    assert(oBuf.writeEnd() == 24);   // a trimmed write must not rewind it
}

void testForwardJumpZeroFills()
{
    CircularBuffer oBuf(16);

    const std::vector<ComplexF> vcHead = makeRun(1, 4);   // indices 1..4
    assert(oBuf.write(1, vcHead.data(), 4));
    assert(oBuf.writeEnd() == 5);

    // Jump forward, leaving [5, 10) never written.
    const std::vector<ComplexF> vcJump = makeRun(10, 2);
    assert(oBuf.write(10, vcJump.data(), 2));
    assert(oBuf.writeEnd() == 12);

    std::vector<ComplexF> vcGot(11);
    assert(oBuf.read(1, vcGot.data(), 11));      // indices 1..11
    for (uint64_t u = 0; u < 4; ++u)
        assert(vcGot[u] == sampleAt(1 + u));     // 1..4 data
    for (uint64_t u = 4; u < 9; ++u)
        assert(vcGot[u] == ComplexF(0.0f, 0.0f)); // 5..9 gap
    for (uint64_t u = 9; u < 11; ++u)
        assert(vcGot[u] == sampleAt(1 + u));     // 10..11 data
}

// The important robustness case, and the one the MATLAB reference gets
// wrong: after a full lap, a forward jump lands on slots that still hold
// last lap's samples. Those must read back as zeros, not as stale data.
void testGapDoesNotExposeStaleLap()
{
    CircularBuffer oBuf(16);

    // Lap one: every slot written with a recognisable non-zero value.
    const std::vector<ComplexF> vcLap = makeRun(1, 16);   // indices 1..16
    assert(oBuf.write(1, vcLap.data(), 16));
    assert(oBuf.writeEnd() == 17);

    // Jump past [17, 21). Those indices map onto offsets 1..4, which still
    // hold lap one's samples for indices 1..4.
    const std::vector<ComplexF> vcJump = makeRun(21, 4);
    assert(oBuf.write(21, vcJump.data(), 4));
    assert(oBuf.writeEnd() == 25);
    assert(oBuf.oldestValid() == 9);

    std::vector<ComplexF> vcGot(8);
    assert(oBuf.read(17, vcGot.data(), 8));       // indices 17..24
    for (uint64_t u = 0; u < 4; ++u)
        assert(vcGot[u] == ComplexF(0.0f, 0.0f)); // 17..20 must be zeroed
    for (uint64_t u = 4; u < 8; ++u)
        assert(vcGot[u] == sampleAt(17 + u));     // 21..24 data
}

// A jump longer than one whole capacity must still leave a clean buffer.
void testJumpBeyondCapacity()
{
    CircularBuffer oBuf(16);

    const std::vector<ComplexF> vcLap = makeRun(1, 16);
    assert(oBuf.write(1, vcLap.data(), 16));

    const std::vector<ComplexF> vcFar = makeRun(1000, 4);
    assert(oBuf.write(1000, vcFar.data(), 4));
    assert(oBuf.writeEnd() == 1004);
    assert(oBuf.oldestValid() == 988);

    std::vector<ComplexF> vcGot(16);
    assert(oBuf.read(988, vcGot.data(), 16));     // indices 988..1003
    for (uint64_t u = 0; u < 12; ++u)
        assert(vcGot[u] == ComplexF(0.0f, 0.0f)); // 988..999 gap
    for (uint64_t u = 12; u < 16; ++u)
        assert(vcGot[u] == sampleAt(988 + u));    // 1000..1003 data
}

void testFullCapacityWrite()
{
    CircularBuffer oBuf(16);

    // Get the write offset away from 0, then write exactly capacity samples
    // so the split covers the whole buffer in two pieces.
    const std::vector<ComplexF> vcHead = makeRun(0, 8);
    assert(oBuf.write(0, vcHead.data(), 8));

    const std::vector<ComplexF> vcFull = makeRun(8, 16);
    assert(oBuf.write(8, vcFull.data(), 16));
    assert(oBuf.writeEnd() == 24);
    assert(oBuf.oldestValid() == 8);

    expectRun(oBuf, 8, 16);
}

void testReadBoundsRejected()
{
    CircularBuffer oBuf(16);

    const std::vector<ComplexF> vcFill = makeRun(0, 16);
    assert(oBuf.write(0, vcFill.data(), 16));
    const std::vector<ComplexF> vcNext = makeRun(16, 8);
    assert(oBuf.write(16, vcNext.data(), 8));
    assert(oBuf.oldestValid() == 8);
    assert(oBuf.writeEnd() == 24);

    std::vector<ComplexF> vcGot(32);

    assert(!oBuf.read(7, vcGot.data(), 2));    // starts before the window
    assert(!oBuf.read(20, vcGot.data(), 8));   // runs past writeEnd
    assert(!oBuf.read(8, vcGot.data(), 17));   // larger than capacity
    assert(oBuf.read(8, vcGot.data(), 16));    // the exact window is fine
}

// Realistic shape: FERS packets of 342 samples into a 2 s buffer at
// fs = 204800, run past one full wrap, then pull out a 1 s CPI.
void testFersLikeStream()
{
    const uint64_t uFs       = 204800;
    const uint64_t uCapacity = 2 * uFs;        // bufSeconds = 2 -> 409600
    const uint64_t uPacket   = 342;            // 1400-byte datagram payload
    const uint64_t uCpi      = uFs;            // Tcpi = 1.0 s

    CircularBuffer oBuf(uCapacity);
    assert(oBuf.isValid());

    uint64_t uIndex = 0;
    while (uIndex < uCapacity + uPacket) {     // push past one wrap
        const std::vector<ComplexF> vcPkt = makeRun(uIndex, uPacket);
        assert(oBuf.write(uIndex, vcPkt.data(), uPacket));
        uIndex += uPacket;
    }

    assert(oBuf.writeEnd() == uIndex);
    assert(oBuf.oldestValid() == uIndex - uCapacity);

    // The most recent CPI must come back intact across the wrap.
    const uint64_t uCpiStart = uIndex - uCpi;
    std::vector<ComplexF> vcCpi(uCpi);
    assert(oBuf.read(uCpiStart, vcCpi.data(), uCpi));
    for (uint64_t u = 0; u < uCpi; ++u)
        assert(vcCpi[u] == sampleAt(uCpiStart + u));
}

// Validity flags: set by write(), cleared by the zero-fill, independent of
// the sample values. This is what lets a quiet input (samples that happen to
// be exactly zero) count as covered while a real gap does not.
void testValidityFlags()
{
    CircularBuffer oBuf(16);

    // Nothing written: nothing valid, and a non-resident window is 0.
    assert(oBuf.validCount(0, 4) == 0);

    // A write of EXACT ZEROS is valid data: it came from a packet.
    std::vector<ComplexF> vcZeros(6, ComplexF(0.0f, 0.0f));
    assert(oBuf.write(100, vcZeros.data(), 6));
    assert(oBuf.validCount(100, 6) == 6);

    // A forward jump: [106, 110) is zero-filled and NOT valid.
    std::vector<ComplexF> vcRun = makeRun(110, 4);
    assert(oBuf.write(110, vcRun.data(), 4));
    assert(oBuf.validCount(100, 14) == 10);   // 6 + 4, hole of 4 excluded
    assert(oBuf.validCount(106, 4) == 0);
    assert(oBuf.validCount(110, 4) == 4);

    // Filling the hole later makes it valid.
    vcRun = makeRun(106, 4);
    assert(oBuf.write(106, vcRun.data(), 4));
    assert(oBuf.validCount(100, 14) == 14);

    // Windows the buffer cannot vouch for count as 0, same rules as read():
    // past the write end, before the oldest resident sample, too large.
    assert(oBuf.validCount(100, 15) == 0);
    assert(oBuf.validCount(0, 1) == 0);
    assert(oBuf.validCount(100, 17) == 0);
    assert(oBuf.validCount(100, 0) == 0);

    // Wrap: index 124 is slot 12 of 16, so this write straddles the seam
    // (slots 12..15 then 0..3) and sets flags on both sides; the forward
    // jump before it zero-fills [114, 124) and clears those flags, which
    // previously held valid data from the writes above.
    vcRun = makeRun(124, 8);
    assert(oBuf.write(124, vcRun.data(), 8));
    assert(oBuf.validCount(124, 8) == 8);
    assert(oBuf.validCount(116, 8) == 0);     // the hole
    assert(oBuf.validCount(116, 16) == 8);    // the whole resident window

    // A gap larger than the buffer clears every slot, then the new write
    // marks only its own.
    vcRun = makeRun(1000, 3);
    assert(oBuf.write(1000, vcRun.data(), 3));
    assert(oBuf.validCount(1000, 3) == 3);
    assert(oBuf.validCount(990, 10) == 0);
    assert(oBuf.validCount(987, 16) == 3);
}

} // namespace

int main()
{
    testConstruction();
    testEmptyOperations();
    testInOrderRoundTrip();
    testWriteStraddlingWrap();
    testOutOfOrderWrite();
    testStaleWriteRejected();
    testPartiallyStaleWriteTrimmed();
    testForwardJumpZeroFills();
    testGapDoesNotExposeStaleLap();
    testJumpBeyondCapacity();
    testFullCapacityWrite();
    testReadBoundsRejected();
    testFersLikeStream();
    testValidityFlags();

    std::printf("test_circular_buffer: all tests passed\n");
    return 0;
}
