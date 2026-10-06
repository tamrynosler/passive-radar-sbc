#ifndef PBR_GPU_COMMON_CUH
#define PBR_GPU_COMMON_CUH

// Helpers every GPU stage uses: the DSI cancellers (src/dsi/gpu/) and the
// range-Doppler stage (src/ard/gpu/).

#include <cstdint>

#include "pbr/check.hpp"   // CUDA_CHECK, and the CUDA library headers

namespace pbr {

// Threads per block for every kernel. blockSum needs a power of two; CGLS's
// adjoint kernel gives one thread to each tap, which caps K at this.
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

} // namespace pbr

#endif // PBR_GPU_COMMON_CUH
