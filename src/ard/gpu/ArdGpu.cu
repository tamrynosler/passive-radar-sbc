// Range-Doppler map formation and CFAR detection on the GPU.
//
// Same algorithm as src/ard/cpu/ArdCpu.cpp, which is the correctness
// reference and explains what the stage does. This file explains only what is
// different because it runs on a GPU.
//
// ---------------------------------------------------------------------------
// Three kernels and one library call
// ---------------------------------------------------------------------------
//
//   1. lagProductKernel   lag product and integrate-and-dump, in one pass
//   2. cuFFT              120 batched transforms of length 1600
//   3. shiftKernel        power, fftshift, and the transpose into display
//                         layout -- all in the pass that has to read and write
//                         anyway, so the reordering is free
//   4. cfarKernel         one thread per cell, direct window sum
//
// ---------------------------------------------------------------------------
// Staying on the device
// ---------------------------------------------------------------------------
//
// processDevice() takes pointers that a GPU canceller already holds, so a
// chained run copies nothing between the two stages. process() is the same
// work with an upload in front and a download behind, for standalone use and
// for validation against MATLAB.
//
// ---------------------------------------------------------------------------
// Precision
// ---------------------------------------------------------------------------
//
// FP32 throughout, as forced by the hardware (Maxwell runs FP64 at 1/32 the
// rate). Expected to agree with the double-precision reference far more
// closely than the canceller did, for two reasons: the summation here is
// hierarchical -- 128 samples per block, then 1600 blocks -- rather than
// 204800 in one go, and there is no ill-conditioned solve amplifying small
// errors. Measured, not assumed; see docs/design_decisions.md.

#include <cublas_v2.h>
#include <cufft.h>

#include "BatchesArdGpu.cuh"

#include "pbr/ArdProcessor.hpp"
#include "pbr/check.hpp"   // must follow the library headers: see check.hpp

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pbr {

namespace {

// ---------------------------------------------------------------------------
// Kernel 1: lag product, integrated and dumped.
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
__global__ void lagProductKernel(const cufftComplex* pcRef,
                                 const cufftComplex* pcCleaned,
                                 cufftComplex* pcDecimated,
                                 uint32_t uRanges, uint32_t uBlocks,
                                 uint32_t uDec)
{
    const uint32_t uIdx = blockIdx.x * blockDim.x + threadIdx.x;
    if (uIdx >= uRanges * uBlocks)
        return;

    const uint32_t r = uIdx % uRanges;          // delay bin
    const uint32_t b = uIdx / uRanges;          // block along the CPI

    const uint32_t uFirst = b * uDec;
    float fRe = 0.0f;
    float fIm = 0.0f;

    for (uint32_t i = 0; i < uDec; ++i) {
        const uint32_t n = uFirst + i;
        // The reference is zero before the start of the CPI.
        if (n < r)
            continue;

        const cufftComplex cS = pcCleaned[n];
        const cufftComplex cR = pcRef[n - r];
        // cleaned * conj(delayed reference)
        fRe += cS.x * cR.x + cS.y * cR.y;
        fIm += cS.y * cR.x - cS.x * cR.y;
    }

    cufftComplex cOut;
    cOut.x = fRe;
    cOut.y = fIm;
    pcDecimated[r * uBlocks + b] = cOut;
}

// ---------------------------------------------------------------------------
// Kernel 3: power, fftshift and transpose, in one pass.
//
// Reads the transform output at [range][k] and writes:
//   * the complex map at [doppler][range], display layout
//   * the power map at the same index, for the detector
//
// The fftshift is just where it writes: transform bin k belongs at display
// row (k + N/2) mod N, which puts zero Doppler in the middle.
// ---------------------------------------------------------------------------
__global__ void shiftKernel(const cufftComplex* pcTransformed,
                            cufftComplex* pcMap, float* pfPower,
                            uint32_t uRanges, uint32_t uBlocks)
{
    const uint32_t uIdx = blockIdx.x * blockDim.x + threadIdx.x;
    if (uIdx >= uRanges * uBlocks)
        return;

    const uint32_t r = uIdx / uBlocks;
    const uint32_t k = uIdx % uBlocks;

    const cufftComplex c = pcTransformed[uIdx];
    const uint32_t d = (k + uBlocks / 2) % uBlocks;
    const uint32_t uDst = d * uRanges + r;

    pcMap[uDst]   = c;
    pfPower[uDst] = c.x * c.x + c.y * c.y;
}

// ---------------------------------------------------------------------------
// Kernel 3b: power only, for a map supplied by the caller.
//
// detect() is handed a map that is already in display layout, so it needs the
// squaring but not the shift or the transpose that shiftKernel also does.
// ---------------------------------------------------------------------------
__global__ void powerKernel(const cufftComplex* pcMap, float* pfPower,
                            uint32_t uCells)
{
    const uint32_t u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= uCells)
        return;
    const cufftComplex c = pcMap[u];
    pfPower[u] = c.x * c.x + c.y * c.y;
}

// ---------------------------------------------------------------------------
// Kernel 4: CFAR.
//
// One thread per cell, walking its own window. Direct and obvious: 273 reads
// per cell for the shipped settings. A summed-area table would cut that to 8,
// and is deliberately not done -- correct first.
//
// Cells whose window runs off the map contribute zero and the divisor stays
// the full training count, matching MATLAB's conv2(..., 'same'). Border
// detections are counted separately so the resulting over-detection is
// visible rather than silent.
// ---------------------------------------------------------------------------
__global__ void cfarKernel(const float* pfPower, uint32_t uRanges,
                           uint32_t uDoppler,
                           int32_t iGuardR, int32_t iGuardD,
                           int32_t iHalfR,  int32_t iHalfD,
                           float fAlphaOverTrain,
                           float fPeakPower,
                           Detection* pDetections, uint32_t uMaxDetections,
                           uint32_t* puCount, uint32_t* puDropped,
                           uint32_t* puBorder,
                           uint64_t uSampleRateHz, uint32_t uDec)
{
    const uint32_t uIdx = blockIdx.x * blockDim.x + threadIdx.x;
    if (uIdx >= uRanges * uDoppler)
        return;

    const int32_t r = static_cast<int32_t>(uIdx % uRanges);
    const int32_t d = static_cast<int32_t>(uIdx / uRanges);

    const float fCell = pfPower[uIdx];

    float fSum = 0.0f;
    for (int32_t dd = -iHalfD; dd <= iHalfD; ++dd) {
        const int32_t iD = d + dd;
        if (iD < 0 || iD >= static_cast<int32_t>(uDoppler))
            continue;
        const bool bGuardD = (dd >= -iGuardD && dd <= iGuardD);

        for (int32_t dr = -iHalfR; dr <= iHalfR; ++dr) {
            if (bGuardD && dr >= -iGuardR && dr <= iGuardR)
                continue;   // guard block, including the cell under test
            const int32_t iR = r + dr;
            if (iR < 0 || iR >= static_cast<int32_t>(uRanges))
                continue;
            fSum += pfPower[static_cast<uint32_t>(iD) * uRanges +
                            static_cast<uint32_t>(iR)];
        }
    }

    const float fThreshold = fAlphaOverTrain * fSum;
    if (!(fCell > fThreshold))
        return;

    const uint32_t uSlot = atomicAdd(puCount, 1u);
    if (uSlot >= uMaxDetections) {
        atomicAdd(puDropped, 1u);
        return;
    }

    Detection oDet;
    oDet.uRangeBin   = static_cast<uint64_t>(r);
    oDet.uDopplerBin = static_cast<uint64_t>(d);
    oDet.dRangeKm    = double(r) * 299792458.0 / double(uSampleRateHz) / 1000.0;
    {
        const double dBinHz = double(uSampleRateHz) /
                              (double(uDec) * double(uDoppler));
        oDet.dDopplerHz = (double(d) - double(uDoppler) / 2.0) * dBinHz;
    }
    oDet.dPowerDb = (fPeakPower > 0.0f)
                  ? 10.0 * log10(double(fCell) / double(fPeakPower)) : 0.0;
    oDet.dSnrDb   = (fThreshold > 0.0f)
                  ? 10.0 * log10(double(fCell) / double(fThreshold)) : 0.0;
    pDetections[uSlot] = oDet;

    if (r < iHalfR || r >= static_cast<int32_t>(uRanges) - iHalfR ||
        d < iHalfD || d >= static_cast<int32_t>(uDoppler) - iHalfD) {
        atomicAdd(puBorder, 1u);
    }
}

uint32_t blocksFor(uint64_t uThreads, uint32_t uBlockSize)
{
    return static_cast<uint32_t>((uThreads + uBlockSize - 1) / uBlockSize);
}

// ---------------------------------------------------------------------------

class ArdGpu : public ArdProcessor {
public:
    ArdGpu() { std::memset(machName, 0, sizeof(machName)); }
    ~ArdGpu() override;

    bool init(const ArdParams& oParams) override;

    bool process(const ComplexF* pcRef, const ComplexF* pcCleaned,
                 ComplexF* pcMap,
                 Detection* pDetections, uint64_t uMaxDetections,
                 ArdStats& oStats) override;

    bool processDevice(const void* pdRef, const void* pdCleaned,
                       ComplexF* pcMap,
                       Detection* pDetections, uint64_t uMaxDetections,
                       ArdStats& oStats) override;

    bool detect(const ComplexF* pcMap,
                Detection* pDetections, uint64_t uMaxDetections,
                ArdStats& oStats) override;

    const char* name() const override { return machName; }

private:
    void release();

    // Everything from the lag products to the power map.
    bool buildMap(const cufftComplex* pdRef, const cufftComplex* pdCleaned);

    // CFAR on whatever is currently in md_fPower, plus the copies back out.
    bool runCfar(Detection* pDetections, uint64_t uMaxDetections,
                 ArdStats& oStats);

    ArdParams      moParams;
    uint64_t       muCells        = 0;
    uint64_t       muTrainCount   = 0;
    double         mdAlpha        = 0.0;

    cublasHandle_t mhCublas       = nullptr;
    cufftHandle    mhPlan         = 0;
    bool           mbPlanValid    = false;

    cufftComplex*  md_cRef        = nullptr;   // uploaded samples (process())
    cufftComplex*  md_cCleaned    = nullptr;
    cufftComplex*  md_cDecimated  = nullptr;   // [range][block]
    cufftComplex*  md_cMap        = nullptr;   // [doppler][range]
    float*         md_fPower      = nullptr;
    Detection*     md_oDetections = nullptr;
    uint32_t*      md_uCounters   = nullptr;   // count, dropped, border

    uint64_t       muDetectCapacity = 0;

    // Only initialised when eAlgorithm is Batches. Holds its own cuFFT plan
    // and scratch; the Direct path never touches it.
    BatchesArdGpu  moBatches;

    char           machName[48];
    bool           mbReady        = false;
};

ArdGpu::~ArdGpu()
{
    release();
}

void ArdGpu::release()
{
    if (md_cRef)        cudaFree(md_cRef);
    if (md_cCleaned)    cudaFree(md_cCleaned);
    if (md_cDecimated)  cudaFree(md_cDecimated);
    if (md_cMap)        cudaFree(md_cMap);
    if (md_fPower)      cudaFree(md_fPower);
    if (md_oDetections) cudaFree(md_oDetections);
    if (md_uCounters)   cudaFree(md_uCounters);
    if (mbPlanValid)    cufftDestroy(mhPlan);
    if (mhCublas)       cublasDestroy(mhCublas);

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
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uSampleRateHz > 0, "sample rate must be non-zero");
    CHECK(oParams.uRangeBins > 0, "range bin count must be non-zero");
    CHECK(oParams.uDopplerDecimation > 0, "Doppler decimation must be non-zero");
    CHECK(oParams.uCpiSamples % oParams.uDopplerDecimation == 0,
          "the decimation factor must divide the CPI exactly");
    CHECK(oParams.uDopplerBins ==
              oParams.uCpiSamples / oParams.uDopplerDecimation,
          "uDopplerBins must equal uCpiSamples / uDopplerDecimation");
    CHECK(oParams.uDopplerBins % 2 == 0,
          "an odd Doppler bin count has no exact centre for zero Doppler");
    CHECK(oParams.dCfarPfa > 0.0 && oParams.dCfarPfa < 1.0,
          "CFAR Pfa must be strictly between 0 and 1");

    const uint64_t uHalfRange   = oParams.uCfarGuardRange + oParams.uCfarTrainRange;
    const uint64_t uHalfDoppler = oParams.uCfarGuardDoppler + oParams.uCfarTrainDoppler;
    CHECK(2 * uHalfRange + 1 <= oParams.uRangeBins,
          "the CFAR window is wider than the map in range");
    CHECK(2 * uHalfDoppler + 1 <= oParams.uDopplerBins,
          "the CFAR window is taller than the map in Doppler");

    CHECK(oParams.uCpiSamples <= 2147483647ULL &&
          oParams.uDopplerBins <= 2147483647ULL,
          "problem dimensions exceed what cuFFT accepts as int");

    moParams = oParams;
    muCells  = oParams.uRangeBins * oParams.uDopplerBins;

    const uint64_t uWindow = (2 * uHalfRange + 1) * (2 * uHalfDoppler + 1);
    const uint64_t uGuard  = (2 * oParams.uCfarGuardRange + 1) *
                             (2 * oParams.uCfarGuardDoppler + 1);
    muTrainCount = uWindow - uGuard;
    CHECK(muTrainCount > 0, "the CFAR window has no training cells");

    const double dN = static_cast<double>(muTrainCount);
    mdAlpha = dN * (std::pow(oParams.dCfarPfa, -1.0 / dN) - 1.0);

    // Generous: at Pfa 1e-5 over 192000 cells only about two false alarms are
    // expected per CPI, so anything near this cap means something is wrong.
    muDetectCapacity = 8192;

    CUBLAS_CHECK(cublasCreate(&mhCublas));

    // 120 independent transforms of length 1600, each contiguous.
    int iN = static_cast<int>(oParams.uDopplerBins);
    CHECK(cufftPlanMany(&mhPlan, 1, &iN,
                        nullptr, 1, iN,
                        nullptr, 1, iN,
                        CUFFT_C2C,
                        static_cast<int>(oParams.uRangeBins)) == CUFFT_SUCCESS,
          "cufftPlanMany failed for the Doppler transform");
    mbPlanValid = true;

    const size_t uCpiBytes  = static_cast<size_t>(oParams.uCpiSamples) * sizeof(cufftComplex);
    const size_t uMapBytes  = static_cast<size_t>(muCells) * sizeof(cufftComplex);
    const size_t uPowBytes  = static_cast<size_t>(muCells) * sizeof(float);
    const size_t uDetBytes  = static_cast<size_t>(muDetectCapacity) * sizeof(Detection);

    CUDA_CHECK(cudaMalloc(&md_cRef,        uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cCleaned,    uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cDecimated,  uMapBytes));
    CUDA_CHECK(cudaMalloc(&md_cMap,        uMapBytes));
    CUDA_CHECK(cudaMalloc(&md_fPower,      uPowBytes));
    CUDA_CHECK(cudaMalloc(&md_oDetections, uDetBytes));
    CUDA_CHECK(cudaMalloc(&md_uCounters,   3 * sizeof(uint32_t)));

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

bool ArdGpu::buildMap(const cufftComplex* pdRef, const cufftComplex* pdCleaned)
{
    const uint32_t uRanges  = static_cast<uint32_t>(moParams.uRangeBins);
    const uint32_t uBlocks  = static_cast<uint32_t>(moParams.uDopplerBins);
    const uint32_t uBlock   = 256;

    // Two routes to the same correlation matrix; see ArdAlgorithm in
    // ArdProcessor.hpp. Everything after this point is shared.
    if (moParams.eAlgorithm == ArdAlgorithm::Batches) {
        if (!moBatches.correlate(pdRef, pdCleaned, md_cDecimated))
            return false;
    } else {
        lagProductKernel<<<blocksFor(muCells, uBlock), uBlock>>>(
            pdRef, pdCleaned, md_cDecimated, uRanges, uBlocks,
            static_cast<uint32_t>(moParams.uDopplerDecimation));
        CUDA_CHECK(cudaGetLastError());
    }

    // In place: the transform overwrites the decimated lag products, which are
    // not needed again.
    CHECK(cufftExecC2C(mhPlan, md_cDecimated, md_cDecimated, CUFFT_FORWARD) ==
              CUFFT_SUCCESS,
          "cufftExecC2C failed for the Doppler transform");

    shiftKernel<<<blocksFor(muCells, uBlock), uBlock>>>(
        md_cDecimated, md_cMap, md_fPower, uRanges, uBlocks);
    CUDA_CHECK(cudaGetLastError());

    return true;
}

bool ArdGpu::runCfar(Detection* pDetections, uint64_t uMaxDetections,
                     ArdStats& oStats)
{
    // The peak, for the dB scale. cublasIsamax finds the largest magnitude;
    // on a non-negative power map that is the peak power.
    int iPeakIndex = 0;
    CUBLAS_CHECK(cublasIsamax(mhCublas, static_cast<int>(muCells),
                              md_fPower, 1, &iPeakIndex));
    float fPeakPower = 0.0f;
    CUDA_CHECK(cudaMemcpy(&fPeakPower, md_fPower + (iPeakIndex - 1),
                          sizeof(float), cudaMemcpyDeviceToHost));
    oStats.dPeakDb = (fPeakPower > 0.0f)
                   ? 10.0 * std::log10(static_cast<double>(fPeakPower)) : 0.0;

    CUDA_CHECK(cudaMemset(md_uCounters, 0, 3 * sizeof(uint32_t)));

    const uint32_t uCapacity = static_cast<uint32_t>(
        (uMaxDetections < muDetectCapacity) ? uMaxDetections : muDetectCapacity);

    const uint32_t uBlock = 256;
    cfarKernel<<<blocksFor(muCells, uBlock), uBlock>>>(
        md_fPower,
        static_cast<uint32_t>(moParams.uRangeBins),
        static_cast<uint32_t>(moParams.uDopplerBins),
        static_cast<int32_t>(moParams.uCfarGuardRange),
        static_cast<int32_t>(moParams.uCfarGuardDoppler),
        static_cast<int32_t>(moParams.uCfarGuardRange + moParams.uCfarTrainRange),
        static_cast<int32_t>(moParams.uCfarGuardDoppler + moParams.uCfarTrainDoppler),
        static_cast<float>(mdAlpha / static_cast<double>(muTrainCount)),
        fPeakPower,
        md_oDetections, uCapacity,
        md_uCounters, md_uCounters + 1, md_uCounters + 2,
        moParams.uSampleRateHz,
        static_cast<uint32_t>(moParams.uDopplerDecimation));
    CUDA_CHECK(cudaGetLastError());

    uint32_t auCounters[3] = { 0, 0, 0 };
    CUDA_CHECK(cudaMemcpy(auCounters, md_uCounters, sizeof(auCounters),
                          cudaMemcpyDeviceToHost));

    // The counter is incremented past the capacity before the slot is
    // rejected, so clamp it to what was actually written.
    const uint32_t uWritten = (auCounters[0] < uCapacity) ? auCounters[0] : uCapacity;
    oStats.uDetections         = uWritten;
    oStats.uDetectionsDropped  = auCounters[1];
    oStats.uDetectionsInBorder = auCounters[2];

    if (uWritten > 0 && pDetections != nullptr) {
        CUDA_CHECK(cudaMemcpy(pDetections, md_oDetections,
                              static_cast<size_t>(uWritten) * sizeof(Detection),
                              cudaMemcpyDeviceToHost));
    }

    return true;
}

bool ArdGpu::processDevice(const void* pdRef, const void* pdCleaned,
                           ComplexF* pcMap,
                           Detection* pDetections, uint64_t uMaxDetections,
                           ArdStats& oStats)
{
    CHECK(mbReady, "ArdGpu::processDevice before a successful init");
    CHECK(pdRef != nullptr && pdCleaned != nullptr,
          "null device pointer passed to ArdGpu::processDevice");
    CHECK(pDetections != nullptr || uMaxDetections == 0,
          "null detection array with non-zero capacity");

    oStats = ArdStats();

    if (!buildMap(static_cast<const cufftComplex*>(pdRef),
                  static_cast<const cufftComplex*>(pdCleaned)))
        return false;

    if (!runCfar(pDetections, uMaxDetections, oStats))
        return false;

    if (pcMap != nullptr) {
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<cufftComplex*>(pcMap), md_cMap,
                              static_cast<size_t>(muCells) * sizeof(cufftComplex),
                              cudaMemcpyDeviceToHost));
    }

    CUDA_CHECK(cudaGetLastError());
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

    const size_t uCpiBytes =
        static_cast<size_t>(moParams.uCpiSamples) * sizeof(cufftComplex);

    CUDA_CHECK(cudaMemcpy(md_cRef,
                          reinterpret_cast<const cufftComplex*>(pcRef),
                          uCpiBytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(md_cCleaned,
                          reinterpret_cast<const cufftComplex*>(pcCleaned),
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

    {
        const uint32_t uBlock = 256;
        powerKernel<<<blocksFor(muCells, uBlock), uBlock>>>(
            md_cMap, md_fPower, static_cast<uint32_t>(muCells));
        CUDA_CHECK(cudaGetLastError());
    }

    return runCfar(pDetections, uMaxDetections, oStats);
}

} // namespace

std::unique_ptr<ArdProcessor> makeArdProcessorGpu()
{
    return std::unique_ptr<ArdProcessor>(new ArdGpu());
}

} // namespace pbr
