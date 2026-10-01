// Unit tests for the Welch spectrum estimator behind the viewer's
// before-processing spectrogram. Plain main() + asserts, no framework.
//
// NDEBUG is undefined deliberately and before <cassert>: the project builds
// Release, which defines NDEBUG and would compile every assert below out.
#undef NDEBUG
#include <cassert>

#include "pbr/Spectrum.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace pbr;

namespace {

const uint64_t kBins    = 512;
const uint64_t kSamples = 204800;   // one CPI at 204800 Hz
const double   kPi      = 3.14159265358979323846;

// A complex tone at a fraction of the sample rate: fs * (fBin / kBins) for a
// bin-centred frequency, so it lands in exactly one bin after the window's
// main lobe is accounted for.
std::vector<ComplexF> tone(double dCyclesPerSample, float fAmplitude)
{
    std::vector<ComplexF> vc(static_cast<size_t>(kSamples));
    for (size_t i = 0; i < vc.size(); ++i) {
        const double dPhase = 2.0 * kPi * dCyclesPerSample * static_cast<double>(i);
        vc[i] = ComplexF(fAmplitude * static_cast<float>(std::cos(dPhase)),
                         fAmplitude * static_cast<float>(std::sin(dPhase)));
    }
    return vc;
}

size_t peakBin(const std::vector<float>& vf)
{
    size_t uBest = 0;
    for (size_t i = 1; i < vf.size(); ++i)
        if (vf[i] > vf[uBest])
            uBest = i;
    return uBest;
}

// A tone at +fs/4 must peak at bin N/2 + N/4; at -fs/8 at N/2 - N/8; DC at
// N/2. This pins the fftshift and the sign of the frequency axis, which is
// what the page's axis labels rely on.
void testTonesLandInTheRightBins()
{
    SpectrumEstimator oEst;
    assert(oEst.init(kBins));
    std::vector<float> vfDb(static_cast<size_t>(kBins));

    std::vector<ComplexF> vc = tone(0.25, 1.0f);
    assert(oEst.compute(vc.data(), kSamples, vfDb.data()));
    assert(peakBin(vfDb) == kBins / 2 + kBins / 4);

    vc = tone(-0.125, 1.0f);
    assert(oEst.compute(vc.data(), kSamples, vfDb.data()));
    assert(peakBin(vfDb) == kBins / 2 - kBins / 8);

    vc = tone(0.0, 1.0f);
    assert(oEst.compute(vc.data(), kSamples, vfDb.data()));
    assert(peakBin(vfDb) == kBins / 2);
    std::printf("  tones: +fs/4, -fs/8 and DC in bins %llu, %llu, %llu\n",
                (unsigned long long)(kBins / 2 + kBins / 4),
                (unsigned long long)(kBins / 2 - kBins / 8),
                (unsigned long long)(kBins / 2));
}

// Twice the amplitude is 6 dB more, and a tone stands well above the
// window's sidelobes: the estimate is a power spectrum, not something
// monotone but arbitrary.
void testLevelsAreDecibels()
{
    SpectrumEstimator oEst;
    assert(oEst.init(kBins));
    std::vector<float> vfA(static_cast<size_t>(kBins));
    std::vector<float> vfB(static_cast<size_t>(kBins));

    std::vector<ComplexF> vc = tone(0.25, 1.0f);
    assert(oEst.compute(vc.data(), kSamples, vfA.data()));
    vc = tone(0.25, 2.0f);
    assert(oEst.compute(vc.data(), kSamples, vfB.data()));

    const size_t uPeak = kBins / 2 + kBins / 4;
    const float fGain = vfB[uPeak] - vfA[uPeak];
    std::printf("  2x amplitude: +%.3f dB (expect 6.021)\n", fGain);
    assert(std::fabs(fGain - 6.0206f) < 0.01f);

    // Far from the tone the Hann sidelobes are below -60 dB.
    assert(vfA[uPeak] - vfA[kBins / 2] > 60.0f);
}

// White noise gives a flat spectrum: every bin within a few dB of the mean
// once 400 segments are averaged.
void testWhiteNoiseIsFlat()
{
    SpectrumEstimator oEst;
    assert(oEst.init(kBins));
    std::vector<ComplexF> vc(static_cast<size_t>(kSamples));
    uint32_t uSeed = 12345;
    for (size_t i = 0; i < vc.size(); ++i) {
        // Uniform in [-1, 1) on each axis from a small LCG: independent
        // enough for a flatness check.
        uSeed = uSeed * 1664525u + 1013904223u;
        const float fI = static_cast<float>(uSeed >> 8) / 8388608.0f - 1.0f;
        uSeed = uSeed * 1664525u + 1013904223u;
        const float fQ = static_cast<float>(uSeed >> 8) / 8388608.0f - 1.0f;
        vc[i] = ComplexF(fI, fQ);
    }
    std::vector<float> vfDb(static_cast<size_t>(kBins));
    assert(oEst.compute(vc.data(), kSamples, vfDb.data()));

    double dMean = 0.0;
    for (size_t i = 0; i < vfDb.size(); ++i) dMean += vfDb[i];
    dMean /= static_cast<double>(vfDb.size());
    float fWorst = 0.0f;
    for (size_t i = 0; i < vfDb.size(); ++i)
        fWorst = std::max(fWorst, std::fabs(vfDb[i] - static_cast<float>(dMean)));
    std::printf("  white noise: mean %.2f dB, worst deviation %.2f dB\n", dMean, fWorst);
    assert(fWorst < 3.0f);
}

// Dead input and a non-finite sample must give the floor, never NaN, since
// the value goes on the wire as an integer.
void testDegenerateInputs()
{
    SpectrumEstimator oEst;
    assert(oEst.init(kBins));
    std::vector<float> vfDb(static_cast<size_t>(kBins));

    std::vector<ComplexF> vc(static_cast<size_t>(kSamples), ComplexF(0.0f, 0.0f));
    assert(oEst.compute(vc.data(), kSamples, vfDb.data()));
    for (size_t i = 0; i < vfDb.size(); ++i)
        assert(vfDb[i] == kSpectrumMinDb);

    vc = tone(0.25, 1.0f);
    vc[100] = ComplexF(std::nan(""), 0.0f);
    assert(oEst.compute(vc.data(), kSamples, vfDb.data()));
    for (size_t i = 0; i < vfDb.size(); ++i)
        assert(std::isfinite(vfDb[i]));

    // A partial trailing segment is ignored, not read past the end.
    assert(oEst.compute(vc.data(), kBins + 7, vfDb.data()));

    // Bad sizes are refused, not guessed.
    assert(!oEst.init(500));
    assert(!oEst.init(4));
}

} // namespace

int main()
{
    testTonesLandInTheRightBins();
    testLevelsAreDecibels();
    testWhiteNoiseIsFlat();
    testDegenerateInputs();
    std::printf("test_spectrum: all tests passed\n");
    return 0;
}
