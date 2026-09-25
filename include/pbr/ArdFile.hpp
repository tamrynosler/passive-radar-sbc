#ifndef PBR_ARD_FILE_HPP
#define PBR_ARD_FILE_HPP

#include <cstdint>

#include "pbr/ArdProcessor.hpp"
#include "pbr/types.hpp"

namespace pbr {

// One range-Doppler map plus its detection list on disk.
//
// Three uses, all of which need the same bytes:
//   * MATLAB writes one as the golden reference; ard_verify compares against it
//   * ard_verify reads a golden MAP back as input, so the CFAR can be tested
//     on its own without the CAF in front of it
//   * the pipeline writes one per CPI for plotting
//
// Layout: a 64-byte header, then the map, then the detections.
//
//   offset  size  contents
//        0     8  magic "PBRARD01"
//        8     8  uRangeBins
//       16     8  uDopplerBins
//       24     8  uSampleRateHz
//       32     8  uDopplerDecimation
//       40     8  uCpiStartIndex (0 if unknown)
//       48     8  uDetections
//       56     8  uStatus: 0 processed, 1 null input (blank frame), 2 a
//                 processing stage failed (blank frame). Was reserved/zero
//                 before 2026-09-25, so older files read as "processed".
//       64  8*R*D map, interleaved float32 I,Q
//      ...    48  per detection
//
// The map is in DISPLAY layout: map[doppler * uRangeBins + range], range
// varying fastest. Loaded into MATLAB or numpy and shown as an image it comes
// out the right way round, range on the x axis, with no transpose.
//
// Detection records are fixed 48-byte: two uint64 bin indices then four
// float64 (range km, Doppler Hz, power dB, SNR dB).

// Values of uStatus. A blank frame (all-zero map, no detections) with a
// non-zero status is how the chain says "I received a CPI but could not
// process it", so a viewer shows that rather than a stale or missing frame.
constexpr uint64_t kArdStatusOk        = 0;
constexpr uint64_t kArdStatusNullInput = 1;   // a channel was all zeros or non-finite
constexpr uint64_t kArdStatusFailed    = 2;   // cancellation or map/CFAR returned false

struct ArdFileHeader {
    uint64_t uRangeBins         = 0;
    uint64_t uDopplerBins       = 0;
    uint64_t uSampleRateHz      = 0;
    uint64_t uDopplerDecimation = 0;
    uint64_t uCpiStartIndex     = 0;
    uint64_t uDetections        = 0;
    uint64_t uStatus            = kArdStatusOk;
};

constexpr size_t kArdHeaderBytes    = 64;
constexpr size_t kArdCellBytes      = 8;    // float32 I, float32 Q
constexpr size_t kArdDetectionBytes = 48;

// Size of a whole frame in the layout above, for this header.
size_t ardFrameBytes(const ArdFileHeader& oHeader);

// Serialise a frame into caller memory, byte for byte what writeArdFile puts
// on disk. This is the one place the layout is produced: the file writer and
// the UDP stream (ArdStream.hpp) both use it, so they cannot disagree.
bool serialiseArdFrame(const ArdFileHeader& oHeader, const ComplexF* pcMap,
                       const Detection* pDetections,
                       uint8_t* pOut, size_t uCapacity, size_t& uLengthOut);

// Decode the header at the front of a serialised frame, with the same
// plausibility bounds the file reader applies, and require uLength to be
// exactly ardFrameBytes() for it. A frame off the wire passes this or is
// dropped.
bool parseArdFrameHeader(const uint8_t* pBytes, size_t uLength,
                         ArdFileHeader& oHeader);

// Write an already-serialised frame to disk in one call.
bool writeArdFrameBytes(const char* pPath, const uint8_t* pBytes, size_t uLength);

// Write a map and its detections. pcMap holds uRangeBins * uDopplerBins cells.
// Serialises into a temporary buffer and calls writeArdFrameBytes(); the
// chain does not use this on its timed path (it serialises once and keeps
// the bytes), the verification tools and MATLAB comparisons do.
bool writeArdFile(const char* pPath, const ArdFileHeader& oHeader,
                  const ComplexF* pcMap,
                  const Detection* pDetections);

// Read the header only, so a caller can size its buffers first.
bool readArdFileHeader(const char* pPath, ArdFileHeader& oHeader);

// Read a map and its detections into caller-owned arrays. Fails rather than
// truncating if either is too small -- a silently short map would process into
// a plausible but wrong answer.
bool readArdFile(const char* pPath, ArdFileHeader& oHeader,
                 ComplexF* pcMap, uint64_t uMapCapacity,
                 Detection* pDetections, uint64_t uMaxDetections);

} // namespace pbr

#endif // PBR_ARD_FILE_HPP
