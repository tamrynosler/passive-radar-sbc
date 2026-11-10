# passive-radar-sbc

FM-band passive bistatic radar (PBR) running in real time on an NVIDIA Jetson
Nano (Tegra X1, sm_53, CUDA 10.2, 4 GB shared RAM).

MSc thesis project, Electrical Engineering, University of Cape Town. The
contribution is a comparison of seven direct-signal-interference (DSI)
cancellers on embedded GPU hardware: suppression and detection quality
against latency and memory.

## Status: complete (November 2026)

Development has ended. This repository is the record of the code, scripts
and scenarios behind the thesis results. The thesis itself is in a separate
repository.

What was built and works:

- The full chain on the Jetson: VITA 49.2 UDP ingest → DSI cancellation →
  CAF range-Doppler map → CA-CFAR → grouping of detections into plots →
  range-Doppler frames over UDP multicast → browser viewer.
- Seven cancellers, each with a CPU and a GPU implementation behind one
  interface: ECA-m, ECA-B, ECA-S, ECA-CD, ECA-LD, CGLS, FBLMS.
- `uhd_to_vita49`: a USRP2 driver that streams VITA 49.2 in the same
  profile as the FERS simulator, so the chain cannot tell the two apart.
- A second surveillance channel (three streams) and an angle of arrival per
  plot.
- Live parameter changes from the viewer, applied at CPI boundaries and
  logged.
- A MATLAB reference chain and golden-file checks for every C++ stage.

What was not done:

- No antennas were connected. The chain ran end to end from the USRP2 on the
  bench, but no off-air target was seen, and the spectrum orientation of the
  USRP path was never checked against a station.
- No tracker. The output is plots per CPI.
- Power and thermal behaviour under load were not measured. The board logs
  over-current alarms under sustained GPU load.

## Results

All figures are from the Jetson with clocks pinned (`nvpmodel -m 0`,
`jetson_clocks`). Full results, method and discussion are in the thesis.

### Cancellation time per 1 s CPI, CPU against GPU

Frozen CPI 0 of `S0_clean` (204800 samples, K = 120 taps), cancellation only,
median of 20 GPU / 3 CPU runs. CPU in FP64, GPU in FP32.
Source: `results/L3_bench/20261026_223906_backends/dsi_bench.csv` and
`ard_bench.csv`.

| Stage | CPU (Nano, FP64) | GPU (Nano) |
|---|---|---|
| ECA-CD | 122.3 ms | **5.3 ms** |
| ECA-LD | 308.4 ms | 17.0 ms |
| FBLMS | 95.0 ms | 34.6 ms |
| ECA-B | 10 129 ms | 47.4 ms |
| ECA-m | 10 307 ms | 52.3 ms |
| ECA-S | 99 653 ms | 107.2 ms |
| CGLS (30 iterations) | 10 240 ms | 208.7 ms |
| Range-Doppler map + CFAR | 52 794 ms (FP32) | 9.0 ms |

Every canceller runs well inside the 1 s budget on the GPU. On the CPU only
ECA-CD, ECA-LD and FBLMS cancel within budget, and the map alone takes 53 s,
so the chain needs the GPU.

### Detection quality with clutter

`S1_clutter`: a 600 s FERS scenario with one aircraft, 28 static scatterers
and 4 slow movers, real FM waveform, two channels, 1 s CPI, 1-D CFAR
(guard 4, train 6, Pfa 1e-5). Source:
`results/L2_performance/quality.csv`, rows `S1_clutter`.

| Canceller | Suppression | Pd (plots) | Target SNR | Chain time / CPI (median) | Peak memory |
|---|---|---|---|---|---|
| ECA-m | 38.3 dB | 0.97 | 31.3 dB | 66.6 ms | 772 MB |
| ECA-B | 50.0 dB | 0.95 | 35.0 dB | 63.1 ms | 771 MB |
| ECA-S | 50.3 dB | 0.97 | 35.2 dB | 125.0 ms | 771 MB |
| ECA-CD | 25.3 dB | 0.34 | 17.2 dB | 12.8 ms | 454 MB |
| ECA-LD | 46.1 dB | 0.99 | 36.7 dB | 25.2 ms | 457 MB |
| CGLS | 47.5 dB | 0.97 | 38.3 dB | 219.8 ms | 455 MB |
| FBLMS | 48.4 dB | 0.98 | 38.4 dB | 42.6 ms | 454 MB |

The thesis recommends **ECA-LD** with a batch of 250 ms or longer: the best
balance of speed and detection. ECA-CD is the fastest but breaks down on
clutter, which its per-carrier model cannot follow. FBLMS gives the best
target SNR; ECA-S suits one channel with few false plots.

Note: `config/prb.conf` and `config/fers.conf` still select ECA-CD, the
default during development. To run the recommended canceller set
`DSI_ALGORITHM = ECA-LD` and `ECA_LD_BATCH_s` to 0.25 or more (the results
above used 1.0, one solve per CPI).

### Correctness

ECA-m, ECA-B, ECA-LD, FBLMS, the range-Doppler map and the CFAR have MATLAB
golden files; the CPU FP64 code matches them to between 4e-13 and 8e-7
relative error. ECA-S, ECA-CD and CGLS are checked GPU against CPU; CGLS was
also checked against an independent MATLAB CGLS. Every GPU FP32 canceller is
within 1e-2 of FP64 with the same suppression to three decimals, and the
FP32 chain gives the same CFAR cells as FP64. Logs in
`results/L1_correctness/`.

## Repository layout

```
passive-radar-sbc/
├── CMakeLists.txt     Jetson build (CMake 3.10, nvcc 10.2)
├── Makefile           Mac-side driver: sync + remote build/run over SSH (`make help`)
├── include/pbr/       public headers, flat
├── src/
│   ├── ingest/        VITA 49.2 decode and write, UDP, circular buffer, config,
│   │                  capture/replay, CPI extraction
│   ├── dsi/           the seven cancellers, cpu/ and gpu/
│   ├── ard/           range-Doppler map, CFAR, plot grouping, bearing, frame stream
│   ├── gpu/           GpuCommon.cuh: helpers shared by the GPU stages
│   ├── display/       HttpServer + ard_viewer.html, served by ard_view
│   └── app/           pbr_run, uhd_to_vita49, ard_view, dsi_verify, ard_verify
├── bench/             dsi_bench, ard_bench, cufft_bench
├── tests/             17 unit tests, one executable per file
├── matlab/            reference chain, golden generators, scoring, thesis figures
├── scenarios/         FERS scenarios (.fersxml) and how each capture was made
├── scripts/           results campaign, CPI sweep, backend bench, clock pinning
├── config/            prb.conf (USRP, 200 kHz), fers.conf (FERS, 204800 Hz)
├── docs/              design records, methodology, findings
└── third_party/       approved external headers (none)
```

`data/` and `results/` are gitignored. Captures and golden files are
regenerated from `scenarios/` (see `scenarios/README.md`); results come from
the scripts in `scripts/`.

## Requirements

- Jetson Nano, L4T R32.7, CUDA 10.2 with cuFFT, cuBLAS and cuSOLVER.
- CMake 3.10 and GCC 7 (stock L4T). C++14 throughout.
- UHD 3.10 on the Nano (`libuhd-dev uhd-host`) for the USRP driver.
- For development: a Mac (or Linux host) with SSH access to the board as
  `Host jetson`, MATLAB for the reference chain, FERS for test signals.

## Build and run

Edit on the host; the Jetson is a build and run target only. `make sync` uses
`rsync --delete`, so edits made on the board are lost.

```bash
make build     # sync, then compile on the Jetson
make test      # unit tests on the Jetson
make run       # the chain, live, clocks pinned (ARGS='--group 239.192.0.1')
make drive     # the USRP driver on the Jetson (ARGS='--synth' without hardware)
make view      # the viewer, forwarded to http://localhost:8080
make replay    # a recorded FERS capture through the chain
make stop      # end every radar process on the Jetson
make pull      # results back to ./results
make host      # local build without CUDA, into ./bin
make host-test # unit tests locally
```

A live session is three terminals: `make view`, then `make run ARGS='--group
239.192.0.1'` (wait for "Listening now"), then `make drive`. Open
http://localhost:8080.

The config is read at run time: after editing `config/*.conf`, `make sync`
is enough.

Directly on the Jetson:

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CUDA_COMPILER=/usr/local/cuda-10.2/bin/nvcc ..
make -j2
```

### From FERS

Start the chain first, then FERS (it takes about 3 s to send its first
packet):

```bash
./bin/pbr_run --live --config config/fers.conf --out results/run1 \
              --capture data/S0_clean/capture.pbrcap
fers-cli <scenario>.fersxml --out-dir=/tmp/out \
         --vita49 <chain-ip>:4991 --vita49-fullscale 0.005 -n=6
```

`--capture` records the stream for replay with `--file`; `--dump-cpi` freezes
one CPI for the verification and bench tools; `--out DIR` writes
`telemetry.csv`, `detections.csv`, `plots.csv` and `changes.log`.

### Verification and benchmarks

```bash
# MATLAB goldens (read the config; point them at fers.conf for FERS data)
/Applications/MATLAB_R2025a.app/bin/matlab -batch "addpath('matlab'); eca_golden"

./bin/dsi_verify --cpi data/S0_clean/cpi_0000.bin \
                 --golden data/S0_clean/golden/golden_ecab.bin --algorithm ECA-B
./bin/dsi_bench  --cpi data/S0_clean/cpi_0000.bin --algorithm ECA-LD --iters 20 \
                 --csv results/dsi_bench.csv
```

The results campaign is scripted: `scripts/results_run.sh` (full-scenario
runs), `scripts/level1_verify.sh` (golden checks), `scripts/cpi_sweep.sh`,
`scripts/backend_bench.sh`. `matlab/quality_table.m` gathers the runs into
`results/L2_performance/quality.csv`; `matlab/results_plotting/` makes the
thesis figures. Timing figures need pinned clocks and a 5 V / 4 A barrel-jack
supply; see `docs/benchmarking-methodology.md`.

## Documentation

| File | Contents |
|---|---|
| `docs/design_decisions.md` | every design decision, with reasons |
| `docs/dsi-cancellation-explained.md` | the cancellers in plain terms |
| `docs/canceller-findings-2026-10.md` | canceller comparison findings |
| `docs/benchmarking-methodology.md` | how timing runs are made |
| `docs/results-rerun-plan.md` | the final results campaign |
| `docs/cpi-sweep-plan.md` | the CPI length sweep |
| `docs/third-channel-aoa-design.md` | second surveillance channel and AoA |
| `docs/UHD_VITA49_Driver_design.md` | the USRP driver |
| `docs/ard-stream-design.md`, `docs/ard-viewer.md` | frame stream and viewer |
| `docs/live-parameters.md` | live parameter changes |
| `docs/legacy-audit.md` | the legacy code reviewed at the start |
| `RESULTS.md` | early results log (to 2026-10-02); superseded by the thesis |

## Licence

None. UCT's IP policy for thesis code is unresolved.
