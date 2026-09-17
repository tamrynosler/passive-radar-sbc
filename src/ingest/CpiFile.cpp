#include "pbr/CpiFile.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace pbr {

namespace {

const char     kMagic[8]    = { 'P', 'B', 'R', 'C', 'P', 'I', '0', '1' };
const uint64_t kMaxSamples  = 1ULL << 32;   // sanity bound on a corrupt header

bool writeU64(std::FILE* pFile, uint64_t uValue)
{
    return std::fwrite(&uValue, sizeof(uValue), 1, pFile) == 1;
}

bool readU64(std::FILE* pFile, uint64_t& uValue)
{
    return std::fread(&uValue, sizeof(uValue), 1, pFile) == 1;
}

// Open and validate the header, leaving the stream positioned at the samples.
std::FILE* openAndReadHeader(const char* pPath, CpiFileHeader& oHeader)
{
    if (pPath == nullptr) {
        std::fprintf(stderr, "cpi: null path\n");
        return nullptr;
    }

    std::FILE* pFile = std::fopen(pPath, "rb");
    if (pFile == nullptr) {
        std::fprintf(stderr, "cpi: cannot open %s: %s\n", pPath, std::strerror(errno));
        return nullptr;
    }

    char achMagic[8];
    if (std::fread(achMagic, 1, sizeof(achMagic), pFile) != sizeof(achMagic) ||
        std::memcmp(achMagic, kMagic, sizeof(kMagic)) != 0) {
        std::fprintf(stderr, "cpi: %s is not a CPI file (bad magic)\n", pPath);
        std::fclose(pFile);
        return nullptr;
    }

    if (!readU64(pFile, oHeader.uSampleCount) ||
        !readU64(pFile, oHeader.uSampleRateHz) ||
        !readU64(pFile, oHeader.uStartIndex)) {
        std::fprintf(stderr, "cpi: %s has a truncated header\n", pPath);
        std::fclose(pFile);
        return nullptr;
    }

    if (oHeader.uSampleCount == 0 || oHeader.uSampleCount > kMaxSamples) {
        std::fprintf(stderr, "cpi: %s declares an implausible sample count (%llu)\n",
                     pPath, static_cast<unsigned long long>(oHeader.uSampleCount));
        std::fclose(pFile);
        return nullptr;
    }

    return pFile;
}

} // namespace

bool writeCpiFile(const char* pPath, const CpiFileHeader& oHeader,
                  const ComplexF* pcRef, const ComplexF* pcSur)
{
    if (pPath == nullptr || pcRef == nullptr || pcSur == nullptr) {
        std::fprintf(stderr, "cpi: null argument to writeCpiFile\n");
        return false;
    }
    if (oHeader.uSampleCount == 0) {
        std::fprintf(stderr, "cpi: refusing to write an empty CPI\n");
        return false;
    }

    std::FILE* pFile = std::fopen(pPath, "wb");
    if (pFile == nullptr) {
        std::fprintf(stderr, "cpi: cannot open %s for writing: %s\n",
                     pPath, std::strerror(errno));
        return false;
    }

    const size_t uCount = static_cast<size_t>(oHeader.uSampleCount);
    bool bOk = std::fwrite(kMagic, 1, sizeof(kMagic), pFile) == sizeof(kMagic) &&
               writeU64(pFile, oHeader.uSampleCount) &&
               writeU64(pFile, oHeader.uSampleRateHz) &&
               writeU64(pFile, oHeader.uStartIndex) &&
               std::fwrite(pcRef, sizeof(ComplexF), uCount, pFile) == uCount &&
               std::fwrite(pcSur, sizeof(ComplexF), uCount, pFile) == uCount;

    if (std::fclose(pFile) != 0)
        bOk = false;

    if (!bOk)
        std::fprintf(stderr, "cpi: writing %s failed: %s\n", pPath, std::strerror(errno));

    return bOk;
}

bool readCpiFileHeader(const char* pPath, CpiFileHeader& oHeader)
{
    std::FILE* pFile = openAndReadHeader(pPath, oHeader);
    if (pFile == nullptr)
        return false;
    std::fclose(pFile);
    return true;
}

bool readCpiFile(const char* pPath, CpiFileHeader& oHeader,
                 ComplexF* pcRef, ComplexF* pcSur, uint64_t uCapacity)
{
    if (pcRef == nullptr || pcSur == nullptr) {
        std::fprintf(stderr, "cpi: null destination buffer\n");
        return false;
    }

    std::FILE* pFile = openAndReadHeader(pPath, oHeader);
    if (pFile == nullptr)
        return false;

    // Refuse rather than truncate: a short read here would look like a
    // perfectly good CPI with a zero-filled tail, which the canceller would
    // happily process into a plausible-looking wrong answer.
    if (oHeader.uSampleCount > uCapacity) {
        std::fprintf(stderr,
                     "cpi: %s holds %llu samples, more than the %llu the caller "
                     "allocated\n", pPath,
                     static_cast<unsigned long long>(oHeader.uSampleCount),
                     static_cast<unsigned long long>(uCapacity));
        std::fclose(pFile);
        return false;
    }

    const size_t uCount = static_cast<size_t>(oHeader.uSampleCount);
    const bool bOk = std::fread(pcRef, sizeof(ComplexF), uCount, pFile) == uCount &&
                     std::fread(pcSur, sizeof(ComplexF), uCount, pFile) == uCount;

    std::fclose(pFile);

    if (!bOk)
        std::fprintf(stderr, "cpi: %s is truncated\n", pPath);

    return bOk;
}

} // namespace pbr
