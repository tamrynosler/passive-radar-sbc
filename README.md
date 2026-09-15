# passive-radar-sbc

FM-based passive bistatic radar (PBR) processing on embedded GPU hardware.

Real-time direct-signal-interference (DSI) cancellation, cross-ambiguity
processing and CFAR detection, ported from a validated MATLAB reference to
native C++/CUDA and benchmarked on an NVIDIA Jetson Nano (Tegra X1, sm_53,
CUDA 10.2). Signal source is the FERS radar simulator streaming VITA 49.2 over
UDP, decoupled from the processing platform so the same ingest interface later
accepts a live receiver front end.


## Status

- [x] Jetson Nano bring-up, CUDA 10.2 verified (`deviceQuery` PASS)
- [x] MATLAB reference chain (FERS → ECA-B → CAF range–Doppler → 2D CA-CFAR)
- [x] cuFFT timing + capacity harness (`bench/`) — built and running on-hardware
- [x] Remote build workflow (edit on laptop → build + run on Jetson over SSH)
- [ ] C++/CUDA VITA 49.2 ingest + circular buffer  ← next
- [ ] DSI cancellation kernels: ECA-S, ECA-B, ECA-CD, CGLS, batches
- [ ] CAF range–Doppler + 2D CA-CFAR
- [ ] Algorithm benchmarking study (suppression vs latency)

> The cuFFT harness compiles, runs, times cleanly and emits valid CSV on the
> Nano. Latency numbers captured so far are **provisional** — taken on Micro-USB
> power with DVFS active (see the measurement note below). Citable figures await
> a stable barrel-jack supply with clocks pinned.


## Repository layout

```
passive-radar-sbc/
├── CMakeLists.txt        top-level build (CMake >= 3.10; see Build)
├── Makefile              laptop-side driver: sync + remote build/run over SSH
├── jetson.mk             thin on-board wrapper around CMake (for direct use)
├── README.md
├── .gitignore
├── bench/                standalone benchmarking harnesses
│   └── cufft_bench.cu    cuFFT timing + memory-ceiling tool
├── include/pbr/          public headers for the processing library
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
- CMake >= 3.10 — the stock version from L4T apt works. The CMakeLists uses
  explicit `-gencode arch=compute_53,code=sm_53` below CMake 3.18 and the modern
  `CMAKE_CUDA_ARCHITECTURES` path at/above it, so a newer CMake is optional.
- A C++14-capable host compiler (g++ 7.5 shipped with L4T is fine). CUDA 10.2
  device code is limited to C++14; do not use C++17 features in `.cu` files.

## Build

Development happens on a laptop; the Jetson is the build/run target. Editing is
done on the laptop and synced to the board over SSH — the top-level `Makefile`
drives a remote CMake build, so you never edit on the Jetson directly (an rsync
with `--delete` mirrors the tree and will clobber on-board edits).

### Everyday workflow (from the laptop)

Requires a `Host jetson` entry in `~/.ssh/config` with key-based auth.

```bash
make build     # rsync tree to the Jetson, then configure + compile there
make test      # build, then run the default cuFFT harness config
make bench     # build, run scripts/run_cufft_bench.sh, pull CSV/logs back
make shell     # interactive shell in the remote repo
make clean-remote
```

### Directly on the Jetson

Stock CMake 3.10 is sufficient. nvcc is pinned by absolute path so the build
does not depend on the shell PATH (a non-interactive SSH shell does not source
the interactive part of `~/.bashrc`):

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CUDA_COMPILER=/usr/local/cuda-10.2/bin/nvcc ..
make -j2
# -> build/cufft_bench
```

`-j2` (not `-j4`) is deliberate: the 4 GB is shared with the GPU, so a
lower job count avoids OOM on heavier template-heavy translation units later.

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

> **Measurement note.** Latency figures are only meaningful on a stable 5 V/4 A
> barrel-jack supply (J48 jumper fitted) with the desktop stopped
> (`sudo systemctl set-default multi-user.target`) and clocks pinned
> (`sudo nvpmodel -m 0 && sudo jetson_clocks`). On Micro-USB power or with DVFS
> active, per-run *means* vary several-fold and can even be misleadingly
> *consistent* while the GPU sits throttled; the per-run *minimum* tracks the
> true kernel time but is not citable on its own. Always record the nvpmodel
> mode alongside results. The sweeps push allocation/compute to the ceiling and
> should only be run on the barrel jack — rail sag under load corrupts the
> microSD rather than failing cleanly.

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

TODO: Confirm UCT's IP policy for thesis code before choosing a licence.
