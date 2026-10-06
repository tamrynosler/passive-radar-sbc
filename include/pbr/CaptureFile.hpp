#ifndef PBR_CAPTURE_FILE_HPP
#define PBR_CAPTURE_FILE_HPP

#include <cstdint>
#include <cstdio>
#include <cstddef>

namespace pbr {

// Raw UDP datagram capture, for replaying a FERS stream without FERS.
//
// UDP is message-oriented, so a flat byte dump would lose datagram
// boundaries. Records are therefore length-prefixed:
//
//   header   "PBRCAP01"        8 bytes
//            uint32 version    little-endian, currently 1
//            uint32 reserved   0
//   record   uint32 length     little-endian
//            length bytes      the datagram exactly as it arrived
//
// Both length fields are written byte by byte in little-endian order, so the
// file is identical on any host and MATLAB can read it with a plain
// fread(fid, 1, 'uint32', 'ieee-le').
//
// Arrival timestamps are deliberately NOT stored. VITA 49 packets carry their
// own timestamps, and wall-clock arrival time only matters for jitter and
// pacing studies we are not doing. Add them as a version 2 format if that
// changes.

constexpr uint32_t kCaptureVersion    = 1;
constexpr size_t   kCaptureHeaderSize = 16;

class CaptureWriter {
public:
    CaptureWriter();
    ~CaptureWriter();

    CaptureWriter(const CaptureWriter&) = delete;
    CaptureWriter& operator=(const CaptureWriter&) = delete;

    bool open(const char* pPath);

    // Close the file, flushing what is buffered. False, logged, if that final
    // write failed: the capture on disk is then incomplete.
    bool close();
    bool isOpen() const { return mpFile != nullptr; }

    // Append one datagram.
    bool write(const uint8_t* pBytes, size_t uLength);

    uint64_t recordCount() const { return muRecords; }
    uint64_t byteCount()   const { return muBytes; }

private:
    std::FILE* mpFile;
    uint64_t   muRecords;
    uint64_t   muBytes;
};

class CaptureReader {
public:
    CaptureReader();
    ~CaptureReader();

    CaptureReader(const CaptureReader&) = delete;
    CaptureReader& operator=(const CaptureReader&) = delete;

    bool open(const char* pPath);
    void close();
    bool isOpen() const { return mpFile != nullptr; }

    // Read the next datagram into pBuffer.
    //
    // Returns true when a record was read and sets uLengthOut. Returns false
    // both at clean end of file and on error; bEndOfFileOut distinguishes
    // them, so a caller can tell "finished" from "corrupt".
    bool read(uint8_t* pBuffer, size_t uCapacity, size_t& uLengthOut,
              bool& bEndOfFileOut);

    uint64_t recordCount() const { return muRecords; }

private:
    std::FILE* mpFile;
    uint64_t   muRecords;
};

} // namespace pbr

#endif // PBR_CAPTURE_FILE_HPP
