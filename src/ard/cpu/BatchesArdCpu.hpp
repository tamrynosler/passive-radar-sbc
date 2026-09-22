#ifndef PBR_ARD_CPU_BATCHES_ARD_CPU_HPP
#define PBR_ARD_CPU_BATCHES_ARD_CPU_HPP

// The batches correlation on the CPU: the correctness reference for
// src/ard/gpu/BatchesArdGpu.cu.
//
// Private to src/ard/cpu/. It is not in include/pbr/ because nothing outside
// the ARD stage constructs one -- ArdCpu owns it and switches to it when
// ArdParams::eAlgorithm says Batches.
//
// It fills the correlation matrix and stops. The Doppler transform, the
// fftshift, the power map and CFAR all stay in ArdCpu and are shared with the
// Direct route, so the two algorithms differ in exactly one stage, which is
// the stage the thesis is comparing.
//
// See ArdAlgorithm::Batches in pbr/ArdProcessor.hpp for the packing that
// makes this produce the same numbers as the direct sum rather than an
// approximation of them.

#include "pbr/types.hpp"

#include <complex>
#include <cstdint>
#include <vector>

namespace pbr {

class BatchesArdCpu {
public:
    // uStride is ArdParams::uDopplerDecimation and must divide uCpiSamples.
    // Derives the transform length via ardBatchTransformLength() and builds
    // the twiddle and bit-reversal tables for it. Returns false, having said
    // why, on a combination it cannot serve.
    bool init(uint64_t uCpiSamples, uint64_t uStride, uint64_t uRangeBins);

    // Fill pcDecimated[r * uBatches + b] for r in [0, uRangeBins) and b in
    // [0, uBatches), matching the direct lag-product-and-sum exactly.
    // pcRef and pcCleaned each hold uCpiSamples samples.
    void correlate(const ComplexF* pcRef, const ComplexF* pcCleaned,
                   std::complex<float>* pcDecimated);

    uint64_t transformLength() const { return muL; }
    uint64_t batches() const { return muBatches; }

private:
    // In-place radix-2 FFT of muL points. No 1/L on the inverse; correlate()
    // folds it into the extraction.
    //
    // Deliberately not shared with EcaCdCpu's fftT, which stores in double
    // and computes in a template parameter. This one is FP32 end to end, to
    // match what cuFFT does on the device.
    void fft(std::complex<float>* pc, bool bInverse) const;

    uint64_t muCpiSamples = 0;
    uint64_t muStride     = 0;   // S
    uint64_t muRangeBins  = 0;   // K
    uint64_t muL          = 0;   // transform length
    uint64_t muBatches    = 0;   // uCpiSamples / S

    std::vector<std::complex<float> > mvcX;        // one block, surveillance
    std::vector<std::complex<float> > mvcY;        // one block, reference
    std::vector<std::complex<float> > mvcTwiddle;  // L/2: exp(-j 2 pi i / L)
    std::vector<uint32_t>             mvuBitReverse;

    bool mbReady = false;
};

} // namespace pbr

#endif // PBR_ARD_CPU_BATCHES_ARD_CPU_HPP
