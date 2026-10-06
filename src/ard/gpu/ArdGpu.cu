// Range-Doppler map formation and CFAR detection on the GPU.
//
// Same algorithm as src/ard/cpu/ArdCpu.cpp, which is the correctness
// reference and explains what the stage does. This file explains only what is
// different because it runs on a GPU.
//
// ---------------------------------------------------------------------------
// Four kernels and two library calls
// ---------------------------------------------------------------------------
//
//   1. lagProductKernel   lag product and integrate-and-dump, in one pass
//                         (or BatchesArdGpu, the other route to the same sums)
//   2. cuFFT              uRangeBins batched transforms of length uDopplerBins
//   3. shiftKernel        power, fftshift, and the transpose into display
//                         layout -- all in the pass that has to read and write
//                         anyway, so the reordering is free
//   4. cuBLAS isamax      the map's peak, for the dB scale
//   5. cfarKernel         one thread per cell, direct window sum
//
// powerKernel replaces 1-3 when detect() is handed a finished map.
//
// ---------------------------------------------------------------------------
// Staying on the device
// ---------------------------------------------------------------------------
//
// processDevice() takes pointers that a GPU canceller already holds, so a
// chained run copies nothing between the two stages. That relies on both
// stages queueing on the default stream, so the map's kernels run after the
// canceller's. process() is the same work with an upload in front and a
// download behind, for standalone use and for validation against MATLAB.
//
// ---------------------------------------------------------------------------
// Precision
// ---------------------------------------------------------------------------
//
// FP32 throughout, as forced by the hardware (Maxwell runs FP64 at 1/32 the
// rate), and so is the CPU reference, so the two differ only in summation
// order: cuFFT against a direct DFT. See docs/design_decisions.md.

#include "BatchesArdGpu.cuh"

#include "pbr/ArdProcessor.hpp"
#include "../ArdInternal.hpp"
#include "../../gpu/GpuCommon.cuh"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pbr {

namespace {

// The CFAR kernel's three counters, one uint32_t each in md_uCounters.
const uint32_t kCounterFound   = 0;   // cells over threshold, written or not
const uint32_t kCounterDropped = 1;   // over threshold but past the capacity
const uint32_t kCounterBorder  = 2;   // written detections in the border
const uint32_t kCounters       = 3;

// ---------------------------------------------------------------------------
// Lag product, integrated and dumped.
//
// One thread per output cell. Each sums uDec products of
// cleaned[n] * conj(ref[n - r]) over one block.
//
// Range is the fastest-varying thread index on purpose: neighbouring threads
// then read neighbouring reference samples, and all of them share the same
// surveillance sample.
//
// Output is in FFT-natural layout, [range][block], because that is what a
// batched cuFFT expects -- each transform contiguous. The transpose into
// display layout happens later, for free.
// ---------------------------------------------------------------------------
__global__ void lagProductKernel(const cufftComplex* d_cRef,
                                 const cufftComplex* d_cCleaned,
                                 cufftComplex* d_cDecimated,
                                 uint32_t uRanges, uint32_t uBlocks,
                                 uint32_t uDec)
{
    const uint32_t uIdx = blockIdx.x * blockDim.x + threadIdx.x;
    if (uIdx >= uRanges * uBlocks)
        return;

    const uint32_t r      = uIdx % uRanges;   // delay bin
    const uint32_t uBlock = uIdx / uRanges;   // block along the CPI

    const uint32_t uFirst = uBlock * uDec;
    float fRe = 0.0f;
    float fIm = 0.0f;

    for (uint32_t i = 0; i < uDec; ++i) {
        const uint32_t n = uFirst + i;
        // The reference is zero before the start of the CPI.
        if (n < r)
            continue;

        const cufftComplex cS = d_cCleaned[n];
        const cufftComplex cR = d_cRef[n - r];
        // cleaned * conj(delayed reference)
        fRe += cS.x * cR.x + cS.y * cR.y;
        fIm += cS.y * cR.x - cS.x * cR.y;
    }

    cufftComplex cOut;
    cOut.x = fRe;
    cOut.y = fIm;
    d_cDecimated[r * uBlocks + uBlock] = cOut;
}

// ---------------------------------------------------------------------------
// Power, fftshift and transpose, in one pass.
//
// Reads the transform output at [range][k] and writes:
//   * the complex map at [doppler][range], display layout
//   * the power map at the same index, for the detector
//
// The fftshift is just where it writes: transform bin k belongs at display
// row (k + N/2) mod N, which puts zero Doppler in the middle.
// ---------------------------------------------------------------------------
__global__ void shiftKernel(const cufftComplex* d_cTransformed,
                            cufftComplex* d_cMap, float* d_fPower,
                            uint32_t uRanges, uint32_t uBlocks)
{
    const uint32_t uIdx = blockIdx.x * blockDim.x + threadIdx.x;
    if (uIdx >= uRanges * uBlocks)
        return;

    const uint32_t r = uIdx / uBlocks;
    const uint32_t k = uIdx % uBlocks;

    const cufftComplex cCell = d_cTransformed[uIdx];
    const uint32_t uRow = (k + uBlocks / 2) % uBlocks;
    const uint32_t uDst = uRow * uRanges + r;

    d_cMap[uDst]   = cCell;
    d_fPower[uDst] = cCell.x * cCell.x + cCell.y * cCell.y;
}

// ---------------------------------------------------------------------------
// Power only, for a map supplied by the caller.
//
// detect() is handed a map that is already in display layout, so it needs the
// squaring but not the shift or the transpose that shiftKernel also does.
// ---------------------------------------------------------------------------
__global__ void powerKernel(const cufftComplex* d_cMap, float* d_fPower,
                            uint32_t uCells)
{
    const uint32_t u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= uCells)
        return;
    const cufftComplex cCell = d_cMap[u];
    d_fPower[u] = cCell.x * cCell.x + cCell.y * cCell.y;
}

// ---------------------------------------------------------------------------
// CFAR.
//
// One thread per cell, walking its own window. Direct and obvious; a
// summed-area table would cut the reads to a handful per cell, and is
// deliberately not done -- correct first.
//
// Cells whose window runs off the map contribute zero and the divisor stays
// the full training count, matching MATLAB's conv2(..., 'same'). Border
// detections are counted separately so the resulting over-detection is
// visible rather than silent.
//
// Detections are appended through an atomic counter, so they land in an
// order that changes from run to run; the host sorts them afterwards.
// ---------------------------------------------------------------------------
__global__ void cfarKernel(const float* d_fPower, uint32_t uRanges,
                           uint32_t uDopplers,
                           int32_t iGuardR, int32_t iGuardD,
                           int32_t iHalfR,  int32_t iHalfD,
                           float fAlphaOverTrain,
                           float fPeakPower,
                           Detection* d_oDetections, uint32_t uMaxDetections,
                           uint32_t* d_uCounters,
                           uint64_t uSampleRateHz, uint32_t uDec)
{
    const uint32_t uIdx = blockIdx.x * blockDim.x + threadIdx.x;
    if (uIdx >= uRanges * uDopplers)
        return;

    const int32_t iRange   = static_cast<int32_t>(uIdx % uRanges);
    const int32_t iDoppler = static_cast<int32_t>(uIdx / uRanges);

    const float fCell = d_fPower[uIdx];

    float fSum = 0.0f;
    for (int32_t iOffD = -iHalfD; iOffD <= iHalfD; ++iOffD) {
        const int32_t iD = iDoppler + iOffD;
        if (iD < 0 || iD >= static_cast<int32_t>(uDopplers))
            continue;
        const bool bGuardD = (iOffD >= -iGuardD && iOffD <= iGuardD);

        for (int32_t iOffR = -iHalfR; iOffR <= iHalfR; ++iOffR) {
            if (bGuardD && iOffR >= -iGuardR && iOffR <= iGuardR)
                continue;   // guard block, including the cell under test
            const int32_t iR = iRange + iOffR;
            if (iR < 0 || iR >= static_cast<int32_t>(uRanges))
                continue;
            fSum += d_fPower[static_cast<uint32_t>(iD) * uRanges +
                             static_cast<uint32_t>(iR)];
        }
    }

    const float fThreshold = fAlphaOverTrain * fSum;
    if (!(fCell > fThreshold))
        return;

    const uint32_t uSlot = atomicAdd(&d_uCounters[kCounterFound], 1u);
    if (uSlot >= uMaxDetections) {
        atomicAdd(&d_uCounters[kCounterDropped], 1u);
        return;
    }

    // The same arithmetic as ardRangeKm() and ardDopplerHz() on the host.
    Detection oDet;
    oDet.uRangeBin   = static_cast<uint64_t>(iRange);
    oDet.uDopplerBin = static_cast<uint64_t>(iDoppler);
    oDet.dRangeKm    = double(iRange) * kSpeedOfLight / double(uSampleRateHz) / 1000.0;
    {
        const double dBinHz = double(uSampleRateHz) /
                              (double(uDec) * double(uDopplers));
        oDet.dDopplerHz = (double(iDoppler) - double(uDopplers) / 2.0) * dBinHz;
    }
    oDet.dPowerDb = (fPeakPower > 0.0f)
                  ? 10.0 * log10(double(fCell) / double(fPeakPower)) : 0.0;
    oDet.dSnrDb   = (fThreshold > 0.0f)
                  ? 10.0 * log10(double(fCell) / double(fThreshold)) : 0.0;
    d_oDetections[uSlot] = oDet;

    if (iRange < iHalfR || iRange >= static_cast<int32_t>(uRanges) - iHalfR ||
        iDoppler < iHalfD || iDoppler >= static_cast<int32_t>(uDopplers) - iHalfD) {
        atomicAdd(&d_uCounters[kCounterBorder], 1u);
    }
}

// Raster order, Doppler bin then range bin: the order the CPU reports in.
bool rasterBefore(const Detection& oA, const Detection& oB)
{
    if (oA.uDopplerBin != oB.uDopplerBin)
        return oA.uDopplerBin < oB.uDopplerBin;
    return oA.uRangeBin < oB.uRangeBin;
}

// ---------------------------------------------------------------------------

class ArdGpu : public ArdProcessor {
public:
    ~ArdGpu() override { release(); }

    bool init(const ArdParams& oParams) override;

    bool process(const ComplexF* pcRef, const ComplexF* pcCleaned,
                 ComplexF* pcMap,
                 Detection* pDetections, uint64_t uMaxDetections,
                 ArdStats& oStats) override;

    bool processDevice(const void* d_pRef, const void* d_pCleaned,
                       ComplexF* pcMap,
                       Detection* pDetections, uint64_t uMaxDetections,
                       ArdStats& oStats) override;

    bool detect(const ComplexF* pcMap,
                Detection* pDetections, uint64_t uMaxDetections,
                ArdStats& oStats) override;

    const char* name() const override { return machName; }

private:
    bool setUp(const ArdParams& oParams);   // init() without the clean-up
    void release();

    // Everything from the lag products to the power map.
    bool buildMap(const cufftComplex* d_cRef, const cufftComplex* d_cCleaned);

    // CFAR on whatever is currently in md_fPower, plus the copies back out.
    bool runCfar(Detection* pDetections, uint64_t uMaxDetections,
                 ArdStats& oStats);

    ArdParams      moParams;
    uint64_t       muCells          = 0;
    float          mfAlphaOverTrain = 0.0f;   // from checkArdParams()

    cublasHandle_t mhCublas         = nullptr;
    cufftHandle    mhPlan           = 0;
    bool           mbPlanValid      = false;

    // All allocated in setUp(), freed in release(). The pointers given to
    // processDevice() are borrowed, never owned.
    cufftComplex*  md_cRef          = nullptr;   // uploaded samples (process())
    cufftComplex*  md_cCleaned      = nullptr;
    cufftComplex*  md_cDecimated    = nullptr;   // [range][block]
    cufftComplex*  md_cMap          = nullptr;   // [doppler][range]
    float*         md_fPower        = nullptr;
    Detection*     md_oDetections   = nullptr;   // kMaxArdDetections
    uint32_t*      md_uCounters     = nullptr;   // kCounters

    // Only initialised when eAlgorithm is Batches. Holds its own cuFFT plan
    // and scratch; the Direct path never touches it.
    BatchesArdGpu  moBatches;

    char           machName[48]     = {};
    bool           mbReady          = false;
};

void ArdGpu::release()
{
    // Deliberately unchecked: nothing can be done about a failed free.
    // cudaFree(nullptr) is a no-op.
    cudaFree(md_cRef);
    cudaFree(md_cCleaned);
    cudaFree(md_cDecimated);
    cudaFree(md_cMap);
    cudaFree(md_fPower);
    cudaFree(md_oDetections);
    cudaFree(md_uCounters);
    if (mbPlanValid)
        cufftDestroy(mhPlan);
    if (mhCublas)
        cublasDestroy(mhCublas);

    md_cRef = md_cCleaned = md_cDecimated = md_cMap = nullptr;
    md_fPower      = nullptr;
    md_oDetections = nullptr;
    md_uCounters   = nullptr;
    mhCublas       = nullptr;
    mbPlanValid    = false;
    mbReady        = false;
}

bool ArdGpu::init(const ArdParams& oParams)
{
    CHECK(!mbReady, "ArdGpu::init called twice");
    if (setUp(oParams))
        return true;
    release();   // whatever was allocated before the failure
    return false;
}

bool ArdGpu::setUp(const ArdParams& oParams)
{
    if (!checkArdParams(oParams, mfAlphaOverTrain))
        return false;

    moParams = oParams;
    muCells  = oParams.uRangeBins * oParams.uDopplerBins;

    CUDA_CHECK(cublasCreate(&mhCublas));

    // uRangeBins independent transforms of length uDopplerBins, each contiguous.
    int iN = static_cast<int>(oParams.uDopplerBins);
    CUDA_CHECK(cufftPlanMany(&mhPlan, 1, &iN,
                             nullptr, 1, iN,
                             nullptr, 1, iN,
                             CUFFT_C2C,
                             static_cast<int>(oParams.uRangeBins)));
    mbPlanValid = true;

    const size_t uCpiBytes  = static_cast<size_t>(oParams.uCpiSamples) * sizeof(cufftComplex);
    const size_t uMapBytes  = static_cast<size_t>(muCells) * sizeof(cufftComplex);
    const size_t uPowBytes  = static_cast<size_t>(muCells) * sizeof(float);
    const size_t uDetBytes  = static_cast<size_t>(kMaxArdDetections) * sizeof(Detection);

    CUDA_CHECK(cudaMalloc(&md_cRef,        uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cCleaned,    uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cDecimated,  uMapBytes));
    CUDA_CHECK(cudaMalloc(&md_cMap,        uMapBytes));
    CUDA_CHECK(cudaMalloc(&md_fPower,      uPowBytes));
    CUDA_CHECK(cudaMalloc(&md_oDetections, uDetBytes));
    CUDA_CHECK(cudaMalloc(&md_uCounters,   kCounters * sizeof(uint32_t)));

    if (oParams.eAlgorithm == ArdAlgorithm::Batches) {
        if (!moBatches.init(oParams.uCpiSamples, oParams.uDopplerDecimation,
                            oParams.uRangeBins))
            return false;
        std::snprintf(machName, sizeof(machName), "ARD-Batches (GPU, L=%llu)",
                      static_cast<unsigned long long>(moBatches.transformLength()));
    } else {
        std::snprintf(machName, sizeof(machName), "ARD-Direct (GPU)");
    }

    mbReady = true;
    return true;
}

bool ArdGpu::buildMap(const cufftComplex* d_cRef, const cufftComplex* d_cCleaned)
{
    const uint32_t uRanges = static_cast<uint32_t>(moParams.uRangeBins);
    const uint32_t uBlocks = static_cast<uint32_t>(moParams.uDopplerBins);

    // Two routes to the same correlation matrix; see ArdAlgorithm in
    // ArdProcessor.hpp. Everything after this point is shared.
    if (moParams.eAlgorithm == ArdAlgorithm::Batches) {
        if (!moBatches.correlate(d_cRef, d_cCleaned, md_cDecimated))
            return false;
    } else {
        lagProductKernel<<<blocksFor(muCells, kBlock), kBlock>>>(
            d_cRef, d_cCleaned, md_cDecimated, uRanges, uBlocks,
            static_cast<uint32_t>(moParams.uDopplerDecimation));
        CUDA_CHECK(cudaGetLastError());
    }

    // In place: the transform overwrites the decimated lag products, which are
    // not needed again.
    CUDA_CHECK(cufftExecC2C(mhPlan, md_cDecimated, md_cDecimated, CUFFT_FORWARD));

    shiftKernel<<<blocksFor(muCells, kBlock), kBlock>>>(
        md_cDecimated, md_cMap, md_fPower, uRanges, uBlocks);
    CUDA_CHECK(cudaGetLastError());

    return true;
}

bool ArdGpu::runCfar(Detection* pDetections, uint64_t uMaxDetections,
                     ArdStats& oStats)
{
    // The peak, for the dB scale. cublasIsamax finds the largest magnitude;
    // on a non-negative power map that is the peak power. It returns to the
    // host, so a fault in an earlier kernel surfaces here.
    int iPeakIndex = 0;
    CUDA_CHECK(cublasIsamax(mhCublas, static_cast<int>(muCells),
                            md_fPower, 1, &iPeakIndex));
    float fPeakPower = 0.0f;
    CUDA_CHECK(cudaMemcpy(&fPeakPower, md_fPower + (iPeakIndex - 1),
                          sizeof(float), cudaMemcpyDeviceToHost));
    oStats.dPeakDb = (fPeakPower > 0.0f)
                   ? 10.0 * std::log10(static_cast<double>(fPeakPower)) : 0.0;

    CUDA_CHECK(cudaMemset(md_uCounters, 0, kCounters * sizeof(uint32_t)));

    const uint32_t uCapacity = static_cast<uint32_t>(
        (uMaxDetections < kMaxArdDetections) ? uMaxDetections : kMaxArdDetections);

    cfarKernel<<<blocksFor(muCells, kBlock), kBlock>>>(
        md_fPower,
        static_cast<uint32_t>(moParams.uRangeBins),
        static_cast<uint32_t>(moParams.uDopplerBins),
        static_cast<int32_t>(moParams.uCfarGuardRange),
        static_cast<int32_t>(moParams.uCfarGuardDoppler),
        static_cast<int32_t>(moParams.uCfarGuardRange + moParams.uCfarTrainRange),
        static_cast<int32_t>(moParams.uCfarGuardDoppler + moParams.uCfarTrainDoppler),
        mfAlphaOverTrain,
        fPeakPower,
        md_oDetections, uCapacity,
        md_uCounters,
        moParams.uSampleRateHz,
        static_cast<uint32_t>(moParams.uDopplerDecimation));
    CUDA_CHECK(cudaGetLastError());

    uint32_t auCounters[kCounters] = { 0, 0, 0 };
    CUDA_CHECK(cudaMemcpy(auCounters, md_uCounters, sizeof(auCounters),
                          cudaMemcpyDeviceToHost));

    // The found counter runs past the capacity before a slot is rejected, so
    // clamp it to what was actually written.
    const uint32_t uWritten = (auCounters[kCounterFound] < uCapacity)
                            ? auCounters[kCounterFound] : uCapacity;
    oStats.uDetections         = uWritten;
    oStats.uDetectionsDropped  = auCounters[kCounterDropped];
    oStats.uDetectionsInBorder = auCounters[kCounterBorder];

    if (uWritten > 0 && pDetections != nullptr) {
        CUDA_CHECK(cudaMemcpy(pDetections, md_oDetections,
                              static_cast<size_t>(uWritten) * sizeof(Detection),
                              cudaMemcpyDeviceToHost));
        // The kernel appends in whatever order its threads finish; put the
        // list in the CPU's raster order so every run reports it the same way.
        std::sort(pDetections, pDetections + uWritten, rasterBefore);
    }

    return true;
}

bool ArdGpu::processDevice(const void* d_pRef, const void* d_pCleaned,
                           ComplexF* pcMap,
                           Detection* pDetections, uint64_t uMaxDetections,
                           ArdStats& oStats)
{
    CHECK(mbReady, "ArdGpu::processDevice before a successful init");
    CHECK(d_pRef != nullptr && d_pCleaned != nullptr,
          "null device pointer passed to ArdGpu::processDevice");
    CHECK(pDetections != nullptr || uMaxDetections == 0,
          "null detection array with non-zero capacity");

    oStats = ArdStats();

    if (!buildMap(static_cast<const cufftComplex*>(d_pRef),
                  static_cast<const cufftComplex*>(d_pCleaned)))
        return false;

    if (!runCfar(pDetections, uMaxDetections, oStats))
        return false;

    if (pcMap != nullptr) {
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<cufftComplex*>(pcMap), md_cMap,
                              static_cast<size_t>(muCells) * sizeof(cufftComplex),
                              cudaMemcpyDeviceToHost));
    }
    return true;
}

bool ArdGpu::process(const ComplexF* pcRef, const ComplexF* pcCleaned,
                     ComplexF* pcMap,
                     Detection* pDetections, uint64_t uMaxDetections,
                     ArdStats& oStats)
{
    CHECK(mbReady, "ArdGpu::process before a successful init");
    CHECK(pcRef != nullptr && pcCleaned != nullptr,
          "null input buffer passed to ArdGpu::process");

    // ComplexF and cufftComplex have the same layout, so the cast is a
    // reinterpretation at the copy boundary only -- never anywhere else.
    const size_t uCpiBytes =
        static_cast<size_t>(moParams.uCpiSamples) * sizeof(cufftComplex);
    CUDA_CHECK(cudaMemcpy(md_cRef, reinterpret_cast<const cufftComplex*>(pcRef),
                          uCpiBytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(md_cCleaned, reinterpret_cast<const cufftComplex*>(pcCleaned),
                          uCpiBytes, cudaMemcpyHostToDevice));

    return processDevice(md_cRef, md_cCleaned, pcMap,
                         pDetections, uMaxDetections, oStats);
}

bool ArdGpu::detect(const ComplexF* pcMap,
                    Detection* pDetections, uint64_t uMaxDetections,
                    ArdStats& oStats)
{
    CHECK(mbReady, "ArdGpu::detect before a successful init");
    CHECK(pcMap != nullptr, "null map passed to ArdGpu::detect");
    CHECK(pDetections != nullptr || uMaxDetections == 0,
          "null detection array with non-zero capacity");

    oStats = ArdStats();

    // Upload the caller's map and derive the power from it, so the detector
    // sees exactly the same input the CAF would have given it.
    CUDA_CHECK(cudaMemcpy(md_cMap, reinterpret_cast<const cufftComplex*>(pcMap),
                          static_cast<size_t>(muCells) * sizeof(cufftComplex),
                          cudaMemcpyHostToDevice));

    powerKernel<<<blocksFor(muCells, kBlock), kBlock>>>(
        md_cMap, md_fPower, static_cast<uint32_t>(muCells));
    CUDA_CHECK(cudaGetLastError());

    return runCfar(pDetections, uMaxDetections, oStats);
}

} // namespace

std::unique_ptr<ArdProcessor> makeArdProcessorGpu()
{
    return std::unique_ptr<ArdProcessor>(new ArdGpu());
}

} // namespace pbr
