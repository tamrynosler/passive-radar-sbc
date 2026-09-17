#include "pbr/ArdFile.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace pbr {

namespace {

const char     kMagic[8]   = { 'P', 'B', 'R', 'A', 'R', 'D', '0', '1' };
const uint64_t kMaxCells   = 1ULL << 28;   // sanity bound on a corrupt header
const uint64_t kMaxDetects = 1ULL << 24;

bool writeU64(std::FILE* pFile, uint64_t uValue)
{
    return std::fwrite(&uValue, sizeof(uValue), 1, pFile) == 1;
}

bool readU64(std::FILE* pFile, uint64_t& uValue)
{
    return std::fread(&uValue, sizeof(uValue), 1, pFile) == 1;
}

// Detections are written field by field rather than as a raw struct dump, so
// the file does not depend on the compiler's padding choices.
bool writeDetection(std::FILE* pFile, const Detection& oDet)
{
    return writeU64(pFile, oDet.uRangeBin) &&
           writeU64(pFile, oDet.uDopplerBin) &&
           std::fwrite(&oDet.dRangeKm,   sizeof(double), 1, pFile) == 1 &&
           std::fwrite(&oDet.dDopplerHz, sizeof(double), 1, pFile) == 1 &&
           std::fwrite(&oDet.dPowerDb,   sizeof(double), 1, pFile) == 1 &&
           std::fwrite(&oDet.dSnrDb,     sizeof(double), 1, pFile) == 1;
}

bool readDetection(std::FILE* pFile, Detection& oDet)
{
    return readU64(pFile, oDet.uRangeBin) &&
           readU64(pFile, oDet.uDopplerBin) &&
           std::fread(&oDet.dRangeKm,   sizeof(double), 1, pFile) == 1 &&
           std::fread(&oDet.dDopplerHz, sizeof(double), 1, pFile) == 1 &&
           std::fread(&oDet.dPowerDb,   sizeof(double), 1, pFile) == 1 &&
           std::fread(&oDet.dSnrDb,     sizeof(double), 1, pFile) == 1;
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
    uint64_t uReserved = 0;
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
        !readU64(pFile, uReserved)) {
        std::fprintf(stderr, "ard file: %s has a truncated header\n", pPath);
        std::fclose(pFile);
        return nullptr;
    }

    const uint64_t uCells = oHeader.uRangeBins * oHeader.uDopplerBins;
    if (oHeader.uRangeBins == 0 || oHeader.uDopplerBins == 0 ||
        uCells > kMaxCells || oHeader.uDetections > kMaxDetects) {
        std::fprintf(stderr, "ard file: %s declares implausible dimensions "
                             "(%llu x %llu, %llu detections)\n", pPath,
                     (unsigned long long)oHeader.uRangeBins,
                     (unsigned long long)oHeader.uDopplerBins,
                     (unsigned long long)oHeader.uDetections);
        std::fclose(pFile);
        return nullptr;
    }

    return pFile;
}

} // namespace

bool writeArdFile(const char* pPath, const ArdFileHeader& oHeader,
                  const ComplexF* pcMap, const Detection* pDetections)
{
    if (pPath == nullptr || pcMap == nullptr) {
        std::fprintf(stderr, "ard file: null argument to writeArdFile\n");
        return false;
    }
    if (oHeader.uDetections > 0 && pDetections == nullptr) {
        std::fprintf(stderr, "ard file: detection count is non-zero but the "
                             "array is null\n");
        return false;
    }

    std::FILE* pFile = std::fopen(pPath, "wb");
    if (pFile == nullptr) {
        std::fprintf(stderr, "ard file: cannot open %s for writing: %s\n",
                     pPath, std::strerror(errno));
        return false;
    }

    const size_t uCells = static_cast<size_t>(oHeader.uRangeBins *
                                              oHeader.uDopplerBins);

    bool bOk = std::fwrite(kMagic, 1, sizeof(kMagic), pFile) == sizeof(kMagic) &&
               writeU64(pFile, oHeader.uRangeBins) &&
               writeU64(pFile, oHeader.uDopplerBins) &&
               writeU64(pFile, oHeader.uSampleRateHz) &&
               writeU64(pFile, oHeader.uDopplerDecimation) &&
               writeU64(pFile, oHeader.uCpiStartIndex) &&
               writeU64(pFile, oHeader.uDetections) &&
               writeU64(pFile, 0) &&
               std::fwrite(pcMap, sizeof(ComplexF), uCells, pFile) == uCells;

    for (uint64_t u = 0; bOk && u < oHeader.uDetections; ++u)
        bOk = writeDetection(pFile, pDetections[static_cast<size_t>(u)]);

    if (std::fclose(pFile) != 0)
        bOk = false;
    if (!bOk)
        std::fprintf(stderr, "ard file: writing %s failed\n", pPath);
    return bOk;
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
