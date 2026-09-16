#ifndef PBR_CIRCULAR_BUFFER_HPP
#define PBR_CIRCULAR_BUFFER_HPP

#include <cstdint>

#include "pbr/types.hpp"

namespace pbr {

// Fixed-capacity sliding window over an absolute sample timeline.
//
// Samples are addressed by ABSOLUTE sample index, derived from a packet's
// VITA 49 timestamp (index = round(packetTime_s * fs)), never by arrival
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
// It does not track coverage either, but it does guarantee that a gap reads
// back as exact zeros: a forward jump zero-fills the skipped span, so stale
// samples from a previous lap through the buffer can never be mistaken for
// current data. The caller decides what a run of zeros means (the MATLAB
// reference treats a CPI window as bad if it is under 99% non-zero).
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

    // One past the highest absolute index ever written; 0 if nothing has
    // been written yet. Equivalent to MATLAB's maxWritten + 1.
    uint64_t writeEnd() const { return muWriteEnd; }

    // Lowest absolute index still resident.
    uint64_t oldestValid() const {
        return (muWriteEnd > muCapacity) ? muWriteEnd - muCapacity : 0;
    }

    uint64_t capacity() const { return muCapacity; }

private:
    // Zero uCount samples from absolute index uAbsIndex. Requires
    // uCount <= muCapacity.
    void zeroSpan(uint64_t uAbsIndex, uint64_t uCount);

    ComplexF* mh_cBuffer;
    uint64_t  muCapacity;
    uint64_t  muWriteEnd;
    bool      mbValid;
};

} // namespace pbr

#endif // PBR_CIRCULAR_BUFFER_HPP
