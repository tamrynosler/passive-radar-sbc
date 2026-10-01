#include "pbr/Spectrum.hpp"

#include "pbr/check.hpp"

#include <algorithm>
#include <cmath>

namespace pbr {

namespace {

// In-place iterative radix-2 forward FFT of n points, n a power of two.
// pTwiddle holds exp(-j 2 pi k / n) for k in [0, n/2).
void fftInPlace(ComplexF* pc, uint64_t n, const ComplexF* pTwiddle)
{
    // Bit-reversal permutation.
    uint64_t j = 0;
    for (uint64_t i = 1; i < n; ++i) {
        uint64_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(pc[i], pc[j]);
    }

    // Butterflies, doubling the span each pass.
    for (uint64_t len = 2; len <= n; len <<= 1) {
        const uint64_t half = len / 2;
        const uint64_t step = n / len;
        for (uint64_t i = 0; i < n; i += len) {
            for (uint64_t k = 0; k < half; ++k) {
                const ComplexF u = pc[i + k];
                const ComplexF v = pc[i + k + half] * pTwiddle[k * step];
                pc[i + k]        = u + v;
                pc[i + k + half] = u - v;
            }
        }
    }
}

} // namespace

bool SpectrumEstimator::init(uint64_t uBins)
{
    CHECK(uBins >= 8 && (uBins & (uBins - 1)) == 0,
          "SpectrumEstimator: bins must be a power of two, at least 8");

    muBins = uBins;
    mvfWindow.resize(static_cast<size_t>(uBins));
    mvcTwiddle.resize(static_cast<size_t>(uBins / 2));
    mvcWork.resize(static_cast<size_t>(uBins));
    mvdPower.resize(static_cast<size_t>(uBins));

    const double dPi = 3.14159265358979323846;
    for (uint64_t i = 0; i < uBins; ++i)
        mvfWindow[static_cast<size_t>(i)] =
            static_cast<float>(0.5 - 0.5 * std::cos(2.0 * dPi * static_cast<double>(i) / static_cast<double>(uBins)));
    for (uint64_t k = 0; k < uBins / 2; ++k) {
        const double dAngle = -2.0 * dPi * static_cast<double>(k) / static_cast<double>(uBins);
        mvcTwiddle[static_cast<size_t>(k)] =
            ComplexF(static_cast<float>(std::cos(dAngle)), static_cast<float>(std::sin(dAngle)));
    }
    return true;
}

bool SpectrumEstimator::compute(const ComplexF* pSamples, uint64_t uSamples, float* pDbOut)
{
    CHECK(muBins > 0, "SpectrumEstimator: compute before init");
    CHECK(pSamples != nullptr && pDbOut != nullptr, "SpectrumEstimator: null buffer");
    CHECK(uSamples >= muBins, "SpectrumEstimator: CPI shorter than one segment");

    const uint64_t uSegments = uSamples / muBins;
    const size_t   uN        = static_cast<size_t>(muBins);
    std::fill(mvdPower.begin(), mvdPower.end(), 0.0);

    for (uint64_t s = 0; s < uSegments; ++s) {
        const ComplexF* pSeg = pSamples + s * muBins;
        for (size_t i = 0; i < uN; ++i)
            mvcWork[i] = pSeg[i] * mvfWindow[i];
        fftInPlace(mvcWork.data(), muBins, mvcTwiddle.data());
        for (size_t i = 0; i < uN; ++i) {
            const float fRe = mvcWork[i].real();
            const float fIm = mvcWork[i].imag();
            mvdPower[i] += static_cast<double>(fRe) * fRe + static_cast<double>(fIm) * fIm;
        }
    }

    // Mean over segments, per bin, then dB, fftshifted: output bin 0 is the
    // transform's bin N/2 (-fs/2), output bin N/2 is the transform's bin 0.
    const double dScale = 1.0 / (static_cast<double>(uSegments) * static_cast<double>(muBins));
    for (size_t o = 0; o < uN; ++o) {
        const size_t k  = (o + uN / 2) % uN;
        const double dP = mvdPower[k] * dScale;
        float fDb = static_cast<float>(10.0 * std::log10(dP + 1e-30));
        if (!std::isfinite(fDb) || fDb < kSpectrumMinDb)
            fDb = kSpectrumMinDb;
        pDbOut[o] = fDb;
    }
    return true;
}

} // namespace pbr
