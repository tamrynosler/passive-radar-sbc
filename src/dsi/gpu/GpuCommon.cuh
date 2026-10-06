#ifndef PBR_DSI_GPU_COMMON_CUH
#define PBR_DSI_GPU_COMMON_CUH

// Device code shared by the GPU cancellers in src/dsi/gpu/.
//
// Samples on the device are cufftComplex. cuComplex, which cuBLAS and
// cuSOLVER take, is the same type (cufft.h typedefs one to the other), so a
// buffer moves between the libraries and the CAF stage with no conversion.

#include <cmath>
#include <cstdint>

#include "pbr/check.hpp"   // CUDA_CHECK, and the CUDA library headers

namespace pbr {

// Threads per block for every kernel here. blockSum needs a power of two;
// CGLS's adjoint kernel gives one thread to each tap, which caps K at this.
const uint32_t kBlock = 256;

inline uint32_t blocksFor(uint64_t uThreads, uint32_t uBlockSize)
{
    return static_cast<uint32_t>((uThreads + uBlockSize - 1) / uBlockSize);
}

// Sum fValue over the block. Every thread gets the total. pfShared must hold
// kBlock floats; it is free again on return. The block must be launched with
// kBlock threads.
inline __device__ float blockSum(float fValue, float* pfShared)
{
    pfShared[threadIdx.x] = fValue;
    __syncthreads();
    for (uint32_t uStride = kBlock / 2; uStride > 0; uStride >>= 1) {
        if (threadIdx.x < uStride)
            pfShared[threadIdx.x] += pfShared[threadIdx.x + uStride];
        __syncthreads();
    }
    const float fTotal = pfShared[0];
    __syncthreads();
    return fTotal;
}

// out[n] = base[n] - sum over k of w[k] r[n-k], or just the sum when d_cBase
// is null. With the surveillance as base it is the clutter subtraction (ECA's
// output, CGLS's residual b - R x and output); without, CGLS's q = R p. Each
// window of uWindow samples uses its own uTaps weights, and the reference is
// zero before the start of the CPI. One thread per sample, every window in
// one launch. Written directly rather than as a matrix product, so the
// delayed-reference matrix R never has to exist for it.
static __global__ void convolveKernel(const cufftComplex* d_cRef,
                                      const cufftComplex* d_cWeights,
                                      const cufftComplex* d_cBase,
                                      cufftComplex* d_cOut,
                                      uint32_t uTotal, uint32_t uTaps, uint32_t uWindow)
{
    const uint32_t n = blockIdx.x * blockDim.x + threadIdx.x;
    if (n >= uTotal)
        return;

    const cufftComplex* d_cW = d_cWeights + static_cast<size_t>(n / uWindow) * uTaps;

    // Taps that would reach back before the start of the CPI contribute
    // nothing, because the reference is zero there.
    const uint32_t uCount = (n + 1 < uTaps) ? (n + 1) : uTaps;

    float fRe = 0.0f;
    float fIm = 0.0f;
    for (uint32_t k = 0; k < uCount; ++k) {
        const cufftComplex cW = d_cW[k];
        const cufftComplex cR = d_cRef[n - k];
        fRe += cW.x * cR.x - cW.y * cR.y;
        fIm += cW.x * cR.y + cW.y * cR.x;
    }

    cufftComplex cOut;
    if (d_cBase != nullptr) {
        cOut.x = d_cBase[n].x - fRe;
        cOut.y = d_cBase[n].y - fIm;
    } else {
        cOut.x = fRe;
        cOut.y = fIm;
    }
    d_cOut[n] = cOut;
}

// Launch convolveKernel over a whole CPI of uTotal samples.
inline bool launchConvolve(const cufftComplex* d_cRef, const cufftComplex* d_cWeights,
                           const cufftComplex* d_cBase, cufftComplex* d_cOut,
                           uint64_t uTotal, uint64_t uTaps, uint64_t uWindow)
{
    convolveKernel<<<blocksFor(uTotal, kBlock), kBlock>>>(
        d_cRef, d_cWeights, d_cBase, d_cOut, static_cast<uint32_t>(uTotal),
        static_cast<uint32_t>(uTaps), static_cast<uint32_t>(uWindow));
    CUDA_CHECK(cudaGetLastError());
    return true;
}

// 20 log10(|sur| / |out|) over uSamples, from two cuBLAS norms, so the host
// does no arithmetic over the CPI. cublasScnrm2 returns its result to the
// host, so it has waited for every kernel queued before it: any kernel error
// has surfaced by the time this returns, and is checked here.
inline bool gpuSuppressionDb(cublasHandle_t hCublas, const cufftComplex* d_cSur,
                             const cufftComplex* d_cOut, uint64_t uSamples,
                             double& dSuppressionDbOut)
{
    float fSurNorm = 0.0f;
    float fOutNorm = 0.0f;
    CUDA_CHECK(cublasScnrm2(hCublas, static_cast<int>(uSamples), d_cSur, 1, &fSurNorm));
    CUDA_CHECK(cublasScnrm2(hCublas, static_cast<int>(uSamples), d_cOut, 1, &fOutNorm));
    CUDA_CHECK(cudaGetLastError());

    dSuppressionDbOut = (fSurNorm > 0.0f && fOutNorm > 0.0f)
        ? 20.0 * std::log10(static_cast<double>(fSurNorm) / static_cast<double>(fOutNorm))
        : 0.0;
    return true;
}

} // namespace pbr

#endif // PBR_DSI_GPU_COMMON_CUH
