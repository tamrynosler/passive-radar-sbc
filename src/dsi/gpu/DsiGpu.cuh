#ifndef PBR_DSI_GPU_DSI_GPU_CUH
#define PBR_DSI_GPU_DSI_GPU_CUH

// Device code shared by the GPU cancellers in src/dsi/gpu/. The helpers every
// GPU stage uses (kBlock, blocksFor, blockSum) are in src/gpu/GpuCommon.cuh.
//
// Samples on the device are cufftComplex. cuComplex, which cuBLAS and
// cuSOLVER take, is the same type (cufft.h typedefs one to the other), so a
// buffer moves between the libraries and the CAF stage with no conversion.

#include <cmath>
#include <cstdint>

#include "../../gpu/GpuCommon.cuh"

namespace pbr {

// out[n] = base[n] - sum over k of w[k] r[n-k], or just the sum when d_cBase
// is null. With the surveillance as base it is the clutter subtraction (ECA's
// output, CGLS's residual b - R x and output); without, CGLS's q = R p. Each
// window of uWindow samples uses its own uTaps weights, and the reference is
// zero before the start of the CPI. Written directly rather than as a matrix
// product, so the delayed-reference matrix R never has to exist for it.
//
// Tiled: a block computes kBlock consecutive outputs of ONE window (the grid
// is x = tiles per window, y = windows, so a block never crosses a window
// boundary). It first loads the window's weights and the kBlock + K - 1
// reference samples those outputs need into shared memory with coalesced
// reads, then every thread sums its taps from shared memory. Without this
// each of the K taps of each output was a separate global read. The tap order
// is unchanged. The reference tile is zero before the start of the CPI, which
// gives the "nothing reaches back before the CPI" rule for free.
// Dynamic shared memory: (kBlock + 2 K - 1) cufftComplex.
static __global__ void convolveKernel(const cufftComplex* d_cRef,
                                      const cufftComplex* d_cWeights,
                                      const cufftComplex* d_cBase,
                                      cufftComplex* d_cOut,
                                      uint32_t uTotal, uint32_t uTaps, uint32_t uWindow)
{
    extern __shared__ cufftComplex acShared[];
    cufftComplex* acW   = acShared;                 // uTaps weights
    cufftComplex* acRef = acShared + uTaps;         // kBlock + uTaps - 1 samples

    const uint32_t uTileStart = blockIdx.x * kBlock;            // inside the window
    const uint32_t uN0        = blockIdx.y * uWindow + uTileStart;   // first output, in the CPI
    const cufftComplex* d_cW  = d_cWeights + static_cast<size_t>(blockIdx.y) * uTaps;

    for (uint32_t k = threadIdx.x; k < uTaps; k += kBlock)
        acW[k] = d_cW[k];

    // acRef[j] = ref[uN0 - (uTaps - 1) + j], zero outside the CPI.
    const uint32_t uRefCount = kBlock + uTaps - 1;
    for (uint32_t j = threadIdx.x; j < uRefCount; j += kBlock) {
        const int64_t iIdx = static_cast<int64_t>(uN0) - (uTaps - 1) + j;
        cufftComplex cZero;
        cZero.x = 0.0f;
        cZero.y = 0.0f;
        acRef[j] = (iIdx >= 0 && iIdx < static_cast<int64_t>(uTotal)) ? d_cRef[iIdx] : cZero;
    }
    __syncthreads();

    const uint32_t n = uN0 + threadIdx.x;
    if (uTileStart + threadIdx.x >= uWindow || n >= uTotal)
        return;

    float fRe = 0.0f;
    float fIm = 0.0f;
    for (uint32_t k = 0; k < uTaps; ++k) {
        const cufftComplex cW = acW[k];
        const cufftComplex cR = acRef[threadIdx.x + uTaps - 1 - k];
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
    CHECK(uTaps >= 1 && uTaps <= 1024, "convolve: the tap count is outside 1..1024");
    CHECK(uWindow >= 1, "convolve: empty window");
    const dim3 oGrid(blocksFor(uWindow, kBlock), blocksFor(uTotal, static_cast<uint32_t>(uWindow)));
    const size_t uShared = (kBlock + 2 * uTaps - 1) * sizeof(cufftComplex);
    convolveKernel<<<oGrid, kBlock, uShared>>>(
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

#endif // PBR_DSI_GPU_DSI_GPU_CUH
