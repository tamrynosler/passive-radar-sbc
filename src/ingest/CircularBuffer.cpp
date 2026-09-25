#include "pbr/CircularBuffer.hpp"

#include "pbr/check.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <new>

namespace pbr {

CircularBuffer::CircularBuffer(uint64_t uCapacity)
    : mh_cBuffer(nullptr),
      mh_uValid(nullptr),
      muCapacity(uCapacity),
      muWriteEnd(0),
      mbValid(false)
{
    if (uCapacity == 0) {
        std::fprintf(stderr, "CircularBuffer: capacity must be non-zero\n");
        return;
    }

    // new[] on a class type default-constructs every element, and
    // std::complex<float>'s default constructor is 0 + 0i, so the buffer
    // starts fully zeroed -- which is what "never written" must look like.
    // nothrow because the project does not use exceptions.
    mh_cBuffer = new (std::nothrow) ComplexF[uCapacity];
    if (mh_cBuffer == nullptr) {
        std::fprintf(stderr, "CircularBuffer: allocation of %llu samples failed\n",
                     static_cast<unsigned long long>(uCapacity));
        return;
    }

    // Validity flags start clear: nothing has been written. The () makes
    // new[] zero-initialise the array.
    mh_uValid = new (std::nothrow) uint8_t[uCapacity]();
    if (mh_uValid == nullptr) {
        std::fprintf(stderr, "CircularBuffer: allocation of %llu validity flags failed\n",
                     static_cast<unsigned long long>(uCapacity));
        delete[] mh_cBuffer;
        mh_cBuffer = nullptr;
        return;
    }

    mbValid = true;
}

CircularBuffer::~CircularBuffer()
{
    delete[] mh_cBuffer;
    delete[] mh_uValid;
}

void CircularBuffer::zeroSpan(uint64_t uAbsIndex, uint64_t uCount)
{
    assert(uCount <= muCapacity);

    const uint64_t uOffset = uAbsIndex % muCapacity;
    const uint64_t uFirst  = std::min(uCount, muCapacity - uOffset);

    std::fill(mh_cBuffer + uOffset,
              mh_cBuffer + uOffset + uFirst,
              ComplexF(0.0f, 0.0f));
    std::memset(mh_uValid + uOffset, 0, static_cast<size_t>(uFirst));

    if (uCount > uFirst) {
        std::fill(mh_cBuffer,
                  mh_cBuffer + (uCount - uFirst),
                  ComplexF(0.0f, 0.0f));
        std::memset(mh_uValid, 0, static_cast<size_t>(uCount - uFirst));
    }
}

bool CircularBuffer::write(uint64_t uAbsIndex, const ComplexF* pcSrc, uint64_t uCount)
{
    CHECK(mbValid, "write on an invalid buffer");

    if (uCount == 0)
        return true;

    CHECK(pcSrc != nullptr, "write with a null source pointer");
    CHECK(uCount <= muCapacity, "write is larger than the buffer capacity");
    CHECK(uAbsIndex <= UINT64_MAX - uCount, "absolute index overflow");

    // Trim any samples that have already fallen out of the window.
    const uint64_t uOldest = oldestValid();

    if (uAbsIndex + uCount <= uOldest) {
        std::fprintf(stderr,
                     "CircularBuffer: dropped stale write at %llu (+%llu); "
                     "oldest valid index is %llu\n",
                     static_cast<unsigned long long>(uAbsIndex),
                     static_cast<unsigned long long>(uCount),
                     static_cast<unsigned long long>(uOldest));
        return false;
    }

    if (uAbsIndex < uOldest) {
        const uint64_t uSkip = uOldest - uAbsIndex;
        uAbsIndex += uSkip;
        pcSrc     += uSkip;
        uCount    -= uSkip;
    }

    // A forward jump leaves a hole between the old write end and this write.
    // Zero it, so it cannot read back as stale data from the previous lap
    // through the buffer. Zeroing more than one full lap is pointless, since
    // a lap's worth of writes overwrites everything anyway.
    if (uAbsIndex > muWriteEnd) {
        uint64_t       uHoleBegin = muWriteEnd;
        const uint64_t uHoleEnd   = uAbsIndex;

        if (uHoleEnd - uHoleBegin > muCapacity)
            uHoleBegin = uHoleEnd - muCapacity;

        zeroSpan(uHoleBegin, uHoleEnd - uHoleBegin);
    }

    // One modulo, then at most two contiguous copies: up to the wrap point,
    // then from the start of the buffer. No per-sample index arithmetic.
    const uint64_t uOffset = uAbsIndex % muCapacity;
    const uint64_t uFirst  = std::min(uCount, muCapacity - uOffset);

    std::memcpy(mh_cBuffer + uOffset, pcSrc, uFirst * sizeof(ComplexF));
    std::memset(mh_uValid + uOffset, 1, static_cast<size_t>(uFirst));

    if (uCount > uFirst) {
        std::memcpy(mh_cBuffer, pcSrc + uFirst,
                    (uCount - uFirst) * sizeof(ComplexF));
        std::memset(mh_uValid, 1, static_cast<size_t>(uCount - uFirst));
    }

    // Out-of-order writes must not rewind the window.
    const uint64_t uEnd = uAbsIndex + uCount;
    if (uEnd > muWriteEnd)
        muWriteEnd = uEnd;

    return true;
}

void CircularBuffer::reset()
{
    if (!mbValid)
        return;
    std::fill(mh_cBuffer, mh_cBuffer + muCapacity, ComplexF(0.0f, 0.0f));
    std::memset(mh_uValid, 0, static_cast<size_t>(muCapacity));
    muWriteEnd = 0;
}

uint64_t CircularBuffer::validCount(uint64_t uAbsIndex, uint64_t uCount) const
{
    // Same residency rules as read(); a window this cannot vouch for counts
    // as having no valid samples, which is the safe answer.
    if (!mbValid || uCount == 0 || uCount > muCapacity ||
        uAbsIndex > UINT64_MAX - uCount ||
        uAbsIndex < oldestValid() || uAbsIndex + uCount > muWriteEnd)
        return 0;

    // The same split as read(): up to the wrap point, then from the start.
    const uint64_t uOffset = uAbsIndex % muCapacity;
    const uint64_t uFirst  = std::min(uCount, muCapacity - uOffset);

    uint64_t uValid = 0;
    for (uint64_t u = 0; u < uFirst; ++u)
        uValid += mh_uValid[uOffset + u];
    for (uint64_t u = 0; u < uCount - uFirst; ++u)
        uValid += mh_uValid[u];

    return uValid;
}

bool CircularBuffer::read(uint64_t uAbsIndex, ComplexF* pcDst, uint64_t uCount) const
{
    CHECK(mbValid, "read on an invalid buffer");

    if (uCount == 0)
        return true;

    CHECK(pcDst != nullptr, "read with a null destination pointer");
    CHECK(uCount <= muCapacity, "read is larger than the buffer capacity");
    CHECK(uAbsIndex <= UINT64_MAX - uCount, "absolute index overflow");
    CHECK(uAbsIndex >= oldestValid(),
          "read starts before the oldest resident sample");
    CHECK(uAbsIndex + uCount <= muWriteEnd,
          "read extends past the newest written sample");

    const uint64_t uOffset = uAbsIndex % muCapacity;
    const uint64_t uFirst  = std::min(uCount, muCapacity - uOffset);

    std::memcpy(pcDst, mh_cBuffer + uOffset, uFirst * sizeof(ComplexF));

    if (uCount > uFirst)
        std::memcpy(pcDst + uFirst, mh_cBuffer,
                    (uCount - uFirst) * sizeof(ComplexF));

    return true;
}

} // namespace pbr
