# RESULTS

Measured results, newest first. Every number here is copied from a file
under `results/` (path given per table); nothing is typed from memory. The
protocol for each set is named so it can be rerun.

## 2026-10-02 (afternoon): CGLS tuning on the laptop CPU, and two findings

**Protocol**: `scripts/cgls_tuning/run_cgls.sh`, the whole chain on
`data/S0_clean/capture.pbrcap` (3 CPIs) on the Mac CPU in FP64, cold
(`CGLS_PRIME = 0`), K = 120; one directory and one `index.csv` row per
test. Laptop times give the scaling only; Nano figures are above. Driven
by the author; plots `plot_cgls.m`, `plot_cgls_sweep.m`.

### Cost against iterations: linear, ~30 ms per iteration per CPI (laptop)

| Iterations | Batch | DSI ms, CPI 0 | ms / iteration | Suppression CPI 0 | Detections CPI 0 |
|---|---|---|---|---|---|
| 30 | 0.25 s | 910 | 30.4 | 41.98 dB | 355 |
| 50 | 0.25 s | 1512 | 30.2 | 49.33 dB | 166 |
| 50 | 0.025 s | 1512 | 30.2 | 49.12 dB | 85 |
| 50 | 0.0025 s | 1481 | 29.6 | 47.99 dB | 50 |
| 80 | 0.0025 s | 2331 | 29.1 | 53.17 dB | 65 |
| 100 | 0.0025 s | 2906 | 29.1 | 53.83 dB | 66 |

(`index.csv` rows cold30, cold50 x3, cold80, cold100.) No fixed part: each
iteration is two passes over N x K, about 49 M complex MACs. Suppression
gains diminish: 42 -> 48 -> 53.2 -> 53.8 dB; CGLS passes ECA-B's 51.6 dB
between 50 and 80 iterations. If the Nano GPU scales linearly from its 484
ms at 30 iterations (~16 ms/iteration), real time runs out near 60; one
GPU point cannot separate a fixed part, so Nano runs at 60 and 100 are
needed (`make prelim ARGS=CGLS` with `CGLS_ITERATIONS` edited).

### Cost against batch length: none; quality changes instead (60 iterations, `sweep60`)

| Batch | Windows | DSI ms | Suppression CPI 0 | CPI 2 | Detections CPI 0 / CPI 2 |
|---|---|---|---|---|---|
| 1 s | 1 | 1775 | 50.67 dB | 50.64 | 98 / 58 |
| 0.25 s | 4 | 1802 | 50.65 | 50.67 | 68 / 48 |
| 0.0625 s | 16 | 1821 | 50.58 | 50.68 | 65 / 45 |
| 0.025 s | 40 | 1835 | 50.57 | 50.72 | 70 / 54 |
| 0.01 s | 100 | 1842 | 50.52 | 50.47 | 57 / 43 |
| 0.0025 s | 400 | 1826 | 50.74 | 50.13 | 18 / 22 |
| 0.00125 s | 800 | 1796 | 50.00 | 49.26 | 9 / 7 |

Time is flat (the same N x K work however the CPI is cut). Suppression is
flat to 0.7 dB. The maps (`sweep_sweep60_cpi0.png`, `_cpi2.png`) are not:
long windows leave a vertical streak through the target's range bin and a
zero-Doppler ridge at close range; at 2.5 ms and shorter those go and the
noise floor rises relative to the peak (fewer samples per weight: 512 or
256 samples for 120 unknowns). Detections fall 98 -> 9, which mixes fewer
false alarms with a weaker target; `targetSnrDb` from `score_run` is the
number to settle that, not yet read for these runs.

### Finding: the warm-up pass primes CGLS from garbage

With `CGLS_PRIME = 1` (the production setting), CPI 0 came out at 17.79 dB,
then 31.44, 39.30 (`smoke` run, scratchpad). With priming off, CPI 0 is
41.9844 dB, identical to four decimals to the Nano's CPU FP64 row. Cause:
`pbr_run` warms the chain with one `cancel()` on a pseudo-random signal,
and a primed CGLS keeps the weights it reached there, so the first real
CPI starts from a solution to noise. The code comment says the first CPI
starts from zero; it does not. Affects every start and every live
parameter change in the production config. Fix proposed (a `resetState()`
on the canceller interface, called after the warm-up); awaiting approval.

### Finding: the Doppler streak through every strong return is the illuminator's envelope

Measured on `cpi_0000.bin`: the reference channel's power envelope
|ref(t)|^2 has a spectrum with lines at 73-79 Hz (-10 to -14 dB relative to
its strongest component), 37-43 Hz and 142-151 Hz, median -26 dB over
0-400 Hz. The map's Doppler profile through the target's range bin (sweep60
batch 1 s, CPI 2) has its strongest off-target cells at +46/+50 Hz and
-47/-53 Hz from the target at -27 to -31 dB, and at +-3 / +-6 Hz at -20 /
-26 dB. At the matched delay the Doppler FFT sees |s(t)|^2 times the target
tone, so every strong return is convolved with the envelope spectrum: a
vertical streak 25-30 dB down, identical for all five cancellers (they act
only at zero Doppler), present with a single 1 s CGLS window, and on the
direct signal at range 0. The band-limited recording of a real FM station
is not constant-envelope. The +-3 / +-6 Hz sidelobes are the Doppler FFT's
rectangular window (no window is applied in the CAF). Consequences: it does
not separate the algorithms and should be excluded from their comparison;
it is a property of real FM worth a figure (envelope spectrum beside the
map); constant-envelope normalisation of the reference is a known
mitigation, not tried.

### Consistency across machines

Mac CPU FP64 and Nano CPU FP64 give 41.9844 dB for cold CGLS at 30
iterations; the Mac is 11x faster per CPI (912 ms vs 10.2 s). The Mac's
CPU ARD stage takes 1.2 s per CPI, the Nano GPU's 13 ms.

## 2026-10-02: baseline, five cancellers, one CPI, Jetson Nano

**Protocol**: `scripts/run_prelim.sh` (`make prelim ARGS=<ALG>`; archived
2026-10-06 to `~/passive-radar-archive/tools/`), one
algorithm per run, all on `data/S0_clean/cpi_0000.bin` (CPI 0 of the
2026-09-16 FERS capture: 204800 samples at 204800 Hz, one aircraft at bin
46 / +99 Hz, no clutter, raw ADC codes -279..280). Clocks pinned
(`nvpmodel -m 0`, `jetson_clocks`, fan 255), `tegrastats` logged, board at
25-33 C throughout. Parameters from `config/fers.conf` with only
`DSI_ALGORITHM` changed, plus `ECA_UPDATE_INTERVAL_s = 0.025` for ECA-B
(the name requires it) and `CGLS_PRIME = 0` for CGLS (`dsi_bench` needs
identical repeated runs). K = 120 taps, 120 range bins, ECA batches 25 ms,
ECA-S update 2.5 ms, ECA-CD batches 2.5 ms with P = 1, CGLS 250 ms windows
and 30 iterations. Exact copies: `config.conf` in each run directory.

Run directories, all under `results/prelim/S0_clean/`:
`ECA-m/20261002_113859`, `ECA-B/20261002_115236`, `ECA-S/20261002_120902`,
`ECA-CD/20261002_120246`, `CGLS/20261002_120722`. An earlier ECA-m run,
`ECA-m/20260926_114353` (board clock was wrong; it is 2026-10-02 too), has
valid accuracy and timing rows and a failed chain step (warm-up fix below).

### Accuracy (`verify_*.txt`, relative L2 of the cleaned surveillance signal)

| Algorithm | C++ CPU FP64 vs MATLAB | GPU vs reference | Suppression, GPU vs reference |
|---|---|---|---|
| ECA-m | 6.573e-10 | 2.053e-4 vs MATLAB | 40.1846 vs 40.1834 dB |
| ECA-B | 8.221e-10 | 6.523e-3 vs MATLAB | 51.6191 vs 51.6193 dB |
| ECA-S | no MATLAB reference | 1.471e-3 vs CPU FP64; 7.278e-3 vs CPU FP32 | 51.8334 vs 51.8333 dB |
| ECA-CD | no MATLAB reference | 6.263e-5 vs CPU FP64; 1.925e-4 vs CPU FP32 | 51.5021 vs 51.5021 dB |
| CGLS | no MATLAB reference | **2.180e-1** vs CPU FP64; **5.823e-2** vs CPU FP32 | 41.7828 vs 41.9844 dB |

- MATLAB goldens exist for ECA-m and ECA-B only (`matlab/eca_golden.m`).
  The other three were checked GPU-against-CPU (`dsi_verify --against`),
  which shows the two implementations agree, not that either matches the
  published algorithm.
- The ECA-S FP32 cross-check was labelled FAIL against a tolerance of 1e-3
  that was set by guesswork; ECA-B's golden check shows ~6e-3 is what FP32
  costs the batched solver. Tolerance raised to 1e-2 on 2026-10-02.
- CGLS: GPU and CPU disagree by 22 % (vs FP64) and 6 % (vs FP32), and by
  0.2 dB of suppression. Thirty cold iterations are far from converged, and
  an unconverged iterative solver amplifies rounding differences between
  steps (the FP32 semiconvergence noted in CLAUDE.md). Open: does the gap
  close with more iterations? Not verified on this evidence.

### Time per CPI (`bench.csv`, `cancel()` only, one 1 s CPI, median of 20 GPU / 3 CPU)

| Algorithm | GPU median | GPU std | GPU x real time | Nano CPU median | CPU x real time |
|---|---|---|---|---|---|
| ECA-CD | **10.34 ms** | 0.11 | 0.010 | 121.4 ms | 0.12 |
| ECA-B | 139.03 ms | 0.21 | 0.139 | 10 119 ms | 10.1 |
| ECA-m | 141.14 ms | 0.74 | 0.140 | 10 421 ms | 10.4 |
| ECA-S | 212.65 ms | 0.40 | 0.212 | 100 724 ms | 100.7 |
| CGLS (30 it.) | 484.30 ms | 1.21 | 0.484 | 10 239 ms | 10.2 |

- GPU time includes the host-to-device and device-to-host copies
  (`cancel()` takes host pointers): the stage latency the chain pays.
- Repeatability: ECA-m GPU median 141.58 ms (first session) vs 141.14 ms
  (second), 0.3 %; CPU 10 309 vs 10 421 ms, 1 %.
- ECA-S on the CPU is 10x ECA-B because the CPU solves 400 windows
  independently; the GPU fast path shares partial Grams between windows.

### Whole chain per CPI (`chain/telemetry.csv`, 3 CPIs of the capture, GPU)

| Algorithm | dsiMs | ardMs | totalMs |
|---|---|---|---|
| ECA-CD | 11.8-12.0 | 13.4 | 25.2-25.5 |
| ECA-B | 141.6-142.0 | 13.5 | 155.1-155.5 |
| ECA-m | 142.0-142.9 | 13.5 | 155.5-156.4 |
| ECA-S | 214.2-214.9 | 13.4 | 227.6-228.4 |
| CGLS | 485.7-486.1 | 13.4 | 499.2-499.5 |

The chain's `dsiMs` matches the bench within 1-3 ms, so the bench is
representative of the chain.

### Power (`tegrastats.log`, mean over each bench row's window; indicative only, windows 2-10 s and including setup)

GPU rows: 4.1-5.4 W total (`POM_5V_IN`), 1.3-3.2 W on the GPU rail. CPU
rows: 2.9-3.1 W total, 1.3 W on the CPU rail. ECA-CD's GPU window is 2 s
and not usable. Energy per CPI is far lower on the GPU because it finishes
70-470x sooner.

### Quality on CPI 0-2 (`chain/score.csv`, `chain/summary.txt`, against `data/S0_clean/truth.csv`)

| Algorithm | Suppression CPI 0 | Ridge vs floor | Target SNR | Ambiguity | False alarms / CPI (on ridge) |
|---|---|---|---|---|---|
| ECA-m | 40.18 dB | -1.4 dB | 36.2 dB | -25.1 dB | 30 (4) |
| ECA-B | 51.62 dB | -26.4 dB | 46.1 dB | -21.3 dB | 66 (0) |
| ECA-S | 51.83 dB | -15.8 dB | 46.0 dB | -24.2 dB | 68 (0) |
| ECA-CD | 51.50 dB | -4.8 dB | 46.3 dB | -28.4 dB | 72 (0) |
| CGLS (cold, 30 it.) | 41.78 dB | **+16.2 dB** | 42.7 dB | -21.3 dB | 288 (249) |

Target detected in every CPI by every algorithm, within 0.3 range bins and
0.3 Hz of the truth. Columns: ridge = median of the zero-Doppler band
relative to the noise floor (negative = notched below it); ambiguity =
largest cell in the target's range bin outside the gate, relative to the
target (ECA-B's +-40 Hz ambiguities are the -21 dB).

- The three batched ECAs are interchangeable in quality on a clutter-free
  scene; only cost separates them. Differences need `S1_clutter`.
- ECA-m is 11 dB behind on a scene with no moving clutter: something is not
  stationary within the second (the FERS receiver clock carries a random
  frequency offset; untested hypothesis).
- Cold CGLS at 30 iterations is unconverged: the zero-Doppler ridge is left
  16 dB above the floor and carries 249 of its 288 false alarms.
- The false-alarm counts are the untuned CFAR (no range guard or training,
  so one aircraft is counted ~10 times along its range sidelobes; the
  direct signal's Doppler sidelobes fill range bins 0-2). Same artefact in
  every run; it does not separate the algorithms.

### Fixed on the way

- `pbr_run` warmed the chain with a constant reference, whose Gram matrix is
  singular in FP32 over a whole-CPI window; ECA-m could never start. Now a
  fixed pseudo-random sequence (`src/app/pbr_run.cpp`).
- macOS 15.7's openrsync fails on `--delete` plus filter rules; the Makefile
  prefers Homebrew rsync.
- The board clock was 6 days slow (NTP not synchronised); result stamps now
  come from the Mac.
