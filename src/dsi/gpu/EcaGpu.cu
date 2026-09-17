// ECA-m and ECA-B on the GPU: the implementation the thesis measures.
//
// Same algorithm as src/dsi/cpu/EcaCpu.cpp, which is the correctness
// reference. Read that file first -- it explains what the algorithm does and
// why. This one explains only what is different because it runs on a GPU.
//
// ---------------------------------------------------------------------------
// Single precision is forced, not chosen
// ---------------------------------------------------------------------------
//
// The Jetson Nano's Maxwell GPU runs double-precision arithmetic at 1/32 the
// single-precision rate. Our workload is about 1.2e10 operations per CPI:
// roughly 0.2 s in single precision, but over 5 s in double -- slower than
// the CPU, and useless. So everything here is FP32, and DSIParams::
// bFloat32Math is ignored.
//
// Measured cost of that, against the MATLAB golden file (see
// docs/design_decisions.md 1d.4):
//
//   double throughout      8.2e-10 relative L2, 51.6193 dB suppression
//   single throughout      4.9e-03 relative L2, 51.6192 dB suppression
//
// The radar answer is identical; the difference is invisible noise deep in
// the residual. It comes almost entirely from rounding the MULTIPLICATIONS,
// not from the order of the additions -- keeping the sums in double while
// rounding the products still gives 4.9e-3. So tree reductions, Kahan
// summation and double accumulators would all buy nothing here, and are not
// attempted.
//
// ---------------------------------------------------------------------------
// What runs where
// ---------------------------------------------------------------------------
//
//   build the delayed-reference matrix   custom kernel (below)
//   Gram matrix, 97% of the work         cuBLAS cherk
//   right-hand side                      cuBLAS cgemv
//   solve all windows at once            cuSOLVER Cpotrf/CpotrsBatched
//   subtract the clutter                 custom kernel (below)
//   suppression figure                   cuBLAS scnrm2
//
// Only two custom kernels, both about ten lines. Everything heavy is a
// library call: less to get wrong, and less to review.
//
// The solve stays on the GPU deliberately. It is only 1.5% of the arithmetic,
// but on the Nano's Cortex-A57 that 1.5% would cost an estimated 75-150 ms
// against a total budget of ~250 ms, with the GPU idle throughout. A single
// precision batched solve costs ~5-15 ms instead, and measurement showed it
// adds no error worth having (4.935e-3 with an FP32 solve against 4.951e-3
// with a double one -- the Gram error already dominates).
//
// ---------------------------------------------------------------------------
// Memory, and the matrix we refuse to build
// ---------------------------------------------------------------------------
//
// cuBLAS needs the delayed-reference matrix R to exist, so unlike the CPU
// version this one does build it -- but only a CHUNK at a time, reusing one
// buffer. Building R for a whole 1 s CPI at once would cost 197 MB on a board
// whose 4 GB is shared with the operating system. A chunk is under 16 MB.
//
// Chunking is also what lets ECA-m (one window covering the whole CPI) use
// exactly the same code as ECA-B: cherk accumulates with beta = 1, so a long
// window is just more chunks summed into the same Gram matrix.
//
// Total device memory is about 25 MB for the shipped parameters.

#include <cublas_v2.h>
#include <cusolverDn.h>

#include "pbr/DSICanceller.hpp"
#include "pbr/check.hpp"   // must follow the library headers: see check.hpp

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pbr {

namespace {

// Largest slice of a window handled in one cuBLAS call. Bounds the R buffer
// at kMaxChunk * K * 8 bytes; 16384 x 120 is 15.7 MB.
const uint64_t kMaxChunkSamples = 16384;

// cuComplex and cufftComplex are the same type (cufft.h typedefs one to the
// other), so buffers allocated here will feed the CAF stage with no
// conversion. cuComplex is used directly to avoid pulling in cuFFT for a
// typedef.
typedef cuComplex DeviceComplex;

// ---------------------------------------------------------------------------
// Kernel 1: build one chunk of the delayed-reference matrix.
//
// Column-major, because that is what cuBLAS expects: element (n, k) sits at
// n + k * uRows. Column k holds the reference delayed by k samples, and is
// zero wherever that would reach back before the start of the CPI -- the same
// rule as the CPU version, and what makes the two agree at the edges.
// ---------------------------------------------------------------------------
__global__ void buildSubspaceKernel(const DeviceComplex* pcRef,
                                    DeviceComplex* pcR,
                                    uint32_t uStart, uint32_t uRows,
                                    uint32_t uTaps)
{
    const uint32_t uIdx = blockIdx.x * blockDim.x + threadIdx.x;
    if (uIdx >= uRows * uTaps)
        return;

    const uint32_t n = uIdx % uRows;        // row: sample within the chunk
    const uint32_t k = uIdx / uRows;        // column: delay in samples
    const uint32_t uAbs = uStart + n;       // sample index within the CPI

    DeviceComplex c;
    c.x = 0.0f;
    c.y = 0.0f;
    if (uAbs >= k)
        c = pcRef[uAbs - k];

    pcR[uIdx] = c;
}

// ---------------------------------------------------------------------------
// Kernel 2: diagonal loading.
//
// One thread per window. Each walks its own 120-entry diagonal, so the whole
// launch is 40 threads doing 120 reads each -- far too small to be worth
// parallelising further, and simple enough to read at a glance.
//
// The trace is summed in float so that the lambda matches what the CPU's FP32
// mode computes. A lambda that differed would show up as a disagreement
// between the two backends that had nothing to do with the code being
// compared.
// ---------------------------------------------------------------------------
__global__ void addLoadingKernel(DeviceComplex* pcGram, uint32_t uTaps,
                                 uint32_t uWindows, float fLoading,
                                 int iAbsolute)
{
    const uint32_t b = blockIdx.x * blockDim.x + threadIdx.x;
    if (b >= uWindows)
        return;

    DeviceComplex* pcG = pcGram + static_cast<size_t>(b) * uTaps * uTaps;

    float fLambda = fLoading;
    if (!iAbsolute) {
        float fTrace = 0.0f;
        for (uint32_t i = 0; i < uTaps; ++i)
            fTrace += pcG[i + i * uTaps].x;
        fLambda = fLoading * (fTrace / static_cast<float>(uTaps));
    }

    for (uint32_t i = 0; i < uTaps; ++i)
        pcG[i + i * uTaps].x += fLambda;
}

// ---------------------------------------------------------------------------
// Kernel 3: subtract the clutter.
//
// out[n] = s[n] - sum over k of w[k] * r[n-k], one thread per output sample,
// every window in a single launch. Written directly rather than as another
// matrix multiply so that R does not have to be rebuilt after the solve.
// ---------------------------------------------------------------------------
__global__ void applyWeightsKernel(const DeviceComplex* pcRef,
                                   const DeviceComplex* pcSur,
                                   const DeviceComplex* pcWeights,
                                   DeviceComplex* pcOut,
                                   uint32_t uTotal, uint32_t uTaps,
                                   uint32_t uWindow)
{
    const uint32_t n = blockIdx.x * blockDim.x + threadIdx.x;
    if (n >= uTotal)
        return;

    const DeviceComplex* pcW = pcWeights +
                               static_cast<size_t>(n / uWindow) * uTaps;

    // Taps that would reach back before the start of the CPI contribute
    // nothing, because the reference is zero there.
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
    cOut.x = pcSur[n].x - fRe;
    cOut.y = pcSur[n].y - fIm;
    pcOut[n] = cOut;
}

uint32_t blocksFor(uint64_t uThreads, uint32_t uBlockSize)
{
    return static_cast<uint32_t>((uThreads + uBlockSize - 1) / uBlockSize);
}

// ---------------------------------------------------------------------------

class EcaGpu : public DSICanceller {
public:
    EcaGpu() { std::memset(machName, 0, sizeof(machName)); }
    ~EcaGpu() override;

    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override { return machName; }

private:
    void release();

    DSIParams          moParams;
    uint64_t           muTaps         = 0;
    uint64_t           muWindows      = 0;
    uint64_t           muWindowSamples= 0;
    uint64_t           muChunkSamples = 0;

    cublasHandle_t     mhCublas       = nullptr;
    cusolverDnHandle_t mhCusolver     = nullptr;

    DeviceComplex*     md_cRef        = nullptr;   // CPI samples, reference
    DeviceComplex*     md_cSur        = nullptr;   // CPI samples, surveillance
    DeviceComplex*     md_cOut        = nullptr;   // cleaned surveillance
    DeviceComplex*     md_cSubspace   = nullptr;   // one chunk of R
    DeviceComplex*     md_cGram       = nullptr;   // windows x K x K
    DeviceComplex*     md_cWeights    = nullptr;   // windows x K (RHS, then w)
    DeviceComplex**    md_ppGram      = nullptr;   // pointer array for cuSOLVER
    DeviceComplex**    md_ppWeights   = nullptr;
    int*               md_piInfo      = nullptr;

    char               machName[32];
    bool               mbReady        = false;
};

EcaGpu::~EcaGpu()
{
    release();
}

void EcaGpu::release()
{
    // Destructors cannot report failure and nothing can be done about a failed
    // free anyway, so these are deliberately unchecked.
    if (md_cRef)      cudaFree(md_cRef);
    if (md_cSur)      cudaFree(md_cSur);
    if (md_cOut)      cudaFree(md_cOut);
    if (md_cSubspace) cudaFree(md_cSubspace);
    if (md_cGram)     cudaFree(md_cGram);
    if (md_cWeights)  cudaFree(md_cWeights);
    if (md_ppGram)    cudaFree(md_ppGram);
    if (md_ppWeights) cudaFree(md_ppWeights);
    if (md_piInfo)    cudaFree(md_piInfo);
    if (mhCublas)     cublasDestroy(mhCublas);
    if (mhCusolver)   cusolverDnDestroy(mhCusolver);

    md_cRef = md_cSur = md_cOut = md_cSubspace = md_cGram = md_cWeights = nullptr;
    md_ppGram = md_ppWeights = nullptr;
    md_piInfo = nullptr;
    mhCublas  = nullptr;
    mhCusolver = nullptr;
    mbReady = false;
}

bool EcaGpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "EcaGpu::init called twice");

    CHECK(oParams.eAlgorithm == DSIAlgorithm::EcaM ||
          oParams.eAlgorithm == DSIAlgorithm::EcaB,
          "EcaGpu handles ECA-m and ECA-B only");
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uClutterTaps > 0, "clutter taps must be non-zero");
    CHECK(oParams.uEstimateSamples > 0 && oParams.uUpdateSamples > 0,
          "estimation and update windows must be non-zero");
    CHECK(oParams.uDopplerTaps == 0,
          "ECA_DOPPLER_TAPS > 0 is not implemented; the clutter subspace is "
          "delay-only for now");
    CHECK(oParams.uEstimateSamples == oParams.uUpdateSamples,
          "an estimation window longer than the update window is ECA-S, which "
          "is not implemented");
    CHECK(oParams.uCpiSamples % oParams.uUpdateSamples == 0,
          "the update window must divide the CPI exactly");
    CHECK(oParams.uEstimateSamples > oParams.uClutterTaps,
          "the estimation window must hold more samples than there are taps "
          "to solve for");
    CHECK(oParams.dDiagonalLoading >= 0.0, "diagonal loading must not be negative");

    // cuBLAS and cuSOLVER take int, not uint64_t, for every dimension.
    CHECK(oParams.uClutterTaps <= 2147483647ULL &&
          oParams.uCpiSamples  <= 2147483647ULL,
          "problem dimensions exceed what cuBLAS accepts as int");

    moParams        = oParams;
    muTaps          = oParams.uClutterTaps;
    muWindowSamples = oParams.uUpdateSamples;
    muWindows       = oParams.uCpiSamples / oParams.uUpdateSamples;
    muChunkSamples  = (muWindowSamples < kMaxChunkSamples) ? muWindowSamples
                                                           : kMaxChunkSamples;

    CUBLAS_CHECK(cublasCreate(&mhCublas));
    CUSOLVER_CHECK(cusolverDnCreate(&mhCusolver));

    const size_t uCpiBytes  = static_cast<size_t>(oParams.uCpiSamples) * sizeof(DeviceComplex);
    const size_t uSubBytes  = static_cast<size_t>(muChunkSamples * muTaps) * sizeof(DeviceComplex);
    const size_t uGramBytes = static_cast<size_t>(muWindows * muTaps * muTaps) * sizeof(DeviceComplex);
    const size_t uWBytes    = static_cast<size_t>(muWindows * muTaps) * sizeof(DeviceComplex);

    // Every allocation happens here and is reused for the life of the object.
    // Nothing allocates inside cancel(), or the benchmark would be timing
    // cudaMalloc.
    CUDA_CHECK(cudaMalloc(&md_cRef,      uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cSur,      uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cOut,      uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cSubspace, uSubBytes));
    CUDA_CHECK(cudaMalloc(&md_cGram,     uGramBytes));
    CUDA_CHECK(cudaMalloc(&md_cWeights,  uWBytes));
    CUDA_CHECK(cudaMalloc(&md_ppGram,    static_cast<size_t>(muWindows) * sizeof(DeviceComplex*)));
    CUDA_CHECK(cudaMalloc(&md_ppWeights, static_cast<size_t>(muWindows) * sizeof(DeviceComplex*)));
    CUDA_CHECK(cudaMalloc(&md_piInfo,    static_cast<size_t>(muWindows) * sizeof(int)));

    // cuSOLVER's batched routines take an array of pointers, one per problem.
    // The pointers never change, so they are built once here rather than on
    // every CPI.
    std::vector<DeviceComplex*> vpGram(static_cast<size_t>(muWindows));
    std::vector<DeviceComplex*> vpWeights(static_cast<size_t>(muWindows));
    for (uint64_t b = 0; b < muWindows; ++b) {
        vpGram[static_cast<size_t>(b)]    = md_cGram + b * muTaps * muTaps;
        vpWeights[static_cast<size_t>(b)] = md_cWeights + b * muTaps;
    }
    CUDA_CHECK(cudaMemcpy(md_ppGram, vpGram.data(),
                          vpGram.size() * sizeof(DeviceComplex*),
                          cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(md_ppWeights, vpWeights.data(),
                          vpWeights.size() * sizeof(DeviceComplex*),
                          cudaMemcpyHostToDevice));

    std::snprintf(machName, sizeof(machName), "%s (GPU)",
                  dsiAlgorithmName(oParams.eAlgorithm));

    mbReady = true;
    return true;
}

bool EcaGpu::cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                    ComplexF* pcOut, DSIStats& oStats)
{
    CHECK(mbReady, "EcaGpu::cancel before a successful init");
    CHECK(pcRef != nullptr && pcSur != nullptr && pcOut != nullptr,
          "null buffer passed to EcaGpu::cancel");
    CHECK(pcOut != pcRef && pcOut != pcSur,
          "the output buffer must not alias either input");

    oStats = DSIStats();

    const int    iTaps    = static_cast<int>(muTaps);
    const size_t uCpiBytes = static_cast<size_t>(moParams.uCpiSamples) * sizeof(DeviceComplex);

    // ComplexF and DeviceComplex have the same layout, so the cast is a
    // reinterpretation at the copy boundary only -- never anywhere else.
    CUDA_CHECK(cudaMemcpy(md_cRef, reinterpret_cast<const DeviceComplex*>(pcRef),
                          uCpiBytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(md_cSur, reinterpret_cast<const DeviceComplex*>(pcSur),
                          uCpiBytes, cudaMemcpyHostToDevice));

    // ---- normal equations, one window at a time ---------------------------
    // Everything below is queued on the default stream without a single
    // synchronise, so the GPU never waits for the host between windows.
    const cuComplex cOne  = make_cuComplex(1.0f, 0.0f);
    const cuComplex cZero = make_cuComplex(0.0f, 0.0f);
    const float     fOne  = 1.0f;
    const float     fZero = 0.0f;

    for (uint64_t b = 0; b < muWindows; ++b) {
        const uint64_t uWindowStart = b * muWindowSamples;

        for (uint64_t uOffset = 0; uOffset < muWindowSamples; uOffset += muChunkSamples) {
            const uint64_t uRows = (uOffset + muChunkSamples <= muWindowSamples)
                                 ? muChunkSamples
                                 : (muWindowSamples - uOffset);
            const uint64_t uStart = uWindowStart + uOffset;
            const bool     bFirst = (uOffset == 0);

            const uint32_t uBlock = 256;
            buildSubspaceKernel<<<blocksFor(uRows * muTaps, uBlock), uBlock>>>(
                md_cRef, md_cSubspace, static_cast<uint32_t>(uStart),
                static_cast<uint32_t>(uRows), static_cast<uint32_t>(muTaps));
            CUDA_CHECK(cudaGetLastError());

            // G += R^H R over this chunk. cherk knows the result is Hermitian
            // and fills only the lower triangle, which is the same halving of
            // the work the CPU version does by hand. beta = 1 after the first
            // chunk is what lets a long window be summed piece by piece.
            CUBLAS_CHECK(cublasCherk(mhCublas,
                                     CUBLAS_FILL_MODE_LOWER, CUBLAS_OP_C,
                                     iTaps, static_cast<int>(uRows),
                                     &fOne, md_cSubspace, static_cast<int>(uRows),
                                     bFirst ? &fZero : &fOne,
                                     md_cGram + b * muTaps * muTaps, iTaps));

            // v += R^H s over this chunk.
            CUBLAS_CHECK(cublasCgemv(mhCublas, CUBLAS_OP_C,
                                     static_cast<int>(uRows), iTaps,
                                     &cOne, md_cSubspace, static_cast<int>(uRows),
                                     md_cSur + uStart, 1,
                                     bFirst ? &cZero : &cOne,
                                     md_cWeights + b * muTaps, 1));
        }
    }

    // ---- diagonal loading --------------------------------------------------
    {
        const uint32_t uBlock = 64;
        addLoadingKernel<<<blocksFor(muWindows, uBlock), uBlock>>>(
            md_cGram, static_cast<uint32_t>(muTaps),
            static_cast<uint32_t>(muWindows),
            static_cast<float>(moParams.dDiagonalLoading),
            moParams.bAbsoluteLoading ? 1 : 0);
        CUDA_CHECK(cudaGetLastError());
    }

    // ---- solve every window in two calls -----------------------------------
    // Cholesky factors all the Gram matrices, then the triangular solve turns
    // the right-hand sides in md_cWeights into the weights in place.
    CUSOLVER_CHECK(cusolverDnCpotrfBatched(mhCusolver, CUBLAS_FILL_MODE_LOWER,
                                           iTaps, md_ppGram, iTaps,
                                           md_piInfo,
                                           static_cast<int>(muWindows)));

    // potrfBatched reports one status per matrix. A non-zero entry means that
    // window's matrix was not positive definite -- almost always an all-zero
    // window from a dropped packet. Check it: a silent failure here produces
    // weights that are quiet nonsense.
    {
        std::vector<int> viInfo(static_cast<size_t>(muWindows), 0);
        CUDA_CHECK(cudaMemcpy(viInfo.data(), md_piInfo,
                              viInfo.size() * sizeof(int), cudaMemcpyDeviceToHost));
        for (uint64_t b = 0; b < muWindows; ++b) {
            if (viInfo[static_cast<size_t>(b)] != 0) {
                std::fprintf(stderr,
                             "EcaGpu: window %llu is not positive definite "
                             "(leading minor %d). The reference signal is "
                             "probably zero or nearly constant over this "
                             "window; raise ECA_DIAGONAL_LOADING if it is "
                             "genuinely ill conditioned.\n",
                             static_cast<unsigned long long>(b),
                             viInfo[static_cast<size_t>(b)]);
                return false;
            }
        }
    }

    // potrsBatched writes a single status rather than one per matrix, and
    // requires exactly one right-hand side per system -- which is our case.
    //
    // That status pointer is a DEVICE pointer, unlike the host int it looks
    // like. Passing a host address here compiles and links, then faults the
    // kernel at run time with "unspecified launch failure" -- so md_piInfo,
    // which is already device memory, is reused for it.
    {
        CUSOLVER_CHECK(cusolverDnCpotrsBatched(mhCusolver, CUBLAS_FILL_MODE_LOWER,
                                               iTaps, 1,
                                               md_ppGram, iTaps,
                                               md_ppWeights, iTaps,
                                               md_piInfo,
                                               static_cast<int>(muWindows)));
        int iInfo = 0;
        CUDA_CHECK(cudaMemcpy(&iInfo, md_piInfo, sizeof(int),
                              cudaMemcpyDeviceToHost));
        CHECK(iInfo == 0, "cusolverDnCpotrsBatched rejected its arguments");
    }

    // ---- subtract ----------------------------------------------------------
    {
        const uint32_t uBlock = 256;
        applyWeightsKernel<<<blocksFor(moParams.uCpiSamples, uBlock), uBlock>>>(
            md_cRef, md_cSur, md_cWeights, md_cOut,
            static_cast<uint32_t>(moParams.uCpiSamples),
            static_cast<uint32_t>(muTaps),
            static_cast<uint32_t>(muWindowSamples));
        CUDA_CHECK(cudaGetLastError());
    }

    // ---- suppression -------------------------------------------------------
    // Computed on the device from the norms of the two signals, so the host
    // does no arithmetic over the CPI.
    float fSurNorm = 0.0f;
    float fOutNorm = 0.0f;
    CUBLAS_CHECK(cublasScnrm2(mhCublas, static_cast<int>(moParams.uCpiSamples),
                              md_cSur, 1, &fSurNorm));
    CUBLAS_CHECK(cublasScnrm2(mhCublas, static_cast<int>(moParams.uCpiSamples),
                              md_cOut, 1, &fOutNorm));

    CUDA_CHECK(cudaMemcpy(reinterpret_cast<DeviceComplex*>(pcOut), md_cOut,
                          uCpiBytes, cudaMemcpyDeviceToHost));

    // The copy above synchronises, so any kernel error has surfaced by now.
    CUDA_CHECK(cudaGetLastError());

    oStats.dSuppressionDb = (fSurNorm > 0.0f && fOutNorm > 0.0f)
        ? 20.0 * std::log10(static_cast<double>(fSurNorm) /
                            static_cast<double>(fOutNorm))
        : 0.0;
    oStats.uIterations = 0;   // direct solve, not iterative

    return true;
}

} // namespace

std::unique_ptr<DSICanceller> makeDSICancellerGpu(DSIAlgorithm eAlgorithm)
{
    switch (eAlgorithm) {
    case DSIAlgorithm::EcaM:
    case DSIAlgorithm::EcaB:
        return std::unique_ptr<DSICanceller>(new EcaGpu());
    default:
        break;
    }

    std::fprintf(stderr, "dsi: %s is not implemented on the GPU backend yet\n",
                 dsiAlgorithmName(eAlgorithm));
    return std::unique_ptr<DSICanceller>();
}

} // namespace pbr
