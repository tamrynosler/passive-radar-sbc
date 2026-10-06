// ECA-CD on the GPU: the implementation the thesis measures.
//
// Same algorithm as src/dsi/cpu/EcaCdCpu.cpp, which is the correctness
// reference and explains what it does and what it approximates. This file
// explains only what is different because it runs on a GPU.
//
// ---------------------------------------------------------------------------
// One library call and one kernel
// ---------------------------------------------------------------------------
//
//   cuFFT             M batched transforms of length L, forward on both
//                     channels and inverse on the cleaned one
//   projectKernel     one block per carrier: accumulate Q^H Q and Q^H y across
//                     the batches, solve the (2P+1)-square system in thread 0,
//                     subtract Q a from the carrier
//
// The carriers are independent, so L blocks run with nothing to share. The
// 1/L that cuFFT's inverse omits is folded into the subtraction, which
// touches every element of the spectrum anyway.
//
// ---------------------------------------------------------------------------
// Agreement with the CPU
// ---------------------------------------------------------------------------
//
// cuFFT and the CPU's radix-2 order their butterflies differently, so the two
// spectra differ at rounding level and the outputs cannot be bit-identical.
// The batch twiddle table is computed once on the host in double and rounded
// to float, the same values the CPU's FP32 mode uses, so that is not a source
// of difference. tests/test_ecacd.cpp measures what remains.

#include <cublas_v2.h>
#include <cufft.h>

#include "pbr/DSICanceller.hpp"
#include "pbr/check.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pbr {

namespace {

const uint32_t kBlock         = 256;
const uint32_t kMaxDopplerTaps = 3;
const uint32_t kMaxCols        = 2 * kMaxDopplerTaps + 1;

// Sum fValue over the block; every thread gets the total.
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

// Index into the M-point table of exp(+j 2 pi i / M) for exp(j 2 pi p m / M),
// p possibly negative.
__device__ uint32_t twiddleIndex(int iP, uint32_t m, uint32_t uM)
{
    const uint32_t uAbs = static_cast<uint32_t>(iP < 0 ? -iP : iP);
    const uint32_t uPos = static_cast<uint32_t>(
        (static_cast<unsigned long long>(uAbs) * m) % uM);
    return (iP < 0 && uPos != 0) ? (uM - uPos) : uPos;
}

// ---------------------------------------------------------------------------
// The projection, one block per carrier k.
//
// Threads stride over the batches accumulating
//     g[d] = sum |R_m|^2 e^{+j 2 pi d m / M},        d = 0..2P
//     v[p] = sum conj(R_m) e^{-j 2 pi p m / M} S_m,  p = -P..P
// so that (Q^H Q)(a, b) = g[b - a] (Toeplitz Hermitian) and v = Q^H y. After
// a block reduction thread 0 solves (Q^H Q) a = v by Gaussian elimination
// with partial pivoting; then every thread subtracts Q a from its batches of
// S and applies the 1/L. A zero pivot means the reference has no energy at
// this carrier; the carrier is left alone.
// ---------------------------------------------------------------------------
__global__ void projectKernel(const cufftComplex* pcRefSpec, cufftComplex* pcSur,
                              const cufftComplex* pcTwiddle,
                              uint32_t uBatch, uint32_t uBatches, int iP,
                              float fInvL)
{
    __shared__ float        afShared[kBlock];
    __shared__ cufftComplex acG[kMaxCols];
    __shared__ cufftComplex acV[kMaxCols];
    __shared__ cufftComplex acA[kMaxCols];
    __shared__ int          iSingular;

    const uint32_t k     = blockIdx.x;
    const uint32_t uCols = 2 * static_cast<uint32_t>(iP) + 1;

    cufftComplex acPartG[kMaxCols];
    cufftComplex acPartV[kMaxCols];
    for (uint32_t u = 0; u < kMaxCols; ++u) {
        acPartG[u] = make_cuComplex(0.0f, 0.0f);
        acPartV[u] = make_cuComplex(0.0f, 0.0f);
    }

    for (uint32_t m = threadIdx.x; m < uBatches; m += kBlock) {
        const cufftComplex cR = pcRefSpec[static_cast<size_t>(m) * uBatch + k];
        const cufftComplex cS = pcSur[static_cast<size_t>(m) * uBatch + k];
        const float fPower = cR.x * cR.x + cR.y * cR.y;
        const cufftComplex cRS = make_cuComplex(cR.x * cS.x + cR.y * cS.y,
                                                cR.x * cS.y - cR.y * cS.x);   // conj(R) S
        for (uint32_t d = 0; d < uCols; ++d) {
            const cufftComplex cTw = pcTwiddle[twiddleIndex(static_cast<int>(d), m, uBatches)];
            acPartG[d].x += fPower * cTw.x;
            acPartG[d].y += fPower * cTw.y;
        }
        for (int p = -iP; p <= iP; ++p) {
            const cufftComplex cTw = pcTwiddle[twiddleIndex(-p, m, uBatches)];
            acPartV[p + iP] = cuCaddf(acPartV[p + iP], cuCmulf(cRS, cTw));
        }
    }

    for (uint32_t u = 0; u < uCols; ++u) {
        const float fGx = blockSum(acPartG[u].x, afShared);
        const float fGy = blockSum(acPartG[u].y, afShared);
        const float fVx = blockSum(acPartV[u].x, afShared);
        const float fVy = blockSum(acPartV[u].y, afShared);
        if (threadIdx.x == 0) {
            acG[u] = make_cuComplex(fGx, fGy);
            acV[u] = make_cuComplex(fVx, fVy);
        }
    }
    __syncthreads();

    if (threadIdx.x == 0) {
        cufftComplex aM[kMaxCols][kMaxCols];
        cufftComplex aX[kMaxCols];
        for (uint32_t a = 0; a < uCols; ++a) {
            for (uint32_t b = 0; b < uCols; ++b)
                aM[a][b] = (b >= a) ? acG[b - a] : cuConjf(acG[a - b]);
            aX[a] = acV[a];
        }

        int iBad = 0;
        for (uint32_t c = 0; c < uCols && !iBad; ++c) {
            uint32_t uPivot = c;
            for (uint32_t r = c + 1; r < uCols; ++r)
                if (cuCabsf(aM[r][c]) > cuCabsf(aM[uPivot][c]))
                    uPivot = r;
            if (!(cuCabsf(aM[uPivot][c]) > 0.0f)) {
                iBad = 1;
                break;
            }
            if (uPivot != c) {
                for (uint32_t b = 0; b < uCols; ++b) {
                    const cufftComplex t = aM[c][b]; aM[c][b] = aM[uPivot][b]; aM[uPivot][b] = t;
                }
                const cufftComplex t = aX[c]; aX[c] = aX[uPivot]; aX[uPivot] = t;
            }
            for (uint32_t r = c + 1; r < uCols; ++r) {
                const cufftComplex cF = cuCdivf(aM[r][c], aM[c][c]);
                for (uint32_t b = c; b < uCols; ++b)
                    aM[r][b] = cuCsubf(aM[r][b], cuCmulf(cF, aM[c][b]));
                aX[r] = cuCsubf(aX[r], cuCmulf(cF, aX[c]));
            }
        }
        if (!iBad) {
            for (uint32_t c = uCols; c-- > 0; ) {
                cufftComplex cSum = aX[c];
                for (uint32_t b = c + 1; b < uCols; ++b)
                    cSum = cuCsubf(cSum, cuCmulf(aM[c][b], aX[b]));
                aX[c] = cuCdivf(cSum, aM[c][c]);
            }
            for (uint32_t u = 0; u < uCols; ++u)
                acA[u] = aX[u];
        }
        iSingular = iBad;
    }
    __syncthreads();

    for (uint32_t m = threadIdx.x; m < uBatches; m += kBlock) {
        const size_t u = static_cast<size_t>(m) * uBatch + k;
        cufftComplex cS = pcSur[u];
        if (!iSingular) {
            const cufftComplex cR = pcRefSpec[u];
            cufftComplex cClutter = make_cuComplex(0.0f, 0.0f);
            for (int p = -iP; p <= iP; ++p) {
                const cufftComplex cTw = pcTwiddle[twiddleIndex(p, m, uBatches)];
                cClutter = cuCaddf(cClutter, cuCmulf(cuCmulf(acA[p + iP], cR), cTw));
            }
            cS = cuCsubf(cS, cClutter);
        }
        pcSur[u] = make_cuComplex(cS.x * fInvL, cS.y * fInvL);
    }
}

// ---------------------------------------------------------------------------

class EcaCdGpu : public DSICanceller {
public:
    EcaCdGpu() { std::memset(machName, 0, sizeof(machName)); }
    ~EcaCdGpu() override;

    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override { return machName; }

    const void* deviceReference() const override { return md_cRef; }
    const void* deviceOutput()    const override { return md_cOut; }
    void setHostOutputEnabled(bool bEnabled) override { mbHostOutput = bEnabled; }

private:
    void release();

    DSIParams      moParams;
    uint64_t       muBatch     = 0;   // L
    uint64_t       muBatches   = 0;   // M

    cublasHandle_t mhCublas    = nullptr;
    cufftHandle    mhPlan      = 0;
    bool           mbPlanValid = false;

    cufftComplex*  md_cRef     = nullptr;   // time domain, kept for the CAF
    cufftComplex*  md_cSur     = nullptr;   // time domain
    cufftComplex*  md_cRefSpec = nullptr;   // [m * L + k]
    cufftComplex*  md_cOut     = nullptr;   // spectrum during, time domain after
    cufftComplex*  md_cTwiddle = nullptr;   // M: exp(+j 2 pi i / M)

    char           machName[32];
    bool           mbReady     = false;
    bool           mbHostOutput = true;
};

EcaCdGpu::~EcaCdGpu()
{
    release();
}

void EcaCdGpu::release()
{
    if (md_cRef)     cudaFree(md_cRef);
    if (md_cSur)     cudaFree(md_cSur);
    if (md_cRefSpec) cudaFree(md_cRefSpec);
    if (md_cOut)     cudaFree(md_cOut);
    if (md_cTwiddle) cudaFree(md_cTwiddle);
    if (mbPlanValid) cufftDestroy(mhPlan);
    if (mhCublas)    cublasDestroy(mhCublas);

    md_cRef = md_cSur = md_cRefSpec = md_cOut = md_cTwiddle = nullptr;
    mhCublas    = nullptr;
    mbPlanValid = false;
    mbReady     = false;
}

bool EcaCdGpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "EcaCdGpu::init called twice");
    CHECK(oParams.eAlgorithm == DSIAlgorithm::EcaCd, "EcaCdGpu handles ECA-CD only");
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uCdBatchSamples > 0, "the ECA-CD batch must be non-zero");
    CHECK((oParams.uCdBatchSamples & (oParams.uCdBatchSamples - 1)) == 0,
          "the ECA-CD batch length must be a power of two (radix-2 FFT)");
    CHECK(oParams.uCpiSamples % oParams.uCdBatchSamples == 0,
          "the ECA-CD batch must divide the CPI exactly");
    CHECK(oParams.uCdDopplerTaps <= kMaxDopplerTaps,
          "ECA_CD_DOPPLER_TAPS is at most 3");
    CHECK(oParams.uCpiSamples <= 2147483647ULL,
          "problem dimensions exceed what cuFFT accepts as int");

    moParams  = oParams;
    muBatch   = oParams.uCdBatchSamples;
    muBatches = oParams.uCpiSamples / oParams.uCdBatchSamples;

    CHECK(muBatches > 2 * oParams.uCdDopplerTaps + 1,
          "too few ECA-CD batches for the Doppler columns; shorten the batch "
          "or reduce ECA_CD_DOPPLER_TAPS");

    CUDA_CHECK(cublasCreate(&mhCublas));

    // M transforms of length L, each contiguous.
    int iL = static_cast<int>(muBatch);
    CUDA_CHECK(cufftPlanMany(&mhPlan, 1, &iL,
                             nullptr, 1, iL,
                             nullptr, 1, iL,
                             CUFFT_C2C,
                             static_cast<int>(muBatches)));
    mbPlanValid = true;

    const size_t uCpiBytes = static_cast<size_t>(oParams.uCpiSamples) * sizeof(cufftComplex);
    CUDA_CHECK(cudaMalloc(&md_cRef,     uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cSur,     uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cRefSpec, uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cOut,     uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cTwiddle, static_cast<size_t>(muBatches) * sizeof(cufftComplex)));

    // Same values as the CPU's FP32 table: computed in double, rounded once.
    {
        const double kPi = 3.14159265358979323846;
        std::vector<cufftComplex> vc(static_cast<size_t>(muBatches));
        for (uint64_t i = 0; i < muBatches; ++i) {
            const double dAngle = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(muBatches);
            vc[static_cast<size_t>(i)] = make_cuComplex(static_cast<float>(std::cos(dAngle)),
                                                        static_cast<float>(std::sin(dAngle)));
        }
        CUDA_CHECK(cudaMemcpy(md_cTwiddle, vc.data(), vc.size() * sizeof(cufftComplex),
                              cudaMemcpyHostToDevice));
    }

    std::snprintf(machName, sizeof(machName), "ECA-CD (GPU)");
    mbReady = true;
    return true;
}

bool EcaCdGpu::cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                      ComplexF* pcOut, DSIStats& oStats)
{
    CHECK(mbReady, "EcaCdGpu::cancel before a successful init");
    CHECK(pcRef != nullptr && pcSur != nullptr,
          "null input buffer passed to EcaCdGpu::cancel");
    CHECK(pcOut != nullptr || !mbHostOutput,
          "null output buffer with host output enabled");
    CHECK(pcOut != pcRef && pcOut != pcSur,
          "the output buffer must not alias either input");

    oStats = DSIStats();

    const size_t uCpiBytes = static_cast<size_t>(moParams.uCpiSamples) * sizeof(cufftComplex);
    CUDA_CHECK(cudaMemcpy(md_cRef, reinterpret_cast<const cufftComplex*>(pcRef),
                          uCpiBytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(md_cSur, reinterpret_cast<const cufftComplex*>(pcSur),
                          uCpiBytes, cudaMemcpyHostToDevice));

    // Forward, out of place so the time-domain reference survives for the CAF.
    CUDA_CHECK(cufftExecC2C(mhPlan, md_cRef, md_cRefSpec, CUFFT_FORWARD));
    CUDA_CHECK(cufftExecC2C(mhPlan, md_cSur, md_cOut, CUFFT_FORWARD));

    projectKernel<<<static_cast<uint32_t>(muBatch), kBlock>>>(
        md_cRefSpec, md_cOut, md_cTwiddle,
        static_cast<uint32_t>(muBatch), static_cast<uint32_t>(muBatches),
        static_cast<int>(moParams.uCdDopplerTaps),
        1.0f / static_cast<float>(muBatch));
    CUDA_CHECK(cudaGetLastError());

    // Back to time, in place. The 1/L was applied by the kernel.
    CUDA_CHECK(cufftExecC2C(mhPlan, md_cOut, md_cOut, CUFFT_INVERSE));

    float fSurNorm = 0.0f;
    float fOutNorm = 0.0f;
    CUDA_CHECK(cublasScnrm2(mhCublas, static_cast<int>(moParams.uCpiSamples),
                              md_cSur, 1, &fSurNorm));
    CUDA_CHECK(cublasScnrm2(mhCublas, static_cast<int>(moParams.uCpiSamples),
                              md_cOut, 1, &fOutNorm));

    if (mbHostOutput) {
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<cufftComplex*>(pcOut), md_cOut,
                              uCpiBytes, cudaMemcpyDeviceToHost));
    }
    CUDA_CHECK(cudaGetLastError());

    oStats.dSuppressionDb = (fSurNorm > 0.0f && fOutNorm > 0.0f)
        ? 20.0 * std::log10(static_cast<double>(fSurNorm) /
                            static_cast<double>(fOutNorm))
        : 0.0;
    oStats.uIterations = 0;

    return true;
}

} // namespace

std::unique_ptr<DSICanceller> makeEcaCdCancellerGpu()
{
    return std::unique_ptr<DSICanceller>(new EcaCdGpu());
}

} // namespace pbr
