#ifndef PBR_ARD_GPU_BATCHES_ARD_GPU_CUH
#define PBR_ARD_GPU_BATCHES_ARD_GPU_CUH

// The batches correlation on the GPU.
//
// Same algorithm as src/ard/cpu/BatchesArdCpu.cpp, which is the correctness
// reference and explains the packing. Private to src/ard/gpu/ -- ArdGpu owns
// one and switches to it when ArdParams::eAlgorithm says Batches. Not in
// include/pbr/ because it takes cufftComplex and nothing outside this stage
// constructs one.
//
// It fills the correlation matrix and stops. The Doppler transform, the
// fftshift, the power map and CFAR stay in ArdGpu and are shared with the
// Direct route, so the two algorithms differ in exactly one stage.
//
// Where the CPU version walks one block at a time, this transforms all of
// them in a single batched cuFFT call: two forward, one inverse, each of
// uBatches transforms of length L.

#include <cufft.h>

#include <cstdint>

namespace pbr {

class BatchesArdGpu {
public:
    BatchesArdGpu() {}
    ~BatchesArdGpu() { release(); }

    // Not copyable: it owns device allocations and a cuFFT plan.
    BatchesArdGpu(const BatchesArdGpu&) = delete;
    BatchesArdGpu& operator=(const BatchesArdGpu&) = delete;

    // uStride is ArdParams::uDopplerDecimation. The caller (ArdGpu) has
    // already checked the parameters. Allocates two uBatches * L scratch
    // buffers and one cuFFT plan, all reused, so correlate() allocates
    // nothing; a failure part-way frees whatever was allocated.
    bool init(uint64_t uCpiSamples, uint64_t uStride, uint64_t uRangeBins);

    // Fill d_cDecimated[r * uBatches + b] for r in [0, uRangeBins), matching
    // the direct lag-product-and-sum. All three pointers are device memory.
    bool correlate(const cufftComplex* d_cRef, const cufftComplex* d_cCleaned,
                   cufftComplex* d_cDecimated);

    uint64_t transformLength() const { return muL; }

private:
    bool setUp(uint64_t uCpiSamples, uint64_t uStride, uint64_t uRangeBins);
    void release();


    uint64_t      muStride     = 0;   // S
    uint64_t      muRangeBins  = 0;   // K
    uint64_t      muL          = 0;   // transform length
    uint64_t      muBatches    = 0;   // uCpiSamples / S

    cufftHandle   mhPlan       = 0;
    bool          mbPlanValid  = false;

    // Allocated in setUp(), freed in release().
    cufftComplex* md_cX        = nullptr;   // [batch][L], surveillance
    cufftComplex* md_cY        = nullptr;   // [batch][L], reference

    bool          mbReady      = false;
};

} // namespace pbr

#endif // PBR_ARD_GPU_BATCHES_ARD_GPU_CUH
