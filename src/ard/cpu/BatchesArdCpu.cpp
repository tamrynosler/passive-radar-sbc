// The batches correlation on the CPU. See BatchesArdCpu.hpp for what it is
// and ArdAlgorithm::Batches in pbr/ArdProcessor.hpp for why the packing below
// reproduces the direct sum exactly.
//
// One block at a time, so the working set is two arrays of L complex floats
// rather than the whole CPI. That is not the fast way to do it -- the GPU
// version transforms all 1600 blocks in one cuFFT call -- but this is the
// reference, and processing blocks independently is what makes the packing
// legible.

#include "BatchesArdCpu.hpp"

#include "pbr/ArdProcessor.hpp"
#include "pbr/check.hpp"

#include <cmath>

namespace pbr {

bool BatchesArdCpu::init(uint64_t uCpiSamples, uint64_t uStride,
                         uint64_t uRangeBins)
{
    CHECK(!mbReady, "BatchesArdCpu::init called twice");

    const uint64_t uL = ardBatchTransformLength(uStride, uRangeBins);
    CHECK(uL > 0, "no usable transform length for this stride and range count");

    muStride     = uStride;
    muRangeBins  = uRangeBins;
    muL          = uL;
    muBatches    = uCpiSamples / uStride;

    mvcX.assign(static_cast<size_t>(uL), ComplexF(0.0f, 0.0f));
    mvcY.assign(static_cast<size_t>(uL), ComplexF(0.0f, 0.0f));

    // Twiddles: computed in double and rounded once, so the table is the
    // closest float to the true value rather than the result of float
    // trigonometry.
    mvcTwiddle.resize(static_cast<size_t>(uL / 2));
    for (uint64_t i = 0; i < uL / 2; ++i) {
        const double dAngle = -2.0 * kPi * static_cast<double>(i) /
                              static_cast<double>(uL);
        mvcTwiddle[static_cast<size_t>(i)] =
            ComplexF(static_cast<float>(std::cos(dAngle)),
                     static_cast<float>(std::sin(dAngle)));
    }

    uint32_t uBits = 0;
    while ((1ULL << uBits) < uL)
        ++uBits;
    mvuBitReverse.resize(static_cast<size_t>(uL));
    for (uint64_t i = 0; i < uL; ++i) {
        uint32_t uRev = 0;
        for (uint32_t uBit = 0; uBit < uBits; ++uBit)
            if (i & (1ULL << uBit))
                uRev |= 1u << (uBits - 1 - uBit);
        mvuBitReverse[static_cast<size_t>(i)] = uRev;
    }

    mbReady = true;
    return true;
}

void BatchesArdCpu::fft(ComplexF* pc, bool bInverse) const
{
    const uint64_t uL = muL;

    for (uint64_t i = 0; i < uL; ++i) {
        const uint64_t j = mvuBitReverse[static_cast<size_t>(i)];
        if (j > i) {
            const ComplexF cSwap = pc[i];
            pc[i] = pc[j];
            pc[j] = cSwap;
        }
    }

    // Butterflies of span 2, 4, ..., L. The twiddle for position i in a span
    // of uLen is exp(-j 2 pi i / uLen) = table[i * L / uLen].
    for (uint64_t uLen = 2; uLen <= uL; uLen <<= 1) {
        const uint64_t uHalf = uLen / 2;
        const uint64_t uStep = uL / uLen;

        for (uint64_t uBlock = 0; uBlock < uL; uBlock += uLen) {
            for (uint64_t i = 0; i < uHalf; ++i) {
                const ComplexF& cTw = mvcTwiddle[static_cast<size_t>(i * uStep)];
                const float fTr = cTw.real();
                const float fTi = bInverse ? -cTw.imag() : cTw.imag();

                ComplexF& cA = pc[uBlock + i];
                ComplexF& cB = pc[uBlock + i + uHalf];

                const float fBr = cB.real(), fBi = cB.imag();
                const float fProdRe = fBr * fTr - fBi * fTi;   // b * twiddle
                const float fProdIm = fBr * fTi + fBi * fTr;
                const float fAr = cA.real(), fAi = cA.imag();

                cA = ComplexF(fAr + fProdRe, fAi + fProdIm);
                cB = ComplexF(fAr - fProdRe, fAi - fProdIm);
            }
        }
    }
}

void BatchesArdCpu::correlate(const ComplexF* pcRef, const ComplexF* pcCleaned,
                              ComplexF* pcDecimated)
{
    const uint64_t uL = muL;
    const uint64_t uS = muStride;
    const uint64_t uK = muRangeBins;
    const uint64_t uB = muBatches;

    // Where the wrapped reference history starts. L >= S + K - 1 guarantees
    // uWrapFirst >= uS, so the head and the tail of Y never collide.
    const uint64_t uWrapFirst = uL - uK + 1;

    const float fInvL = 1.0f / static_cast<float>(uL);

    for (uint64_t uBatch = 0; uBatch < uB; ++uBatch) {
        const uint64_t uBlockStart = uBatch * uS;

        // ---- pack ---------------------------------------------------------
        // X: this block of surveillance, zero-padded. Only these uS samples
        // are non-zero, which is what keeps the integration length -- and so
        // the map -- identical to the direct route.
        for (uint64_t i = 0; i < uS; ++i)
            mvcX[static_cast<size_t>(i)] = pcCleaned[uBlockStart + i];
        for (uint64_t i = uS; i < uL; ++i)
            mvcX[static_cast<size_t>(i)] = ComplexF(0.0f, 0.0f);

        // Y: this block of reference at the head, and the K-1 samples BEFORE
        // the block in the wrapped tail, where the circular correlation will
        // look for them. Before the start of the CPI the reference is zero,
        // which is the same convention the direct route uses.
        for (uint64_t j = 0; j < uS; ++j)
            mvcY[static_cast<size_t>(j)] = pcRef[uBlockStart + j];
        for (uint64_t j = uS; j < uWrapFirst; ++j)
            mvcY[static_cast<size_t>(j)] = ComplexF(0.0f, 0.0f);
        for (uint64_t j = uWrapFirst; j < uL; ++j) {
            // j - L is negative: this is uBlockStart - (L - j) samples back.
            const uint64_t uBack = uL - j;
            if (uBlockStart >= uBack) {
                mvcY[static_cast<size_t>(j)] = pcRef[uBlockStart - uBack];
            } else {
                mvcY[static_cast<size_t>(j)] = ComplexF(0.0f, 0.0f);
            }
        }

        // ---- correlate ----------------------------------------------------
        fft(&mvcX[0], false);
        fft(&mvcY[0], false);

        for (uint64_t k = 0; k < uL; ++k) {
            const ComplexF& cX = mvcX[static_cast<size_t>(k)];
            const ComplexF& cY = mvcY[static_cast<size_t>(k)];
            // X * conj(Y)
            mvcX[static_cast<size_t>(k)] = ComplexF(
                cX.real() * cY.real() + cX.imag() * cY.imag(),
                cX.imag() * cY.real() - cX.real() * cY.imag());
        }

        fft(&mvcX[0], true);

        // ---- extract ------------------------------------------------------
        // Lags 0..K-1 are the range bins; the rest of the transform is the
        // part of the correlation the map does not cover. The 1/L the inverse
        // transform owes is applied here.
        for (uint64_t r = 0; r < uK; ++r) {
            const ComplexF& cLag = mvcX[static_cast<size_t>(r)];
            pcDecimated[static_cast<size_t>(r * uB + uBatch)] =
                ComplexF(cLag.real() * fInvL, cLag.imag() * fInvL);
        }
    }
}

} // namespace pbr
