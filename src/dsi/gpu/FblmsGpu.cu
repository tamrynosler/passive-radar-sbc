// FBLMS on the GPU: the implementation the thesis measures.
//
// Same algorithm as src/dsi/cpu/FblmsCpu.cpp, which is the correctness
// reference and explains the ten steps, the two 1/M scalings and the state
// kept across CPIs. This file explains only what is different because it runs
// on a GPU.
//
// ---------------------------------------------------------------------------
// One batched FFT, then one kernel for the whole loop
// ---------------------------------------------------------------------------
//
// The reference FFTs do not depend on the weights, so all B blocks are
// transformed in one batched cuFFT call. The weight loop cannot be
// parallelised: each block's error needs the weights the previous block
// produced. Each step works on only M = 2N points, so launching a kernel per
// step was all overhead. Instead one thread block of M threads (M <= 1024) runs all B
// blocks in turn, with the weights, the power and the work buffers in shared
// memory, and its own radix-2 FFT (fftBlock) for the four transforms per
// block. The Nano has one SM, so one thread block loses nothing.
//
// ---------------------------------------------------------------------------
// Precision
// ---------------------------------------------------------------------------
//
// FP32 throughout, forced by Maxwell's 1/32-rate FP64. The reference FFTs are
// cuFFT's and the in-loop ones are fftBlock's, not HostFft's, so the result
// matches the CPU's FP32 mode to rounding, not bit for bit.

#include "pbr/DSICanceller.hpp"
#include "../DsiInternal.hpp"
#include "DsiGpu.cuh"

#include <climits>
#include <cmath>

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

// ---------------------------------------------------------------------------
// The in-block FFT
// ---------------------------------------------------------------------------

// M = 2N is a power of two, at most kMaxPoints: one thread per point.
constexpr uint32_t kMaxPoints = 1024;

// exp(-j 2 pi i / M), i = 0 .. M/2-1, computed on the host in double.
__constant__ float2 c_cTwiddle[kMaxPoints / 2];

// In-place radix-2 FFT of uPoints points in shared memory, called by all
// uPoints threads of the block. Unnormalised, like cuFFT's; bInverse uses
// the conjugate twiddles. The first __syncthreads makes the caller's writes
// visible, the last one makes the result visible to the caller.
__device__ void fftBlock(cufftComplex* s_cData, uint32_t uPoints, bool bInverse)
{
    const uint32_t t = threadIdx.x;
    const uint32_t uStages = __ffs(uPoints) - 1;   // log2(M)

    // bit-reversal permutation, through a register
    __syncthreads();
    const cufftComplex cMoved = s_cData[__brev(t) >> (32 - uStages)];
    __syncthreads();
    s_cData[t] = cMoved;
    __syncthreads();

    // each butterfly stage: thread t < M/2 does one pair, the rest idle
    for (uint32_t uStage = 0; uStage < uStages; ++uStage) {
        if (t < uPoints / 2) {
            const uint32_t uHalf = 1u << uStage;
            const uint32_t uJ    = t & (uHalf - 1);
            const uint32_t uLow  = ((t - uJ) << 1) + uJ;
            const float2   cW    = c_cTwiddle[uJ * (uPoints / 2 / uHalf)];
            const float    fWi   = bInverse ? -cW.y : cW.y;
            const cufftComplex cA = s_cData[uLow];
            const cufftComplex cB = s_cData[uLow + uHalf];
            cufftComplex cT;
            cT.x = cB.x * cW.x - cB.y * fWi;
            cT.y = cB.x * fWi + cB.y * cW.x;
            cufftComplex cSum, cDiff;
            cSum.x  = cA.x + cT.x;  cSum.y  = cA.y + cT.y;
            cDiff.x = cA.x - cT.x;  cDiff.y = cA.y - cT.y;
            s_cData[uLow]         = cSum;
            s_cData[uLow + uHalf] = cDiff;
        }
        __syncthreads();
    }
}

// All B blocks of the weight loop in one thread block of M threads, thread m
// owning bin m. Steps 4-10 of the algorithm (see FblmsCpu.cpp); the arithmetic
// order is that of the old per-step kernels. d_cXAll holds the FFT of every
// block's input.
__global__ void fblmsBlocksKernel(const cufftComplex* d_cXAll, const cufftComplex* d_cSur,
                                  cufftComplex* d_cOut, cufftComplex* d_cW, float* d_fP,
                                  uint32_t uBlocks, uint32_t uTaps,
                                  float fMu, float fLambda, float fOneMinus, float fInvM,
                                  bool bInitPower)
{
    __shared__ cufftComplex s_cW[kMaxPoints];
    __shared__ cufftComplex s_cA[kMaxPoints];
    __shared__ cufftComplex s_cB[kMaxPoints];
    __shared__ float        s_fP[kMaxPoints];

    const uint32_t m = threadIdx.x;   // launched with M = 2 uTaps threads
    s_cW[m] = d_cW[m];
    s_fP[m] = d_fP[m];

    for (uint32_t k = 0; k < uBlocks; ++k) {
        const cufftComplex cX = d_cXAll[static_cast<size_t>(k) * uTaps * 2 + m];

        // 3-4. P = lambda P + (1 - lambda) |X|^2; the very first block starts at |X|^2
        const float fX2 = cX.x * cX.x + cX.y * cX.y;
        float fP = (bInitPower && k == 0) ? fX2 : s_fP[m];
        fP = fLambda * fP + fOneMinus * fX2;
        s_fP[m] = fP;

        // 5. Y = X W;  y = IFFT(Y)
        cufftComplex cY;
        cY.x = cX.x * s_cW[m].x - cX.y * s_cW[m].y;
        cY.y = cX.x * s_cW[m].y + cX.y * s_cW[m].x;
        s_cA[m] = cY;
        fftBlock(s_cA, 2 * uTaps, true);

        // 6. e = d - y / M on the last N points; B = [0 ; e]
        cufftComplex cE;
        cE.x = 0.0f;
        cE.y = 0.0f;
        if (m >= uTaps) {
            const uint32_t uPos = k * uTaps + (m - uTaps);
            const cufftComplex cSur = d_cSur[uPos];
            cE.x = cSur.x - s_cA[m].x * fInvM;
            cE.y = cSur.y - s_cA[m].y * fInvM;
            d_cOut[uPos] = cE;
        }
        s_cB[m] = cE;

        // 7. E = FFT([0 ; e])
        fftBlock(s_cB, 2 * uTaps, false);

        // 8. G = conj(X) E (mu / P), zero where P = 0
        cufftComplex cG;
        cG.x = 0.0f;
        cG.y = 0.0f;
        if (fP != 0.0f) {
            const cufftComplex cEf = s_cB[m];
            const float fScale = fMu / fP;
            cG.x = (cX.x * cEf.x + cX.y * cEf.y) * fScale;
            cG.y = (cX.x * cEf.y - cX.y * cEf.x) * fScale;
        }
        s_cA[m] = cG;

        // 9. g = IFFT(G) / M, first N kept
        fftBlock(s_cA, 2 * uTaps, true);
        cufftComplex cGc;
        cGc.x = 0.0f;
        cGc.y = 0.0f;
        if (m < uTaps) {
            cGc.x = s_cA[m].x * fInvM;
            cGc.y = s_cA[m].y * fInvM;
        }
        s_cA[m] = cGc;

        // 10. W += 2 FFT(gc)
        fftBlock(s_cA, 2 * uTaps, false);
        s_cW[m].x += 2.0f * s_cA[m].x;
        s_cW[m].y += 2.0f * s_cA[m].y;
    }

    d_cW[m] = s_cW[m];
    d_fP[m] = s_fP[m];
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
    bool            mbPlanBatch  = false;

    // All allocated in setUp(), freed in release().
    cufftComplex*   md_cRef      = nullptr;   // CPI samples, reference
    cufftComplex*   md_cSur      = nullptr;   // CPI samples, surveillance
    cufftComplex*   md_cOut      = nullptr;   // cleaned surveillance
    cufftComplex*   md_cXAll     = nullptr;   // B x M, block inputs, then their FFTs

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
    cudaFree(md_cW);
    cudaFree(md_fP);
    cudaFree(md_cTail);
    if (mbPlanBatch)
        cufftDestroy(mhPlanBatch);
    if (mhCublas)
        cublasDestroy(mhCublas);

    md_cRef = md_cSur = md_cOut = md_cXAll = md_cW = md_cTail = nullptr;
    md_fP = nullptr;
    mbPlanBatch = false;
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
    CHECK(2 * static_cast<uint64_t>(oParams.uFblmsTaps) <= kMaxPoints,
          "FBLMS on the GPU needs 2 * FBLMS_TAPS <= 1024 (one thread per FFT point)");

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

    // Twiddles for fftBlock, in double on the host so they are as accurate as cuFFT's.
    float2 acTwiddle[kMaxPoints / 2];
    const double dPi = 3.14159265358979323846;
    for (uint32_t i = 0; i < muLength / 2; ++i) {
        const double dAngle = -2.0 * dPi * static_cast<double>(i) / static_cast<double>(muLength);
        acTwiddle[i].x = static_cast<float>(std::cos(dAngle));
        acTwiddle[i].y = static_cast<float>(std::sin(dAngle));
    }
    CUDA_CHECK(cudaMemcpyToSymbol(c_cTwiddle, acTwiddle, (muLength / 2) * sizeof(float2)));

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

    // 4-10. the whole sequential weight loop, one launch
    fblmsBlocksKernel<<<1, uLength>>>(md_cXAll, md_cSur, md_cOut, md_cW, md_fP,
                                         static_cast<uint32_t>(muBlocks), uTaps,
                                         fMu, fLambda, fOneMinus, fInvM, !mbPowerSet);
    CUDA_CHECK(cudaGetLastError());
    mbPowerSet = true;
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
