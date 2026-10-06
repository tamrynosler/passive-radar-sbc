#include "pbr/ArdFile.hpp"

#include "pbr/check.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pbr {

namespace {

const char     kMagic[8]   = { 'P', 'B', 'R', 'A', 'R', 'D', '0', '1' };
const uint64_t kMaxCells   = 1ULL << 28;   // sanity bound on a corrupt header
const uint64_t kMaxDetects = 1ULL << 24;

bool readU64(std::FILE* pFile, uint64_t& uValue)
{
    return std::fread(&uValue, sizeof(uValue), 1, pFile) == 1;
}

// Detections are read field by field rather than as a raw struct load, so
// the file does not depend on the compiler's padding choices.
bool readDetection(std::FILE* pFile, Detection& oDet)
{
    return readU64(pFile, oDet.uRangeBin) &&
           readU64(pFile, oDet.uDopplerBin) &&
           std::fread(&oDet.dRangeKm,   sizeof(double), 1, pFile) == 1 &&
           std::fread(&oDet.dDopplerHz, sizeof(double), 1, pFile) == 1 &&
           std::fread(&oDet.dPowerDb,   sizeof(double), 1, pFile) == 1 &&
           std::fread(&oDet.dSnrDb,     sizeof(double), 1, pFile) == 1;
}

// Native byte order, like the fwrite-based writer this replaced: aarch64 and
// x86 are both little-endian, which is what the format documents.
void putU64(uint8_t* p, uint64_t uValue)   { std::memcpy(p, &uValue, sizeof(uValue)); }
void putF64(uint8_t* p, double dValue)     { std::memcpy(p, &dValue, sizeof(dValue)); }
uint64_t getU64(const uint8_t* p)          { uint64_t u; std::memcpy(&u, p, sizeof(u)); return u; }

// The bounds every frame, from a file or off the wire, must pass. Each
// dimension is bounded on its own before they are multiplied, so a corrupt
// header cannot wrap the product round to something small.
bool headerIsPlausible(const ArdFileHeader& oHeader, const char* pWhat)
{
    if (oHeader.uRangeBins == 0 || oHeader.uDopplerBins == 0 ||
        oHeader.uRangeBins > kMaxCells || oHeader.uDopplerBins > kMaxCells ||
        oHeader.uRangeBins * oHeader.uDopplerBins > kMaxCells ||
        oHeader.uDetections > kMaxDetects) {
        std::fprintf(stderr, "ard frame: %s declares implausible dimensions "
                             "(%llu x %llu, %llu detections)\n", pWhat,
                     (unsigned long long)oHeader.uRangeBins,
                     (unsigned long long)oHeader.uDopplerBins,
                     (unsigned long long)oHeader.uDetections);
        return false;
    }
    return true;
}

std::FILE* openAndReadHeader(const char* pPath, ArdFileHeader& oHeader)
{
    if (pPath == nullptr) {
        std::fprintf(stderr, "ard file: null path\n");
        return nullptr;
    }

    std::FILE* pFile = std::fopen(pPath, "rb");
    if (pFile == nullptr) {
        std::fprintf(stderr, "ard file: cannot open %s: %s\n",
                     pPath, std::strerror(errno));
        return nullptr;
    }

    char achMagic[8];
    if (std::fread(achMagic, 1, sizeof(achMagic), pFile) != sizeof(achMagic) ||
        std::memcmp(achMagic, kMagic, sizeof(kMagic)) != 0) {
        std::fprintf(stderr, "ard file: %s is not an ARD file (bad magic)\n", pPath);
        std::fclose(pFile);
        return nullptr;
    }

    if (!readU64(pFile, oHeader.uRangeBins) ||
        !readU64(pFile, oHeader.uDopplerBins) ||
        !readU64(pFile, oHeader.uSampleRateHz) ||
        !readU64(pFile, oHeader.uDopplerDecimation) ||
        !readU64(pFile, oHeader.uCpiStartIndex) ||
        !readU64(pFile, oHeader.uDetections) ||
        !readU64(pFile, oHeader.uStatus)) {
        std::fprintf(stderr, "ard file: %s has a truncated header\n", pPath);
        std::fclose(pFile);
        return nullptr;
    }

    if (!headerIsPlausible(oHeader, pPath)) {
        std::fclose(pFile);
        return nullptr;
    }
    return pFile;
}

} // namespace

size_t ardFrameBytes(const ArdFileHeader& oHeader)
{
    return kArdHeaderBytes +
           kArdCellBytes * static_cast<size_t>(oHeader.uRangeBins * oHeader.uDopplerBins) +
           kArdDetectionBytes * static_cast<size_t>(oHeader.uDetections);
}

bool serialiseArdFrame(const ArdFileHeader& oHeader, const ComplexF* pcMap,
                       const Detection* pDetections,
                       uint8_t* pOut, size_t uCapacity, size_t& uLengthOut)
{
    uLengthOut = 0;
    CHECK(pOut != nullptr && pcMap != nullptr, "serialiseArdFrame: null argument");
    CHECK(oHeader.uDetections == 0 || pDetections != nullptr,
          "serialiseArdFrame: detection count is non-zero but the array is null");
    if (!headerIsPlausible(oHeader, "the frame to serialise"))
        return false;

    const size_t uTotal = ardFrameBytes(oHeader);
    CHECK(uTotal <= uCapacity, "serialiseArdFrame: output buffer too small");

    uint8_t* p = pOut;
    std::memcpy(p, kMagic, sizeof(kMagic));   p += 8;
    putU64(p, oHeader.uRangeBins);            p += 8;
    putU64(p, oHeader.uDopplerBins);          p += 8;
    putU64(p, oHeader.uSampleRateHz);         p += 8;
    putU64(p, oHeader.uDopplerDecimation);    p += 8;
    putU64(p, oHeader.uCpiStartIndex);        p += 8;
    putU64(p, oHeader.uDetections);           p += 8;
    putU64(p, oHeader.uStatus);               p += 8;

    const size_t uCells = static_cast<size_t>(oHeader.uRangeBins * oHeader.uDopplerBins);
    std::memcpy(p, pcMap, uCells * kArdCellBytes);
    p += uCells * kArdCellBytes;

    // Field by field, so the layout does not depend on struct padding.
    for (uint64_t u = 0; u < oHeader.uDetections; ++u) {
        const Detection& oDet = pDetections[static_cast<size_t>(u)];
        putU64(p,      oDet.uRangeBin);
        putU64(p + 8,  oDet.uDopplerBin);
        putF64(p + 16, oDet.dRangeKm);
        putF64(p + 24, oDet.dDopplerHz);
        putF64(p + 32, oDet.dPowerDb);
        putF64(p + 40, oDet.dSnrDb);
        p += kArdDetectionBytes;
    }

    uLengthOut = uTotal;
    return true;
}

bool parseArdFrameHeader(const uint8_t* pBytes, size_t uLength, ArdFileHeader& oHeader)
{
    CHECK(pBytes != nullptr, "parseArdFrameHeader: null bytes");
    CHECK(uLength >= kArdHeaderBytes, "parseArdFrameHeader: shorter than a header");
    CHECK(std::memcmp(pBytes, kMagic, sizeof(kMagic)) == 0, "parseArdFrameHeader: bad magic");

    oHeader.uRangeBins         = getU64(pBytes + 8);
    oHeader.uDopplerBins       = getU64(pBytes + 16);
    oHeader.uSampleRateHz      = getU64(pBytes + 24);
    oHeader.uDopplerDecimation = getU64(pBytes + 32);
    oHeader.uCpiStartIndex     = getU64(pBytes + 40);
    oHeader.uDetections        = getU64(pBytes + 48);
    oHeader.uStatus            = getU64(pBytes + 56);

    if (!headerIsPlausible(oHeader, "the frame"))
        return false;
    CHECK(ardFrameBytes(oHeader) == uLength,
          "parseArdFrameHeader: frame length disagrees with its header");
    return true;
}

bool writeArdFrameBytes(const char* pPath, const uint8_t* pBytes, size_t uLength)
{
    CHECK(pPath != nullptr && pBytes != nullptr && uLength > 0,
          "writeArdFrameBytes: null argument");

    std::FILE* pFile = std::fopen(pPath, "wb");
    if (pFile == nullptr) {
        std::fprintf(stderr, "ard file: cannot open %s for writing: %s\n",
                     pPath, std::strerror(errno));
        return false;
    }
    bool bOk = std::fwrite(pBytes, 1, uLength, pFile) == uLength;
    if (std::fclose(pFile) != 0)
        bOk = false;
    if (!bOk)
        std::fprintf(stderr, "ard file: writing %s failed\n", pPath);
    return bOk;
}

bool writeArdFile(const char* pPath, const ArdFileHeader& oHeader,
                  const ComplexF* pcMap, const Detection* pDetections)
{
    if (pPath == nullptr || pcMap == nullptr) {
        std::fprintf(stderr, "ard file: null argument to writeArdFile\n");
        return false;
    }
    // Checked before the buffer is sized from the header, which happens
    // before serialiseArdFrame gets to check it.
    if (!headerIsPlausible(oHeader, pPath))
        return false;
    std::vector<uint8_t> vuFrame(ardFrameBytes(oHeader));
    size_t uLength = 0;
    if (!serialiseArdFrame(oHeader, pcMap, pDetections, vuFrame.data(), vuFrame.size(), uLength))
        return false;
    return writeArdFrameBytes(pPath, vuFrame.data(), uLength);
}

bool readArdFileHeader(const char* pPath, ArdFileHeader& oHeader)
{
    std::FILE* pFile = openAndReadHeader(pPath, oHeader);
    if (pFile == nullptr)
        return false;
    std::fclose(pFile);
    return true;
}

bool readArdFile(const char* pPath, ArdFileHeader& oHeader,
                 ComplexF* pcMap, uint64_t uMapCapacity,
                 Detection* pDetections, uint64_t uMaxDetections)
{
    if (pcMap == nullptr) {
        std::fprintf(stderr, "ard file: null map destination\n");
        return false;
    }

    std::FILE* pFile = openAndReadHeader(pPath, oHeader);
    if (pFile == nullptr)
        return false;

    const uint64_t uCells = oHeader.uRangeBins * oHeader.uDopplerBins;
    if (uCells > uMapCapacity) {
        std::fprintf(stderr, "ard file: %s holds %llu cells, more than the "
                             "%llu the caller allocated\n", pPath,
                     (unsigned long long)uCells,
                     (unsigned long long)uMapCapacity);
        std::fclose(pFile);
        return false;
    }
    if (oHeader.uDetections > uMaxDetections) {
        std::fprintf(stderr, "ard file: %s holds %llu detections, more than the "
                             "%llu the caller allocated\n", pPath,
                     (unsigned long long)oHeader.uDetections,
                     (unsigned long long)uMaxDetections);
        std::fclose(pFile);
        return false;
    }

    bool bOk = std::fread(pcMap, sizeof(ComplexF),
                          static_cast<size_t>(uCells), pFile) ==
               static_cast<size_t>(uCells);

    for (uint64_t u = 0; bOk && u < oHeader.uDetections; ++u)
        bOk = readDetection(pFile, pDetections[static_cast<size_t>(u)]);

    std::fclose(pFile);
    if (!bOk)
        std::fprintf(stderr, "ard file: %s is truncated\n", pPath);
    return bOk;
}

} // namespace pbr
