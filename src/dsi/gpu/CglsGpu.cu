// CGLS on the GPU: the implementation the thesis measures.
//
// Same algorithm as src/dsi/cpu/CglsCpu.cpp, which is the correctness
// reference and explains the iteration, the fixed count, the line-minimiser
// alpha and the residual refresh. This file explains only what is different
// because it runs on a GPU.
//
// ---------------------------------------------------------------------------
// Every window at once, and no host synchronisation inside the loop
// ---------------------------------------------------------------------------
//
// The CPU solves the windows one after another. Here all of them advance
// together: each kernel launch does one step of the iteration for every
// window, and the per-window scalars -- gamma, Re(p^H s) -- live in device
// memory, computed by one block per window (reduceKernel, stepKernel) and read by the next kernel. The
// host queues 3 + 4I + I/10 launches per CPI for I iterations and never waits
// for any of them until the very end. Per iteration:
//
//   convolveKernel   q = R p                          tiles of 256 samples
//   stepKernel       alpha; x += alpha p; r -= alpha q  one block per window
//   adjointKernel    partial s = R^H r per segment      windows x segments blocks
//   reduceKernel     sum partials; beta; p = s + beta p one block per window
//
// plus a second convolveKernel every kCglsResidualRefresh iterations to
// recompute r = b - R x. convolveKernel is shared with ECA (DsiGpu.cuh). The
// adjoint is split over segments of each window so that enough blocks exist
// to fill the GPU; reduceKernel adds the partials in a fixed order, so runs
// are still bit-identical (the sum is grouped differently from the CPU's).
// The products with R are written directly rather than through cuBLAS, so the
// delayed-reference matrix is never built: the reference is read in place,
// which is what makes the memory footprint a few vectors.
//
// ---------------------------------------------------------------------------
// Priming
// ---------------------------------------------------------------------------
//
// With bCglsPrime the weights buffer is simply not cleared between CPIs, so
// window w starts from where window w finished last time. This is why priming
// is defined same-window rather than previous-window: a chain through the
// windows would serialise a loop that is otherwise embarrassingly parallel.
//
// ---------------------------------------------------------------------------
// Precision
// ---------------------------------------------------------------------------
//
// FP32 throughout, forced by Maxwell's 1/32-rate FP64. The per-window dot
// products are tree reductions, so they round differently from the CPU's
// serial sums and alpha and beta differ in the last bit each iteration;
// tests/test_cgls.cpp measures how far that carries.

#include "pbr/DSICanceller.hpp"
#include "../DsiInternal.hpp"
#include "DsiGpu.cuh"

#include <climits>
#include <vector>

namespace pbr {

namespace {

// ---------------------------------------------------------------------------
// The step. One block per window.
//
//   delta = |q|^2;  alpha = Re(p^H s) / delta;  x += alpha p;  r -= alpha q
//
// Re(p^H s) was left in d_fPs by the reduce kernel. On a refresh iteration
// the residual update is skipped and a convolveKernel recomputes r from x.
// A window that has stopped (see below) is left alone; each step taken is
// counted in d_uSteps.
// ---------------------------------------------------------------------------
__global__ void stepKernel(const cufftComplex* d_cQ, cufftComplex* d_cR,
                           const cufftComplex* d_cP, cufftComplex* d_cX,
                           const float* d_fPs, bool* d_bStopped, uint32_t* d_uSteps,
                           uint32_t uTaps, uint32_t uWindow, bool bUpdateResidual)
{
    __shared__ float afShared[kBlock];

    const uint32_t w     = blockIdx.x;
    const uint32_t uBase = w * uWindow;

    if (d_bStopped[w])   // the same for every thread of the block
        return;

    float fPartial = 0.0f;
    for (uint32_t n = threadIdx.x; n < uWindow; n += kBlock) {
        const cufftComplex cQ = d_cQ[uBase + n];
        fPartial += cQ.x * cQ.x + cQ.y * cQ.y;
    }
    const float fDelta = blockSum(fPartial, afShared);

    // A zero delta means p is zero: the residual is already orthogonal to
    // every column and there is nothing left to fit. Stop this window for the
    // rest of the CPI, exactly where the CPU stops it, so x keeps its value.
    // fDelta is the same in every thread, so the whole block returns.
    if (!(fDelta > 0.0f)) {
        if (threadIdx.x == 0)
            d_bStopped[w] = true;
        return;
    }
    const float fAlpha = d_fPs[w] / fDelta;

    for (uint32_t k = threadIdx.x; k < uTaps; k += kBlock) {
        const size_t u = static_cast<size_t>(w) * uTaps + k;
        d_cX[u].x += fAlpha * d_cP[u].x;
        d_cX[u].y += fAlpha * d_cP[u].y;
    }
    if (bUpdateResidual) {
        for (uint32_t n = threadIdx.x; n < uWindow; n += kBlock) {
            d_cR[uBase + n].x -= fAlpha * d_cQ[uBase + n].x;
            d_cR[uBase + n].y -= fAlpha * d_cQ[uBase + n].y;
        }
    }
    if (threadIdx.x == 0)
        ++d_uSteps[w];
}

// ---------------------------------------------------------------------------
// The new direction, in two kernels.
//
//   s = R^H r;  gammaNew = |s|^2;  beta = gammaNew / gamma;  p = s + beta p
//   gamma = gammaNew;  ps = Re(p^H s)
//
// Thread k sums conj(r[n-k]) r_res[n] over the window. One block per window
// left the GPU nearly idle, so each window is cut into uSegs segments and
// adjointKernel gives one block to each (grid = uSegs x windows). A block
// walks its segment kBlock samples at a time: the residual chunk and the
// reference samples it needs go into shared memory with coalesced loads, then
// thread k (one per tap) accumulates from shared memory. The block writes its
// partial s to d_cPartial[(w * uSegs + seg) * K + k].
//
// reduceKernel (one block per window, one thread per tap) adds the uSegs
// partials in a fixed order, so every run gives the same bits, and does the
// rest: gammaNew, beta, p, ps. With bInit set this is the start of a window:
// p = s and beta plays no part. A stopped window is left alone by both.
// ---------------------------------------------------------------------------
__global__ void adjointKernel(const cufftComplex* d_cRef, const cufftComplex* d_cR,
                              cufftComplex* d_cPartial, const bool* d_bStopped,
                              uint32_t uTaps, uint32_t uWindow, uint32_t uSegs)
{
    __shared__ cufftComplex acRes[kBlock];
    __shared__ cufftComplex acRef[2 * kBlock];   // kBlock + uTaps - 1 <= 2 kBlock - 1

    const uint32_t w = blockIdx.y;
    if (d_bStopped[w])   // the same for every thread of the block
        return;
    const uint32_t uBase   = w * uWindow;
    const uint32_t uSegLen = (uWindow + uSegs - 1) / uSegs;
    const uint32_t uBegin  = uBase + blockIdx.x * uSegLen;
    uint32_t       uEnd    = uBegin + uSegLen;
    if (uEnd > uBase + uWindow)
        uEnd = uBase + uWindow;
    const uint32_t k = threadIdx.x;

    float fRe = 0.0f;
    float fIm = 0.0f;
    for (uint32_t uN0 = uBegin; uN0 < uEnd; uN0 += kBlock) {
        uint32_t uCount = uEnd - uN0;
        if (uCount > kBlock)
            uCount = kBlock;

        // acRes[t] = r[uN0 + t];  acRef[j] = ref[uN0 - (uTaps - 1) + j],
        // zero before the start of the CPI.
        __syncthreads();   // the previous chunk is finished with the tiles
        cufftComplex cZero;
        cZero.x = 0.0f;
        cZero.y = 0.0f;
        acRes[threadIdx.x] = (threadIdx.x < uCount) ? d_cR[uN0 + threadIdx.x] : cZero;
        for (uint32_t j = threadIdx.x; j < uCount + uTaps - 1; j += kBlock) {
            const int64_t iIdx = static_cast<int64_t>(uN0) - (uTaps - 1) + j;
            acRef[j] = (iIdx >= 0) ? d_cRef[iIdx] : cZero;
        }
        __syncthreads();

        if (k < uTaps) {
            for (uint32_t t = 0; t < uCount; ++t) {
                const cufftComplex cA = acRef[t + uTaps - 1 - k];
                const cufftComplex cB = acRes[t];
                fRe += cA.x * cB.x + cA.y * cB.y;
                fIm += cA.x * cB.y - cA.y * cB.x;
            }
        }
    }

    if (k < uTaps) {
        cufftComplex cS;
        cS.x = fRe;
        cS.y = fIm;
        d_cPartial[(static_cast<size_t>(w) * uSegs + blockIdx.x) * uTaps + k] = cS;
    }
}

__global__ void reduceKernel(const cufftComplex* d_cPartial, cufftComplex* d_cP,
                             float* d_fGamma, float* d_fPs, const bool* d_bStopped,
                             uint32_t uTaps, uint32_t uSegs, bool bInit)
{
    __shared__ float afShared[kBlock];

    const uint32_t w = blockIdx.x;
    if (d_bStopped[w])   // the same for every thread of the block
        return;
    const uint32_t k    = threadIdx.x;
    const bool     bTap = (k < uTaps);

    cufftComplex cS;
    cS.x = 0.0f;
    cS.y = 0.0f;
    if (bTap) {
        for (uint32_t uSeg = 0; uSeg < uSegs; ++uSeg) {
            const cufftComplex cPart = d_cPartial[(static_cast<size_t>(w) * uSegs + uSeg) * uTaps + k];
            cS.x += cPart.x;
            cS.y += cPart.y;
        }
    }

    const float fGammaNew = blockSum(bTap ? (cS.x * cS.x + cS.y * cS.y) : 0.0f, afShared);
    // gamma can only be zero here after an underflow, since a zero gradient
    // stops the window first; the guard matches the CPU's.
    const float fGammaOld = d_fGamma[w];
    const float fBeta = (bInit || !(fGammaOld > 0.0f)) ? 0.0f : fGammaNew / fGammaOld;

    cufftComplex cP;
    cP.x = 0.0f;
    cP.y = 0.0f;
    if (bTap) {
        const size_t u = static_cast<size_t>(w) * uTaps + k;
        cP.x = cS.x + fBeta * d_cP[u].x;
        cP.y = cS.y + fBeta * d_cP[u].y;
        d_cP[u] = cP;
    }

    const float fPs = blockSum(bTap ? (cP.x * cS.x + cP.y * cS.y) : 0.0f, afShared);
    if (k == 0) {
        d_fGamma[w] = fGammaNew;
        d_fPs[w]    = fPs;
    }
}

// ---------------------------------------------------------------------------

class CglsGpu : public DSICanceller {
public:
    ~CglsGpu() override { release(); }

    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override { return "CGLS (GPU)"; }

    const void* deviceReference() const override { return md_cRef; }
    const void* deviceOutput()    const override { return md_cOut; }
    void setHostOutputEnabled(bool bEnabled) override { mbHostOutput = bEnabled; }

private:
    bool setUp(const DSIParams& oParams);   // init() without the clean-up
    bool iterate();                         // every window, all iterations
    void release();

    DSIParams       moParams;
    uint64_t        muTaps          = 0;
    uint64_t        muWindows       = 0;
    uint64_t        muWindowSamples = 0;

    cublasHandle_t  mhCublas        = nullptr;

    // All allocated in setUp(), freed in release().
    cufftComplex*   md_cRef         = nullptr;   // CPI samples, reference
    cufftComplex*   md_cSur         = nullptr;   // CPI samples, surveillance
    cufftComplex*   md_cOut         = nullptr;   // cleaned surveillance
    cufftComplex*   md_cR           = nullptr;   // residual, CPI samples
    cufftComplex*   md_cQ           = nullptr;   // R p, CPI samples
    cufftComplex*   md_cX           = nullptr;   // windows x K, the weights
    cufftComplex*   md_cP           = nullptr;   // windows x K, search direction
    float*          md_fGamma       = nullptr;   // per window, |s|^2
    float*          md_fPs          = nullptr;   // per window, Re(p^H s)
    bool*           md_bStopped     = nullptr;   // per window, nothing left to fit
    uint32_t*       md_uSteps       = nullptr;   // per window, steps taken this CPI
    cufftComplex*   md_cSPartial    = nullptr;   // windows x segments x K, partial s
    uint32_t        muSegs          = 1;         // adjoint segments per window

    bool            mbHaveWeights   = false;
    bool            mbReady         = false;
    bool            mbHostOutput    = true;
};

void CglsGpu::release()
{
    // Destructors cannot report failure and nothing can be done about a failed
    // free anyway, so these are deliberately unchecked. cudaFree(nullptr) is
    // a no-op.
    cudaFree(md_cRef);
    cudaFree(md_cSur);
    cudaFree(md_cOut);
    cudaFree(md_cR);
    cudaFree(md_cQ);
    cudaFree(md_cX);
    cudaFree(md_cP);
    cudaFree(md_fGamma);
    cudaFree(md_fPs);
    cudaFree(md_bStopped);
    cudaFree(md_uSteps);
    cudaFree(md_cSPartial);
    if (mhCublas)
        cublasDestroy(mhCublas);

    md_cRef = md_cSur = md_cOut = md_cR = md_cQ = md_cX = md_cP = nullptr;
    md_fGamma = md_fPs = nullptr;
    md_bStopped = nullptr;
    md_uSteps   = nullptr;
    md_cSPartial = nullptr;
    mhCublas = nullptr;
    mbReady  = false;
}

bool CglsGpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "CglsGpu::init called twice");
    if (setUp(oParams))
        return true;
    release();   // whatever was allocated before the failure
    return false;
}

bool CglsGpu::setUp(const DSIParams& oParams)
{
    if (!cglsParamsOk(oParams))
        return false;
    CHECK(oParams.uClutterTaps <= kBlock,
          "the GPU adjoint kernel gives one thread per tap; K is over kBlock");
    CHECK(oParams.uCpiSamples % oParams.uCglsBatchSamples == 0,
          "the CPI is not a whole number of CGLS windows");
    // Kernel arguments are uint32_t and cuBLAS takes int.
    CHECK(oParams.uCpiSamples <= INT_MAX,
          "the CPI is longer than the kernels and cuBLAS can index");

    moParams        = oParams;
    muTaps          = oParams.uClutterTaps;
    muWindowSamples = oParams.uCglsBatchSamples;
    muWindows       = oParams.uCpiSamples / oParams.uCglsBatchSamples;

    CUDA_CHECK(cublasCreate(&mhCublas));

    const size_t uCpiBytes = static_cast<size_t>(oParams.uCpiSamples) * sizeof(cufftComplex);
    const size_t uWBytes   = static_cast<size_t>(muWindows * muTaps) * sizeof(cufftComplex);

    // Every allocation happens here and is reused for the life of the object.
    CUDA_CHECK(cudaMalloc(&md_cRef,   uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cSur,   uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cOut,   uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cR,     uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cQ,     uCpiBytes));
    // Enough adjoint blocks to fill the GPU (about 64), but a segment of at
    // least K samples.
    muSegs = static_cast<uint32_t>(64 / muWindows);
    if (muSegs > muWindowSamples / muTaps)
        muSegs = static_cast<uint32_t>(muWindowSamples / muTaps);
    if (muSegs < 1)
        muSegs = 1;

    CUDA_CHECK(cudaMalloc(&md_cSPartial, uWBytes * muSegs));
    CUDA_CHECK(cudaMalloc(&md_cX,     uWBytes));
    CUDA_CHECK(cudaMalloc(&md_cP,     uWBytes));
    CUDA_CHECK(cudaMalloc(&md_fGamma, static_cast<size_t>(muWindows) * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&md_fPs,    static_cast<size_t>(muWindows) * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&md_bStopped, static_cast<size_t>(muWindows) * sizeof(bool)));
    CUDA_CHECK(cudaMalloc(&md_uSteps,   static_cast<size_t>(muWindows) * sizeof(uint32_t)));

    mbReady = true;
    return true;
}

bool CglsGpu::iterate()
{
    const uint64_t uTotal  = moParams.uCpiSamples;
    const uint32_t uTaps   = static_cast<uint32_t>(muTaps);
    const uint32_t uWindow = static_cast<uint32_t>(muWindowSamples);
    const uint32_t uWindows = static_cast<uint32_t>(muWindows);

    CUDA_CHECK(cudaMemset(md_bStopped, 0, static_cast<size_t>(muWindows) * sizeof(bool)));
    CUDA_CHECK(cudaMemset(md_uSteps,   0, static_cast<size_t>(muWindows) * sizeof(uint32_t)));

    // r = b - R x;  s = R^H r;  p = s;  gamma = |s|^2;  ps = |s|^2
    if (!launchConvolve(md_cRef, md_cX, md_cSur, md_cR, uTotal, muTaps, muWindowSamples))
        return false;
    adjointKernel<<<dim3(muSegs, uWindows), kBlock>>>(md_cRef, md_cR, md_cSPartial,
                                                      md_bStopped, uTaps, uWindow, muSegs);
    CUDA_CHECK(cudaGetLastError());
    reduceKernel<<<uWindows, kBlock>>>(md_cSPartial, md_cP, md_fGamma, md_fPs,
                                       md_bStopped, uTaps, muSegs, true);
    CUDA_CHECK(cudaGetLastError());

    for (uint64_t uIt = 1; uIt <= moParams.uMaxIterations; ++uIt) {
        const bool bRefresh = (uIt % kCglsResidualRefresh == 0);

        // q = R p
        if (!launchConvolve(md_cRef, md_cP, nullptr, md_cQ, uTotal, muTaps, muWindowSamples))
            return false;

        stepKernel<<<uWindows, kBlock>>>(md_cQ, md_cR, md_cP, md_cX, md_fPs,
                                         md_bStopped, md_uSteps, uTaps, uWindow, !bRefresh);
        CUDA_CHECK(cudaGetLastError());

        // r = b - R x, computed honestly every kCglsResidualRefresh steps.
        if (bRefresh &&
            !launchConvolve(md_cRef, md_cX, md_cSur, md_cR, uTotal, muTaps, muWindowSamples))
            return false;

        adjointKernel<<<dim3(muSegs, uWindows), kBlock>>>(md_cRef, md_cR, md_cSPartial,
                                                          md_bStopped, uTaps, uWindow, muSegs);
        CUDA_CHECK(cudaGetLastError());
        reduceKernel<<<uWindows, kBlock>>>(md_cSPartial, md_cP, md_fGamma, md_fPs,
                                           md_bStopped, uTaps, muSegs, false);
        CUDA_CHECK(cudaGetLastError());
    }
    return true;
}

bool CglsGpu::cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                     ComplexF* pcOut, DSIStats& oStats)
{
    CHECK(mbReady, "CglsGpu::cancel before a successful init");
    CHECK(pcRef != nullptr && pcSur != nullptr,
          "null input buffer passed to CglsGpu::cancel");
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

    // Start from zero, or from where each window ended up last CPI.
    if (!moParams.bCglsPrime || !mbHaveWeights) {
        CUDA_CHECK(cudaMemset(md_cX, 0,
                              static_cast<size_t>(muWindows * muTaps) * sizeof(cufftComplex)));
    }

    if (!iterate())
        return false;

    // The output is b - R x computed fresh, as on the CPU.
    if (!launchConvolve(md_cRef, md_cX, md_cSur, md_cOut, moParams.uCpiSamples,
                        muTaps, muWindowSamples))
        return false;

    if (!gpuSuppressionDb(mhCublas, md_cSur, md_cOut, moParams.uCpiSamples,
                          oStats.dSuppressionDb))
        return false;
    if (mbHostOutput) {
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<cufftComplex*>(pcOut), md_cOut,
                              uCpiBytes, cudaMemcpyDeviceToHost));
    }

    // Iterations actually run: the most any window took, as on the CPU.
    std::vector<uint32_t> vuSteps(static_cast<size_t>(muWindows), 0);
    CUDA_CHECK(cudaMemcpy(vuSteps.data(), md_uSteps, vuSteps.size() * sizeof(uint32_t),
                          cudaMemcpyDeviceToHost));
    oStats.uIterations = 0;
    for (size_t u = 0; u < vuSteps.size(); ++u)
        if (vuSteps[u] > oStats.uIterations)
            oStats.uIterations = vuSteps[u];

    mbHaveWeights = true;
    return true;
}

} // namespace

std::unique_ptr<DSICanceller> makeCglsCancellerGpu()
{
    return std::unique_ptr<DSICanceller>(new CglsGpu());
}

} // namespace pbr
