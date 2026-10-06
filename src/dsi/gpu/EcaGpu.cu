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
// single-precision rate, which would make this slower than the CPU. So
// everything here is FP32, and DSIParams::bFloat32Math is ignored.
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
//   subtract the clutter                 convolveKernel (DsiGpu.cuh)
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
// M = T_A/T_S times, which measured about ten times ECA-B's cost (1d.9).
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
// but on the Nano's Cortex-A57 that 1.5% would be a large share of the CPI
// budget with the GPU idle throughout, and an FP32 batched solve measured no
// error worth having over a double one -- the Gram error already dominates.
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

#include "pbr/DSICanceller.hpp"
#include "../DsiInternal.hpp"
#include "DsiGpu.cuh"

#include <climits>
#include <cstdio>
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
const double kBytesPerMiB    = 1024.0 * 1024.0;

// ---------------------------------------------------------------------------
// Build one slice of the delayed-reference matrix.
//
// Column-major, because that is what cuBLAS expects: element (n, k) sits at
// n + k * uRows. Column k holds the reference delayed by k samples, and is
// zero wherever that would reach back before the start of the CPI -- the same
// rule as the CPU version, and what makes the two agree at the edges.
// ---------------------------------------------------------------------------
__global__ void buildSubspaceKernel(const cufftComplex* d_cRef,
                                    cufftComplex* d_cSubspace,
                                    uint32_t uStart, uint32_t uRows,
                                    uint32_t uTaps)
{
    const uint32_t uIdx = blockIdx.x * blockDim.x + threadIdx.x;
    if (uIdx >= uRows * uTaps)
        return;

    const uint32_t n = uIdx % uRows;        // row: sample within the slice
    const uint32_t k = uIdx / uRows;        // column: delay in samples
    const uint32_t uAbs = uStart + n;       // sample index within the CPI

    cufftComplex cValue;
    cValue.x = 0.0f;
    cValue.y = 0.0f;
    if (uAbs >= k)
        cValue = d_cRef[uAbs - k];

    d_cSubspace[uIdx] = cValue;
}

// ---------------------------------------------------------------------------
// Share the chunk Grams out among the windows. ECA-S only.
//
// Window w's Gram is the sum of the M chunk Grams its estimation window
// spans, starting at chunk d_uFirstChunk[w]. That array is filled once, on
// the host, from dsiEstimationStart(), so both backends place their windows
// by one rule.
//
// Only the LOWER TRIANGLE is summed: cherk fills only that and potrfBatched
// with FILL_MODE_LOWER reads only that, so the upper half is never
// meaningful. The kernel is bound by reading the chunk Grams M times over, so
// skipping it halves the runtime.
//
// The right-hand sides ride on the threads that land on the diagonal, which
// saves a second launch and costs nothing -- there are K of them against K^2.
// ---------------------------------------------------------------------------
__global__ void accumulateWindowsKernel(const cufftComplex* d_cChunkGram,
                                        const cufftComplex* d_cChunkRhs,
                                        const uint32_t* d_uFirstChunk,
                                        cufftComplex* d_cGram,
                                        cufftComplex* d_cRhs,
                                        uint32_t uTaps, uint32_t uWindows,
                                        uint32_t uSpan)
{
    const uint32_t uIdx  = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t uEach = uTaps * uTaps;
    if (uIdx >= uWindows * uEach)
        return;

    const uint32_t w      = uIdx / uEach;
    const uint32_t uEntry = uIdx % uEach;
    // Column-major, as cuBLAS left it: entry (i, j) sits at i + j * K.
    const uint32_t i = uEntry % uTaps;
    const uint32_t j = uEntry / uTaps;
    if (j > i)
        return;

    const uint32_t uFirst = d_uFirstChunk[w];

    float fRe = 0.0f;
    float fIm = 0.0f;
    for (uint32_t m = 0; m < uSpan; ++m) {
        const cufftComplex cChunk =
            d_cChunkGram[static_cast<size_t>(uFirst + m) * uEach + uEntry];
        fRe += cChunk.x;
        fIm += cChunk.y;
    }
    d_cGram[static_cast<size_t>(w) * uEach + uEntry] = make_cuComplex(fRe, fIm);

    if (i == j) {
        float fVr = 0.0f;
        float fVi = 0.0f;
        for (uint32_t m = 0; m < uSpan; ++m) {
            const cufftComplex cChunk =
                d_cChunkRhs[static_cast<size_t>(uFirst + m) * uTaps + i];
            fVr += cChunk.x;
            fVi += cChunk.y;
        }
        d_cRhs[static_cast<size_t>(w) * uTaps + i] = make_cuComplex(fVr, fVi);
    }
}

// ---------------------------------------------------------------------------
// Diagonal loading.
//
// One thread per window, each walking its own K-entry diagonal -- far too
// little work to be worth parallelising further, and simple enough to read
// at a glance.
//
// The trace is summed in float so that the lambda matches what the CPU's FP32
// mode computes. A lambda that differed would show up as a disagreement
// between the two backends that had nothing to do with the code being
// compared.
// ---------------------------------------------------------------------------
__global__ void addLoadingKernel(cufftComplex* d_cGram, uint32_t uTaps,
                                 uint32_t uWindows, float fLoading,
                                 bool bAbsolute)
{
    const uint32_t w = blockIdx.x * blockDim.x + threadIdx.x;
    if (w >= uWindows)
        return;

    cufftComplex* d_cG = d_cGram + static_cast<size_t>(w) * uTaps * uTaps;

    float fLambda = fLoading;
    if (!bAbsolute) {
        float fTrace = 0.0f;
        for (uint32_t i = 0; i < uTaps; ++i)
            fTrace += d_cG[i + i * uTaps].x;
        fLambda = fLoading * (fTrace / static_cast<float>(uTaps));
    }

    for (uint32_t i = 0; i < uTaps; ++i)
        d_cG[i + i * uTaps].x += fLambda;
}

// ---------------------------------------------------------------------------

class EcaGpu : public DSICanceller {
public:
    ~EcaGpu() override { release(); }

    bool init(const DSIParams& oParams) override;
    bool cancel(const ComplexF* pcRef, const ComplexF* pcSur,
                ComplexF* pcOut, DSIStats& oStats) override;
    const char* name() const override { return machName; }

    const void* deviceReference() const override { return md_cRef; }
    const void* deviceOutput()    const override { return md_cOut; }
    void setHostOutputEnabled(bool bEnabled) override { mbHostOutput = bEnabled; }

private:
    bool setUp(const DSIParams& oParams);   // init() without the clean-up
    bool allocate();                        // within kMaxDeviceBytes
    bool uploadWindowTables();              // pointer arrays, first chunks
    void release();

    // The steps of cancel(), in order.
    bool buildNormalEquations();            // one Gram and RHS per chunk
    bool shareChunks();                     // ECA-S: chunk Grams to windows
    bool addLoading();
    bool solve();                           // RHS in md_cWeights -> weights

    DSIParams          moParams;
    uint64_t           muTaps         = 0;
    uint64_t           muWindows      = 0;   // update windows per CPI
    uint64_t           muWindowSamples= 0;   // T_S, one set of weights applied
    uint64_t           muSpan         = 0;   // M = T_A / T_S, chunks per estimate
    bool               mbChunked      = false;   // M > 1: ECA-S shares chunk Grams
    uint64_t           muBuildSamples = 0;   // rows of R built per cuBLAS call

    cublasHandle_t     mhCublas       = nullptr;
    cusolverDnHandle_t mhCusolver     = nullptr;

    // All allocated in allocate(), freed in release().
    cufftComplex*      md_cRef        = nullptr;   // CPI samples, reference
    cufftComplex*      md_cSur        = nullptr;   // CPI samples, surveillance
    cufftComplex*      md_cOut        = nullptr;   // cleaned surveillance
    cufftComplex*      md_cSubspace   = nullptr;   // one slice of R
    cufftComplex*      md_cGram       = nullptr;   // windows x K x K
    cufftComplex*      md_cWeights    = nullptr;   // windows x K (RHS, then w)
    // ECA-S only; null when M == 1, where the chunk IS the window.
    cufftComplex*      md_cChunkGram  = nullptr;   // chunks x K x K
    cufftComplex*      md_cChunkRhs   = nullptr;   // chunks x K
    uint32_t*          md_uFirstChunk = nullptr;   // windows: first chunk estimated from
    cufftComplex**     md_ppGram      = nullptr;   // pointer array for cuSOLVER
    cufftComplex**     md_ppWeights   = nullptr;
    int*               md_piInfo      = nullptr;   // cuSOLVER status, per window

    char               machName[32]   = {};
    bool               mbReady        = false;
    // When the range-Doppler stage reads deviceOutput() directly there is no
    // reason to move 1.6 MB back to the host every CPI.
    bool               mbHostOutput   = true;
};

void EcaGpu::release()
{
    // Destructors cannot report failure and nothing can be done about a failed
    // free anyway, so these are deliberately unchecked. cudaFree(nullptr) is
    // a no-op.
    cudaFree(md_cRef);
    cudaFree(md_cSur);
    cudaFree(md_cOut);
    cudaFree(md_cSubspace);
    cudaFree(md_cGram);
    cudaFree(md_cWeights);
    cudaFree(md_cChunkGram);
    cudaFree(md_cChunkRhs);
    cudaFree(md_uFirstChunk);
    cudaFree(md_ppGram);
    cudaFree(md_ppWeights);
    cudaFree(md_piInfo);
    if (mhCublas)
        cublasDestroy(mhCublas);
    if (mhCusolver)
        cusolverDnDestroy(mhCusolver);

    md_cRef = md_cSur = md_cOut = md_cSubspace = md_cGram = md_cWeights = nullptr;
    md_cChunkGram = md_cChunkRhs = nullptr;
    md_uFirstChunk = nullptr;
    md_ppGram = md_ppWeights = nullptr;
    md_piInfo = nullptr;
    mhCublas  = nullptr;
    mhCusolver = nullptr;
    mbReady = false;
}

bool EcaGpu::init(const DSIParams& oParams)
{
    CHECK(!mbReady, "EcaGpu::init called twice");
    if (setUp(oParams))
        return true;
    release();   // whatever was allocated before the failure
    return false;
}

bool EcaGpu::setUp(const DSIParams& oParams)
{
    if (!ecaParamsOk(oParams))
        return false;
    // Kernel arguments are uint32_t, and cuBLAS and cuSOLVER take int.
    CHECK(oParams.uClutterTaps <= INT_MAX && oParams.uCpiSamples <= INT_MAX,
          "problem dimensions exceed what the kernels and cuBLAS can index");

    moParams          = oParams;
    muTaps            = oParams.uClutterTaps;
    muWindowSamples   = oParams.uUpdateSamples;
    muWindows         = oParams.uCpiSamples / oParams.uUpdateSamples;
    muSpan            = oParams.uEstimateSamples / oParams.uUpdateSamples;
    mbChunked         = (muSpan > 1);
    // A chunk is one update window long; the build buffer holds a slice of one.
    muBuildSamples    = (muWindowSamples < kMaxBuildSamples) ? muWindowSamples
                                                             : kMaxBuildSamples;

    CUDA_CHECK(cublasCreate(&mhCublas));
    CUDA_CHECK(cusolverDnCreate(&mhCusolver));

    if (!allocate() || !uploadWindowTables())
        return false;

    std::snprintf(machName, sizeof(machName), "%s (GPU)",
                  dsiAlgorithmName(oParams.eAlgorithm));
    mbReady = true;
    return true;
}

bool EcaGpu::allocate()
{
    const size_t uCpiBytes  = static_cast<size_t>(moParams.uCpiSamples) * sizeof(cufftComplex);
    const size_t uSubBytes  = static_cast<size_t>(muBuildSamples * muTaps) * sizeof(cufftComplex);
    const size_t uGramBytes = static_cast<size_t>(muWindows * muTaps * muTaps) * sizeof(cufftComplex);
    const size_t uWBytes    = static_cast<size_t>(muWindows * muTaps) * sizeof(cufftComplex);
    // ECA-S alone needs somewhere to put the per-chunk Grams. With M == 1 the
    // chunk is the window, so cherk writes straight into md_cGram and these
    // are never allocated.
    const size_t uChunkGramBytes = mbChunked ? uGramBytes : 0;
    const size_t uChunkRhsBytes  = mbChunked ? uWBytes    : 0;
    const size_t uPerWindowBytes = static_cast<size_t>(muWindows) *
                                   (2 * sizeof(cufftComplex*) + sizeof(int) + sizeof(uint32_t));

    // Refuse a footprint that would push the board into zram rather than
    // failing cleanly. The arithmetic is dominated by the two Gram arrays,
    // which grow as 1 / T_S.
    const size_t uTotal = 3 * uCpiBytes + uSubBytes + uGramBytes + uWBytes +
                          uChunkGramBytes + uChunkRhsBytes + uPerWindowBytes;
    char achMsg[256];
    std::snprintf(achMsg, sizeof(achMsg),
                  "this configuration needs %.0f MB of device memory, over "
                  "the %.0f MB ceiling: %llu windows x %llu taps. Lengthen "
                  "ECA_UPDATE_INTERVAL_s.",
                  double(uTotal) / kBytesPerMiB, double(kMaxDeviceBytes) / kBytesPerMiB,
                  static_cast<unsigned long long>(muWindows),
                  static_cast<unsigned long long>(muTaps));
    CHECK(uTotal <= kMaxDeviceBytes, achMsg);

    // Every allocation happens here and is reused for the life of the object.
    // Nothing allocates inside cancel(), or the benchmark would be timing
    // cudaMalloc.
    CUDA_CHECK(cudaMalloc(&md_cRef,      uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cSur,      uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cOut,      uCpiBytes));
    CUDA_CHECK(cudaMalloc(&md_cSubspace, uSubBytes));
    CUDA_CHECK(cudaMalloc(&md_cGram,     uGramBytes));
    CUDA_CHECK(cudaMalloc(&md_cWeights,  uWBytes));
    if (mbChunked) {
        CUDA_CHECK(cudaMalloc(&md_cChunkGram,  uChunkGramBytes));
        CUDA_CHECK(cudaMalloc(&md_cChunkRhs,   uChunkRhsBytes));
        CUDA_CHECK(cudaMalloc(&md_uFirstChunk, static_cast<size_t>(muWindows) * sizeof(uint32_t)));
    }
    CUDA_CHECK(cudaMalloc(&md_ppGram,    static_cast<size_t>(muWindows) * sizeof(cufftComplex*)));
    CUDA_CHECK(cudaMalloc(&md_ppWeights, static_cast<size_t>(muWindows) * sizeof(cufftComplex*)));
    CUDA_CHECK(cudaMalloc(&md_piInfo,    static_cast<size_t>(muWindows) * sizeof(int)));
    return true;
}

bool EcaGpu::uploadWindowTables()
{
    // cuSOLVER's batched routines take an array of pointers, one per problem.
    // The pointers never change, so they are built once here rather than on
    // every CPI.
    std::vector<cufftComplex*> vpGram(static_cast<size_t>(muWindows));
    std::vector<cufftComplex*> vpWeights(static_cast<size_t>(muWindows));
    for (uint64_t w = 0; w < muWindows; ++w) {
        vpGram[static_cast<size_t>(w)]    = md_cGram + w * muTaps * muTaps;
        vpWeights[static_cast<size_t>(w)] = md_cWeights + w * muTaps;
    }
    CUDA_CHECK(cudaMemcpy(md_ppGram, vpGram.data(),
                          vpGram.size() * sizeof(cufftComplex*), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(md_ppWeights, vpWeights.data(),
                          vpWeights.size() * sizeof(cufftComplex*), cudaMemcpyHostToDevice));

    // ECA-S: the first chunk each window's estimate is summed from, by the
    // rule both backends share.
    if (mbChunked) {
        std::vector<uint32_t> vuFirstChunk(static_cast<size_t>(muWindows));
        for (uint64_t w = 0; w < muWindows; ++w) {
            const uint64_t uStart = dsiEstimationStart(w * muWindowSamples, muWindowSamples,
                                                       moParams.uEstimateSamples,
                                                       moParams.uCpiSamples);
            vuFirstChunk[static_cast<size_t>(w)] = static_cast<uint32_t>(uStart / muWindowSamples);
        }
        CUDA_CHECK(cudaMemcpy(md_uFirstChunk, vuFirstChunk.data(),
                              vuFirstChunk.size() * sizeof(uint32_t), cudaMemcpyHostToDevice));
    }
    return true;
}

bool EcaGpu::buildNormalEquations()
{
    const int         iTaps = static_cast<int>(muTaps);
    const cuComplex   cOne  = make_cuComplex(1.0f, 0.0f);
    const cuComplex   cZero = make_cuComplex(0.0f, 0.0f);
    const float       fOne  = 1.0f;
    const float       fZero = 0.0f;

    // One Gram per CHUNK of T_S samples. There are exactly muWindows of them,
    // because a chunk is one update window long. With M == 1 these are the
    // window Grams already and nothing more is needed; with M > 1 they are
    // shared out by shareChunks().
    cufftComplex* const d_cGramDst = mbChunked ? md_cChunkGram : md_cGram;
    cufftComplex* const d_cRhsDst  = mbChunked ? md_cChunkRhs  : md_cWeights;

    for (uint64_t uChunk = 0; uChunk < muWindows; ++uChunk) {
        const uint64_t uChunkStart = uChunk * muWindowSamples;

        for (uint64_t uOffset = 0; uOffset < muWindowSamples; uOffset += muBuildSamples) {
            const uint64_t uRows = (uOffset + muBuildSamples <= muWindowSamples)
                                 ? muBuildSamples
                                 : (muWindowSamples - uOffset);
            const uint64_t uStart = uChunkStart + uOffset;
            const bool     bFirst = (uOffset == 0);

            buildSubspaceKernel<<<blocksFor(uRows * muTaps, kBlock), kBlock>>>(
                md_cRef, md_cSubspace, static_cast<uint32_t>(uStart),
                static_cast<uint32_t>(uRows), static_cast<uint32_t>(muTaps));
            CUDA_CHECK(cudaGetLastError());

            // G += R^H R over this slice. cherk knows the result is Hermitian
            // and fills only the lower triangle, which is the same halving of
            // the work the CPU version does by hand. beta = 1 after the first
            // slice is what lets a long chunk be summed piece by piece.
            CUDA_CHECK(cublasCherk(mhCublas,
                                   CUBLAS_FILL_MODE_LOWER, CUBLAS_OP_C,
                                   iTaps, static_cast<int>(uRows),
                                   &fOne, md_cSubspace, static_cast<int>(uRows),
                                   bFirst ? &fZero : &fOne,
                                   d_cGramDst + uChunk * muTaps * muTaps, iTaps));

            // v += R^H s over this slice.
            CUDA_CHECK(cublasCgemv(mhCublas, CUBLAS_OP_C,
                                   static_cast<int>(uRows), iTaps,
                                   &cOne, md_cSubspace, static_cast<int>(uRows),
                                   md_cSur + uStart, 1,
                                   bFirst ? &cZero : &cOne,
                                   d_cRhsDst + uChunk * muTaps, 1));
        }
    }
    return true;
}

bool EcaGpu::shareChunks()
{
    if (!mbChunked)
        return true;
    accumulateWindowsKernel<<<blocksFor(muWindows * muTaps * muTaps, kBlock), kBlock>>>(
        md_cChunkGram, md_cChunkRhs, md_uFirstChunk, md_cGram, md_cWeights,
        static_cast<uint32_t>(muTaps), static_cast<uint32_t>(muWindows),
        static_cast<uint32_t>(muSpan));
    CUDA_CHECK(cudaGetLastError());
    return true;
}

bool EcaGpu::addLoading()
{
    // Relative loading of zero adds nothing. Skipped, as the CPU skips it, so
    // a non-finite trace cannot turn "no loading" into NaN.
    if (!moParams.bAbsoluteLoading && !(moParams.dDiagonalLoading > 0.0))
        return true;
    addLoadingKernel<<<blocksFor(muWindows, kBlock), kBlock>>>(
        md_cGram, static_cast<uint32_t>(muTaps), static_cast<uint32_t>(muWindows),
        static_cast<float>(moParams.dDiagonalLoading), moParams.bAbsoluteLoading);
    CUDA_CHECK(cudaGetLastError());
    return true;
}

bool EcaGpu::solve()
{
    const int iTaps = static_cast<int>(muTaps);

    // Cholesky factors all the Gram matrices, then the triangular solve turns
    // the right-hand sides in md_cWeights into the weights in place.
    CUDA_CHECK(cusolverDnCpotrfBatched(mhCusolver, CUBLAS_FILL_MODE_LOWER,
                                       iTaps, md_ppGram, iTaps,
                                       md_piInfo, static_cast<int>(muWindows)));

    // potrfBatched reports one status per matrix. A non-zero entry means that
    // window's matrix was not positive definite -- almost always an all-zero
    // window from a dropped packet. Check it: a silent failure here produces
    // weights that are quiet nonsense. This read is one of the two places the
    // host waits for the GPU inside cancel().
    std::vector<int> viInfo(static_cast<size_t>(muWindows), 0);
    CUDA_CHECK(cudaMemcpy(viInfo.data(), md_piInfo,
                          viInfo.size() * sizeof(int), cudaMemcpyDeviceToHost));
    for (uint64_t w = 0; w < muWindows; ++w) {
        if (viInfo[static_cast<size_t>(w)] != 0) {
            std::fprintf(stderr,
                         "EcaGpu: window %llu is not positive definite "
                         "(leading minor %d). The reference signal is "
                         "probably zero or nearly constant over this "
                         "window; raise ECA_DIAGONAL_LOADING if it is "
                         "genuinely ill conditioned.\n",
                         static_cast<unsigned long long>(w),
                         viInfo[static_cast<size_t>(w)]);
            return false;
        }
    }

    // potrsBatched writes a single status rather than one per matrix, and
    // requires exactly one right-hand side per system -- which is our case.
    //
    // That status pointer is a DEVICE pointer, unlike the host int it looks
    // like. Passing a host address here compiles and links, then faults the
    // kernel at run time with "unspecified launch failure" -- so md_piInfo,
    // which is already device memory, is reused for it.
    CUDA_CHECK(cusolverDnCpotrsBatched(mhCusolver, CUBLAS_FILL_MODE_LOWER,
                                       iTaps, 1,
                                       md_ppGram, iTaps,
                                       md_ppWeights, iTaps,
                                       md_piInfo, static_cast<int>(muWindows)));
    int iInfo = 0;
    CUDA_CHECK(cudaMemcpy(&iInfo, md_piInfo, sizeof(int), cudaMemcpyDeviceToHost));
    CHECK(iInfo == 0, "cusolverDnCpotrsBatched rejected its arguments");
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

    const size_t uCpiBytes = static_cast<size_t>(moParams.uCpiSamples) * sizeof(cufftComplex);

    // ComplexF and cufftComplex have the same layout, so the cast is a
    // reinterpretation at the copy boundary only -- never anywhere else.
    CUDA_CHECK(cudaMemcpy(md_cRef, reinterpret_cast<const cufftComplex*>(pcRef),
                          uCpiBytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(md_cSur, reinterpret_cast<const cufftComplex*>(pcSur),
                          uCpiBytes, cudaMemcpyHostToDevice));

    // Everything is queued on the default stream. The host waits only for the
    // two solver status reads in solve() and for the suppression norms.
    if (!buildNormalEquations() || !shareChunks() || !addLoading() || !solve())
        return false;

    // out = s - R w, every window in one launch.
    if (!launchConvolve(md_cRef, md_cWeights, md_cSur, md_cOut, moParams.uCpiSamples,
                        muTaps, muWindowSamples))
        return false;

    if (!gpuSuppressionDb(mhCublas, md_cSur, md_cOut, moParams.uCpiSamples,
                          oStats.dSuppressionDb))
        return false;
    if (mbHostOutput) {
        CUDA_CHECK(cudaMemcpy(reinterpret_cast<cufftComplex*>(pcOut), md_cOut,
                              uCpiBytes, cudaMemcpyDeviceToHost));
    }

    oStats.uIterations = 0;   // direct solve, not iterative
    return true;
}

} // namespace

std::unique_ptr<DSICanceller> makeEcaCancellerGpu()
{
    return std::unique_ptr<DSICanceller>(new EcaGpu());
}

} // namespace pbr
