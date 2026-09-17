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
//       56     8  reserved, zero
//       64  8*R*D map, interleaved float32 I,Q
//      ...    48  per detection
//
// The map is in DISPLAY layout: map[doppler * uRangeBins + range], range
// varying fastest. Loaded into MATLAB or numpy and shown as an image it comes
// out the right way round, range on the x axis, with no transpose.
//
// Detection records are fixed 48-byte: two uint64 bin indices then four
// float64 (range km, Doppler Hz, power dB, SNR dB).

struct ArdFileHeader {
    uint64_t uRangeBins         = 0;
    uint64_t uDopplerBins       = 0;
    uint64_t uSampleRateHz      = 0;
    uint64_t uDopplerDecimation = 0;
    uint64_t uCpiStartIndex     = 0;
    uint64_t uDetections        = 0;
};

// Write a map and its detections. pcMap holds uRangeBins * uDopplerBins cells.
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
