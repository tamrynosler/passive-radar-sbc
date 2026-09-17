#ifndef PBR_CPI_FILE_HPP
#define PBR_CPI_FILE_HPP

#include <cstdint>

#include "pbr/types.hpp"

namespace pbr {

// One extracted CPI on disk: both channels, aligned, in physical units.
//
// This is the hand-off between ingest and everything downstream. Freezing a
// CPI to a file buys three things:
//
//   * a benchmark that measures cancellation and nothing else -- no sockets,
//     no packet decode, no FERS, and the same input bytes on every run and
//     for every algorithm, so the comparison is fair by construction
//   * a golden-file check against MATLAB that starts from identical samples,
//     which sidesteps the unresolved .h5 amplitude scaling entirely
//   * a way to reproduce a specific CPI that misbehaved
//
// Layout: a 32-byte header, then the reference channel, then the
// surveillance channel. Each channel is uSampleCount interleaved
// float32 (I, Q) pairs -- the same layout as ComplexF, written little-endian.
//
//   offset  size  contents
//        0     8  magic "PBRCPI01"
//        8     8  uSampleCount
//       16     8  uSampleRateHz
//       24     8  uStartIndex, the absolute sample index of the first sample
//
// Little-endian and host float layout are assumed: the Mac, the Jetson and
// MATLAB on either all agree, and the file is regenerable test data rather
// than an archive format. The magic catches a truncated or foreign file.

struct CpiFileHeader {
    uint64_t uSampleCount  = 0;
    uint64_t uSampleRateHz = 0;
    uint64_t uStartIndex   = 0;
};

// Write one CPI. pcRef and pcSur each hold oHeader.uSampleCount samples.
// Returns false, having logged the reason, on any I/O failure.
bool writeCpiFile(const char* pPath, const CpiFileHeader& oHeader,
                  const ComplexF* pcRef, const ComplexF* pcSur);

// Read the header only, so a caller can size its buffers before reading.
bool readCpiFileHeader(const char* pPath, CpiFileHeader& oHeader);

// Read one CPI into caller-owned buffers, each of uCapacity samples. Fails if
// the file holds more samples than that, rather than truncating silently.
bool readCpiFile(const char* pPath, CpiFileHeader& oHeader,
                 ComplexF* pcRef, ComplexF* pcSur, uint64_t uCapacity);

} // namespace pbr

#endif // PBR_CPI_FILE_HPP
