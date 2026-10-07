// Table building for HostFft. See HostFft.hpp.

#include "HostFft.hpp"

#include "pbr/check.hpp"
#include "pbr/types.hpp"

#include <cmath>

namespace pbr {

bool HostFft::init(uint64_t uLength)
{
    CHECK(uLength >= 2 && (uLength & (uLength - 1)) == 0,
          "HostFft length must be a power of two, at least 2");

    muLength = uLength;
    mvcTwiddle.resize(static_cast<size_t>(uLength / 2));
    for (uint64_t i = 0; i < uLength / 2; ++i) {
        const double dAngle = -2.0 * kPi * static_cast<double>(i) / static_cast<double>(uLength);
        mvcTwiddle[static_cast<size_t>(i)] = std::complex<double>(std::cos(dAngle), std::sin(dAngle));
    }

    // Bit reversal for the in-place radix-2 ordering.
    uint32_t uBits = 0;
    while ((1ULL << uBits) < uLength)
        ++uBits;
    mvuBitReverse.resize(static_cast<size_t>(uLength));
    for (uint64_t i = 0; i < uLength; ++i) {
        uint32_t uRev = 0;
        for (uint32_t uBit = 0; uBit < uBits; ++uBit)
            if (i & (1ULL << uBit))
                uRev |= 1u << (uBits - 1 - uBit);
        mvuBitReverse[static_cast<size_t>(i)] = uRev;
    }
    return true;
}

} // namespace pbr
