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
// memory, computed by one block per window and read by the next kernel. The
// host queues about 90 launches per CPI and never waits for any of them
// until the very end. Per iteration:
//
//   convolveKernel   q = R p                          one thread per sample
//   stepKernel       alpha; x += alpha p; r -= alpha q  one block per window
//   adjointKernel    s = R^H r; beta; p = s + beta p    one block per window
//
// plus a second convolveKernel on refresh iterations to recompute r = b - R x.
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

#include <cublas_v2.h>

#include "pbr/DSICanceller.hpp"
#include "pbr/check.hpp"   // must follow the library headers: see check.hpp

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pbr {

namespace {

typedef cuComplex DeviceComplex;

// Every kernel here uses this block size. The per-window kernels reduce
// across the block in shared memory, which needs a power of two, and the
// adjoint kernel gives one thread to each tap, which caps K at this.
const uint32_t kBlock = 256;

// Recompute r = b - R x from scratch this often. Same constant as the CPU.
const uint64_t kResidualRefresh = 10;

// Sum fValue over the block. Every thread gets the total. pfShared must hold
// kBlock floats; it is free again on return.
__device__ float blockSum(float fValue, float* pfShared)
{
    pfShared[threadIdx.x] = fValue;
    __syncthreads();
    for (uint32_t s = kBlock / 2; s > 0; s >>= 1) {
        if (threadIdx.x < s)
            pfShared[threadIdx.x] += pfShared[threadIdx.x + s];
        __syncthreads();
    }
    const float fTotal = pfShared[0];
    __syncthreads();
    return fTotal;
}

// ---------------------------------------------------------------------------
// Kernel 1: out = R w, or base - R w when pcBase is given.
//
// out[n] = sum over k of w[k] r[n-k], with the window's own weights and the
// reference zero before the start of the CPI. One thread per sample, every
// window in one launch -- the same shape as EcaGpu's applyWeightsKernel.
// Without a base it computes q = R p; with the surveillance as base it
// computes the residual b - R x and, at the end, the output.
// ---------------------------------------------------------------------------
__global__ void convolveKernel(const DeviceComplex* pcRef,
                               const DeviceComplex* pcWeights,
                               const DeviceComplex* pcBase,
                               DeviceComplex* pcOut,
                               uint32_t uTotal, uint32_t uTaps, uint32_t uWindow)
{
    const uint32_t n = blockIdx.x * blockDim.x + threadIdx.x;
    if (n >= uTotal)
        return;

    const DeviceComplex* pcW = pcWeights + static_cast<size_t>(n / uWindow) * uTaps;
    const uint32_t uCount = (n + 1 < uTaps) ? (n + 1) : uTaps;

    float fRe = 0.0f;
    float fIm = 0.0f;
    for (uint32_t k = 0; k < uCount; ++k) {
        const DeviceComplex cW = pcW[k];
        const DeviceComplex cR = pcRef[n - k];
        fRe += cW.x * cR.x - cW.y * cR.y;
        fIm += cW.x * cR.y + cW.y * cR.x;
    }

    DeviceComplex cOut;
    if (pcBase != nullptr) {
        cOut.x = pcBase[n].x - fRe;
        cOut.y = pcBase[n].y - fIm;
    } else {
        cOut.x = fRe;
        cOut.y = fIm;
    }
    pcOut[n] = cOut;
}

// ---------------------------------------------------------------------------
// Kernel 2: the step. One block per window.
//
//   delta = |q|^2;  alpha = Re(p^H s) / delta;  x += alpha p;  r -= alpha q
//
// Re(p^H s) was left in pfPs by the adjoint kernel. On a refresh iteration
// the residual update is skipped and a convolveKernel recomputes r from x.
// ---------------------------------------------------------------------------
__global__ void stepKernel(const DeviceComplex* pcQ, DeviceComplex* pcR,
                           const DeviceComplex* pcP, DeviceComplex* pcX,
                           const float* pfPs,
                           uint32_t uTaps, uint32_t uWindow, int iUpdateResidual)
{
    __shared__ float afShared[kBlock];

    const uint32_t w     = blockIdx.x;
    const uint32_t uBase = w * uWindow;

    float fPartial = 0.0f;
    for (uint32_t n = threadIdx.x; n < uWindow; n += kBlock) {
        const DeviceComplex c = pcQ[uBase + n];
        fPartial += c.x * c.x + c.y * c.y;
    }
    const float fDelta = blockSum(fPartial, afShared);

    // A zero delta means p is zero and there is nothing left to fit; a zero
    // step leaves x and r as they are, which is the right answer.
    const float fAlpha = (fDelta > 0.0f) ? pfPs[w] / fDelta : 0.0f;

    for (uint32_t k = threadIdx.x; k < uTaps; k += kBlock) {
        const size_t u = static_cast<size_t>(w) * uTaps + k;
        pcX[u].x += fAlpha * pcP[u].x;
        pcX[u].y += fAlpha * pcP[u].y;
    }
    if (iUpdateResidual) {
        for (uint32_t n = threadIdx.x; n < uWindow; n += kBlock) {
            pcR[uBase + n].x -= fAlpha * pcQ[uBase + n].x;
            pcR[uBase + n].y -= fAlpha * pcQ[uBase + n].y;
        }
    }
}

// ---------------------------------------------------------------------------
// Kernel 3: the new direction. One block per window, one thread per tap.
//
//   s = R^H r;  gammaNew = |s|^2;  beta = gammaNew / gamma;  p = s + beta p
//   gamma = gammaNew;  ps = Re(p^H s)
//
// Thread k sums conj(r[n-k]) r_res[n] over the window: neighbouring threads
// read neighbouring reference samples and share the residual sample, so the
// reads coalesce. With iInit set this is the start of a window: p = s and
// beta plays no part.
// ---------------------------------------------------------------------------
__global__ void adjointKernel(const DeviceComplex* pcRef, const DeviceComplex* pcR,
                              DeviceComplex* pcP, float* pfGamma, float* pfPs,
                              uint32_t uTaps, uint32_t uWindow, int iInit)
{
    __shared__ float afShared[kBlock];

    const uint32_t w     = blockIdx.x;
    const uint32_t uBase = w * uWindow;
    const uint32_t k     = threadIdx.x;
    const bool     bTap  = (k < uTaps);

    DeviceComplex cS;
    cS.x = 0.0f;
    cS.y = 0.0f;
    if (bTap) {
        // The reference is zero before the start of the CPI.
        const uint32_t uFirst = (uBase > k) ? uBase : k;
        for (uint32_t n = uFirst; n < uBase + uWindow; ++n) {
            const DeviceComplex cA = pcRef[n - k];
            const DeviceComplex cB = pcR[n];
            cS.x += cA.x * cB.x + cA.y * cB.y;
            cS.y += cA.x * cB.y - cA.y * cB.x;
        }
    }

    const float fGammaNew = blockSum(bTap ? (cS.x * cS.x + cS.y * cS.y) : 0.0f, afShared);
    const float fGammaOld = pfGamma[w];
    const float fBeta = (iInit || !(fGammaOld > 0.0f)) ? 0.0f : fGammaNew / fGammaOld;

    DeviceComplex cP;
    cP.x = 0.0f;
    cP.y = 0.0f;
    if (bTap) {
        const size_t u = static_cast<size_t>(w) * uTaps + k;
        cP.x = cS.x + fBeta * pcP[u].x;
        cP.y = cS.y + fBeta * pcP[u].y;
        pcP[u] = cP;
    }

    const float fPs = blockSum(bTap ? (cP.x * cS.x + cP.y * cS.y) : 0.0f, afShared);
    if (k == 0) {
        pfGamma[w] = fGammaNew;
        pfPs[w]    = fPs;
    }
}

uint32_t blocksFor(uint64_t uThreads, uint32_t uBlockSize)
{
    return static_cast<uint32_t>((uThreads + uBlockSize - 1) / uBlockSize);
}

// ---------------------------------------------------------------------------

class CglsGpu : public DSICanceller {
public:
    CglsGpu() { std::memset(machName, 0, sizeof(machName)); }
    ~CglsGpu() override;

    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override { return machName; }

    const void* deviceReference() const override { return md_cRef; }
    const void* deviceOutput()    const override { return md_cOut; }
    void setHostOutputEnabled(bool bEnabled) override { mbHostOutput = bEnabled; }

private:
    void release();

    DSIParams       moParams;
    uint64_t        muTaps          = 0;
    uint64_t        muWindows       = 0;
    uint64_t        muWindowSamples = 0;

    cublasHandle_t  mhCublas        = nullptr;

    DeviceComplex*  md_cRef         = nullptr;   // CPI samples, reference
    DeviceComplex*  md_cSur         = nullptr;   // CPI samples, surveillance
    DeviceComplex*  md_cOut         = nullptr;   // cleaned surveillance
    DeviceComplex*  md_cR           = nullptr;   // residual, CPI samples
    DeviceComplex*  md_cQ           = nullptr;   // R p, CPI samples
    DeviceComplex*  md_cX           = nullptr;   // windows x K, the weights
    DeviceComplex*  md_cP           = nullptr;   // windows x K, search direction
    float*          md_fGamma       = nullptr;   // per window, |s|^2
    float*          md_fPs          = nullptr;   // per window, Re(p^H s)

    bool            mbHaveWeights   = false;
    char            machName[32];
    bool            mbReady         = false;
    bool            mbHostOutput    = true;
};

CglsGpu::~CglsGpu()
{
    release();
}

void CglsGpu::release()
{
    if (md_cRef)   cudaFree(md_cRef);
    if (md_cSur)   cudaFree(md_cSur);
    if (md_cOut)   cudaFree(md_cOut);
    if (md_cR)     cudaFree(md_cR);
    if (md_cQ)     cudaFree(md_cQ);
    if (md_cX)     cudaFree(md_cX);
    if (md_cP)     cudaFree(md_cP);
    if (md_fGamma) cudaFree(md_fGamma);
    if (md_fPs)    cudaFree(md_fPs);
    if (mhCublas)  cublasDestroy(mhCublas);

    md_cRef = md_cSur = md_cOut = md_cR = md_cQ = md_cX = md_cP = nullptr;
    md_fGamma = md_fPs = nullptr;
    mhCublas = nullptr;
    mbReady  = false;
}

bool CglsGpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "CglsGpu::init called twice");
    CHECK(oParams.eAlgorithm == DSIAlgorithm::Cgls, "CglsGpu handles CGLS only");
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uClutterTaps > 0, "clutter taps must be non-zero");
    CHECK(oParams.uUpdateSamples > 0, "the update window must be non-zero");
    CHECK(oParams.uMaxIterations > 0, "CGLS needs at least one iteration");
    CHECK(oParams.uDopplerTaps == 0,
          "ECA_DOPPLER_TAPS > 0 is not implemented; the clutter subspace is "
          "delay-only for now");
    CHECK(oParams.uEstimateSamples == oParams.uUpdateSamples,
          "CGLS estimates from the window it applies to; the estimation and "
          "update windows must be equal");
    CHECK(oParams.uCpiSamples % oParams.uUpdateSamples == 0,
          "the update window must divide the CPI exactly");
    CHECK(oParams.uUpdateSamples > oParams.uClutterTaps,
          "the window must hold more samples than there are taps to solve for");
    CHECK(oParams.uClutterTaps <= kBlock,
          "the GPU adjoint kernel gives one thread per tap and has 256 threads");
    CHECK(oParams.uCpiSamples <= 2147483647ULL,
          "problem dimensions exceed what cuBLAS accepts as int");

    moParams        = oParams;
    muTaps          = oParams.uClutterTaps;
    muWindowSamples = oParams.uUpdateSamples;
    muWindows       = oParams.uCpiSamples / oParams.uUpdateSamples;

    CUBLAS_CHECK(cublasCreate(&mhCublas));

    const size_t uCpiBytes = static_cast<size_t>(oParams.uCpiSamples) * sizeof(DeviceComplex);
    const size_t uWBytes   = static_cast<size_t>(muWindows * muTaps) * sizeof(DeviceComplex);

    // Every allocation happens here and is reused for the life of the object.
    CUDA_CHECK(cudaMalloc(&md_cRef,   uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cSur,   uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cOut,   uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cR,     uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cQ,     uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cX,     uWBytes));
    CUDA_CHECK(cudaMalloc(&md_cP,     uWBytes));
    CUDA_CHECK(cudaMalloc(&md_fGamma, static_cast<size_t>(muWindows) * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&md_fPs,    static_cast<size_t>(muWindows) * sizeof(float)));
    CUDA_CHECK(cudaMemset(md_cX, 0, uWBytes));

    std::snprintf(machName, sizeof(machName), "CGLS (GPU)");

    mbReady = true;
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

    const uint32_t uTotal  = static_cast<uint32_t>(moParams.uCpiSamples);
    const uint32_t uTaps   = static_cast<uint32_t>(muTaps);
    const uint32_t uWindow = static_cast<uint32_t>(muWindowSamples);
    const uint32_t uWins   = static_cast<uint32_t>(muWindows);
    const size_t   uCpiBytes = static_cast<size_t>(moParams.uCpiSamples) * sizeof(DeviceComplex);

    CUDA_CHECK(cudaMemcpy(md_cRef, reinterpret_cast<const DeviceComplex*>(pcRef),
                          uCpiBytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(md_cSur, reinterpret_cast<const DeviceComplex*>(pcSur),
                          uCpiBytes, cudaMemcpyHostToDevice));

    // Start from zero, or from where each window ended up last CPI.
    if (!moParams.bCglsPrime || !mbHaveWeights) {
        CUDA_CHECK(cudaMemset(md_cX, 0,
                              static_cast<size_t>(muWindows * muTaps) * sizeof(DeviceComplex)));
    }

    // r = b - R x;  s = R^H r;  p = s;  gamma = |s|^2;  ps = |s|^2
    convolveKernel<<<blocksFor(uTotal, kBlock), kBlock>>>(
        md_cRef, md_cX, md_cSur, md_cR, uTotal, uTaps, uWindow);
    CUDA_CHECK(cudaGetLastError());
    adjointKernel<<<uWins, kBlock>>>(md_cRef, md_cR, md_cP, md_fGamma, md_fPs,
                                     uTaps, uWindow, 1);
    CUDA_CHECK(cudaGetLastError());

    for (uint64_t it = 1; it <= moParams.uMaxIterations; ++it) {
        const bool bRefresh = (it % kResidualRefresh == 0);

        convolveKernel<<<blocksFor(uTotal, kBlock), kBlock>>>(
            md_cRef, md_cP, nullptr, md_cQ, uTotal, uTaps, uWindow);
        CUDA_CHECK(cudaGetLastError());

        stepKernel<<<uWins, kBlock>>>(md_cQ, md_cR, md_cP, md_cX, md_fPs,
                                      uTaps, uWindow, bRefresh ? 0 : 1);
        CUDA_CHECK(cudaGetLastError());

        if (bRefresh) {
            convolveKernel<<<blocksFor(uTotal, kBlock), kBlock>>>(
                md_cRef, md_cX, md_cSur, md_cR, uTotal, uTaps, uWindow);
            CUDA_CHECK(cudaGetLastError());
        }

        adjointKernel<<<uWins, kBlock>>>(md_cRef, md_cR, md_cP, md_fGamma, md_fPs,
                                         uTaps, uWindow, 0);
        CUDA_CHECK(cudaGetLastError());
    }

    // The output is b - R x computed fresh, as on the CPU.
    convolveKernel<<<blocksFor(uTotal, kBlock), kBlock>>>(
        md_cRef, md_cX, md_cSur, md_cOut, uTotal, uTaps, uWindow);
    CUDA_CHECK(cudaGetLastError());

    // Suppression from the two norms; cublasScnrm2 synchronises, so any
    // kernel error has surfaced by the time it returns.
    float fSurNorm = 0.0f;
    float fOutNorm = 0.0f;
    CUBLAS_CHECK(cublasScnrm2(mhCublas, static_cast<int>(uTotal), md_cSur, 1, &fSurNorm));
    CUBLAS_CHECK(cublasScnrm2(mhCublas, static_cast<int>(uTotal), md_cOut, 1, &fOutNorm));

    if (mbHostOutput) {
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<DeviceComplex*>(pcOut), md_cOut,
                              uCpiBytes, cudaMemcpyDeviceToHost));
    }
    CUDA_CHECK(cudaGetLastError());

    mbHaveWeights = true;

    oStats.dSuppressionDb = (fSurNorm > 0.0f && fOutNorm > 0.0f)
        ? 20.0 * std::log10(static_cast<double>(fSurNorm) /
                            static_cast<double>(fOutNorm))
        : 0.0;
    oStats.uIterations = moParams.uMaxIterations;

    return true;
}

} // namespace

std::unique_ptr<DSICanceller> makeCglsCancellerGpu()
{
    return std::unique_ptr<DSICanceller>(new CglsGpu());
}

} // namespace pbr
