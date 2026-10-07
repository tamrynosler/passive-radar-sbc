// ECA-LD on the GPU: the implementation the thesis measures.
//
// Same algorithm as src/dsi/cpu/EcaLdCpu.cpp, which is the correctness
// reference and explains the five steps, the Toeplitz approximation and the
// degenerate-batch guard. This file explains only what is different because it
// runs on a GPU.
//
// ---------------------------------------------------------------------------
// All batches at once
// ---------------------------------------------------------------------------
//
// The batches do not depend on each other, so all B of them are processed in
// the same launches. The three correlations take five batched cuFFT calls
// (three forward, two inverse) with one gather kernel before and one product
// kernel between. The Levinson solve is one block per batch: K sequential
// steps, each with two block reductions, one thread per tap. The solve is
// sequential in K, but the batches run in parallel. The shared convolve kernel
// then applies the weights. Nine launches per CPI, none waiting on the host.
//
// The reductions use blockSum, whose tree order is fixed, so a run is
// deterministic: the same input gives the same bits every time.
//
// ---------------------------------------------------------------------------
// Precision
// ---------------------------------------------------------------------------
//
// FP32 throughout, forced by Maxwell's 1/32-rate FP64. The FFTs are cuFFT's,
// not HostFft's, and the sums run in a tree order, so the result matches the
// CPU's FP32 mode to rounding, not bit for bit.

#include "pbr/DSICanceller.hpp"
#include "../DsiInternal.hpp"
#include "DsiGpu.cuh"

#include <climits>
#include <cstdio>

namespace pbr {

namespace {

// One thread per element u of the B x M grid: batch b = u / M, position
// j = u % M. A takes the reference from n0-K+1 (zero before the CPI), DR and DS
// take the batch itself placed at index K-1.
__global__ void gatherKernel(const cufftComplex* d_cRef, const cufftComplex* d_cSur,
                             cufftComplex* d_cA, cufftComplex* d_cDR, cufftComplex* d_cDS,
                             uint32_t uTaps, uint32_t uBatch, uint32_t uLength, uint32_t uTotal)
{
    const uint32_t u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= uTotal)
        return;
    const int64_t iB = u / uLength;
    const int64_t iJ = u % uLength;
    const int64_t iK = uTaps;
    const int64_t iL = uBatch;
    const int64_t iN0 = iB * iL;

    cufftComplex cZero;
    cZero.x = 0.0f;
    cZero.y = 0.0f;

    const int64_t iSrcA = iN0 - iK + 1 + iJ;
    d_cA[u] = (iJ <= iL + iK - 2 && iSrcA >= 0) ? d_cRef[iSrcA] : cZero;

    const bool bBatch = (iJ >= iK - 1 && iJ < iK - 1 + iL);
    d_cDR[u] = bBatch ? d_cRef[iN0 + iJ - (iK - 1)] : cZero;
    d_cDS[u] = bBatch ? d_cSur[iN0 + iJ - (iK - 1)] : cZero;
}

// DR = conj(A) DR;  DS = conj(A) DS
__global__ void productKernel(const cufftComplex* d_cA, cufftComplex* d_cDR,
                              cufftComplex* d_cDS, uint32_t uTotal)
{
    const uint32_t u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= uTotal)
        return;
    const cufftComplex cA = d_cA[u];   // conj: use cA.x and -cA.y
    const cufftComplex cR = d_cDR[u];
    const cufftComplex cS = d_cDS[u];
    cufftComplex cOutR, cOutS;
    cOutR.x = cA.x * cR.x + cA.y * cR.y;
    cOutR.y = cA.x * cR.y - cA.y * cR.x;
    cOutS.x = cA.x * cS.x + cA.y * cS.y;
    cOutS.y = cA.x * cS.y - cA.y * cS.x;
    d_cDR[u] = cOutR;
    d_cDS[u] = cOutS;
}

// Levinson-Durbin for one batch per block, the recursion of EcaLdCpu::levinson.
// Thread i owns tap i. Every thread holds rr0, beta and the running scalars in
// registers, computed identically from the block sums. Every __syncthreads
// and blockSum is reached by every thread: inactive threads add zero.
__global__ void levinsonKernel(const cufftComplex* d_cRef, const cufftComplex* d_cCross,
                               cufftComplex* d_cW, uint32_t* d_uDegenerate,
                               uint32_t uTaps, uint32_t uLength, float fInvM, float fLoad)
{
    __shared__ cufftComplex acRr[kBlock];
    __shared__ cufftComplex acBvec[kBlock];
    __shared__ cufftComplex acX[kBlock];
    __shared__ cufftComplex acY[kBlock];
    __shared__ cufftComplex acYNew[kBlock];
    __shared__ float afShared[kBlock];

    const uint32_t uI = threadIdx.x;
    const bool bTap = uI < uTaps;
    const size_t uBase = static_cast<size_t>(blockIdx.x) * uLength;
    cufftComplex* d_cWb = d_cW + static_cast<size_t>(blockIdx.x) * uTaps;

    if (bTap) {
        acRr[uI].x   = d_cRef[uBase + uI].x * fInvM;
        acRr[uI].y   = d_cRef[uBase + uI].y * fInvM;
        acBvec[uI].x = d_cCross[uBase + uI].x * fInvM;
        acBvec[uI].y = d_cCross[uBase + uI].y * fInvM;
    }
    if (uI == 0) {
        acRr[0].x = acRr[0].x * fLoad;
        acRr[0].y = 0.0f;
    }
    __syncthreads();

    const float fRr0 = acRr[0].x;
    if (!(fRr0 > 0.0f)) {
        if (bTap) {
            d_cWb[uI].x = 0.0f;
            d_cWb[uI].y = 0.0f;
        }
        if (uI == 0)
            atomicAdd(d_uDegenerate, 1u);
        return;
    }

    if (uI == 0) {
        acX[0].x = acBvec[0].x / fRr0;
        acX[0].y = acBvec[0].y / fRr0;
        if (uTaps > 1) {
            acY[0].x = -acRr[1].x / fRr0;
            acY[0].y = -acRr[1].y / fRr0;
        }
    }
    __syncthreads();

    float fBeta = fRr0;
    if (uTaps > 1)
        fBeta = fRr0 + (acRr[1].x * acY[0].x + acRr[1].y * acY[0].y);   // Re(conj(rr1) y0)

    for (uint32_t uM = 1; uM < uTaps; ++uM) {
        // fBeta is the same in every thread, so this exit is uniform.
        if (!(fBeta > 0.0f)) {
            if (bTap) {
                d_cWb[uI].x = 0.0f;
                d_cWb[uI].y = 0.0f;
            }
            if (uI == 0)
                atomicAdd(d_uDegenerate, 1u);
            return;
        }

        // mu = (bvec[m] - sum_{i<m} rr[m-i] x[i]) / beta
        float fTermRe = 0.0f, fTermIm = 0.0f;
        if (uI < uM) {
            const cufftComplex cR = acRr[uM - uI];
            const cufftComplex cX = acX[uI];
            fTermRe = cR.x * cX.x - cR.y * cX.y;
            fTermIm = cR.x * cX.y + cR.y * cX.x;
        }
        const float fSumRe = blockSum(fTermRe, afShared);
        const float fSumIm = blockSum(fTermIm, afShared);
        const float fMuRe = (acBvec[uM].x - fSumRe) / fBeta;
        const float fMuIm = (acBvec[uM].y - fSumIm) / fBeta;

        // x[i] += mu conj(y[m-1-i]), old y;  x[m] = mu
        if (uI < uM) {
            const cufftComplex cYr = acY[uM - 1 - uI];   // conj: (cYr.x, -cYr.y)
            acX[uI].x = acX[uI].x + (fMuRe * cYr.x + fMuIm * cYr.y);
            acX[uI].y = acX[uI].y + (fMuIm * cYr.x - fMuRe * cYr.y);
        }
        if (uI == uM) {
            acX[uM].x = fMuRe;
            acX[uM].y = fMuIm;
        }
        __syncthreads();

        if (uM < uTaps - 1) {
            // alpha = -(rr[m+1] + sum_{i<m} rr[m-i] y[i]) / beta
            fTermRe = 0.0f;
            fTermIm = 0.0f;
            if (uI < uM) {
                const cufftComplex cR = acRr[uM - uI];
                const cufftComplex cY = acY[uI];
                fTermRe = cR.x * cY.x - cR.y * cY.y;
                fTermIm = cR.x * cY.y + cR.y * cY.x;
            }
            const float fSumYRe = blockSum(fTermRe, afShared);
            const float fSumYIm = blockSum(fTermIm, afShared);
            const float fAlphaRe = -(acRr[uM + 1].x + fSumYRe) / fBeta;
            const float fAlphaIm = -(acRr[uM + 1].y + fSumYIm) / fBeta;

            // ynew[i] = y[i] + alpha conj(y[m-1-i]), from the old y
            if (uI < uM) {
                const cufftComplex cYr = acY[uM - 1 - uI];
                acYNew[uI].x = acY[uI].x + (fAlphaRe * cYr.x + fAlphaIm * cYr.y);
                acYNew[uI].y = acY[uI].y + (fAlphaIm * cYr.x - fAlphaRe * cYr.y);
            }
            if (uI == uM) {
                acYNew[uM].x = fAlphaRe;
                acYNew[uM].y = fAlphaIm;
            }
            __syncthreads();
            if (uI <= uM)
                acY[uI] = acYNew[uI];
            __syncthreads();

            fBeta = (1.0f - (fAlphaRe * fAlphaRe + fAlphaIm * fAlphaIm)) * fBeta;
        }
    }

    if (bTap)
        d_cWb[uI] = acX[uI];
}

// ---------------------------------------------------------------------------

class EcaLdGpu : public DSICanceller {
public:
    ~EcaLdGpu() override { release(); }

    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override { return "ECA-LD (GPU)"; }

    const void* deviceReference() const override { return md_cRef; }
    const void* deviceOutput()    const override { return md_cOut; }
    void setHostOutputEnabled(bool bEnabled) override { mbHostOutput = bEnabled; }

private:
    bool setUp(const DSIParams& oParams);   // init() without the clean-up
    void release();

    DSIParams       moParams;
    uint64_t        muTaps       = 0;   // K
    uint64_t        muBatch      = 0;   // L
    uint64_t        muBatches    = 0;   // B
    uint64_t        muLength     = 0;   // M

    cublasHandle_t  mhCublas     = nullptr;
    cufftHandle     mhPlan       = 0;   // M-point, batch B
    bool            mbPlan       = false;

    // All allocated in setUp(), freed in release().
    cufftComplex*   md_cRef      = nullptr;   // CPI samples, reference
    cufftComplex*   md_cSur      = nullptr;   // CPI samples, surveillance
    cufftComplex*   md_cOut      = nullptr;   // cleaned surveillance
    cufftComplex*   md_cA        = nullptr;   // B x M, a, then A
    cufftComplex*   md_cDR       = nullptr;   // B x M, dr, DR, then cRef
    cufftComplex*   md_cDS       = nullptr;   // B x M, ds, DS, then cCross
    cufftComplex*   md_cW        = nullptr;   // B x K, weights
    uint32_t*       md_uDegenerate = nullptr; // 1, degenerate batches this CPI

    bool            mbReady      = false;
    bool            mbHostOutput = true;
};

void EcaLdGpu::release()
{
    // Destructors cannot report failure and nothing can be done about a failed
    // free anyway, so these are deliberately unchecked.
    cudaFree(md_cRef);
    cudaFree(md_cSur);
    cudaFree(md_cOut);
    cudaFree(md_cA);
    cudaFree(md_cDR);
    cudaFree(md_cDS);
    cudaFree(md_cW);
    cudaFree(md_uDegenerate);
    if (mbPlan)
        cufftDestroy(mhPlan);
    if (mhCublas)
        cublasDestroy(mhCublas);

    md_cRef = md_cSur = md_cOut = md_cA = md_cDR = md_cDS = md_cW = nullptr;
    md_uDegenerate = nullptr;
    mbPlan   = false;
    mhCublas = nullptr;
    mbReady  = false;
}

bool EcaLdGpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "EcaLdGpu::init called twice");
    if (setUp(oParams))
        return true;
    release();   // whatever was allocated before the failure
    return false;
}

bool EcaLdGpu::setUp(const DSIParams& oParams)
{
    if (!ecaLdParamsOk(oParams))
        return false;
    CHECK(oParams.uClutterTaps <= kBlock,
          "ECA-LD on the GPU gives one thread to each tap, so the taps cannot exceed the block size");
    // Kernel arguments are uint32_t; cuFFT and cuBLAS take int.
    CHECK(oParams.uCpiSamples <= INT_MAX,
          "the CPI is longer than the kernels and cuFFT can index");

    moParams  = oParams;
    muTaps    = oParams.uClutterTaps;
    muBatch   = oParams.uEcaLdBatchSamples;
    muBatches = oParams.uCpiSamples / muBatch;

    muLength = 2;
    while (muLength < muBatch + 2 * muTaps - 2)
        muLength <<= 1;

    CHECK(muBatches * muLength <= INT_MAX, "the batched FFT is too large for cuFFT");

    CUDA_CHECK(cublasCreate(&mhCublas));

    const size_t uCpiBytes = static_cast<size_t>(oParams.uCpiSamples) * sizeof(cufftComplex);
    const size_t uGridBytes = static_cast<size_t>(muBatches) * muLength * sizeof(cufftComplex);

    // Every allocation and plan happens here and is reused for the life of
    // the object.
    CUDA_CHECK(cudaMalloc(&md_cRef, uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cSur, uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cOut, uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cA,   uGridBytes));
    CUDA_CHECK(cudaMalloc(&md_cDR,  uGridBytes));
    CUDA_CHECK(cudaMalloc(&md_cDS,  uGridBytes));
    CUDA_CHECK(cudaMalloc(&md_cW,   static_cast<size_t>(muBatches) * muTaps * sizeof(cufftComplex)));
    CUDA_CHECK(cudaMalloc(&md_uDegenerate, sizeof(uint32_t)));

    CUDA_CHECK(cufftPlan1d(&mhPlan, static_cast<int>(muLength), CUFFT_C2C,
                           static_cast<int>(muBatches)));
    mbPlan = true;

    mbReady = true;
    return true;
}

bool EcaLdGpu::cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                      ComplexF* pcOut, DSIStats& oStats)
{
    CHECK(mbReady, "EcaLdGpu::cancel before a successful init");
    CHECK(pcRef != nullptr && pcSur != nullptr,
          "null input buffer passed to EcaLdGpu::cancel");
    CHECK(pcOut != nullptr || !mbHostOutput,
          "null output buffer with host output enabled");
    CHECK(pcOut != pcRef && pcOut != pcSur,
          "the output buffer must not alias either input");

    oStats = DSIStats();

    const size_t uCpiBytes = static_cast<size_t>(moParams.uCpiSamples) * sizeof(cufftComplex);
    const uint32_t uTaps   = static_cast<uint32_t>(muTaps);
    const uint32_t uBatch  = static_cast<uint32_t>(muBatch);
    const uint32_t uLength = static_cast<uint32_t>(muLength);
    const uint32_t uTotal  = static_cast<uint32_t>(muBatches * muLength);
    const uint32_t uGrid   = blocksFor(uTotal, kBlock);
    const float    fInvM   = 1.0f / static_cast<float>(muLength);
    const float    fLoad   = 1.0f + static_cast<float>(moParams.dDiagonalLoading);

    // ComplexF and cufftComplex have the same layout, so the cast is a
    // reinterpretation at the copy boundary only -- never anywhere else.
    CUDA_CHECK(cudaMemcpy(md_cRef, reinterpret_cast<const cufftComplex*>(pcRef),
                          uCpiBytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(md_cSur, reinterpret_cast<const cufftComplex*>(pcSur),
                          uCpiBytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemset(md_uDegenerate, 0, sizeof(uint32_t)));

    // 1. every batch's a, dr, ds
    gatherKernel<<<uGrid, kBlock>>>(md_cRef, md_cSur, md_cA, md_cDR, md_cDS,
                                    uTaps, uBatch, uLength, uTotal);
    CUDA_CHECK(cudaGetLastError());

    // 2. correlations: three forward FFTs, the products, two inverse FFTs
    CUDA_CHECK(cufftExecC2C(mhPlan, md_cA,  md_cA,  CUFFT_FORWARD));
    CUDA_CHECK(cufftExecC2C(mhPlan, md_cDR, md_cDR, CUFFT_FORWARD));
    CUDA_CHECK(cufftExecC2C(mhPlan, md_cDS, md_cDS, CUFFT_FORWARD));
    productKernel<<<uGrid, kBlock>>>(md_cA, md_cDR, md_cDS, uTotal);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cufftExecC2C(mhPlan, md_cDR, md_cDR, CUFFT_INVERSE));
    CUDA_CHECK(cufftExecC2C(mhPlan, md_cDS, md_cDS, CUFFT_INVERSE));

    // 3-4. loading and the Levinson solve, one block per batch
    levinsonKernel<<<static_cast<uint32_t>(muBatches), kBlock>>>(
        md_cDR, md_cDS, md_cW, md_uDegenerate, uTaps, uLength, fInvM, fLoad);
    CUDA_CHECK(cudaGetLastError());

    // 5. out = sur - sum_k w[k] r[n-k], each batch with its own weights
    if (!launchConvolve(md_cRef, md_cW, md_cSur, md_cOut, moParams.uCpiSamples,
                        muTaps, muBatch))
        return false;

    if (!gpuSuppressionDb(mhCublas, md_cSur, md_cOut, moParams.uCpiSamples,
                          oStats.dSuppressionDb))
        return false;
    if (mbHostOutput) {
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<cufftComplex*>(pcOut), md_cOut,
                              uCpiBytes, cudaMemcpyDeviceToHost));
    }

    uint32_t uDegenerate = 0;
    CUDA_CHECK(cudaMemcpy(&uDegenerate, md_uDegenerate, sizeof(uint32_t),
                          cudaMemcpyDeviceToHost));
    if (uDegenerate > 0)
        std::fprintf(stderr, "ECA-LD: %u of %llu batches were degenerate, weights set to zero\n",
                     uDegenerate, static_cast<unsigned long long>(muBatches));

    oStats.uIterations = 0;   // not an iterative solver
    return true;
}

} // namespace

std::unique_ptr<DSICanceller> makeEcaLdCancellerGpu()
{
    return std::unique_ptr<DSICanceller>(new EcaLdGpu());
}

} // namespace pbr
