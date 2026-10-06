// bench/cufft_bench.cu
//
// cuFFT timing + capacity harness for the Jetson Nano (Tegra X1, sm_53, CUDA 10.2).
//
// Purpose
// -------
// Establish real, on-hardware performance and memory baselines for the passive
// bistatic radar processing chain BEFORE committing the full port to a fixed
// geometry. It answers two design questions directly:
//
//   1. "Where does memory give out?"  -> sweep size/batch until allocation fails.
//   2. "Where does compute give out?" -> per-transform latency vs a real-time
//                                        budget, so you can pick a CPI / FFT
//                                        length / bin count you can actually afford.
//
// Mapping to the radar chain
// ---------------------------
//   N (size)  : FFT length along the transform dimension.
//               - Range/matched-filter FFT: samples per CPI segment
//                 (e.g. 204800 = 1 s CPI at 204.8 kS/s).
//               - Doppler FFT: number of coherent pulses / slow-time samples.
//   batch     : number of independent transforms run together
//               (e.g. number of range bins for a batched Doppler FFT).
//
//   The two configs from the plan:
//     --size 204800 --batch 1        single long transform (full CPI)
//     --size 128    --batch 1600     batched Doppler-style transform
//
// Notes for Tegra X1
// ------------------
//   * Unified memory: the "4 GB" is physically shared with the CPU and OS, so the
//     allocation ceiling depends on system memory pressure. Run headless (as you
//     do over SSH) with little else running to reach the true ceiling.
//   * Single precision (cufftComplex / C2C) is used throughout. FP32 is the design
//     target for this work anyway; on sm_53 FP64 throughput is ~1/32 of FP32, so a
//     double-precision harness would characterise a path you would not deploy.
//   * cuFFT is fastest on sizes whose prime factors are all in {2,3,5,7}. The tool
//     reports the largest prime factor of N and warns when it exceeds 7, because a
//     large prime factor (e.g. a CPI length that happens to be prime) can cost an
//     order of magnitude. 204800 = 2^13 * 5^2 -> 5-smooth, efficient.
//
// Build
// -----
//   Via CMake (make build), into build/cufft_bench.
//
// Examples
// --------
//   ./build/cufft_bench --size 204800 --batch 1 --iters 100
//   ./build/cufft_bench --size 128 --batch 1600 --iters 200 --budget-ms 5
//   ./build/cufft_bench --sweep-size --min-size 1024 --max-size 33554432 --batch 1 --csv > size_sweep.csv
//   ./build/cufft_bench --sweep-batch --size 128 --max-batch 1048576 --csv > batch_sweep.csv
//
// CSV goes to stdout (redirect to a file); human-readable progress goes to stderr,
// so you can log and watch at the same time.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pbr/check.hpp"   // CUDA_CHECK, and the CUDA library headers
#include "BenchCommon.hpp"

namespace {

const double kBytesPerMiB  = 1048576.0;
const int    kWarmupRuns   = 3;    // the first exec pays for lazy initialisation
const int    kExitCudaError = 2;   // an unexpected CUDA failure, not a ceiling
const int    kExitPointFailed = 3; // a single-point run that did not fit

// One (size, batch) measurement. The CSV columns keep these names.
struct PointResult {
    uint64_t    uSize           = 0;
    uint64_t    uBatch          = 0;
    bool        bOk             = false;
    const char* pFailReason     = "";
    double      dPlanMs         = 0.0;
    double      dExecMeanMs     = 0.0;
    double      dExecMinMs      = 0.0;
    double      dExecStdMs      = 0.0;
    size_t      uWorkspaceBytes = 0;
    size_t      uDataBytes      = 0;
    size_t      uMemFreeAfter   = 0;
    double      dGflops         = 0.0;
    double      dThroughputMsps = 0.0;
    uint64_t    uLargestPrime   = 1;
    bool        bWithinBudget   = true;
};

struct BenchConfig {
    int    iIters    = 100;
    bool   bManaged  = false;   // cudaMallocManaged (unified) vs cudaMalloc (device)
    bool   bInPlace  = true;    // in-place C2C
    double dBudgetMs = -1.0;    // below zero: no budget check
};

// Everything one point allocates, freed together by releasePoint().
struct PointBuffers {
    cufftHandle   hPlan      = 0;
    bool          bPlanValid = false;
    void*         d_pWork    = nullptr;
    cufftComplex* d_cIn      = nullptr;
    cufftComplex* d_cOut     = nullptr;   // == d_cIn when in place
};

void releasePoint(PointBuffers& oBuffers)
{
    if (oBuffers.d_cOut != oBuffers.d_cIn)
        cudaFree(oBuffers.d_cOut);
    cudaFree(oBuffers.d_cIn);
    cudaFree(oBuffers.d_pWork);
    if (oBuffers.bPlanValid)
        cufftDestroy(oBuffers.hPlan);
    oBuffers = PointBuffers();
}

const char* cufftErrorName(cufftResult eResult)
{
    switch (eResult) {
    case CUFFT_SUCCESS:        return "SUCCESS";
    case CUFFT_INVALID_PLAN:   return "INVALID_PLAN";
    case CUFFT_ALLOC_FAILED:   return "ALLOC_FAILED";
    case CUFFT_INVALID_VALUE:  return "INVALID_VALUE";
    case CUFFT_INTERNAL_ERROR: return "INTERNAL_ERROR";
    case CUFFT_EXEC_FAILED:    return "EXEC_FAILED";
    case CUFFT_SETUP_FAILED:   return "SETUP_FAILED";
    case CUFFT_INVALID_SIZE:   return "INVALID_SIZE";
    default:                   return "UNKNOWN";
    }
}

uint64_t largestPrimeFactor(uint64_t uN)
{
    uint64_t uLargest = 1;
    while (uN % 2 == 0) { uLargest = 2; uN /= 2; }
    for (uint64_t uFactor = 3; uFactor * uFactor <= uN; uFactor += 2)
        while (uN % uFactor == 0) { uLargest = uFactor; uN /= uFactor; }
    if (uN > 1)
        uLargest = uN;
    return uLargest;
}

// Plan, allocate and time one point into oResult. Running out of memory is a
// result, not an error: it returns true with bOk false and the reason, so a
// sweep can walk right up to the ceiling and report it. False only on an
// unexpected CUDA failure, already logged.
bool measurePoint(const BenchConfig& oConfig, PointBuffers& oBuffers, PointResult& oResult)
{
    const uint64_t uSize  = oResult.uSize;
    const uint64_t uBatch = oResult.uBatch;

    if (cufftCreate(&oBuffers.hPlan) != CUFFT_SUCCESS) {
        oResult.pFailReason = "cufftCreate";
        return true;
    }
    oBuffers.bPlanValid = true;
    // Manage the workspace ourselves, so its size is measured and running out
    // of memory for it is caught.
    cufftSetAutoAllocation(oBuffers.hPlan, 0);

    // ---- plan creation, timed ------------------------------------------------
    int aiDims[1] = { static_cast<int>(uSize) };
    cudaEvent_t hStart, hStop;
    CUDA_CHECK(cudaEventCreate(&hStart));
    CUDA_CHECK(cudaEventCreate(&hStop));
    CUDA_CHECK(cudaEventRecord(hStart));
    size_t uWorkspace = 0;
    const cufftResult ePlan = cufftMakePlanMany(oBuffers.hPlan, 1, aiDims,
                                                nullptr, 1, 0, nullptr, 1, 0,
                                                CUFFT_C2C, static_cast<int>(uBatch),
                                                &uWorkspace);
    CUDA_CHECK(cudaEventRecord(hStop));
    CUDA_CHECK(cudaEventSynchronize(hStop));
    float fPlanMs = 0.0f;
    CUDA_CHECK(cudaEventElapsedTime(&fPlanMs, hStart, hStop));
    cudaEventDestroy(hStart);
    cudaEventDestroy(hStop);
    if (ePlan != CUFFT_SUCCESS) {
        oResult.pFailReason = (ePlan == CUFFT_ALLOC_FAILED) ? "plan_alloc" : cufftErrorName(ePlan);
        return true;
    }
    oResult.dPlanMs         = fPlanMs;
    oResult.uWorkspaceBytes = uWorkspace;

    // ---- workspace and data buffers ------------------------------------------
    // A failed allocation leaves a sticky error; cudaGetLastError clears it.
    if (uWorkspace > 0 && cudaMalloc(&oBuffers.d_pWork, uWorkspace) != cudaSuccess) {
        cudaGetLastError();
        oResult.pFailReason = "workspace_malloc";
        return true;
    }
    cufftSetWorkArea(oBuffers.hPlan, oBuffers.d_pWork);

    const size_t uElements = static_cast<size_t>(uSize * uBatch);
    const size_t uBytes    = uElements * sizeof(cufftComplex);
    oResult.uDataBytes = oConfig.bInPlace ? uBytes : 2 * uBytes;

    cudaError_t eIn = oConfig.bManaged
        ? cudaMallocManaged(reinterpret_cast<void**>(&oBuffers.d_cIn), uBytes)
        : cudaMalloc(reinterpret_cast<void**>(&oBuffers.d_cIn), uBytes);
    if (eIn != cudaSuccess) {
        cudaGetLastError();
        oResult.pFailReason = "input_malloc";
        return true;
    }
    if (oConfig.bInPlace) {
        oBuffers.d_cOut = oBuffers.d_cIn;
    } else {
        cudaError_t eOut = oConfig.bManaged
            ? cudaMallocManaged(reinterpret_cast<void**>(&oBuffers.d_cOut), uBytes)
            : cudaMalloc(reinterpret_cast<void**>(&oBuffers.d_cOut), uBytes);
        if (eOut != cudaSuccess) {
            cudaGetLastError();
            oResult.pFailReason = "output_malloc";
            return true;
        }
    }

    // The values are irrelevant to timing; the input only has to be defined.
    if (oConfig.bManaged) {
        for (size_t u = 0; u < uElements; ++u) {
            const uint32_t uHash = static_cast<uint32_t>(u * 2654435761u);
            oBuffers.d_cIn[u].x = static_cast<float>(uHash & 0xFFFF) / 65536.0f - 0.5f;
            oBuffers.d_cIn[u].y = 0.0f;
        }
    } else {
        CUDA_CHECK(cudaMemset(oBuffers.d_cIn, 0, uBytes));
    }

    // ---- warm-up, then the timed runs ----------------------------------------
    for (int i = 0; i < kWarmupRuns; ++i) {
        const cufftResult eExec = cufftExecC2C(oBuffers.hPlan, oBuffers.d_cIn,
                                               oBuffers.d_cOut, CUFFT_FORWARD);
        if (eExec != CUFFT_SUCCESS) {
            oResult.pFailReason = cufftErrorName(eExec);
            return true;
        }
    }
    CUDA_CHECK(cudaDeviceSynchronize());

    CUDA_CHECK(cudaEventCreate(&hStart));
    CUDA_CHECK(cudaEventCreate(&hStop));
    std::vector<double> vdMs;
    vdMs.reserve(static_cast<size_t>(oConfig.iIters));
    for (int i = 0; i < oConfig.iIters; ++i) {
        CUDA_CHECK(cudaEventRecord(hStart));
        const cufftResult eExec = cufftExecC2C(oBuffers.hPlan, oBuffers.d_cIn,
                                               oBuffers.d_cOut, CUFFT_FORWARD);
        CUDA_CHECK(cudaEventRecord(hStop));
        CUDA_CHECK(cudaEventSynchronize(hStop));
        // A failed transform returns at once and would be timed as a fast one.
        if (eExec != CUFFT_SUCCESS) {
            cudaEventDestroy(hStart);
            cudaEventDestroy(hStop);
            oResult.pFailReason = cufftErrorName(eExec);
            return true;
        }
        float fMs = 0.0f;
        CUDA_CHECK(cudaEventElapsedTime(&fMs, hStart, hStop));
        vdMs.push_back(static_cast<double>(fMs));
    }
    cudaEventDestroy(hStart);
    cudaEventDestroy(hStop);

    // ---- statistics ----------------------------------------------------------
    // Population standard deviation, as this tool has always reported it.
    const pbrbench::TimingSummary oTiming = pbrbench::summarise(vdMs);
    double dSq = 0.0;
    for (size_t u = 0; u < vdMs.size(); ++u)
        dSq += (vdMs[u] - oTiming.dMeanMs) * (vdMs[u] - oTiming.dMeanMs);
    oResult.dExecMeanMs = oTiming.dMeanMs;
    oResult.dExecMinMs  = oTiming.dMinMs;
    oResult.dExecStdMs  = std::sqrt(dSq / static_cast<double>(vdMs.size()));

    // The standard 5 N log2(N) operations per complex transform, times batch.
    const double dFlops = 5.0 * static_cast<double>(uSize) *
                          std::log2(static_cast<double>(uSize)) * static_cast<double>(uBatch);
    oResult.dGflops         = dFlops / (oResult.dExecMeanMs * 1e-3) / 1e9;
    oResult.dThroughputMsps = (static_cast<double>(uSize) * static_cast<double>(uBatch)) /
                              (oResult.dExecMeanMs * 1e-3) / 1e6;

    size_t uFree = 0, uTotal = 0;
    CUDA_CHECK(cudaMemGetInfo(&uFree, &uTotal));
    oResult.uMemFreeAfter = uFree;

    if (oConfig.dBudgetMs > 0.0)
        oResult.bWithinBudget = (oResult.dExecMeanMs <= oConfig.dBudgetMs);
    oResult.bOk = true;
    return true;
}

// One point, start to finish. False only on an unexpected CUDA failure.
bool runPoint(uint64_t uSize, uint64_t uBatch, const BenchConfig& oConfig, PointResult& oResult)
{
    oResult = PointResult();
    oResult.uSize         = uSize;
    oResult.uBatch        = uBatch;
    oResult.uLargestPrime = largestPrimeFactor(uSize);

    PointBuffers oBuffers;
    const bool bOk = measurePoint(oConfig, oBuffers, oResult);
    releasePoint(oBuffers);
    return bOk;
}

// ---- reporting --------------------------------------------------------------

void printCsvHeader()
{
    std::printf("n,batch,ok,fail_reason,plan_ms,exec_mean_ms,exec_min_ms,exec_std_ms,"
                "gflops,throughput_msps,workspace_MB,data_MB,mem_free_MB,"
                "largest_prime,within_budget\n");
}

void printCsvRow(const PointResult& oResult)
{
    std::printf("%zu,%zu,%d,%s,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%zu,%d\n",
                static_cast<size_t>(oResult.uSize), static_cast<size_t>(oResult.uBatch),
                oResult.bOk ? 1 : 0, oResult.bOk ? "" : oResult.pFailReason,
                oResult.dPlanMs, oResult.dExecMeanMs, oResult.dExecMinMs, oResult.dExecStdMs,
                oResult.dGflops, oResult.dThroughputMsps,
                oResult.uWorkspaceBytes / kBytesPerMiB, oResult.uDataBytes / kBytesPerMiB,
                oResult.uMemFreeAfter / kBytesPerMiB, static_cast<size_t>(oResult.uLargestPrime),
                oResult.bWithinBudget ? 1 : 0);
    std::fflush(stdout);
}

void printHuman(const PointResult& oResult, const BenchConfig& oConfig)
{
    if (!oResult.bOk) {
        std::fprintf(stderr, "  N=%-9zu batch=%-8zu  FAILED (%s)\n",
                     static_cast<size_t>(oResult.uSize), static_cast<size_t>(oResult.uBatch),
                     oResult.pFailReason);
        return;
    }
    std::fprintf(stderr,
                 "  N=%-9zu batch=%-8zu  exec %.3f ms (min %.3f, sd %.3f)  "
                 "%.1f GFLOP/s  %.1f MS/s  data %.0f MB  ws %.0f MB  free %.0f MB",
                 static_cast<size_t>(oResult.uSize), static_cast<size_t>(oResult.uBatch),
                 oResult.dExecMeanMs, oResult.dExecMinMs, oResult.dExecStdMs,
                 oResult.dGflops, oResult.dThroughputMsps,
                 oResult.uDataBytes / kBytesPerMiB, oResult.uWorkspaceBytes / kBytesPerMiB,
                 oResult.uMemFreeAfter / kBytesPerMiB);
    if (oConfig.dBudgetMs > 0.0)
        std::fprintf(stderr, "  [%s budget]", oResult.bWithinBudget ? "WITHIN" : "OVER");
    // cuFFT is fastest on sizes whose prime factors are all 2, 3, 5 or 7.
    if (oResult.uLargestPrime > 7)
        std::fprintf(stderr, "  (!) largest prime factor %zu > 7: cuFFT slow, "
                             "prefer a 7-smooth size",
                     static_cast<size_t>(oResult.uLargestPrime));
    std::fprintf(stderr, "\n");
}

// ---- command line -----------------------------------------------------------

struct Options {
    uint64_t    uSize       = 204800;
    uint64_t    uBatch      = 1;
    uint64_t    uMinSize    = 1024;
    uint64_t    uMaxSize    = 67108864;
    uint64_t    uMaxBatch   = 4194304;
    bool        bSweepSize  = false;
    bool        bSweepBatch = false;
    bool        bCsv        = false;
    BenchConfig oConfig;
};

void printUsage(const char* pProgram)
{
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --size N            FFT length (default 204800)\n"
        "  --batch B           number of transforms (default 1)\n"
        "  --iters I           timed iterations (default 100)\n"
        "  --managed           use unified memory (cudaMallocManaged)\n"
        "  --outofplace        separate input/output buffers\n"
        "  --budget-ms X       flag configs whose mean latency exceeds X ms\n"
        "  --sweep-size        double N from --min-size to --max-size at fixed batch\n"
        "  --sweep-batch       double batch from 1 up to --max-batch at fixed size\n"
        "  --min-size N        sweep lower bound (default 1024)\n"
        "  --max-size N        sweep upper bound (default 67108864)\n"
        "  --max-batch B       batch sweep upper bound (default 4194304)\n"
        "  --csv               emit CSV rows on stdout\n"
        "  --help\n", pProgram);
}

// 0 to run, otherwise the exit code: 0 for --help, 1 for a bad argument.
int parseArgs(int argc, char* argv[], Options& oOptions, bool& bRun)
{
    bRun = false;
    for (int i = 1; i < argc; ++i) {
        const std::string sArg = argv[i];
        const bool bHasValue   = (i + 1 < argc);
        const bool bTakesValue = (sArg == "--size" || sArg == "--batch" || sArg == "--iters" ||
                                  sArg == "--budget-ms" || sArg == "--min-size" ||
                                  sArg == "--max-size" || sArg == "--max-batch");
        if (bTakesValue && !bHasValue) {
            printUsage(argv[0]);
            return 1;
        }

        if      (sArg == "--size")        oOptions.uSize     = std::strtoull(argv[++i], nullptr, 10);
        else if (sArg == "--batch")       oOptions.uBatch    = std::strtoull(argv[++i], nullptr, 10);
        else if (sArg == "--iters")       oOptions.oConfig.iIters    = std::atoi(argv[++i]);
        else if (sArg == "--budget-ms")   oOptions.oConfig.dBudgetMs = std::atof(argv[++i]);
        else if (sArg == "--min-size")    oOptions.uMinSize  = std::strtoull(argv[++i], nullptr, 10);
        else if (sArg == "--max-size")    oOptions.uMaxSize  = std::strtoull(argv[++i], nullptr, 10);
        else if (sArg == "--max-batch")   oOptions.uMaxBatch = std::strtoull(argv[++i], nullptr, 10);
        else if (sArg == "--managed")     oOptions.oConfig.bManaged = true;
        else if (sArg == "--outofplace")  oOptions.oConfig.bInPlace = false;
        else if (sArg == "--sweep-size")  oOptions.bSweepSize  = true;
        else if (sArg == "--sweep-batch") oOptions.bSweepBatch = true;
        else if (sArg == "--csv")         oOptions.bCsv = true;
        else if (sArg == "--help")        { printUsage(argv[0]); return 0; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", sArg.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }
    bRun = true;
    return 0;
}

bool printDevice(const BenchConfig& oConfig)
{
    int iDevice = 0;
    cudaDeviceProp oProp;
    CUDA_CHECK(cudaGetDevice(&iDevice));
    CUDA_CHECK(cudaGetDeviceProperties(&oProp, iDevice));
    size_t uFree = 0, uTotal = 0;
    CUDA_CHECK(cudaMemGetInfo(&uFree, &uTotal));
    std::fprintf(stderr, "Device: %s  sm_%d%d  total %.0f MB  free %.0f MB  "
                         "memory=%s  placement=%s\n\n",
                 oProp.name, oProp.major, oProp.minor, uTotal / kBytesPerMiB, uFree / kBytesPerMiB,
                 oConfig.bManaged ? "managed" : "device",
                 oConfig.bInPlace ? "in-place" : "out-of-place");
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    Options oOptions;
    bool bRun = false;
    const int iParse = parseArgs(argc, argv, oOptions, bRun);
    if (!bRun)
        return iParse;

    const BenchConfig& oConfig = oOptions.oConfig;
    if (!printDevice(oConfig))
        return kExitCudaError;
    if (oOptions.bCsv)
        printCsvHeader();

    PointResult oResult;
    if (oOptions.bSweepSize) {
        std::fprintf(stderr, "Sweeping N = %zu .. %zu (x2) at batch=%zu\n",
                     static_cast<size_t>(oOptions.uMinSize), static_cast<size_t>(oOptions.uMaxSize),
                     static_cast<size_t>(oOptions.uBatch));
        for (uint64_t uSize = oOptions.uMinSize; uSize <= oOptions.uMaxSize; uSize *= 2) {
            if (!runPoint(uSize, oOptions.uBatch, oConfig, oResult))
                return kExitCudaError;
            printHuman(oResult, oConfig);
            if (oOptions.bCsv) printCsvRow(oResult);
            if (!oResult.bOk) {
                std::fprintf(stderr, "-> ceiling reached at N=%zu (%s)\n",
                             static_cast<size_t>(uSize), oResult.pFailReason);
                break;
            }
        }
    } else if (oOptions.bSweepBatch) {
        std::fprintf(stderr, "Sweeping batch = 1 .. %zu (x2) at N=%zu\n",
                     static_cast<size_t>(oOptions.uMaxBatch), static_cast<size_t>(oOptions.uSize));
        for (uint64_t uBatch = 1; uBatch <= oOptions.uMaxBatch; uBatch *= 2) {
            if (!runPoint(oOptions.uSize, uBatch, oConfig, oResult))
                return kExitCudaError;
            printHuman(oResult, oConfig);
            if (oOptions.bCsv) printCsvRow(oResult);
            if (!oResult.bOk) {
                std::fprintf(stderr, "-> ceiling reached at batch=%zu (%s)\n",
                             static_cast<size_t>(uBatch), oResult.pFailReason);
                break;
            }
        }
    } else {
        if (!runPoint(oOptions.uSize, oOptions.uBatch, oConfig, oResult))
            return kExitCudaError;
        printHuman(oResult, oConfig);
        if (oOptions.bCsv) printCsvRow(oResult);
        if (!oResult.bOk)
            return kExitPointFailed;
    }
    return 0;
}
