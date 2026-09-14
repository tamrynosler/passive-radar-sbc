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
//   Via CMake (see top-level CMakeLists.txt), or directly:
//     nvcc -O3 -arch=sm_53 bench/cufft_bench.cu -lcufft -o cufft_bench
//
// Examples
// --------
//   ./cufft_bench --size 204800 --batch 1 --iters 100
//   ./cufft_bench --size 128 --batch 1600 --iters 200 --budget-ms 5
//   ./cufft_bench --sweep-size --min-size 1024 --max-size 33554432 --batch 1 --csv > size_sweep.csv
//   ./cufft_bench --sweep-batch --size 128 --max-batch 1048576 --csv > batch_sweep.csv
//
// CSV goes to stdout (redirect to a file); human-readable progress goes to stderr,
// so you can log and watch at the same time.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#include <cuda_runtime.h>
#include <cufft.h>

// ---- error handling ---------------------------------------------------------

#define CUDA_CHECK(x)                                                          \
  do {                                                                         \
    cudaError_t err = (x);                                                     \
    if (err != cudaSuccess) {                                                  \
      fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__, __LINE__,            \
              cudaGetErrorString(err));                                        \
      std::exit(2);                                                            \
    }                                                                          \
  } while (0)

static const char* cufftErr(cufftResult r) {
  switch (r) {
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

// ---- helpers ----------------------------------------------------------------

static size_t largestPrimeFactor(size_t n) {
  size_t lpf = 1;
  while (n % 2 == 0) { lpf = 2; n /= 2; }
  for (size_t f = 3; f * f <= n; f += 2) {
    while (n % f == 0) { lpf = f; n /= f; }
  }
  if (n > 1) lpf = n;
  return lpf;
}

struct Result {
  size_t n = 0, batch = 0;
  bool ok = false;
  const char* fail_reason = "";
  double plan_ms = 0, exec_mean_ms = 0, exec_min_ms = 0, exec_std_ms = 0;
  size_t workspace_bytes = 0, data_bytes = 0;
  size_t mem_free_after = 0, mem_total = 0;
  double gflops = 0, throughput_msps = 0;
  size_t largest_prime = 1;
  bool within_budget = true;
};

struct Config {
  int iters = 100;
  bool managed = false;   // cudaMallocManaged (unified) vs cudaMalloc (device)
  bool inplace = true;    // in-place C2C
  double budget_ms = -1;  // <0 means no budget check
};

// Run one (N, batch) point. Never aborts on resource exhaustion: returns ok=false
// with a reason, so sweeps can walk right up to the ceiling and report it.
static Result runOne(size_t n, size_t batch, const Config& cfg) {
  Result R;
  R.n = n;
  R.batch = batch;
  R.largest_prime = largestPrimeFactor(n);

  int rank = 1;
  int dims[1] = {static_cast<int>(n)};

  cufftHandle plan;
  if (cufftCreate(&plan) != CUFFT_SUCCESS) { R.fail_reason = "cufftCreate"; return R; }
  cufftSetAutoAllocation(plan, 0);  // manage workspace ourselves so we can measure/catch OOM

  // --- plan creation (timed) ---
  cudaEvent_t p0, p1;
  CUDA_CHECK(cudaEventCreate(&p0));
  CUDA_CHECK(cudaEventCreate(&p1));
  CUDA_CHECK(cudaEventRecord(p0));
  size_t ws = 0;
  cufftResult pr = cufftMakePlanMany(plan, rank, dims,
                                     nullptr, 1, 0,
                                     nullptr, 1, 0,
                                     CUFFT_C2C, static_cast<int>(batch), &ws);
  CUDA_CHECK(cudaEventRecord(p1));
  CUDA_CHECK(cudaEventSynchronize(p1));
  float pms = 0;
  CUDA_CHECK(cudaEventElapsedTime(&pms, p0, p1));
  cudaEventDestroy(p0);
  cudaEventDestroy(p1);
  if (pr != CUFFT_SUCCESS) {
    R.fail_reason = (pr == CUFFT_ALLOC_FAILED) ? "plan_alloc" : cufftErr(pr);
    cufftDestroy(plan);
    return R;
  }
  R.plan_ms = pms;
  R.workspace_bytes = ws;

  // --- workspace ---
  void* work = nullptr;
  if (ws > 0) {
    if (cudaMalloc(&work, ws) != cudaSuccess) {
      cudaGetLastError();  // clear sticky error
      R.fail_reason = "workspace_malloc";
      cufftDestroy(plan);
      return R;
    }
  }
  cufftSetWorkArea(plan, work);

  // --- data buffers ---
  const size_t elems = n * batch;
  const size_t bytes = elems * sizeof(cufftComplex);
  R.data_bytes = cfg.inplace ? bytes : 2 * bytes;

  auto allocBuf = [&](cufftComplex** p) -> cudaError_t {
    return cfg.managed ? cudaMallocManaged((void**)p, bytes)
                       : cudaMalloc((void**)p, bytes);
  };

  cufftComplex *in = nullptr, *out = nullptr;
  if (allocBuf(&in) != cudaSuccess) {
    cudaGetLastError();
    R.fail_reason = "input_malloc";
    if (work) cudaFree(work);
    cufftDestroy(plan);
    return R;
  }
  if (cfg.inplace) {
    out = in;
  } else if (allocBuf(&out) != cudaSuccess) {
    cudaGetLastError();
    R.fail_reason = "output_malloc";
    cudaFree(in);
    if (work) cudaFree(work);
    cufftDestroy(plan);
    return R;
  }

  // Initialise input. Values are irrelevant to timing; we just need it defined.
  if (cfg.managed) {
    for (size_t i = 0; i < elems; ++i) {
      uint32_t h = (uint32_t)(i * 2654435761u);
      in[i].x = (float)(h & 0xFFFF) / 65536.0f - 0.5f;
      in[i].y = 0.0f;
    }
  } else {
    CUDA_CHECK(cudaMemset(in, 0, bytes));
  }

  // --- warmup (first exec includes lazy init) ---
  for (int w = 0; w < 3; ++w) {
    cufftResult er = cufftExecC2C(plan, in, out, CUFFT_FORWARD);
    if (er != CUFFT_SUCCESS) {
      R.fail_reason = cufftErr(er);
      if (out != in) cudaFree(out);
      cudaFree(in);
      if (work) cudaFree(work);
      cufftDestroy(plan);
      return R;
    }
  }
  CUDA_CHECK(cudaDeviceSynchronize());

  // --- timed loop ---
  cudaEvent_t e0, e1;
  CUDA_CHECK(cudaEventCreate(&e0));
  CUDA_CHECK(cudaEventCreate(&e1));
  std::vector<double> times;
  times.reserve(cfg.iters);
  for (int it = 0; it < cfg.iters; ++it) {
    CUDA_CHECK(cudaEventRecord(e0));
    cufftExecC2C(plan, in, out, CUFFT_FORWARD);
    CUDA_CHECK(cudaEventRecord(e1));
    CUDA_CHECK(cudaEventSynchronize(e1));
    float ms = 0;
    CUDA_CHECK(cudaEventElapsedTime(&ms, e0, e1));
    times.push_back((double)ms);
  }
  cudaEventDestroy(e0);
  cudaEventDestroy(e1);

  // --- statistics ---
  double sum = 0, mn = 1e300;
  for (double t : times) { sum += t; mn = std::min(mn, t); }
  double mean = sum / times.size();
  double var = 0;
  for (double t : times) var += (t - mean) * (t - mean);
  var /= times.size();
  R.exec_mean_ms = mean;
  R.exec_min_ms = mn;
  R.exec_std_ms = std::sqrt(var);

  // FLOPs: standard 5 N log2(N) per complex transform, times batch.
  double flops = 5.0 * (double)n * std::log2((double)n) * (double)batch;
  R.gflops = flops / (mean * 1e-3) / 1e9;
  R.throughput_msps = ((double)n * (double)batch) / (mean * 1e-3) / 1e6;

  size_t freeb = 0, totb = 0;
  CUDA_CHECK(cudaMemGetInfo(&freeb, &totb));
  R.mem_free_after = freeb;
  R.mem_total = totb;

  if (cfg.budget_ms > 0) R.within_budget = (mean <= cfg.budget_ms);
  R.ok = true;

  if (out != in) cudaFree(out);
  cudaFree(in);
  if (work) cudaFree(work);
  cufftDestroy(plan);
  return R;
}

// ---- reporting --------------------------------------------------------------

static void printHeaderCSV() {
  printf("n,batch,ok,fail_reason,plan_ms,exec_mean_ms,exec_min_ms,exec_std_ms,"
         "gflops,throughput_msps,workspace_MB,data_MB,mem_free_MB,"
         "largest_prime,within_budget\n");
}

static void printRowCSV(const Result& R) {
  printf("%zu,%zu,%d,%s,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%zu,%d\n",
         R.n, R.batch, R.ok ? 1 : 0, R.ok ? "" : R.fail_reason,
         R.plan_ms, R.exec_mean_ms, R.exec_min_ms, R.exec_std_ms,
         R.gflops, R.throughput_msps,
         R.workspace_bytes / 1048576.0, R.data_bytes / 1048576.0,
         R.mem_free_after / 1048576.0, R.largest_prime, R.within_budget ? 1 : 0);
  fflush(stdout);
}

static void printHuman(const Result& R, const Config& cfg) {
  if (!R.ok) {
    fprintf(stderr, "  N=%-9zu batch=%-8zu  FAILED (%s)\n",
            R.n, R.batch, R.fail_reason);
    return;
  }
  fprintf(stderr,
          "  N=%-9zu batch=%-8zu  exec %.3f ms (min %.3f, sd %.3f)  "
          "%.1f GFLOP/s  %.1f MS/s  data %.0f MB  ws %.0f MB  free %.0f MB",
          R.n, R.batch, R.exec_mean_ms, R.exec_min_ms, R.exec_std_ms,
          R.gflops, R.throughput_msps,
          R.data_bytes / 1048576.0, R.workspace_bytes / 1048576.0,
          R.mem_free_after / 1048576.0);
  if (cfg.budget_ms > 0)
    fprintf(stderr, "  [%s budget]", R.within_budget ? "WITHIN" : "OVER");
  if (R.largest_prime > 7)
    fprintf(stderr, "  (!) largest prime factor %zu > 7: cuFFT slow, "
                    "prefer a 7-smooth size", R.largest_prime);
  fprintf(stderr, "\n");
}

// ---- CLI --------------------------------------------------------------------

static void usage(const char* prog) {
  fprintf(stderr,
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
    "  --help\n", prog);
}

int main(int argc, char** argv) {
  size_t size = 204800, batch = 1;
  size_t min_size = 1024, max_size = 67108864, max_batch = 4194304;
  bool sweep_size = false, sweep_batch = false, csv = false;
  Config cfg;

  auto need = [&](int& i) -> const char* {
    if (i + 1 >= argc) { usage(argv[0]); std::exit(1); }
    return argv[++i];
  };

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if      (a == "--size")       size = strtoull(need(i), nullptr, 10);
    else if (a == "--batch")      batch = strtoull(need(i), nullptr, 10);
    else if (a == "--iters")      cfg.iters = atoi(need(i));
    else if (a == "--managed")    cfg.managed = true;
    else if (a == "--outofplace") cfg.inplace = false;
    else if (a == "--budget-ms")  cfg.budget_ms = atof(need(i));
    else if (a == "--sweep-size") sweep_size = true;
    else if (a == "--sweep-batch")sweep_batch = true;
    else if (a == "--min-size")   min_size = strtoull(need(i), nullptr, 10);
    else if (a == "--max-size")   max_size = strtoull(need(i), nullptr, 10);
    else if (a == "--max-batch")  max_batch = strtoull(need(i), nullptr, 10);
    else if (a == "--csv")        csv = true;
    else if (a == "--help")       { usage(argv[0]); return 0; }
    else { fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(argv[0]); return 1; }
  }

  int dev = 0;
  cudaDeviceProp prop;
  CUDA_CHECK(cudaGetDevice(&dev));
  CUDA_CHECK(cudaGetDeviceProperties(&prop, dev));
  size_t freeb = 0, totb = 0;
  CUDA_CHECK(cudaMemGetInfo(&freeb, &totb));
  fprintf(stderr, "Device: %s  sm_%d%d  total %.0f MB  free %.0f MB  "
                  "memory=%s  placement=%s\n\n",
          prop.name, prop.major, prop.minor, totb / 1048576.0, freeb / 1048576.0,
          cfg.managed ? "managed" : "device",
          cfg.inplace ? "in-place" : "out-of-place");

  if (csv) printHeaderCSV();

  if (sweep_size) {
    fprintf(stderr, "Sweeping N = %zu .. %zu (x2) at batch=%zu\n",
            min_size, max_size, batch);
    for (size_t n = min_size; n <= max_size; n *= 2) {
      Result R = runOne(n, batch, cfg);
      printHuman(R, cfg);
      if (csv) printRowCSV(R);
      if (!R.ok) { fprintf(stderr, "-> ceiling reached at N=%zu (%s)\n",
                           n, R.fail_reason); break; }
    }
  } else if (sweep_batch) {
    fprintf(stderr, "Sweeping batch = 1 .. %zu (x2) at N=%zu\n", max_batch, size);
    for (size_t b = 1; b <= max_batch; b *= 2) {
      Result R = runOne(size, b, cfg);
      printHuman(R, cfg);
      if (csv) printRowCSV(R);
      if (!R.ok) { fprintf(stderr, "-> ceiling reached at batch=%zu (%s)\n",
                           b, R.fail_reason); break; }
    }
  } else {
    Result R = runOne(size, batch, cfg);
    printHuman(R, cfg);
    if (csv) printRowCSV(R);
    if (!R.ok) return 3;
  }

  return 0;
}
