#include "pbr/Spectrum.hpp"

#include "pbr/check.hpp"

#include <algorithm>
#include <cmath>

namespace pbr {

namespace {

// Added to every bin's power before the log, so an exactly silent bin gives
// a large negative number rather than -inf.
const double kPowerFloor = 1e-30;

// Only every kSegmentStride-th segment is transformed and averaged. The
// display is 512 bins; 40 averages (400 / 10) already give a dB bin a standard
// deviation of about 0.7 dB, which a spectrogram cannot show, and it is a tenth
// of the CPU time on the Jetson.
const uint64_t kSegmentStride = 10;

// In-place iterative radix-2 forward FFT of uN points, uN a power of two.
// pcTwiddle holds exp(-j 2 pi k / uN) for k in [0, uN/2).
void fftInPlace(ComplexF* pc, uint64_t uN, const ComplexF* pcTwiddle)
{
    // Bit-reversal permutation.
    uint64_t j = 0;
    for (uint64_t i = 1; i < uN; ++i) {
        uint64_t uBit = uN >> 1;
        for (; (j & uBit) != 0; uBit >>= 1)
            j ^= uBit;
        j ^= uBit;
        if (i < j)
            std::swap(pc[i], pc[j]);
    }

    // Butterflies, doubling the span each pass.
    for (uint64_t uLen = 2; uLen <= uN; uLen <<= 1) {
        const uint64_t uHalf = uLen / 2;
        const uint64_t uStep = uN / uLen;
        for (uint64_t i = 0; i < uN; i += uLen) {
            for (uint64_t k = 0; k < uHalf; ++k) {
                const ComplexF cTop    = pc[i + k];
                const ComplexF cW      = pcTwiddle[k * uStep];
                const ComplexF cLow    = pc[i + k + uHalf];
                // Real and imaginary parts written out: std::complex operator*
                // calls a NaN-checking helper without -ffast-math.
                const float fBotRe = cLow.real() * cW.real() - cLow.imag() * cW.imag();
                const float fBotIm = cLow.real() * cW.imag() + cLow.imag() * cW.real();
                pc[i + k]         = ComplexF(cTop.real() + fBotRe, cTop.imag() + fBotIm);
                pc[i + k + uHalf] = ComplexF(cTop.real() - fBotRe, cTop.imag() - fBotIm);
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

    for (uint64_t i = 0; i < uBins; ++i)
        mvfWindow[static_cast<size_t>(i)] =
            static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(uBins)));
    for (uint64_t k = 0; k < uBins / 2; ++k) {
        const double dAngle = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(uBins);
        mvcTwiddle[static_cast<size_t>(k)] =
            ComplexF(static_cast<float>(std::cos(dAngle)), static_cast<float>(std::sin(dAngle)));
    }
    return true;
}

bool SpectrumEstimator::compute(const ComplexF* pcSamples, uint64_t uSamples, float* pfDbOut)
{
    CHECK(muBins > 0, "SpectrumEstimator: compute before init");
    CHECK(pcSamples != nullptr && pfDbOut != nullptr, "SpectrumEstimator: null buffer");
    CHECK(uSamples >= muBins, "SpectrumEstimator: CPI shorter than one segment");

    const uint64_t uSegments = uSamples / muBins;
    uint64_t       uUsed     = 0;   // segments actually averaged
    const size_t   uN        = static_cast<size_t>(muBins);
    std::fill(mvdPower.begin(), mvdPower.end(), 0.0);

    for (uint64_t uSegment = 0; uSegment < uSegments; uSegment += kSegmentStride) {
        ++uUsed;
        const ComplexF* pcSegment = pcSamples + uSegment * muBins;
        for (size_t i = 0; i < uN; ++i)
            mvcWork[i] = pcSegment[i] * mvfWindow[i];
        fftInPlace(mvcWork.data(), muBins, mvcTwiddle.data());
        for (size_t i = 0; i < uN; ++i) {
            const float fRe = mvcWork[i].real();
            const float fIm = mvcWork[i].imag();
            mvdPower[i] += static_cast<double>(fRe) * fRe + static_cast<double>(fIm) * fIm;
        }
    }

    // Mean over segments, per bin, then dB, fftshifted: output bin 0 is the
    // transform's bin N/2 (-fs/2), output bin N/2 is the transform's bin 0.
    const double dScale = 1.0 / (static_cast<double>(uUsed) * static_cast<double>(muBins));
    for (size_t uOut = 0; uOut < uN; ++uOut) {
        const size_t k  = (uOut + uN / 2) % uN;
        const double dP = mvdPower[k] * dScale;
        float fDb = static_cast<float>(10.0 * std::log10(dP + kPowerFloor));
        if (!std::isfinite(fDb) || fDb < kSpectrumMinDb)
            fDb = kSpectrumMinDb;
        pfDbOut[uOut] = fDb;
    }
    return true;
}

double spectrumRmsBandwidthHz(const float* pfDb, uint64_t uBins, double dSampleRateHz,
                              double* pdCentreHz)
{
    *pdCentreHz = 0.0;
    if (pfDb == nullptr || uBins == 0)
        return 0.0;
    const size_t uN = static_cast<size_t>(uBins);

    std::vector<double> vdPower(uN);
    for (size_t i = 0; i < uN; ++i)
        vdPower[i] = std::pow(10.0, static_cast<double>(pfDb[i]) / 10.0);

    // The floor: mean of the quietest tenth of the bins.
    std::vector<double> vdSorted(vdPower);
    std::sort(vdSorted.begin(), vdSorted.end());
    const size_t uQuiet = std::max<size_t>(1, uN / 10);
    double dFloor = 0.0;
    for (size_t i = 0; i < uQuiet; ++i)
        dFloor += vdSorted[i];
    dFloor /= static_cast<double>(uQuiet);

    double dSumW = 0.0, dSumWF = 0.0;
    std::vector<double> vdWeight(uN);
    for (size_t i = 0; i < uN; ++i) {
        const double dF = (static_cast<double>(i) - static_cast<double>(uN / 2)) *
                          dSampleRateHz / static_cast<double>(uN);
        vdWeight[i] = std::max(vdPower[i] - dFloor, 0.0);
        dSumW  += vdWeight[i];
        dSumWF += vdWeight[i] * dF;
    }
    if (!(dSumW > 0.0))
        return 0.0;

    const double dCentre = dSumWF / dSumW;
    double dSumVar = 0.0;
    for (size_t i = 0; i < uN; ++i) {
        const double dF = (static_cast<double>(i) - static_cast<double>(uN / 2)) *
                          dSampleRateHz / static_cast<double>(uN) - dCentre;
        dSumVar += vdWeight[i] * dF * dF;
    }
    *pdCentreHz = dCentre;
    return std::sqrt(dSumVar / dSumW);
}

} // namespace pbr
