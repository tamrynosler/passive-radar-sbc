// FBLMS on the GPU: the implementation the thesis measures.
//
// Same algorithm as src/dsi/cpu/FblmsCpu.cpp, which is the correctness
// reference and explains the ten steps, the two 1/M scalings and the state
// kept across CPIs. This file explains only what is different because it runs
// on a GPU.
//
// ---------------------------------------------------------------------------
// One batched FFT, then a sequential loop
// ---------------------------------------------------------------------------
//
// The reference FFTs do not depend on the weights, so all B blocks are
// transformed in one batched cuFFT call. The weight loop cannot be
// parallelised: each block's error needs the weights the previous block
// produced. One CPI is therefore B x (4 cuFFT + 5 kernel) launches, queued
// without a host wait. Each launch works on only M points, so the cost is
// launch overhead, not arithmetic.
//
// The per-bin power recursion is done inside the gradient kernel, which
// reads X and E and writes G.
//
// ---------------------------------------------------------------------------
// Precision
// ---------------------------------------------------------------------------
//
// FP32 throughout, forced by Maxwell's 1/32-rate FP64. The FFTs are cuFFT's,
// not HostFft's, so the result matches the CPU's FP32 mode to rounding, not
// bit for bit.

#include "pbr/DSICanceller.hpp"
#include "../DsiInternal.hpp"
#include "DsiGpu.cuh"

#include <climits>

namespace pbr {

namespace {

// Block k, position m of the 2N-point input: the N reference samples before
// the block, then the block itself. Block 0 takes its first half from the
// tail of the previous CPI.
__global__ void gatherKernel(const cufftComplex* d_cRef, const cufftComplex* d_cTail,
                             cufftComplex* d_cXAll, uint32_t uTaps, uint32_t uTotal)
{
    const uint32_t u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= uTotal)
        return;
    const uint32_t k = u / (2 * uTaps);
    const uint32_t m = u % (2 * uTaps);
    if (m < uTaps)
        d_cXAll[u] = (k == 0) ? d_cTail[m] : d_cRef[(k - 1) * uTaps + m];
    else
        d_cXAll[u] = d_cRef[k * uTaps + m - uTaps];
}

// Y = X W
__global__ void multiplyKernel(const cufftComplex* d_cX, const cufftComplex* d_cW,
                               cufftComplex* d_cY, uint32_t uLength)
{
    const uint32_t m = blockIdx.x * blockDim.x + threadIdx.x;
    if (m >= uLength)
        return;
    const cufftComplex cX = d_cX[m];
    const cufftComplex cW = d_cW[m];
    cufftComplex cY;
    cY.x = cX.x * cW.x - cX.y * cW.y;
    cY.y = cX.x * cW.y + cX.y * cW.x;
    d_cY[m] = cY;
}

// e = d - y / M on the last N points of the block; writes the output and
// [0 ; e] for the FFT.
__global__ void errorKernel(const cufftComplex* d_cY, const cufftComplex* d_cSur,
                            cufftComplex* d_cOut, cufftComplex* d_cEPad,
                            uint32_t uBlockStart, uint32_t uTaps, uint32_t uLength,
                            float fInvM)
{
    const uint32_t m = blockIdx.x * blockDim.x + threadIdx.x;
    if (m >= uLength)
        return;
    cufftComplex cE;
    cE.x = 0.0f;
    cE.y = 0.0f;
    if (m >= uTaps) {
        const uint32_t n = m - uTaps;
        const cufftComplex cY   = d_cY[m];
        const cufftComplex cSur = d_cSur[uBlockStart + n];
        cE.x = cSur.x - cY.x * fInvM;
        cE.y = cSur.y - cY.y * fInvM;
        d_cOut[uBlockStart + n] = cE;
    }
    d_cEPad[m] = cE;
}

// P = lambda P + (1 - lambda) |X|^2;  G = conj(X) E (mu / P), zero where P = 0.
// With bInitPower set (the very first block) P starts at |X|^2.
__global__ void gradientKernel(const cufftComplex* d_cX, const cufftComplex* d_cE,
                               float* d_fP, cufftComplex* d_cG, uint32_t uLength,
                               float fMu, float fLambda, float fOneMinus, bool bInitPower)
{
    const uint32_t m = blockIdx.x * blockDim.x + threadIdx.x;
    if (m >= uLength)
        return;
    const cufftComplex cX = d_cX[m];
    const float fX2 = cX.x * cX.x + cX.y * cX.y;
    float fP = bInitPower ? fX2 : d_fP[m];
    fP = fLambda * fP + fOneMinus * fX2;
    d_fP[m] = fP;

    cufftComplex cG;
    cG.x = 0.0f;
    cG.y = 0.0f;
    if (fP != 0.0f) {
        const cufftComplex cE = d_cE[m];
        const float fScale = fMu / fP;
        cG.x = (cX.x * cE.x + cX.y * cE.y) * fScale;
        cG.y = (cX.x * cE.y - cX.y * cE.x) * fScale;
    }
    d_cG[m] = cG;
}

// The gradient constraint: gc = g / M on the first N points, zero elsewhere.
__global__ void constrainKernel(const cufftComplex* d_cG, cufftComplex* d_cGc,
                                uint32_t uTaps, uint32_t uLength, float fInvM)
{
    const uint32_t m = blockIdx.x * blockDim.x + threadIdx.x;
    if (m >= uLength)
        return;
    cufftComplex cGc;
    cGc.x = 0.0f;
    cGc.y = 0.0f;
    if (m < uTaps) {
        cGc.x = d_cG[m].x * fInvM;
        cGc.y = d_cG[m].y * fInvM;
    }
    d_cGc[m] = cGc;
}

// W += 2 Gc
__global__ void updateKernel(cufftComplex* d_cW, const cufftComplex* d_cGc, uint32_t uLength)
{
    const uint32_t m = blockIdx.x * blockDim.x + threadIdx.x;
    if (m >= uLength)
        return;
    d_cW[m].x += 2.0f * d_cGc[m].x;
    d_cW[m].y += 2.0f * d_cGc[m].y;
}

// ---------------------------------------------------------------------------

class FblmsGpu : public DSICanceller {
public:
    ~FblmsGpu() override { release(); }

    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override { return "FBLMS (GPU)"; }

    const void* deviceReference() const override { return md_cRef; }
    const void* deviceOutput()    const override { return md_cOut; }
    void setHostOutputEnabled(bool bEnabled) override { mbHostOutput = bEnabled; }
    bool resetState() override;

private:
    bool setUp(const DSIParams& oParams);   // init() without the clean-up
    bool runBlocks();                       // the sequential weight loop
    void release();

    DSIParams       moParams;
    uint64_t        muTaps       = 0;   // N
    uint64_t        muLength     = 0;   // M = 2N
    uint64_t        muBlocks     = 0;   // B

    cublasHandle_t  mhCublas     = nullptr;
    cufftHandle     mhPlanBatch  = 0;   // M-point, batch B
    cufftHandle     mhPlanOne    = 0;   // M-point, single
    bool            mbPlanBatch  = false;
    bool            mbPlanOne    = false;

    // All allocated in setUp(), freed in release().
    cufftComplex*   md_cRef      = nullptr;   // CPI samples, reference
    cufftComplex*   md_cSur      = nullptr;   // CPI samples, surveillance
    cufftComplex*   md_cOut      = nullptr;   // cleaned surveillance
    cufftComplex*   md_cXAll     = nullptr;   // B x M, block inputs, then their FFTs
    cufftComplex*   md_cY        = nullptr;   // M
    cufftComplex*   md_cEPad     = nullptr;   // M, [0 ; e], then E
    cufftComplex*   md_cG        = nullptr;   // M, G, then g
    cufftComplex*   md_cGc       = nullptr;   // M, gc, then Gc

    // State kept across CPIs.
    cufftComplex*   md_cW        = nullptr;   // M, frequency-domain weights
    float*          md_fP        = nullptr;   // M, per-bin reference power
    cufftComplex*   md_cTail     = nullptr;   // N, last N reference samples
    bool            mbPowerSet   = false;

    bool            mbReady      = false;
    bool            mbHostOutput = true;
};

void FblmsGpu::release()
{
    // Destructors cannot report failure and nothing can be done about a failed
    // free anyway, so these are deliberately unchecked.
    cudaFree(md_cRef);
    cudaFree(md_cSur);
    cudaFree(md_cOut);
    cudaFree(md_cXAll);
    cudaFree(md_cY);
    cudaFree(md_cEPad);
    cudaFree(md_cG);
    cudaFree(md_cGc);
    cudaFree(md_cW);
    cudaFree(md_fP);
    cudaFree(md_cTail);
    if (mbPlanBatch)
        cufftDestroy(mhPlanBatch);
    if (mbPlanOne)
        cufftDestroy(mhPlanOne);
    if (mhCublas)
        cublasDestroy(mhCublas);

    md_cRef = md_cSur = md_cOut = md_cXAll = md_cY = nullptr;
    md_cEPad = md_cG = md_cGc = md_cW = md_cTail = nullptr;
    md_fP = nullptr;
    mbPlanBatch = mbPlanOne = false;
    mhCublas = nullptr;
    mbReady  = false;
}

bool FblmsGpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "FblmsGpu::init called twice");
    if (setUp(oParams))
        return true;
    release();   // whatever was allocated before the failure
    return false;
}

bool FblmsGpu::setUp(const DSIParams& oParams)
{
    if (!fblmsParamsOk(oParams))
        return false;
    // Kernel arguments are uint32_t; cuFFT and cuBLAS take int.
    CHECK(oParams.uCpiSamples <= INT_MAX,
          "the CPI is longer than the kernels and cuFFT can index");

    moParams = oParams;
    muTaps   = oParams.uFblmsTaps;
    muLength = 2 * muTaps;
    muBlocks = oParams.uCpiSamples / muTaps;

    CHECK(muLength <= INT_MAX && muBlocks * muLength <= INT_MAX,
          "the batched FFT is too large for cuFFT");

    CUDA_CHECK(cublasCreate(&mhCublas));

    const size_t uCpiBytes = static_cast<size_t>(oParams.uCpiSamples) * sizeof(cufftComplex);
    const size_t uMBytes   = static_cast<size_t>(muLength) * sizeof(cufftComplex);

    // Every allocation and plan happens here and is reused for the life of
    // the object.
    CUDA_CHECK(cudaMalloc(&md_cRef,  uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cSur,  uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cOut,  uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cXAll, static_cast<size_t>(muBlocks) * uMBytes));
    CUDA_CHECK(cudaMalloc(&md_cY,    uMBytes));
    CUDA_CHECK(cudaMalloc(&md_cEPad, uMBytes));
    CUDA_CHECK(cudaMalloc(&md_cG,    uMBytes));
    CUDA_CHECK(cudaMalloc(&md_cGc,   uMBytes));
    CUDA_CHECK(cudaMalloc(&md_cW,    uMBytes));
    CUDA_CHECK(cudaMalloc(&md_fP,    static_cast<size_t>(muLength) * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&md_cTail, static_cast<size_t>(muTaps) * sizeof(cufftComplex)));

    CUDA_CHECK(cudaMemset(md_cW,    0, uMBytes));
    CUDA_CHECK(cudaMemset(md_fP,    0, static_cast<size_t>(muLength) * sizeof(float)));
    CUDA_CHECK(cudaMemset(md_cTail, 0, static_cast<size_t>(muTaps) * sizeof(cufftComplex)));
    mbPowerSet = false;

    CUDA_CHECK(cufftPlan1d(&mhPlanBatch, static_cast<int>(muLength), CUFFT_C2C,
                           static_cast<int>(muBlocks)));
    mbPlanBatch = true;
    CUDA_CHECK(cufftPlan1d(&mhPlanOne, static_cast<int>(muLength), CUFFT_C2C, 1));
    mbPlanOne = true;

    mbReady = true;
    return true;
}

bool FblmsGpu::resetState()
{
    CHECK(mbReady, "FblmsGpu::resetState before a successful init");
    CUDA_CHECK(cudaMemset(md_cW,    0, static_cast<size_t>(muLength) * sizeof(cufftComplex)));
    CUDA_CHECK(cudaMemset(md_fP,    0, static_cast<size_t>(muLength) * sizeof(float)));
    CUDA_CHECK(cudaMemset(md_cTail, 0, static_cast<size_t>(muTaps) * sizeof(cufftComplex)));
    mbPowerSet = false;
    return true;
}

bool FblmsGpu::runBlocks()
{
    const uint32_t uTaps   = static_cast<uint32_t>(muTaps);
    const uint32_t uLength = static_cast<uint32_t>(muLength);
    const uint32_t uGrid   = blocksFor(muLength, kBlock);
    const float    fInvM   = 1.0f / static_cast<float>(muLength);
    const float    fMu     = static_cast<float>(moParams.dFblmsStep);
    const float    fLambda = static_cast<float>(moParams.dFblmsForget);
    const float    fOneMinus = 1.0f - fLambda;

    // 2-3. every block's input, and its FFT, in one batched call
    const uint64_t uAll = muBlocks * muLength;
    gatherKernel<<<blocksFor(uAll, kBlock), kBlock>>>(md_cRef, md_cTail, md_cXAll,
                                                      uTaps, static_cast<uint32_t>(uAll));
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cufftExecC2C(mhPlanBatch, md_cXAll, md_cXAll, CUFFT_FORWARD));

    for (uint64_t k = 0; k < muBlocks; ++k) {
        cufftComplex* d_cX = md_cXAll + k * muLength;

        // 5. Y = X W;  y = IFFT(Y)
        multiplyKernel<<<uGrid, kBlock>>>(d_cX, md_cW, md_cY, uLength);
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cufftExecC2C(mhPlanOne, md_cY, md_cY, CUFFT_INVERSE));

        // 6-7. e = d - y / M;  E = FFT([0 ; e])
        errorKernel<<<uGrid, kBlock>>>(md_cY, md_cSur, md_cOut, md_cEPad,
                                       static_cast<uint32_t>(k * muTaps), uTaps, uLength, fInvM);
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cufftExecC2C(mhPlanOne, md_cEPad, md_cEPad, CUFFT_FORWARD));

        // 3-4, 8. power, then G
        gradientKernel<<<uGrid, kBlock>>>(d_cX, md_cEPad, md_fP, md_cG, uLength,
                                          fMu, fLambda, fOneMinus, !mbPowerSet);
        CUDA_CHECK(cudaGetLastError());
        mbPowerSet = true;

        // 9. g = IFFT(G) / M, first N kept
        CUDA_CHECK(cufftExecC2C(mhPlanOne, md_cG, md_cG, CUFFT_INVERSE));
        constrainKernel<<<uGrid, kBlock>>>(md_cG, md_cGc, uTaps, uLength, fInvM);
        CUDA_CHECK(cudaGetLastError());

        // 10. W += 2 FFT(gc)
        CUDA_CHECK(cufftExecC2C(mhPlanOne, md_cGc, md_cGc, CUFFT_FORWARD));
        updateKernel<<<uGrid, kBlock>>>(md_cW, md_cGc, uLength);
        CUDA_CHECK(cudaGetLastError());
    }
    return true;
}

bool FblmsGpu::cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                      ComplexF* pcOut, DSIStats& oStats)
{
    CHECK(mbReady, "FblmsGpu::cancel before a successful init");
    CHECK(pcRef != nullptr && pcSur != nullptr,
          "null input buffer passed to FblmsGpu::cancel");
    CHECK(pcOut != nullptr || !mbHostOutput,
          "null output buffer with host output enabled");
    CHECK(pcOut != pcRef && pcOut != pcSur,
          "the output buffer must not alias either input");

    oStats = DSIStats();

    const size_t uCpiBytes = static_cast<size_t>(moParams.uCpiSamples) * sizeof(cufftComplex);

    // ComplexF and cufftComplex have the same layout, so the cast is a
    // reinterpretation at the copy boundary only -- never anywhere else.
    CUDA_CHECK(cudaMemcpy(md_cRef, reinterpret_cast<const cufftComplex*>(pcRef),
                          uCpiBytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(md_cSur, reinterpret_cast<const cufftComplex*>(pcSur),
                          uCpiBytes, cudaMemcpyHostToDevice));

    if (!runBlocks())
        return false;

    // The next CPI's first block needs the last N reference samples of this one.
    CUDA_CHECK(cudaMemcpy(md_cTail, md_cRef + (moParams.uCpiSamples - muTaps),
                          static_cast<size_t>(muTaps) * sizeof(cufftComplex),
                          cudaMemcpyDeviceToDevice));

    if (!gpuSuppressionDb(mhCublas, md_cSur, md_cOut, moParams.uCpiSamples,
                          oStats.dSuppressionDb))
        return false;
    if (mbHostOutput) {
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<cufftComplex*>(pcOut), md_cOut,
                              uCpiBytes, cudaMemcpyDeviceToHost));
    }
    oStats.uIterations = 0;   // not an iterative solver
    return true;
}

} // namespace

std::unique_ptr<DSICanceller> makeFblmsCancellerGpu()
{
    return std::unique_ptr<DSICanceller>(new FblmsGpu());
}

} // namespace pbr
