// ECA-m, ECA-B and ECA-S on the GPU: the implementation the thesis measures.
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
//   share partial Grams between windows  custom kernel (below, ECA-S only)
//   solve all windows at once            cuSOLVER Cpotrf/CpotrsBatched
//   subtract the clutter                 custom kernel (below)
//   suppression figure                   cuBLAS scnrm2
//
// Everything heavy is a library call: less to get wrong, and less to review.
//
// ---------------------------------------------------------------------------
// ECA-S: one Gram per chunk, shared between the windows that span it
// ---------------------------------------------------------------------------
//
// ECA-S's estimation windows overlap by (T_A - T_S)/T_A -- 90% at 25 ms over
// 2.5 ms -- so summing each from scratch recomputes nearly every product
// M = T_A/T_S times. Measured that way: 1460 ms per CPI against ECA-B's 155.
//
// Instead the CPI is cut into chunks of T_S, one Gram is computed per chunk
// (so the total sample-work equals ECA-B's exactly), and each window's Gram is
// the sum of the M chunk Grams its estimation window spans. The chunk length
// IS the update window length, so there are exactly muWindows of them and
// chunk c covers update window c.
//
// This is not Colone's recursive slide (G_{w+1} = G_w - leaving + entering).
// That reads two chunks per window rather than M, but it serialises the
// windows and accumulates add/subtract rounding across every step in FP32.
// Summing M chunks is fully parallel and has no drift -- if anything it is
// slightly more accurate than the direct form, M partial sums combined
// pairwise beating one serial sum over T_A. See docs/design_decisions.md 1d.9,
// which also records why the estimation window is quantised to a chunk
// boundary and what that costs.
//
// M == 1 is ECA-m and ECA-B: the chunk IS the window, so the chunk arrays are
// neither allocated nor touched and those two take byte-for-byte the path
// they always did.
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
// window is just more chunks summed into the same Gram matrix. ECA-S is the
// same loop again with the chunks drawn from the estimation window rather
// than the update window; the subtract kernel does not change at all.
//
// Total device memory is about 25 MB for the shipped ECA-B parameters. The
// Gram buffer is one K x K matrix per UPDATE window, and ECA-S needs a second
// one of the same size for the chunk Grams, so at 2.5 ms updates it is 400
// matrices twice over: 114 MB all told. kMaxDeviceBytes below caps that; the
// measured memory budget behind the number is in design_decisions.md 1d.10.

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

// Largest slice of a chunk handled in one cuBLAS call. Bounds the R buffer
// at kMaxBuildSamples * K * 8 bytes; 16384 x 120 is 15.7 MB.
const uint64_t kMaxBuildSamples = 16384;

// Ceiling on everything this canceller allocates on the device. Not a
// hardware limit -- the Nano has about 2900 MB of true headroom -- but a
// policy ceiling that sits above the whole useful parameter range and below
// where the board starts reclaiming page cache and swapping to zram. It
// exists to turn a mistyped ECA_UPDATE_INTERVAL_s into a named error rather
// than a thrashing live run. See docs/design_decisions.md 1d.10 for the
// measurements. ECA-S at the shipped settings uses 114 MB of it.
const size_t kMaxDeviceBytes = 512u * 1024u * 1024u;

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
// Kernel 1b: share the chunk Grams out among the windows. ECA-S only.
//
// Window w's Gram is the sum of the M chunk Grams its estimation window
// spans, starting at chunk a = clamp(w - (M-1)/2, 0, C - M). That is exactly
// what dsiEstimationStart() computes in samples; repeating it here in chunks
// rather than passing an index array keeps one rule in two places that cannot
// drift, because the host asserts the two agree (see cancel()).
//
// Only the LOWER TRIANGLE is summed: cherk fills only that and potrfBatched
// with FILL_MODE_LOWER reads only that, so the upper half is never
// meaningful. The kernel is bound by reading the chunk Grams M times over, so
// skipping it halves the runtime.
//
// The right-hand sides ride on the threads that land on the diagonal, which
// saves a second launch and costs nothing -- there are K of them against K^2.
// ---------------------------------------------------------------------------
__global__ void accumulateWindowsKernel(const DeviceComplex* pcChunkGram,
                                        const DeviceComplex* pcChunkRhs,
                                        DeviceComplex* pcGram,
                                        DeviceComplex* pcRhs,
                                        uint32_t uTaps, uint32_t uWindows,
                                        uint32_t uSpan)
{
    const uint32_t uIdx  = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t uEach = uTaps * uTaps;
    if (uIdx >= uWindows * uEach)
        return;

    const uint32_t w = uIdx / uEach;
    const uint32_t e = uIdx % uEach;
    // Column-major, as cuBLAS left it: entry (i, j) sits at i + j * K.
    const uint32_t i = e % uTaps;
    const uint32_t j = e / uTaps;
    if (j > i)
        return;

    // The chunk count equals the window count: the chunk length IS the
    // update window length.
    const uint32_t uBack = (uSpan - 1) / 2;
    uint32_t a = (w > uBack) ? (w - uBack) : 0;
    if (a + uSpan > uWindows)
        a = uWindows - uSpan;

    float fRe = 0.0f;
    float fIm = 0.0f;
    for (uint32_t m = 0; m < uSpan; ++m) {
        const DeviceComplex c =
            pcChunkGram[static_cast<size_t>(a + m) * uEach + e];
        fRe += c.x;
        fIm += c.y;
    }
    pcGram[static_cast<size_t>(w) * uEach + e] = make_cuComplex(fRe, fIm);

    if (i == j) {
        float fVr = 0.0f;
        float fVi = 0.0f;
        for (uint32_t m = 0; m < uSpan; ++m) {
            const DeviceComplex c =
                pcChunkRhs[static_cast<size_t>(a + m) * uTaps + i];
            fVr += c.x;
            fVi += c.y;
        }
        pcRhs[static_cast<size_t>(w) * uTaps + i] = make_cuComplex(fVr, fVi);
    }
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

    const void* deviceReference() const override { return md_cRef; }
    const void* deviceOutput()    const override { return md_cOut; }
    void setHostOutputEnabled(bool bEnabled) override { mbHostOutput = bEnabled; }

private:
    void release();

    DSIParams          moParams;
    uint64_t           muTaps         = 0;
    uint64_t           muWindows      = 0;   // update windows per CPI
    uint64_t           muWindowSamples= 0;   // T_S, one set of weights applied
    uint64_t           muEstimateSamples = 0;// T_A, one Gram summed over
    uint64_t           muSpan         = 0;   // M = T_A / T_S, chunks per estimate
    uint64_t           muBuildSamples = 0;   // rows of R built per cuBLAS call

    cublasHandle_t     mhCublas       = nullptr;
    cusolverDnHandle_t mhCusolver     = nullptr;

    DeviceComplex*     md_cRef        = nullptr;   // CPI samples, reference
    DeviceComplex*     md_cSur        = nullptr;   // CPI samples, surveillance
    DeviceComplex*     md_cOut        = nullptr;   // cleaned surveillance
    DeviceComplex*     md_cSubspace   = nullptr;   // one slice of R
    DeviceComplex*     md_cGram       = nullptr;   // windows x K x K
    DeviceComplex*     md_cWeights    = nullptr;   // windows x K (RHS, then w)
    // ECA-S only; null when M == 1, where the chunk IS the window.
    DeviceComplex*     md_cChunkGram  = nullptr;   // chunks x K x K
    DeviceComplex*     md_cChunkRhs   = nullptr;   // chunks x K
    DeviceComplex**    md_ppGram      = nullptr;   // pointer array for cuSOLVER
    DeviceComplex**    md_ppWeights   = nullptr;
    int*               md_piInfo      = nullptr;

    char               machName[32];
    bool               mbReady        = false;
    // When the range-Doppler stage reads deviceOutput() directly there is no
    // reason to move 1.6 MB back to the host every CPI.
    bool               mbHostOutput   = true;
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
    if (md_cChunkGram) cudaFree(md_cChunkGram);
    if (md_cChunkRhs)  cudaFree(md_cChunkRhs);
    if (md_ppGram)    cudaFree(md_ppGram);
    if (md_ppWeights) cudaFree(md_ppWeights);
    if (md_piInfo)    cudaFree(md_piInfo);
    if (mhCublas)     cublasDestroy(mhCublas);
    if (mhCusolver)   cusolverDnDestroy(mhCusolver);

    md_cRef = md_cSur = md_cOut = md_cSubspace = md_cGram = md_cWeights = nullptr;
    md_cChunkGram = md_cChunkRhs = nullptr;
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
          oParams.eAlgorithm == DSIAlgorithm::EcaB ||
          oParams.eAlgorithm == DSIAlgorithm::EcaS,
          "EcaGpu handles ECA-m, ECA-B and ECA-S only");
    CHECK(oParams.uCpiSamples > 0, "CPI length must be non-zero");
    CHECK(oParams.uClutterTaps > 0, "clutter taps must be non-zero");
    CHECK(oParams.uEstimateSamples > 0 && oParams.uUpdateSamples > 0,
          "estimation and update windows must be non-zero");
    CHECK(oParams.uDopplerTaps == 0,
          "ECA_DOPPLER_TAPS > 0 is not implemented; the clutter subspace is "
          "delay-only for now");
    // Same window rule as the CPU backend: the name and the windows must
    // agree, so a benchmark row cannot carry the wrong label.
    if (oParams.eAlgorithm == DSIAlgorithm::EcaS) {
        CHECK(oParams.uEstimateSamples > oParams.uUpdateSamples,
              "ECA-S needs an estimation window longer than the update window; "
              "equal windows are ECA-B");
    } else {
        CHECK(oParams.uEstimateSamples == oParams.uUpdateSamples,
              "an estimation window longer than the update window is ECA-S, "
              "not ECA-B");
    }
    CHECK(oParams.uEstimateSamples <= oParams.uCpiSamples,
          "the estimation window cannot be longer than the CPI");
    CHECK(oParams.uCpiSamples % oParams.uUpdateSamples == 0,
          "the update window must divide the CPI exactly");
    // The chunk sharing below needs the estimation window to be a whole
    // number of update windows.
    CHECK(oParams.uEstimateSamples % oParams.uUpdateSamples == 0,
          "the update window must divide the estimation window exactly");
    CHECK(oParams.uEstimateSamples > oParams.uClutterTaps,
          "the estimation window must hold more samples than there are taps "
          "to solve for");
    CHECK(oParams.dDiagonalLoading >= 0.0, "diagonal loading must not be negative");

    // cuBLAS and cuSOLVER take int, not uint64_t, for every dimension.
    CHECK(oParams.uClutterTaps <= 2147483647ULL &&
          oParams.uCpiSamples  <= 2147483647ULL,
          "problem dimensions exceed what cuBLAS accepts as int");

    moParams          = oParams;
    muTaps            = oParams.uClutterTaps;
    muWindowSamples   = oParams.uUpdateSamples;
    muEstimateSamples = oParams.uEstimateSamples;
    muWindows         = oParams.uCpiSamples / oParams.uUpdateSamples;
    muSpan            = oParams.uEstimateSamples / oParams.uUpdateSamples;
    // A chunk is one update window long; the build buffer holds a slice of one.
    muBuildSamples    = (muWindowSamples < kMaxBuildSamples) ? muWindowSamples
                                                             : kMaxBuildSamples;

    CUBLAS_CHECK(cublasCreate(&mhCublas));
    CUSOLVER_CHECK(cusolverDnCreate(&mhCusolver));

    const size_t uCpiBytes  = static_cast<size_t>(oParams.uCpiSamples) * sizeof(DeviceComplex);
    const size_t uSubBytes  = static_cast<size_t>(muBuildSamples * muTaps) * sizeof(DeviceComplex);
    const size_t uGramBytes = static_cast<size_t>(muWindows * muTaps * muTaps) * sizeof(DeviceComplex);
    const size_t uWBytes    = static_cast<size_t>(muWindows * muTaps) * sizeof(DeviceComplex);
    // ECA-S alone needs somewhere to put the per-chunk Grams. With M == 1 the
    // chunk is the window, so cherk writes straight into md_cGram and these
    // are never allocated.
    const bool   bShared    = (muSpan > 1);
    const size_t uChunkGramBytes = bShared ? uGramBytes : 0;
    const size_t uChunkRhsBytes  = bShared ? uWBytes    : 0;

    // Refuse a footprint that would push the board into zram rather than
    // failing cleanly. The arithmetic is dominated by the two Gram arrays,
    // which grow as 1 / T_S.
    {
        const size_t uTotal = 3 * uCpiBytes + uSubBytes + uGramBytes + uWBytes +
                              uChunkGramBytes + uChunkRhsBytes +
                              static_cast<size_t>(muWindows) *
                                  (2 * sizeof(DeviceComplex*) + sizeof(int));
        char achMsg[256];
        std::snprintf(achMsg, sizeof(achMsg),
                      "this configuration needs %.0f MB of device memory, over "
                      "the %.0f MB ceiling: %llu windows x %llu taps. Lengthen "
                      "ECA_UPDATE_INTERVAL_s.",
                      double(uTotal) / 1048576.0,
                      double(kMaxDeviceBytes) / 1048576.0,
                      static_cast<unsigned long long>(muWindows),
                      static_cast<unsigned long long>(muTaps));
        CHECK(uTotal <= kMaxDeviceBytes, achMsg);
    }

    // Every allocation happens here and is reused for the life of the object.
    // Nothing allocates inside cancel(), or the benchmark would be timing
    // cudaMalloc.
    CUDA_CHECK(cudaMalloc(&md_cRef,      uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cSur,      uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cOut,      uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cSubspace, uSubBytes));
    CUDA_CHECK(cudaMalloc(&md_cGram,     uGramBytes));
    CUDA_CHECK(cudaMalloc(&md_cWeights,  uWBytes));
    if (bShared) {
        CUDA_CHECK(cudaMalloc(&md_cChunkGram, uChunkGramBytes));
        CUDA_CHECK(cudaMalloc(&md_cChunkRhs,  uChunkRhsBytes));
    }
    CUDA_CHECK(cudaMalloc(&md_ppGram,    static_cast<size_t>(muWindows) * sizeof(DeviceComplex*)));
    CUDA_CHECK(cudaMalloc(&md_ppWeights, static_cast<size_t>(muWindows) * sizeof(DeviceComplex*)));
    CUDA_CHECK(cudaMalloc(&md_piInfo,    static_cast<size_t>(muWindows) * sizeof(int)));

    // accumulateWindowsKernel recomputes each window's first chunk itself,
    // in chunks, where dsiEstimationStart() works in samples. They are the
    // same arithmetic and must stay that way -- a divergence would silently
    // estimate from the wrong samples, and no output check would name the
    // cause. Verified here, once, rather than in the timed path.
    for (uint64_t w = 0; w < muWindows; ++w) {
        const uint64_t uBack = (muSpan - 1) / 2;
        uint64_t a = (w > uBack) ? (w - uBack) : 0;
        if (a + muSpan > muWindows)
            a = muWindows - muSpan;
        CHECK(a * muWindowSamples ==
                  dsiEstimationStart(w * muWindowSamples, muWindowSamples,
                                     muEstimateSamples, oParams.uCpiSamples),
              "the device window rule disagrees with dsiEstimationStart");
    }

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
    CHECK(pcRef != nullptr && pcSur != nullptr,
          "null input buffer passed to EcaGpu::cancel");
    CHECK(pcOut != nullptr || !mbHostOutput,
          "null output buffer with host output enabled");
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

    // One Gram per CHUNK of T_S samples. There are exactly muWindows of them,
    // because a chunk is one update window long. With M == 1 these are the
    // window Grams already and nothing more is needed; with M > 1 they are
    // shared out by accumulateWindowsKernel below.
    DeviceComplex* const pcGramDst = (muSpan > 1) ? md_cChunkGram : md_cGram;
    DeviceComplex* const pcRhsDst  = (muSpan > 1) ? md_cChunkRhs  : md_cWeights;

    for (uint64_t c = 0; c < muWindows; ++c) {
        const uint64_t uChunkStart = c * muWindowSamples;

        for (uint64_t uOffset = 0; uOffset < muWindowSamples; uOffset += muBuildSamples) {
            const uint64_t uRows = (uOffset + muBuildSamples <= muWindowSamples)
                                 ? muBuildSamples
                                 : (muWindowSamples - uOffset);
            const uint64_t uStart = uChunkStart + uOffset;
            const bool     bFirst = (uOffset == 0);

            const uint32_t uBlock = 256;
            buildSubspaceKernel<<<blocksFor(uRows * muTaps, uBlock), uBlock>>>(
                md_cRef, md_cSubspace, static_cast<uint32_t>(uStart),
                static_cast<uint32_t>(uRows), static_cast<uint32_t>(muTaps));
            CUDA_CHECK(cudaGetLastError());

            // G += R^H R over this slice. cherk knows the result is Hermitian
            // and fills only the lower triangle, which is the same halving of
            // the work the CPU version does by hand. beta = 1 after the first
            // slice is what lets a long chunk be summed piece by piece.
            CUBLAS_CHECK(cublasCherk(mhCublas,
                                     CUBLAS_FILL_MODE_LOWER, CUBLAS_OP_C,
                                     iTaps, static_cast<int>(uRows),
                                     &fOne, md_cSubspace, static_cast<int>(uRows),
                                     bFirst ? &fZero : &fOne,
                                     pcGramDst + c * muTaps * muTaps, iTaps));

            // v += R^H s over this slice.
            CUBLAS_CHECK(cublasCgemv(mhCublas, CUBLAS_OP_C,
                                     static_cast<int>(uRows), iTaps,
                                     &cOne, md_cSubspace, static_cast<int>(uRows),
                                     md_cSur + uStart, 1,
                                     bFirst ? &cZero : &cOne,
                                     pcRhsDst + c * muTaps, 1));
        }
    }

    // ---- share the chunks out among the windows (ECA-S only) --------------
    if (muSpan > 1) {
        const uint32_t uBlock = 256;
        accumulateWindowsKernel<<<blocksFor(muWindows * muTaps * muTaps, uBlock),
                                  uBlock>>>(
            md_cChunkGram, md_cChunkRhs, md_cGram, md_cWeights,
            static_cast<uint32_t>(muTaps), static_cast<uint32_t>(muWindows),
            static_cast<uint32_t>(muSpan));
        CUDA_CHECK(cudaGetLastError());
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

    if (mbHostOutput) {
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<DeviceComplex*>(pcOut), md_cOut,
                              uCpiBytes, cudaMemcpyDeviceToHost));
    } else {
        // Nothing synchronises otherwise, and the suppression figures below
        // are read back by cublasScnrm2 which does -- but make the wait
        // explicit so a kernel error surfaces here rather than later.
        CUDA_CHECK(cudaDeviceSynchronize());
    }

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

// The other GPU algorithms live in their own files; this is the one place
// that knows all of them.
std::unique_ptr<DSICanceller> makeCglsCancellerGpu();
std::unique_ptr<DSICanceller> makeEcaCdCancellerGpu();

std::unique_ptr<DSICanceller> makeDSICancellerGpu(DSIAlgorithm eAlgorithm)
{
    switch (eAlgorithm) {
    case DSIAlgorithm::EcaM:
    case DSIAlgorithm::EcaB:
    case DSIAlgorithm::EcaS:
        return std::unique_ptr<DSICanceller>(new EcaGpu());
    case DSIAlgorithm::EcaCd:
        return makeEcaCdCancellerGpu();
    case DSIAlgorithm::Cgls:
        return makeCglsCancellerGpu();
    }

    std::fprintf(stderr, "dsi: unknown algorithm %d on the GPU backend\n",
                 static_cast<int>(eAlgorithm));
    return std::unique_ptr<DSICanceller>();
}

} // namespace pbr
