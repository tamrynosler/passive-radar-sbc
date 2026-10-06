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
//   3. multiplyConjKernel  X = X * conj(Y), pointwise
//   4. cuFFT             inverse, same plan
//   5. extractKernel     take lags 0..K-1, apply 1/L, transpose into
//                        [range][block] for the Doppler transform that follows
//
// Everything stays on the device and nothing synchronises with the host, so
// the whole correlation is one stream of work queued behind the canceller's.

#include "BatchesArdGpu.cuh"

#include "pbr/ArdProcessor.hpp"
#include "../../gpu/GpuCommon.cuh"

#include <climits>

namespace pbr {

namespace {

// ---------------------------------------------------------------------------
// Pack one block of each channel into transform-length arrays.
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
__global__ void packKernel(const cufftComplex* d_cRef,
                           const cufftComplex* d_cCleaned,
                           cufftComplex* d_cX, cufftComplex* d_cY,
                           uint32_t uStride, uint32_t uL, uint32_t uBatches,
                           uint32_t uWrapFirst)
{
    const uint32_t uIdx = blockIdx.x * blockDim.x + threadIdx.x;
    if (uIdx >= uL * uBatches)
        return;

    const uint32_t j           = uIdx % uL;   // position within the transform
    const uint32_t uBatch      = uIdx / uL;   // which block
    const uint32_t uBlockStart = uBatch * uStride;

    const cufftComplex cZero = make_cuFloatComplex(0.0f, 0.0f);

    d_cX[uIdx] = (j < uStride) ? d_cCleaned[uBlockStart + j] : cZero;

    if (j < uStride) {
        d_cY[uIdx] = d_cRef[uBlockStart + j];
    } else if (j >= uWrapFirst) {
        // j - L is negative: this is uBack samples before the block.
        const uint32_t uBack = uL - j;
        d_cY[uIdx] = (uBlockStart >= uBack) ? d_cRef[uBlockStart - uBack] : cZero;
    } else {
        d_cY[uIdx] = cZero;
    }
}

// ---------------------------------------------------------------------------
// X = X * conj(Y), in place.
//
// Correlation in time is a pointwise product with a conjugate in frequency,
// which is the whole reason for the two transforms either side of it.
// ---------------------------------------------------------------------------
__global__ void multiplyConjKernel(cufftComplex* d_cX, const cufftComplex* d_cY,
                                   uint32_t uCount)
{
    const uint32_t u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= uCount)
        return;

    const cufftComplex cX = d_cX[u];
    const cufftComplex cY = d_cY[u];
    cufftComplex cOut;
    cOut.x = cX.x * cY.x + cX.y * cY.y;
    cOut.y = cX.y * cY.x - cX.x * cY.y;
    d_cX[u] = cOut;
}

// ---------------------------------------------------------------------------
// Take the lags the map covers and transpose.
//
// The transform holds L lags per block; the map wants the first uRangeBins of
// them, laid out [range][block] so each Doppler transform that follows is
// contiguous. cuFFT's inverse is unnormalised, so the 1/L it owes is applied
// here rather than in a pass of its own.
//
// Range is the slow index and block the fast one, so neighbouring threads
// write neighbouring cells of the output.
// ---------------------------------------------------------------------------
__global__ void extractKernel(const cufftComplex* d_cX,
                              cufftComplex* d_cDecimated,
                              uint32_t uL, uint32_t uBatches,
                              uint32_t uRangeBins, float fInvL)
{
    const uint32_t uIdx = blockIdx.x * blockDim.x + threadIdx.x;
    if (uIdx >= uRangeBins * uBatches)
        return;

    const uint32_t uBatch = uIdx % uBatches;
    const uint32_t r      = uIdx / uBatches;

    const cufftComplex cLag = d_cX[uBatch * uL + r];
    cufftComplex cOut;
    cOut.x = cLag.x * fInvL;
    cOut.y = cLag.y * fInvL;
    d_cDecimated[uIdx] = cOut;
}

} // namespace

// ---------------------------------------------------------------------------

void BatchesArdGpu::release()
{
    // Deliberately unchecked: nothing can be done about a failed free.
    // cudaFree(nullptr) is a no-op.
    cudaFree(md_cX);
    cudaFree(md_cY);
    if (mbPlanValid)
        cufftDestroy(mhPlan);

    md_cX       = nullptr;
    md_cY       = nullptr;
    mbPlanValid = false;
    mbReady     = false;
}

bool BatchesArdGpu::init(uint64_t uCpiSamples, uint64_t uStride, uint64_t uRangeBins)
{
    CHECK(!mbReady, "BatchesArdGpu::init called twice");
    if (setUp(uCpiSamples, uStride, uRangeBins))
        return true;
    release();   // whatever was allocated before the failure
    return false;
}

bool BatchesArdGpu::setUp(uint64_t uCpiSamples, uint64_t uStride, uint64_t uRangeBins)
{
    const uint64_t uL = ardBatchTransformLength(uStride, uRangeBins);
    CHECK(uL > 0, "no usable transform length for this stride and range count");

    muStride     = uStride;
    muRangeBins  = uRangeBins;
    muL          = uL;
    muBatches    = uCpiSamples / uStride;

    // cuFFT takes int, and the kernels index the scratch with uint32_t.
    const uint64_t uScratch = muBatches * uL;
    CHECK(uScratch <= INT_MAX, "the batches scratch is larger than cuFFT and the kernels can index");

    int iL = static_cast<int>(uL);
    CUDA_CHECK(cufftPlanMany(&mhPlan, 1, &iL,
                             nullptr, 1, iL,
                             nullptr, 1, iL,
                             CUFFT_C2C,
                             static_cast<int>(muBatches)));
    mbPlanValid = true;

    const size_t uBytes = static_cast<size_t>(uScratch) * sizeof(cufftComplex);
    CUDA_CHECK(cudaMalloc(&md_cX, uBytes));
    CUDA_CHECK(cudaMalloc(&md_cY, uBytes));

    mbReady = true;
    return true;
}

bool BatchesArdGpu::correlate(const cufftComplex* d_cRef,
                              const cufftComplex* d_cCleaned,
                              cufftComplex* d_cDecimated)
{
    CHECK(mbReady, "BatchesArdGpu::correlate before a successful init");
    CHECK(d_cRef != nullptr && d_cCleaned != nullptr && d_cDecimated != nullptr,
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
        d_cRef, d_cCleaned, md_cX, md_cY, uStride, uL, uBatches, uWrapFirst);
    CUDA_CHECK(cudaGetLastError());

    CUDA_CHECK(cufftExecC2C(mhPlan, md_cX, md_cX, CUFFT_FORWARD));
    CUDA_CHECK(cufftExecC2C(mhPlan, md_cY, md_cY, CUFFT_FORWARD));

    multiplyConjKernel<<<blocksFor(uScratch, kBlock), kBlock>>>(
        md_cX, md_cY, static_cast<uint32_t>(uScratch));
    CUDA_CHECK(cudaGetLastError());

    CUDA_CHECK(cufftExecC2C(mhPlan, md_cX, md_cX, CUFFT_INVERSE));

    extractKernel<<<blocksFor(muRangeBins * muBatches, kBlock), kBlock>>>(
        md_cX, d_cDecimated, uL, uBatches, uRanges,
        1.0f / static_cast<float>(muL));
    CUDA_CHECK(cudaGetLastError());

    return true;
}

} // namespace pbr
