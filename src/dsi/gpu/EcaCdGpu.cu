// ECA-CD on the GPU: the implementation the thesis measures.
//
// Same algorithm as src/dsi/cpu/EcaCdCpu.cpp, which is the correctness
// reference and explains what it does and what it approximates. This file
// explains only what is different because it runs on a GPU.
//
// ---------------------------------------------------------------------------
// cuFFT and one kernel
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
// touches every element of the spectrum anyway. The suppression figure comes
// from two cuBLAS norms, as for every GPU canceller.
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

#include "pbr/DSICanceller.hpp"
#include "../DsiInternal.hpp"
#include "GpuCommon.cuh"

#include <climits>
#include <vector>

namespace pbr {

namespace {

const uint32_t kMaxCols = 2 * kMaxCdDopplerTaps + 1;

// Index into the M-point table of exp(+j 2 pi i / M) for exp(j 2 pi p m / M),
// p possibly negative.
__device__ uint32_t twiddleIndex(int iP, uint32_t m, uint32_t uM)
{
    const uint32_t uAbs = static_cast<uint32_t>(iP < 0 ? -iP : iP);
    const uint32_t uPos = static_cast<uint32_t>(
        (static_cast<unsigned long long>(uAbs) * m) % uM);
    return (iP < 0 && uPos != 0) ? (uM - uPos) : uPos;
}

// Solve acSystem x = acSolution in place by Gaussian elimination with partial
// pivoting; on return acSolution holds x. The same steps as the CPU's
// solveSmallSystemT. False on a zero pivot: the reference has no energy at
// this carrier, and the carrier is left alone.
__device__ bool solveSmallSystem(cufftComplex acSystem[kMaxCols][kMaxCols],
                                 cufftComplex acSolution[kMaxCols], uint32_t uCols)
{
    for (uint32_t uCol = 0; uCol < uCols; ++uCol) {
        uint32_t uPivot = uCol;
        for (uint32_t uRow = uCol + 1; uRow < uCols; ++uRow)
            if (cuCabsf(acSystem[uRow][uCol]) > cuCabsf(acSystem[uPivot][uCol]))
                uPivot = uRow;
        if (!(cuCabsf(acSystem[uPivot][uCol]) > 0.0f))
            return false;
        if (uPivot != uCol) {
            for (uint32_t u = 0; u < uCols; ++u) {
                const cufftComplex cSwap = acSystem[uCol][u];
                acSystem[uCol][u] = acSystem[uPivot][u];
                acSystem[uPivot][u] = cSwap;
            }
            const cufftComplex cSwap = acSolution[uCol];
            acSolution[uCol] = acSolution[uPivot];
            acSolution[uPivot] = cSwap;
        }
        for (uint32_t uRow = uCol + 1; uRow < uCols; ++uRow) {
            const cufftComplex cFactor = cuCdivf(acSystem[uRow][uCol], acSystem[uCol][uCol]);
            for (uint32_t u = uCol; u < uCols; ++u)
                acSystem[uRow][u] = cuCsubf(acSystem[uRow][u], cuCmulf(cFactor, acSystem[uCol][u]));
            acSolution[uRow] = cuCsubf(acSolution[uRow], cuCmulf(cFactor, acSolution[uCol]));
        }
    }
    for (uint32_t uCol = uCols; uCol-- > 0; ) {
        cufftComplex cSum = acSolution[uCol];
        for (uint32_t u = uCol + 1; u < uCols; ++u)
            cSum = cuCsubf(cSum, cuCmulf(acSystem[uCol][u], acSolution[u]));
        acSolution[uCol] = cuCdivf(cSum, acSystem[uCol][uCol]);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The projection, one block per carrier k.
//
// Threads stride over the batches accumulating
//     g[d] = sum |R_m|^2 e^{+j 2 pi d m / M},        d = 0..2P
//     v[p] = sum conj(R_m) e^{-j 2 pi p m / M} S_m,  p = -P..P
// so that (Q^H Q)(a, b) = g[b - a] (Toeplitz Hermitian) and v = Q^H y. After
// a block reduction thread 0 solves (Q^H Q) a = v; then every thread
// subtracts Q a from its batches of S and applies the 1/L.
// ---------------------------------------------------------------------------
__global__ void projectKernel(const cufftComplex* d_cRefSpec, cufftComplex* d_cSpec,
                              const cufftComplex* d_cTwiddle,
                              uint32_t uBatch, uint32_t uBatches, int iP,
                              float fInvL)
{
    __shared__ float        afShared[kBlock];
    __shared__ cufftComplex acG[kMaxCols];
    __shared__ cufftComplex acV[kMaxCols];
    __shared__ cufftComplex acWeights[kMaxCols];
    __shared__ bool         bSolved;

    const uint32_t k     = blockIdx.x;
    const uint32_t uCols = 2 * static_cast<uint32_t>(iP) + 1;

    cufftComplex acPartG[kMaxCols];
    cufftComplex acPartV[kMaxCols];
    for (uint32_t u = 0; u < kMaxCols; ++u) {
        acPartG[u] = make_cuComplex(0.0f, 0.0f);
        acPartV[u] = make_cuComplex(0.0f, 0.0f);
    }

    for (uint32_t m = threadIdx.x; m < uBatches; m += kBlock) {
        const cufftComplex cR = d_cRefSpec[static_cast<size_t>(m) * uBatch + k];
        const cufftComplex cS = d_cSpec[static_cast<size_t>(m) * uBatch + k];
        const float fPower = cR.x * cR.x + cR.y * cR.y;
        const cufftComplex cRS = make_cuComplex(cR.x * cS.x + cR.y * cS.y,
                                                cR.x * cS.y - cR.y * cS.x);   // conj(R) S
        for (uint32_t uD = 0; uD < uCols; ++uD) {
            const cufftComplex cTw = d_cTwiddle[twiddleIndex(static_cast<int>(uD), m, uBatches)];
            acPartG[uD].x += fPower * cTw.x;
            acPartG[uD].y += fPower * cTw.y;
        }
        for (int iDoppler = -iP; iDoppler <= iP; ++iDoppler) {
            const cufftComplex cTw = d_cTwiddle[twiddleIndex(-iDoppler, m, uBatches)];
            acPartV[iDoppler + iP] = cuCaddf(acPartV[iDoppler + iP], cuCmulf(cRS, cTw));
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
        cufftComplex acSystem[kMaxCols][kMaxCols];
        cufftComplex acSolution[kMaxCols];
        for (uint32_t uRow = 0; uRow < uCols; ++uRow) {
            for (uint32_t uCol = 0; uCol < uCols; ++uCol)
                acSystem[uRow][uCol] = (uCol >= uRow) ? acG[uCol - uRow] : cuConjf(acG[uRow - uCol]);
            acSolution[uRow] = acV[uRow];
        }
        bSolved = solveSmallSystem(acSystem, acSolution, uCols);
        if (bSolved)
            for (uint32_t u = 0; u < uCols; ++u)
                acWeights[u] = acSolution[u];
    }
    __syncthreads();

    for (uint32_t m = threadIdx.x; m < uBatches; m += kBlock) {
        const size_t u = static_cast<size_t>(m) * uBatch + k;
        cufftComplex cS = d_cSpec[u];
        if (bSolved) {
            const cufftComplex cR = d_cRefSpec[u];
            cufftComplex cClutter = make_cuComplex(0.0f, 0.0f);
            for (int iDoppler = -iP; iDoppler <= iP; ++iDoppler) {
                const cufftComplex cTw = d_cTwiddle[twiddleIndex(iDoppler, m, uBatches)];
                cClutter = cuCaddf(cClutter, cuCmulf(cuCmulf(acWeights[iDoppler + iP], cR), cTw));
            }
            cS = cuCsubf(cS, cClutter);
        }
        d_cSpec[u] = make_cuComplex(cS.x * fInvL, cS.y * fInvL);
    }
}

// ---------------------------------------------------------------------------

class EcaCdGpu : public DSICanceller {
public:
    ~EcaCdGpu() override { release(); }

    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override { return "ECA-CD (GPU)"; }

    const void* deviceReference() const override { return md_cRef; }
    const void* deviceOutput()    const override { return md_cOut; }
    void setHostOutputEnabled(bool bEnabled) override { mbHostOutput = bEnabled; }

private:
    bool setUp(const DSIParams& oParams);   // init() without the clean-up
    void release();

    DSIParams      moParams;
    uint64_t       muBatch     = 0;   // L
    uint64_t       muBatches   = 0;   // M

    cublasHandle_t mhCublas    = nullptr;
    cufftHandle    mhPlan      = 0;
    bool           mbPlanValid = false;

    // All allocated in setUp(), freed in release().
    cufftComplex*  md_cRef     = nullptr;   // time domain, kept for the CAF
    cufftComplex*  md_cSur     = nullptr;   // time domain
    cufftComplex*  md_cRefSpec = nullptr;   // [m * L + k]
    cufftComplex*  md_cOut     = nullptr;   // spectrum during, time domain after
    cufftComplex*  md_cTwiddle = nullptr;   // M: exp(+j 2 pi i / M)

    bool           mbReady     = false;
    bool           mbHostOutput = true;
};

void EcaCdGpu::release()
{
    // Deliberately unchecked, as in EcaGpu::release(). cudaFree(nullptr) is
    // a no-op.
    cudaFree(md_cRef);
    cudaFree(md_cSur);
    cudaFree(md_cRefSpec);
    cudaFree(md_cOut);
    cudaFree(md_cTwiddle);
    if (mbPlanValid)
        cufftDestroy(mhPlan);
    if (mhCublas)
        cublasDestroy(mhCublas);

    md_cRef = md_cSur = md_cRefSpec = md_cOut = md_cTwiddle = nullptr;
    mhCublas    = nullptr;
    mbPlanValid = false;
    mbReady     = false;
}

bool EcaCdGpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "EcaCdGpu::init called twice");
    if (setUp(oParams))
        return true;
    release();   // whatever was allocated before the failure
    return false;
}

bool EcaCdGpu::setUp(const DSIParams& oParams)
{
    if (!ecaCdParamsOk(oParams))
        return false;
    // cuFFT and cuBLAS take int.
    CHECK(oParams.uCpiSamples <= INT_MAX,
          "the CPI is longer than cuFFT and cuBLAS can index");

    moParams  = oParams;
    muBatch   = oParams.uCdBatchSamples;
    muBatches = oParams.uCpiSamples / oParams.uCdBatchSamples;

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

    // The same values as the CPU's FP32 table: computed in double, rounded once.
    const std::vector<std::complex<double>> vcExact = batchTwiddles(muBatches);
    std::vector<cufftComplex> vcTwiddle(vcExact.size());
    for (size_t u = 0; u < vcExact.size(); ++u)
        vcTwiddle[u] = make_cuComplex(static_cast<float>(vcExact[u].real()),
                                      static_cast<float>(vcExact[u].imag()));
    CUDA_CHECK(cudaMemcpy(md_cTwiddle, vcTwiddle.data(), vcTwiddle.size() * sizeof(cufftComplex),
                          cudaMemcpyHostToDevice));

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

    // ComplexF and cufftComplex have the same layout, so the cast is a
    // reinterpretation at the copy boundary only -- never anywhere else.
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

    if (!gpuSuppressionDb(mhCublas, md_cSur, md_cOut, moParams.uCpiSamples,
                          oStats.dSuppressionDb))
        return false;
    if (mbHostOutput) {
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<cufftComplex*>(pcOut), md_cOut,
                              uCpiBytes, cudaMemcpyDeviceToHost));
    }

    oStats.uIterations = 0;   // direct
    return true;
}

} // namespace

std::unique_ptr<DSICanceller> makeEcaCdCancellerGpu()
{
    return std::unique_ptr<DSICanceller>(new EcaCdGpu());
}

} // namespace pbr
