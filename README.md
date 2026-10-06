# passive-radar-sbc

FM-based passive bistatic radar (PBR) processing on embedded GPU hardware.

Real-time direct-signal-interference (DSI) cancellation, cross-ambiguity
processing and CFAR detection, ported from a validated MATLAB reference to
native C++/CUDA and benchmarked on an NVIDIA Jetson Nano (Tegra X1, sm_53,
CUDA 10.2). The signal source is a USRP2 with a BasicRX, read over UHD by our
own driver and streamed as VITA 49.2 over UDP multicast; the FERS radar
simulator speaks the same profile and remains the test source. Range-Doppler
frames are multicast to a small C++ viewer that serves a browser page, from
which the chain's parameters can be changed while it runs.


## Status (2026-09-29)

- [x] Jetson Nano bring-up, CUDA 10.2, remote build workflow (edit on the
      Mac, build and run on the Jetson over SSH; `make help`)
- [x] MATLAB reference chain (FERS → ECA-B → CAF range–Doppler → 2D CA-CFAR)
- [x] VITA 49.2 ingest, absolute-index circular buffer with packet-based
      validity flags, CPI extraction, config file
- [x] DSI cancellation, CPU and GPU: ECA-m, ECA-B, ECA-S, ECA-CD, CGLS
- [x] CAF range–Doppler map (Direct and Batches) and 2D CA-CFAR on the GPU
- [x] `uhd_to_vita49`: USRP2/N210 → VITA 49.2 multicast, byte-identical to
      FERS's profile, verified on the Nano and from the Mac
- [x] ARD frames streamed over UDP multicast; `ard_view` (C++) serves the
      browser page; nothing written to disk unless asked
- [x] Blank frames with a status word on dead or unprocessable input;
      no crash, no stop
- [x] Live parameter changes over an HTTP control port, applied at CPI
      boundaries, logged; sample-rate mismatch adapts automatically
- [x] **Full chain in real time on the Jetson from the USRP2**: 25 ms per
      1.024 s CPI with clocks pinned (ECA-CD), 80 to 230 ms unpinned
- [ ] Antennas connected, spectrum orientation checked, first aircraft  ← next
- [ ] Algorithm benchmarking study (suppression vs latency), the thesis
      contribution; MATLAB goldens to be regenerated for the 200 kHz config

> Timing figures with clocks pinned come from `make run`, which needs an
> interactive sudo password; unpinned numbers are provisional.

Design records: `docs/UHD_VITA49_Driver_design.md`, `docs/ard-stream-design.md`,
`docs/live-parameters.md`, `docs/design_decisions.md`.


## Repository layout

```
passive-radar-sbc/
├── CMakeLists.txt        top-level build (CMake >= 3.10; see Build)
├── Makefile              laptop-side driver: sync + remote build/run over SSH
│                         (`make help` lists every target)
├── README.md
├── .gitignore
├── bench/                standalone benchmarking harnesses
│   └── cufft_bench.cu    cuFFT timing + memory-ceiling tool
├── include/pbr/          public headers for the processing library (flat)
├── src/
│   ├── ingest/           VITA 49.2 UDP receiver, circular buffer, config
│   ├── dsi/              DSI cancellation: ECA-B, ECA-S, ECA-CD, CGLS
│   ├── ard/              CAF / range–Doppler map, 2D CA-CFAR, map output
│   ├── display/          HttpServer + ard_viewer.html, served by ard_view
│   └── app/              pbr_run, uhd_to_vita49, ard_view, probes, verifiers
├── tests/                unit + integration tests, MATLAB-vs-CUDA validation
├── matlab/               the reference implementation (validation ground truth)
├── config/               prb.conf (USRP, 200 kHz) and fers.conf (FERS, 204800 Hz)
├── third_party/          approved external headers (currently none)
├── scripts/              run_live.sh, replay_udp.py, bench wrappers
├── docs/                 architecture notes, benchmarking methodology, results
└── cmake/                helper modules
```

`src/` is grouped by pipeline stage, not by library: cuFFT and cuBLAS calls
live in the stage that uses them rather than behind a wrapper layer. Public
headers stay flat in `include/pbr/` regardless of which subdirectory
implements them, so every include reads `#include "pbr/<Name>.hpp"`.

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
make sync      # edited config/*.conf only — 2 s, no compile
make build     # edited any C++/CUDA — sync then compile on the Jetson
make test      # unit tests on the Jetson
make run       # live run, clocks pinned, tegrastats logged
make drive     # the USRP driver on the Jetson
make view      # the viewer, forwarded to http://localhost:8080
make stop      # end every radar process left on the Jetson
make replay    # replay a recorded capture through the whole chain
make verify    # golden-file checks against MATLAB
make prelim    # one algorithm on one frozen CPI (ARGS=ECA-m)
make push-data # copy data/SCENARIO to the Jetson
make bench     # DSI timing sweep (ECA-m, ECA-B)
make pull      # copy results back to ./results
```

`make help` lists them all. Everything about a run — algorithm, backend,
CFAR and ECA settings — lives in `config/prb.conf`, which is read at **run**
time. So the tuning loop is just `make sync && make run`: no rebuild, and no
command-line flags to remember.

Local builds, for fast iteration without the board (no CUDA, so the GPU
backends are absent and asking for them fails loudly):

```bash
make host        # every tool into ./bin
make host-test   # unit tests locally
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

## Ingest and CPI extraction

The ingest layer is plain C++14 and POSIX sockets with no CUDA in it, so it
builds and runs on the laptop as well as the Jetson. Local runs prove
function; all timing numbers must come from the Jetson.

```bash
make host          # every tool into ./bin
make host-test     # build and run every unit test locally
```

`ingest_probe` is the live checkpoint. Start it **before** `fers-cli`, since it
binds the UDP port:

```bash
./bin/ingest_probe --seconds 30 --capture data/S0_clean/capture.pbrcap
# then, in another terminal:
fers-cli <scenario>.fersxml --out-dir=/tmp/out \
         --vita49 127.0.0.1:4991 --vita49-fullscale 0.0022 -n=6
```

It verifies, against the live stream, that every sample lands at the absolute
index its VITA 49 timestamp implies, reads back identically, and survives in
the ring buffer while being overwritten — then reports how many CPIs it
extracted. Exit status is 0 on PASS.

`ingest_replay` runs a recorded capture through the identical path, so every
stage stays testable without live FERS:

```bash
./bin/ingest_replay --file data/S0_clean/capture.pbrcap --csv /tmp/cpp_decode.csv
```

The `--csv` output feeds `matlab/compare_vita49_capture.m`, which decodes the
same bytes with `parse_vita49_packet.m` and compares field by field. A capture
recorded at a different `--vita49-fullscale` than the config needs
`--fullscale` to override it.

## DSI cancellation

Two steps. First freeze one CPI from a recorded capture, then run the
canceller against it. Nothing here needs FERS or a network.

```bash
./bin/ingest_replay --file data/S0_clean/capture.pbrcap --fullscale 0.005 \
                    --dump-cpi data/S0_clean/cpi_0000.bin

./bin/dsi_bench --cpi data/S0_clean/cpi_0000.bin --algorithm ECA-B --iters 10 \
                --csv results/L3_bench/dsi_bench.csv
```

`--algorithm` and `--backend` override `config/prb.conf` when given; with
neither, the run is fully described by the tracked config file. The harness
loads the CPI once, calls `init()` once, discards a warm-up iteration, and
times only the cancellation — so every algorithm sees byte-identical input and
no allocation lands inside the timed region.

Measured on the **Jetson**, first CPI of the shipped capture, one session,
clocks not pinned (provisional):

| Algorithm | Backend | Suppression | Time per CPI | vs real time |
|---|---|---|---|---|
| ECA-m | GPU | 40.18 dB | 151.8 ms | 0.15× |
| ECA-B | GPU | 51.62 dB | **142.1 ms** | **0.14×** |
| ECA-m | CPU | 40.18 dB | 9719.9 ms | 9.72× |
| ECA-B | CPU | 51.62 dB | 9632.1 ms | 9.63× |

The GPU is **68× faster than the CPU** and runs at 7× real time; the CPU
cannot keep up at all.

The 11.4 dB gain is why ECA-B exists: one set of weights for a whole second
cannot follow clutter that changes within it. ECA-m and ECA-B taking the same
time is not a mistake — batching costs the same arithmetic and buys
adaptivity, not speed.

**Host timings are order-of-magnitude only.** Citable figures come from the
Jetson with clocks pinned; see the measurement note below.

### Validation against MATLAB

`matlab/eca_golden.m` reads the same frozen CPI, cancels in double precision,
and writes golden outputs; `dsi_verify` compares the C++ canceller against
them and exits non-zero on failure:

```bash
/Applications/MATLAB_R2025a.app/bin/matlab -batch "addpath('matlab'); eca_golden"
./bin/dsi_verify --cpi data/S0_clean/cpi_0000.bin --golden data/S0_clean/golden/golden_ecam.bin --algorithm ECA-m
./bin/dsi_verify --cpi data/S0_clean/cpi_0000.bin --golden data/S0_clean/golden/golden_ecab.bin --algorithm ECA-B
```

The golden file records the parameters it was generated with, and
`dsi_verify` refuses to compare if they disagree with the loaded config, so a
stale golden file fails loudly. Re-run this pair of checks after any change
to `src/dsi/`.

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
