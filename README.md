# smartpol-pbr

FM-based passive bistatic radar (PBR) processing on embedded GPU hardware.

Real-time direct-signal-interference (DSI) cancellation, cross-ambiguity
processing and CFAR detection, ported from a validated MATLAB reference to
native C++/CUDA and benchmarked on an NVIDIA Jetson Nano (Tegra X1, sm_53,
CUDA 10.2). Signal source is the FERS radar simulator streaming VITA 49.2 over
UDP, decoupled from the processing platform so the same ingest interface later
accepts a live receiver front end.

> Rename this repo to match your thesis / lab naming before publishing.

## Status

- [x] Jetson Nano bring-up, CUDA 10.2 verified (`deviceQuery` PASS)
- [x] MATLAB reference chain (FERS → ECA-B → CAF range–Doppler → 2D CA-CFAR)
- [x] cuFFT timing + capacity harness (`bench/`)
- [ ] C++/CUDA VITA 49.2 ingest + circular buffer
- [ ] DSI cancellation kernels: ECA-S, ECA-B, ECA-CD, CGLS, batches
- [ ] CAF range–Doppler + 2D CA-CFAR
- [ ] Algorithm benchmarking study (suppression vs latency)

## Repository layout

```
smartpol-pbr/
├── CMakeLists.txt        top-level build (needs CMake >= 3.18)
├── README.md
├── .gitignore
├── bench/                standalone benchmarking harnesses
│   └── cufft_bench.cu    cuFFT timing + memory-ceiling tool
├── include/smartpol/     public headers for the processing library
├── src/
│   ├── ingest/           VITA 49.2 UDP receiver + lock-free circular buffer
│   ├── dsp/              DSI cancellation, CAF, CFAR kernels
│   ├── fft/              cuFFT plan/lifetime wrappers
│   ├── linalg/           cuBLAS / cuSOLVER wrappers (ECA, CGLS)
│   └── app/              executables (offline runner, live pipeline)
├── tests/                unit + integration tests, MATLAB-vs-CUDA validation
├── matlab/               the reference implementation (validation ground truth)
├── config/               FERS scenarios (.fersxml), geometry + processing params
├── scripts/              run wrappers, tegrastats logging, profiling helpers
├── docs/                 architecture notes, benchmarking methodology, results
└── cmake/                helper modules
```

Large data (captured IQ, FERS output, `.mat` files, benchmark CSVs, tegrastats
logs) is **not** version-controlled — it lives on the microSD card at `/data`
and is regenerated from the FERS scenarios in `config/`. See `.gitignore`.

## Requirements

- Jetson Nano, L4T R32.7.x, CUDA 10.2 (cuFFT, cuBLAS; cuSOLVER on Orin)
- CMake >= 3.18 (the 3.10 from apt will not build CUDA targets — see below)
- A C++17 host compiler (the g++ shipped with L4T is fine)

## Build

Modern CMake, once (`~/.local/bin` on PATH):

```bash
python3 -m pip install --user "cmake>=3.18"
```

Then:

```bash
cmake -S . -B build -DCMAKE_CUDA_ARCHITECTURES=53
cmake --build build -j4
# -> build/cufft_bench
```

No-CMake fallback for the harness alone:

```bash
nvcc -O3 -arch=sm_53 bench/cufft_bench.cu -lcufft -o cufft_bench
```

## Running the benchmark

Find the memory ceiling (single long transform, doubling N until allocation fails):

```bash
./cufft_bench --sweep-size --min-size 1024 --max-size 268435456 --batch 1 --csv > size_sweep.csv
```

Find the batch ceiling at a fixed Doppler length:

```bash
./cufft_bench --sweep-batch --size 128 --max-batch 4194304 --csv > batch_sweep.csv
```

Characterise the two plan configs against a real-time budget:

```bash
./cufft_bench --size 204800 --batch 1    --iters 200 --budget-ms 1000   # full 1 s CPI
./cufft_bench --size 128    --batch 1600 --iters 200 --budget-ms 5      # batched Doppler
```

Under controlled clocks/fan with a tegrastats trace (for the methodology chapter):

```bash
chmod +x scripts/run_cufft_bench.sh
scripts/run_cufft_bench.sh -- --sweep-size --min-size 1024 --max-size 33554432 --batch 1 --csv
# results + tegrastats.log land in /data/bench/<timestamp>/
```

### Reading the output

Each row reports mean/min/std execution time, effective GFLOP/s, sample
throughput, workspace and data footprint, and free memory after allocation.
Two design signals to watch:

- **Memory ceiling** — the N or batch at which a row reports `FAILED`, and which
  allocation (`plan_alloc`, `workspace_malloc`, `input_malloc`) hit the wall
  first. On Tegra the 4 GB is shared with CPU/OS, so run headless for the true
  ceiling.
- **7-smoothness** — the tool warns when N has a prime factor above 7. cuFFT is
  fastest on sizes factoring into {2,3,5,7}; a CPI length that lands on a large
  prime can cost an order of magnitude. Pick FFT lengths accordingly
  (204800 = 2¹³·5² is fine).

## Licence

Confirm your institution's IP policy for thesis code before choosing a licence.
If you are free to open-source, MIT or BSD-3-Clause are common for academic work.
