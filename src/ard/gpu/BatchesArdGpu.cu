// The batches correlation on the GPU. See BatchesArdGpu.cuh for what it is,
// src/ard/cpu/BatchesArdCpu.cpp for the reference implementation, and
// ArdAlgorithm::Batches in pbr/ArdProcessor.hpp for why the packing below
// reproduces the direct sum exactly rather than approximating it.
//
// ---------------------------------------------------------------------------
// Three kernels and three library calls
// ---------------------------------------------------------------------------
//
//   1. packKernel        build X and Y for every block at once
//   2. cuFFT (x2)        forward, uBatches transforms of length L
//   3. multiplyConjKern  X = X * conj(Y), pointwise
//   4. cuFFT             inverse, same plan
//   5. extractKernel     take lags 0..K-1, apply 1/L, transpose into
//                        [range][block] for the Doppler transform that follows
//
// Everything stays on the device and nothing synchronises with the host, so
// the whole correlation is one stream of work queued behind the canceller's.

#include "BatchesArdGpu.cuh"

#include "pbr/ArdProcessor.hpp"
#include "pbr/check.hpp"   // must follow the library headers: see check.hpp

#include <cstdio>

namespace pbr {

namespace {

const uint32_t kBlock = 256;

uint32_t blocksFor(uint64_t uThreads, uint32_t uBlockSize)
{
    return static_cast<uint32_t>((uThreads + uBlockSize - 1) / uBlockSize);
}

// ---------------------------------------------------------------------------
// Kernel 1: pack one block of each channel into transform-length arrays.
//
// One thread per (block, position). X holds the block's uStride surveillance
// samples and then zeros -- so only uStride samples contribute and the
// integration length is unchanged. Y holds the block's reference samples at
// the head and the uRangeBins-1 samples from BEFORE the block in the wrapped
// tail, which is where the circular correlation looks for them.
//
// Before the start of the CPI the reference is zero, the same convention the
// direct kernel uses when it skips n < r.
// ---------------------------------------------------------------------------
__global__ void packKernel(const cufftComplex* pcRef,
                           const cufftComplex* pcCleaned,
                           cufftComplex* pcX, cufftComplex* pcY,
                           uint32_t uStride, uint32_t uL, uint32_t uBatches,
                           uint32_t uWrapFirst)
{
    const uint32_t uIdx = blockIdx.x * blockDim.x + threadIdx.x;
    if (uIdx >= uL * uBatches)
        return;

    const uint32_t j  = uIdx % uL;          // position within the transform
    const uint32_t b  = uIdx / uL;          // which block
    const uint32_t n0 = b * uStride;

    const cufftComplex cZero = make_cuFloatComplex(0.0f, 0.0f);

    pcX[uIdx] = (j < uStride) ? pcCleaned[n0 + j] : cZero;

    if (j < uStride) {
        pcY[uIdx] = pcRef[n0 + j];
    } else if (j >= uWrapFirst) {
        // j - L is negative: this is uBack samples before the block.
        const uint32_t uBack = uL - j;
        pcY[uIdx] = (n0 >= uBack) ? pcRef[n0 - uBack] : cZero;
    } else {
        pcY[uIdx] = cZero;
    }
}

// ---------------------------------------------------------------------------
// Kernel 2: X = X * conj(Y), in place.
//
// Correlation in time is a pointwise product with a conjugate in frequency,
// which is the whole reason for the two transforms either side of it.
// ---------------------------------------------------------------------------
__global__ void multiplyConjKernel(cufftComplex* pcX, const cufftComplex* pcY,
                                   uint32_t uCount)
{
    const uint32_t u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= uCount)
        return;

    const cufftComplex cX = pcX[u];
    const cufftComplex cY = pcY[u];
    cufftComplex cOut;
    cOut.x = cX.x * cY.x + cX.y * cY.y;
    cOut.y = cX.y * cY.x - cX.x * cY.y;
    pcX[u] = cOut;
}

// ---------------------------------------------------------------------------
// Kernel 3: take the lags the map covers and transpose.
//
// The transform holds L lags per block; the map wants the first uRangeBins of
// them, laid out [range][block] so each Doppler transform that follows is
// contiguous. cuFFT's inverse is unnormalised, so the 1/L it owes is applied
// here rather than in a pass of its own.
//
// Range is the slow index and block the fast one, so neighbouring threads
// write neighbouring cells of the output.
// ---------------------------------------------------------------------------
__global__ void extractKernel(const cufftComplex* pcX,
                              cufftComplex* pcDecimated,
                              uint32_t uL, uint32_t uBatches,
                              uint32_t uRangeBins, float fInvL)
{
    const uint32_t uIdx = blockIdx.x * blockDim.x + threadIdx.x;
    if (uIdx >= uRangeBins * uBatches)
        return;

    const uint32_t b = uIdx % uBatches;
    const uint32_t r = uIdx / uBatches;

    const cufftComplex c = pcX[b * uL + r];
    cufftComplex cOut;
    cOut.x = c.x * fInvL;
    cOut.y = c.y * fInvL;
    pcDecimated[uIdx] = cOut;
}

} // namespace

// ---------------------------------------------------------------------------

BatchesArdGpu::~BatchesArdGpu()
{
    release();
}

void BatchesArdGpu::release()
{
    if (md_cX)       cudaFree(md_cX);
    if (md_cY)       cudaFree(md_cY);
    if (mbPlanValid) cufftDestroy(mhPlan);

    md_cX       = nullptr;
    md_cY       = nullptr;
    mbPlanValid = false;
    mbReady     = false;
}

bool BatchesArdGpu::init(uint64_t uCpiSamples, uint64_t uStride,
                         uint64_t uRangeBins)
{
    CHECK(!mbReady, "BatchesArdGpu::init called twice");
    CHECK(uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(uStride > 0, "the block stride must be non-zero");
    CHECK(uRangeBins > 0, "range bin count must be non-zero");
    CHECK(uCpiSamples % uStride == 0,
          "the block stride must divide the CPI exactly");

    const uint64_t uL = ardBatchTransformLength(uStride, uRangeBins);
    CHECK(uL > 0, "no usable transform length for this stride and range count");

    muCpiSamples = uCpiSamples;
    muStride     = uStride;
    muRangeBins  = uRangeBins;
    muL          = uL;
    muBatches    = uCpiSamples / uStride;

    const uint64_t uScratch = muBatches * uL;
    CHECK(uScratch <= 2147483647ULL,
          "the batches scratch exceeds what cuFFT accepts as int");
    CHECK(uL <= 2147483647ULL,
          "the batches transform length exceeds what cuFFT accepts as int");

    int iL = static_cast<int>(uL);
    CHECK(cufftPlanMany(&mhPlan, 1, &iL,
                        nullptr, 1, iL,
                        nullptr, 1, iL,
                        CUFFT_C2C,
                        static_cast<int>(muBatches)) == CUFFT_SUCCESS,
          "cufftPlanMany failed for the batches correlation");
    mbPlanValid = true;

    const size_t uBytes = static_cast<size_t>(uScratch) * sizeof(cufftComplex);
    CUDA_CHECK(cudaMalloc(&md_cX, uBytes));
    CUDA_CHECK(cudaMalloc(&md_cY, uBytes));

    mbReady = true;
    return true;
}

bool BatchesArdGpu::correlate(const cufftComplex* pdRef,
                              const cufftComplex* pdCleaned,
                              cufftComplex* pdDecimated)
{
    CHECK(mbReady, "BatchesArdGpu::correlate before a successful init");
    CHECK(pdRef != nullptr && pdCleaned != nullptr && pdDecimated != nullptr,
          "null device pointer passed to BatchesArdGpu::correlate");

    const uint32_t uL       = static_cast<uint32_t>(muL);
    const uint32_t uBatches = static_cast<uint32_t>(muBatches);
    const uint32_t uStride  = static_cast<uint32_t>(muStride);
    const uint32_t uRanges  = static_cast<uint32_t>(muRangeBins);
    const uint64_t uScratch = muBatches * muL;

    // L >= S + K - 1 guarantees this is at least uStride, so the head and the
    // wrapped tail of Y never collide. Checked in init via the length rule.
    const uint32_t uWrapFirst = uL - uRanges + 1;

    packKernel<<<blocksFor(uScratch, kBlock), kBlock>>>(
        pdRef, pdCleaned, md_cX, md_cY, uStride, uL, uBatches, uWrapFirst);
    CUDA_CHECK(cudaGetLastError());

    CHECK(cufftExecC2C(mhPlan, md_cX, md_cX, CUFFT_FORWARD) == CUFFT_SUCCESS,
          "cufftExecC2C failed on the surveillance blocks");
    CHECK(cufftExecC2C(mhPlan, md_cY, md_cY, CUFFT_FORWARD) == CUFFT_SUCCESS,
          "cufftExecC2C failed on the reference blocks");

    multiplyConjKernel<<<blocksFor(uScratch, kBlock), kBlock>>>(
        md_cX, md_cY, static_cast<uint32_t>(uScratch));
    CUDA_CHECK(cudaGetLastError());

    CHECK(cufftExecC2C(mhPlan, md_cX, md_cX, CUFFT_INVERSE) == CUFFT_SUCCESS,
          "cufftExecC2C failed on the correlation");

    extractKernel<<<blocksFor(muRangeBins * muBatches, kBlock), kBlock>>>(
        md_cX, pdDecimated, uL, uBatches, uRanges,
        1.0f / static_cast<float>(muL));
    CUDA_CHECK(cudaGetLastError());

    return true;
}

} // namespace pbr
