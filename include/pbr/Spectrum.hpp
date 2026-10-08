#ifndef PBR_SPECTRUM_HPP
#define PBR_SPECTRUM_HPP

#include <cstdint>
#include <vector>

#include "pbr/types.hpp"

namespace pbr {

// The floor a spectrum value is clamped to: a segment holding a non-finite
// sample, or no signal at all, lands here rather than at -inf or NaN.
const float kSpectrumMinDb = -300.0f;

// Welch-averaged power spectrum of one CPI, for the viewer's
// before-processing spectrogram (docs/ard-stream-design.md 4.4).
//
// The CPI is cut into consecutive segments of bins() samples; every 10th
// is Hann-windowed and FFT'd, and |X|^2 is averaged over those. The
// output is bins() values in dB, fftshifted so bin 0 is -fs/2 and bin
// bins()/2 is the carrier, the same orientation as the range-Doppler map's
// Doppler axis. Plain C++ radix-2, so the host build has it too and the
// GPU is not touched; a CPI of 204800 samples at 512 bins holds 400
// segments, of which 40 are transformed (see kSegmentStride in Spectrum.cpp).
class SpectrumEstimator {
public:
    // uBins must be a power of two, at least 8. May be called again to
    // change the size.
    bool init(uint64_t uBins);
    uint64_t bins() const { return muBins; }

    // pfDbOut must hold bins() floats. uSamples must be at least bins(); a
    // trailing partial segment is ignored. Values are relative: the mean
    // over segments of |X|^2 / bins(), so a level is comparable between
    // CPIs and between the two channels, not to an absolute reference.
    bool compute(const ComplexF* pcSamples, uint64_t uSamples, float* pfDbOut);

private:
    uint64_t              muBins = 0;
    std::vector<float>    mvfWindow;    // Hann, bins() long
    std::vector<ComplexF> mvcTwiddle;   // bins()/2: exp(-j 2 pi k / bins())
    std::vector<ComplexF> mvcWork;      // one segment, windowed then transformed
    std::vector<double>   mvdPower;     // |X|^2 accumulated over the segments
};

// RMS bandwidth of an fftshifted dB spectrum as written by SpectrumEstimator::compute().
// Linear power per bin; the floor is the mean of the quietest tenth of the bins and is
// subtracted and clipped at zero. Returns the one-sided RMS width in Hz about the power
// centroid; *pdCentreHz receives the centroid (signed Hz, 0 = carrier). Returns 0 with
// centre 0 when nothing is above the floor. A flat band of two-sided width W reads
// W / sqrt(12).
double spectrumRmsBandwidthHz(const float* pfDb, uint64_t uBins, double dSampleRateHz,
                              double* pdCentreHz);

} // namespace pbr

#endif // PBR_SPECTRUM_HPP
