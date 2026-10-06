#include "pbr/CaptureFile.hpp"

#include "pbr/check.hpp"

#include <cstring>

namespace pbr {

namespace {

const char kMagic[8] = { 'P', 'B', 'R', 'C', 'A', 'P', '0', '1' };

// Little-endian by explicit byte assembly, so the file does not depend on
// host byte order and MATLAB can read it with 'ieee-le'.
void putLe32(uint8_t* p, uint32_t uValue)
{
    p[0] = static_cast<uint8_t>(uValue);
    p[1] = static_cast<uint8_t>(uValue >> 8);
    p[2] = static_cast<uint8_t>(uValue >> 16);
    p[3] = static_cast<uint8_t>(uValue >> 24);
}

uint32_t getLe32(const uint8_t* p)
{
    return  static_cast<uint32_t>(p[0])        |
           (static_cast<uint32_t>(p[1]) <<  8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

// ---------------------------------------------------------------------------
// CaptureWriter
// ---------------------------------------------------------------------------

CaptureWriter::CaptureWriter()
    : mpFile(nullptr), muRecords(0), muBytes(0)
{
}

CaptureWriter::~CaptureWriter()
{
    close();
}

bool CaptureWriter::open(const char* pPath)
{
    CHECK(pPath != nullptr, "CaptureWriter::open with a null path");
    CHECK(mpFile == nullptr, "CaptureWriter::open on an already-open file");

    mpFile = std::fopen(pPath, "wb");
    if (mpFile == nullptr) {
        std::fprintf(stderr, "CaptureWriter: cannot open %s for writing\n", pPath);
        return false;
    }

    uint8_t auHeader[kCaptureHeaderSize];
    std::memset(auHeader, 0, sizeof(auHeader));
    std::memcpy(auHeader, kMagic, sizeof(kMagic));
    putLe32(auHeader + 8, kCaptureVersion);
    putLe32(auHeader + 12, 0);

    if (std::fwrite(auHeader, 1, sizeof(auHeader), mpFile) != sizeof(auHeader)) {
        std::fprintf(stderr, "CaptureWriter: failed to write the file header\n");
        close();
        return false;
    }

    muRecords = 0;
    muBytes   = 0;
    return true;
}

void CaptureWriter::close()
{
    if (mpFile != nullptr) {
        std::fclose(mpFile);
        mpFile = nullptr;
    }
}

bool CaptureWriter::write(const uint8_t* pBytes, size_t uLength)
{
    CHECK(mpFile != nullptr, "CaptureWriter::write on a closed file");
    CHECK(pBytes != nullptr, "CaptureWriter::write with a null buffer");
    CHECK(uLength > 0, "CaptureWriter::write with a zero-length record");
    CHECK(uLength <= 0xFFFFFFFFull, "CaptureWriter::write record too large");

    uint8_t auLength[4];
    putLe32(auLength, static_cast<uint32_t>(uLength));

    if (std::fwrite(auLength, 1, sizeof(auLength), mpFile) != sizeof(auLength) ||
        std::fwrite(pBytes, 1, uLength, mpFile) != uLength) {
        std::fprintf(stderr, "CaptureWriter: write failed after %llu records\n",
                     static_cast<unsigned long long>(muRecords));
        return false;
    }

    ++muRecords;
    muBytes += uLength;
    return true;
}

// ---------------------------------------------------------------------------
// CaptureReader
// ---------------------------------------------------------------------------

CaptureReader::CaptureReader()
    : mpFile(nullptr), muRecords(0)
{
}

CaptureReader::~CaptureReader()
{
    close();
}

bool CaptureReader::open(const char* pPath)
{
    CHECK(pPath != nullptr, "CaptureReader::open with a null path");
    CHECK(mpFile == nullptr, "CaptureReader::open on an already-open file");

    mpFile = std::fopen(pPath, "rb");
    if (mpFile == nullptr) {
        std::fprintf(stderr, "CaptureReader: cannot open %s for reading\n", pPath);
        return false;
    }

    uint8_t auHeader[kCaptureHeaderSize];
    if (std::fread(auHeader, 1, sizeof(auHeader), mpFile) != sizeof(auHeader)) {
        std::fprintf(stderr, "CaptureReader: %s is too short to hold a header\n", pPath);
        close();
        return false;
    }

    if (std::memcmp(auHeader, kMagic, sizeof(kMagic)) != 0) {
        std::fprintf(stderr, "CaptureReader: %s is not a PBRCAP capture file\n", pPath);
        close();
        return false;
    }

    const uint32_t uVersion = getLe32(auHeader + 8);
    if (uVersion != kCaptureVersion) {
        std::fprintf(stderr, "CaptureReader: %s is version %u, expected %u\n",
                     pPath, uVersion, kCaptureVersion);
        close();
        return false;
    }

    muRecords = 0;
    return true;
}

void CaptureReader::close()
{
    if (mpFile != nullptr) {
        std::fclose(mpFile);
        mpFile = nullptr;
    }
}

bool CaptureReader::read(uint8_t* pBuffer, size_t uCapacity, size_t& uLengthOut,
                         bool& bEndOfFileOut)
{
    uLengthOut    = 0;
    bEndOfFileOut = false;

    if (mpFile == nullptr || pBuffer == nullptr || uCapacity == 0) {
        std::fprintf(stderr, "CaptureReader::read with a closed file or bad buffer\n");
        return false;
    }

    uint8_t auLength[4];
    const size_t uHeaderRead = std::fread(auLength, 1, sizeof(auLength), mpFile);

    if (uHeaderRead == 0 && std::feof(mpFile)) {
        bEndOfFileOut = true;
        return false;
    }
    if (uHeaderRead != sizeof(auLength)) {
        std::fprintf(stderr, "CaptureReader: truncated record header after %llu records\n",
                     static_cast<unsigned long long>(muRecords));
        return false;
    }

    const uint32_t uLength = getLe32(auLength);

    if (uLength == 0 || uLength > uCapacity) {
        std::fprintf(stderr,
                     "CaptureReader: record %llu declares %u bytes, buffer holds %zu\n",
                     static_cast<unsigned long long>(muRecords), uLength, uCapacity);
        return false;
    }

    if (std::fread(pBuffer, 1, uLength, mpFile) != uLength) {
        std::fprintf(stderr, "CaptureReader: truncated record %llu payload\n",
                     static_cast<unsigned long long>(muRecords));
        return false;
    }

    uLengthOut = uLength;
    ++muRecords;
    return true;
}

} // namespace pbr
