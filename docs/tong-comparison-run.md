# Run brief: CGLS on S0_long with Tong's (2014) TK1 parameters

Written 2026-10-20 for an agent working in this repository. Read `CLAUDE.md`
first, especially the "SSH and the network" section.

## Goal

Record one run, ideally two, of the chain on the Jetson Nano with the
processing parameters as close as possible to those Tong (2014) timed on the
Jetson TK1. The thesis compares the two systems in
`tab:results.realtime.uct` (Report_v2, Chapter 4). At the moment it sets
Tong's CGLS against our ECA-CD at a 1 s CPI, which is not like for like.
This run supplies a like-for-like cancellation time.

Do not edit the thesis, `config/fers.conf`, or any existing result. Do not
commit unless the author asks.

## Tong's reference figures

Source: `Papers/00_theses/tong_2014_scalable-realtime-processing-chain_phd.pdf`.

| Quantity | TK1 value | Tong's table, printed page (PDF page) |
|---|---|---|
| Cancellation: CGLS, 10 iterations, 8 sub-CPIs of 102 400 samples (0.5 s), 210 range bins | 3059 ms | 3.1, p. 125 (155) |
| Map: XF algorithm, CPI of 819 200 samples (4 s), 280 range bins, cropped to 1601 Doppler bins | 1420 ms | 3.3, p. 128 (158) |
| Whole chain, off-line from local disk, time between maps | 4690 ms | 3.9, p. 135 (165) |
| CFAR (not on the TK1: ARDView on a Core 2 Quad PC): GOCA, 4 guard cells, 6 reference cells per side, Pfa 1e-5 | n/a | 3.9, p. 135 |

Tong's cancellation timing is a one-shot measurement from CPU memory to CPU
memory (p. 124), so it was most likely cold, not primed.

## The settings to use

| Key | Value | Why |
|---|---|---|
| `DSI_ALGORITHM` | `CGLS` | Tong's canceller (set by the script) |
| `CPI_LENGTH_s` | `4.0` | 819 200 samples at 204 800 Hz, as Tong |
| `CPI_HOP_s` | `4.0` | back-to-back CPIs, as every other run |
| `CGLS_BATCH_s` | `0.5` | Tong's 8 sub-CPIs of 0.5 s |
| `CGLS_ITERATIONS` | `10` | Tong's timing table (not the 2 of his field runs) |
| `N_CLUTTER_TAPS` | `210` | Tong's 210 range bins in the cancellation |
| `N_RANGE_BINS` | `210` | must be at most `N_CLUTTER_TAPS` (`src/ingest/Config.cpp:288`), so Tong's 280-bin map cannot be matched together with his 210 taps |
| `DOPPLER_DECIMATION` | `512` | 819 200 / 512 = 1600 Doppler bins over ±200 Hz at 0.25 Hz: Tong's 1601 |
| `CGLS_PRIME` | `0` for run A, `1` for run B | A matches Tong's one-shot timing; B is how the chain runs in operation. The cost per CPI should be the same. |

Set by `scripts/results_run.sh` and left as they are: GPU backend; CFAR 1-D
along Doppler, guard 4, training 6, Pfa 1e-5. In cells that is Tong's
4 guard / 6 reference / 1e-5, but our CFAR is cell-averaging, not GOCA. One
surveillance channel (`CHANNELS = 2`). Frames kept (`FRAMES = 1`), so the
scoring can measure target SNR; frame writing is not counted in `totalMs`.

These cannot be matched, so list them in the report: the GPU (Maxwell, 128
cores, against Kepler, 192); the CGLS implementation (ours against Tong's
cuBLAS); the map algorithm (Batches against XF) and its 210 range bins
against 280; the CFAR type and where it runs; the sample rate (204.8 kHz
against about 200 kHz); and the data (the FERS S0_long capture against
Tong's field recordings).

## Steps

### 0. Clocks (needs the author)

`jetson_clocks` needs sudo with a TTY and a password, so you cannot run it.
Ask the author to run `ssh -t jetson 'sudo jetson_clocks'` and to tell you
when it is done. Do not start a timed run before then. Every run writes a
`clocks` line in its `run.txt`; if it says `NOT pinned`, the timing cannot be
cited, so stop and tell the author.

### 1. Let `results_run.sh run` take a base config

`runOne` always derives the run's config from `config/fers.conf`. Make the
smallest change that lets an environment variable choose the base, with the
default unchanged:

```bash
# in runOne(), scripts/results_run.sh
makeConfig "$sConf" "$sAlg" "$iChannels" "$iFrames" "${BASE_CONF:-config/fers.conf}" || return 1
```

Also add `echo "baseConf     ${BASE_CONF:-config/fers.conf}"` to
`writeManifest`, so `run.txt` records which base was used. Update the usage
comment at the top of the script to mention `BASE_CONF`.

### 2. Two base configs

Copy `config/fers.conf` to `config/tong2014_cold.conf` and
`config/tong2014_primed.conf`, and in each change only the keys in the table
above (`CGLS_PRIME` 0 and 1 respectively). Each key must appear exactly once;
the script's `setKey` fails otherwise. Put a two-line comment at the top of
each file saying what it is for and pointing to this brief.

### 3. Build

`make build` (it syncs to the board and compiles). If you changed nothing in
C++, it only needs to sync, but run it anyway so the board has the script
change and the two configs.

### 4. Truth for a 4 s CPI (on the Mac)

`data/S0_long/truth.csv` is for 1 s CPIs. Make the 4 s one with the run's
config:

```bash
/Applications/MATLAB_R2025a.app/bin/matlab -batch "addpath('matlab'); fers_truth('scenarios/S0_long/scenario.fersxml', 'config/tong2014_cold.conf', 'data/S0_long/truth_T4.0.csv')"
```

Check that it has about 150 rows, and that the range bins stay inside 0 to
209 (the S0_long target sits near bin 42 at t = 0).

### 5. The two runs (on the board, in the foreground of ssh)

Backgrounding over ssh does not work (`CLAUDE.md`). Run each one in the
foreground with a long tool timeout (10 minutes). Use a separate results root,
so the main `L2_performance/runs.log` is not touched:

```bash
ssh jetson 'cd ~/passive-radar-sbc && SOURCE_GIT=<Mac git short hash> BASE_CONF=config/tong2014_cold.conf scripts/results_run.sh run S0_long data/S0_long/capture.pbrcap CGLS 2 1 /data/results/L2_tong'
ssh jetson 'cd ~/passive-radar-sbc && SOURCE_GIT=<Mac git short hash> BASE_CONF=config/tong2014_primed.conf scripts/results_run.sh run S0_long data/S0_long/capture.pbrcap CGLS 2 1 /data/results/L2_tong'
```

If a run fails, read its `run.log`. If `DOPPLER_DECIMATION = 512` is the
cause (for example, a transform length or kernel limit), rerun with 128 and
say so in the report; do not change any other setting to make it work.

### 6. Pull and check

`make pull`. The runs land in `results/L2_tong/S0_long/CGLS/<stamp>_2ch/`.
For each run check:

- `exit=0` in `results/L2_tong/runs.log`
- the `run.txt` clocks line reads `pinned`
- `config.conf` has the keys in the table
- `telemetry.csv` has about 150 rows, all status 0

### 7. Score (on the Mac)

```bash
/Applications/MATLAB_R2025a.app/bin/matlab -batch "addpath('matlab'); d = 'results/L2_tong/S0_long/CGLS/<stamp>_2ch'; score_run(d, 'data/S0_long/truth_T4.0.csv', [d '/config.conf'], 'gateKm', 2, 'gateHz', 3)"
```

Use the 2 km / 3 Hz gate the thesis uses.

## What to report back

For each run, quote values from its `summary.txt` and `telemetry.csv`;
nothing from memory:

| Quantity | Run A (cold) | Run B (primed) | Tong, TK1 |
|---|---|---|---|
| Stamp, path | | | |
| Clocks line from run.txt | | | |
| CPIs processed | | | single measurement |
| Cancellation `dsiMs`, median / max | | | 3059 |
| Map + CFAR `ardMs`, median / max | | | 1420 (map only) |
| Total `totalMs`, median / max | | | 4690 (time between maps) |
| Real-time factor (median total / 4000 ms) | | | 1.17 |
| Suppression, mean dB | | | |
| Pd, false plots per CPI (±200 Hz), target SNR | | | |
| Memory: system used max, process VmHWM max | | | |

Also report: every setting that differed from the table above and why; any
warning in `run.log`; and the diff of the script change. If either run could
not be made, say which step failed, with the error, rather than substituting
a different configuration.
