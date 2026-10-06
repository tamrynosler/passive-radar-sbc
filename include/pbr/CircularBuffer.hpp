#ifndef PBR_CIRCULAR_BUFFER_HPP
#define PBR_CIRCULAR_BUFFER_HPP

#include <cstdint>

#include "pbr/types.hpp"

namespace pbr {

// Fixed-capacity sliding window over an absolute sample timeline.
//
// Samples are addressed by ABSOLUTE sample index, derived from a packet's
// VITA 49 timestamp (index = floor(packetTime_s * fs)), never by arrival
// order. Two consequences that matter:
//
//   * Out-of-order packets land in the right place, because each write
//     carries its own position. There is no "first packet" anchor, so the
//     reference and surveillance channels share one un-anchored timeline by
//     construction and stay aligned regardless of which stream's packet is
//     parsed first.
//   * The window always holds the most recent capacity() samples. Anything
//     older than oldestValid() is gone.
//
// The buffer is deliberately scale-agnostic: it knows nothing about int16
// ADC codes, --vita49-fullscale, channel roles or CPIs. Descaling and role
// resolution belong to the ingest layer above it.
//
// It does track which slots hold data that arrived in a packet. Every slot
// carries a validity flag: write() sets it, and the zero-fill of a forward
// jump clears it, so validCount() over a window is the number of samples
// that came from packets, whatever their values. This replaced a coverage
// test on sample values (the MATLAB reference's "under 99% non-zero is bad"),
// which could not tell a zero-filled gap from a quiet input: with no antenna
// connected the ADC reads codes of -2..2 and every CPI was rejected.
//
// The zero-fill stays as well: a gap reads back as exact zeros AND is
// flagged invalid, so stale samples from a previous lap through the buffer
// can never be mistaken for current data by either route.
//
// Not thread-safe, single-threaded by design for now. Not copyable: it owns
// a raw allocation.
class CircularBuffer {
public:
    explicit CircularBuffer(uint64_t uCapacity);
    ~CircularBuffer();

    CircularBuffer(const CircularBuffer&) = delete;
    CircularBuffer& operator=(const CircularBuffer&) = delete;

    // False if construction failed (zero capacity or allocation failure).
    // Check this before use -- the project does not use exceptions, so a
    // constructor cannot report failure any other way.
    bool isValid() const { return mbValid; }

    // Write uCount samples starting at absolute index uAbsIndex.
    // Samples that have already fallen out of the window are trimmed from
    // the front; a write that is entirely stale is rejected. Returns false
    // on rejection or on a bad argument.
    bool write(uint64_t uAbsIndex, const ComplexF* pcSrc, uint64_t uCount);

    // Copy out uCount samples starting at absolute index uAbsIndex. The
    // whole requested window must be resident, otherwise this returns false
    // and leaves pcDst untouched.
    bool read(uint64_t uAbsIndex, ComplexF* pcDst, uint64_t uCount) const;

    // Forget everything: samples zeroed, flags cleared, write end back to 0,
    // as if just constructed. For a source whose timeline jumped backwards
    // (a driver restarted from a different clock), where waiting for wall
    // time to catch up could take days. No allocation.
    void reset();

    // Number of samples in [uAbsIndex, uAbsIndex + uCount) that were written
    // by a packet, as opposed to zero-filled or never written. 0 if the
    // window is not fully resident (same rule as read()) or on a bad
    // argument, so a caller comparing it with uCount rejects such windows.
    uint64_t validCount(uint64_t uAbsIndex, uint64_t uCount) const;

    // One past the highest absolute index ever written; 0 if nothing has
    // been written yet. Equivalent to MATLAB's maxWritten + 1.
    uint64_t writeEnd() const { return muWriteEnd; }

    // Lowest absolute index still resident.
    uint64_t oldestValid() const {
        return (muWriteEnd > muCapacity) ? muWriteEnd - muCapacity : 0;
    }

    uint64_t capacity() const { return muCapacity; }

private:
    // Zero uCount samples from absolute index uAbsIndex and clear their
    // validity flags. Requires uCount <= muCapacity.
    void zeroSpan(uint64_t uAbsIndex, uint64_t uCount);

    ComplexF* mh_cBuffer;
    uint8_t*  mh_uValid;      // one flag per slot, same indexing as mh_cBuffer
    uint64_t  muCapacity;
    uint64_t  muWriteEnd;
    bool      mbValid;
};

} // namespace pbr

#endif // PBR_CIRCULAR_BUFFER_HPP
