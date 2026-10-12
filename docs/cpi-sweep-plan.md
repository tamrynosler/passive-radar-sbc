# CPI length sweep: quality and cost against the integration time

Written 2026-10-12, agreed with the author the same day. **Run the same
day: 56 runs, all clean; findings and table in
`results/cpi_sweeps/README.md`.** tegrastats was not started, so there
are no power figures; the memory logs are complete. The chain needed one
fix first (pbr_run copies channel 1's map back every CPI when a second
channel exists, so the bearing no longer depends on a frame being
written or streamed). One knob, the
CPI length, moves everything at once: Doppler resolution, integration
gain, update rate, map size and the cost of every stage. This sweep
measures all of it on the Nano for the three-channel chain.

## 1. The sweep

| CPI_LENGTH_s | samples | Doppler bins | bin Hz | CFAR guard / train (Doppler cells) | CPIs in 600 s |
|---|---|---|---|---|---|
| 0.5 | 102400 | 800 | 2.0 | 2 / 4 | 1200 |
| 1.0 | 204800 | 1600 | 1.0 | 4 / 8 | 600 |
| 1.5 | 307200 | 2400 | 0.67 | 6 / 12 | 400 |
| 2.0 | 409600 | 3200 | 0.5 | 8 / 16 | 300 |
| 2.5 | 512000 | 4000 | 0.4 | 10 / 20 | 240 |
| 3.0 | 614400 | 4800 | 0.33 | 12 / 24 | 200 |
| 3.5 | 716800 | 5600 | 0.29 | 14 / 28 | 171 |
| 4.0 | 819200 | 6400 | 0.25 | 16 / 32 | 150 |

Every length divides cleanly at 204800 Hz: the Doppler decimation (128),
the 25 ms ECA batches (5120), the 2.5 ms ECA-CD batch (512), the 0.25 s
CGLS batch (51200), the 0.1 s ECA-LD batch (20480) and the 128 FBLMS taps
all tile all eight. `CPI_HOP_s` equals the CPI: back-to-back windows.

Cancellers: all seven (ECA-CD, ECA-B, ECA-S, ECA-LD, FBLMS, ECA-m, CGLS),
GPU, three channels (`config/fers.conf` with the S0_aoa receiver names).
56 runs.

Per-run config, derived from `config/fers.conf` by `scripts/cpi_sweep.sh`
and saved beside each run as `config.conf` (`pbr_run --save-config`):

- `CPI_LENGTH_s`, `CPI_HOP_s`: the length.
- `BUFFER_LENGTH_s = 8`: at least twice the longest CPI (13 MB per
  channel).
- `CFAR_GUARD_DOPPLER = round(4 T)`, `CFAR_TRAIN_DOPPLER = round(8 T)`:
  the window is **constant in hertz** (4 Hz guard, 8 Hz training either
  side), not in cells. In cells it would shrink from 8 Hz at 1 s to 2 Hz
  at 4 s, inside the target's own peak, and detection would collapse for
  a reason unrelated to the CPI. Range cells unchanged (0 / 0).
- `DSI_ALGORITHM`: the canceller; ECA-B and ECA-m get
  `ECA_UPDATE_INTERVAL_s = 0.025` (equal to the batch), as in every run
  so far.
- `ECA_LD_BATCH_s = 0.1`: 25 ms is the known broken operating point
  (`docs/canceller-comparison-plan.md` §1); 0.1 s matches ECA-B's
  suppression.
- `ARD_STREAM = 0` (no viewer); `ARD_WRITE_FILES = 1` for ECA-CD only,
  `ARD_SNAPSHOT_EVERY = 1`. The frames feed the SNR, width, ridge and
  ambiguity measurements; one canceller per length keeps the disk at
  about 1.5 GB for the whole sweep (6 MB per frame at 4 s).
- Everything else as `fers.conf`.

Clocks pinned (`sudo jetson_clocks`, holds until reboot) and
`tegrastats --interval 1000` logging for the whole sweep, both started
by the author. Replay, not live: the per-CPI cost is the same, and the
sweep runs unattended in tmux on the board.

## 2. Data

One 600 s recording of S0_aoa, `data/S0_aoa/capture_600s.pbrcap`
(about 1.5 GB, three streams), recorded on the Mac with the cheap
recording config (4 range bins, so no datagram is lost) and copied to
the board. It covers the zero-Doppler crossing at t = 300 s, which the
smear measurement needs. The 88 s capture is too short: 22 CPIs at 4 s.

A truth file per length, `data/S0_aoa/truth_T<len>.csv`, from
`fers_truth.m` with that length's config (it reads the CPI length from
the config; the per-run configs are generated on the Mac too).

## 3. What is measured

### Quality, per run, from `score_run.m` against the truth file

1. **Target SNR in the map**: the peak within the gate over the noise
   floor, per CPI, from the frames (ECA-CD runs). Theory: +3 dB per
   doubling of T, 9 dB from 0.5 s to 4 s. Where the slope falls short,
   the signal or the target has stopped being coherent over the CPI.
2. **Doppler width** of the target peak, -3 dB, in Hz, per CPI. Expected
   1/T far from the crossing; wider near t = 300 s at long T (the target's
   Doppler moves ~0.14 Hz/s there: 0.56 Hz over 4 s against a 0.25 Hz
   bin). Range walk is 600 m in 4 s against a 1.5 km bin: confirm it
   stays inside one bin.
3. **Pd** per CPI: a plot inside a gate of fixed physical size around
   the truth (km and Hz, not bins).
4. **False plots per second**, not per CPI: the CFAR sees 192000 cells
   per second whatever T is, so at fixed Pfa the rate should be flat; a
   trend is a sidelobe or notch effect. Split by Doppler span as
   `score_run.m` does.
5. **Suppression**, both channels (`suppressionDb`, `suppression2Db`).
   Batched cancellers should not move; ECA-m solves the whole CPI and
   will.
6. **Zero-Doppler ridge level** and **ECA-B's 40 Hz ambiguity level**
   (`score_run.m`, ECA-CD frames; the ambiguity row from an ECA-B run
   at 1 s and 4 s if frames are wanted there too).
7. **AoA error**: the target plot's `bearingDeg` (plots.csv) against
   `apparentBearingDeg`, RMS and mean per run. Phase error falls as
   1/sqrt(SNR), so it should improve with T: a second check of the
   integration gain.
8. **Information age and update rate**: a detection at the end of a CPI
   describes the target from T/2 ago, plus processing; report
   T/2 + median totalMs, and 1/T updates per second.

### Cost, per run, from `telemetry.csv` and the samplers

1. **Per-stage time**: dsiMs, ardMs, dsi2Ms, ard2Ms, aoaMs, ardWriteMs,
   totalMs: best, median, p99. Reported absolute and as a **real-time
   factor** (totalMs / 1000 T). Linear stages keep a constant factor, so
   a longer CPI buys no slack; fixed overheads and FBLMS's per-block
   launches show their own slopes.
2. **Fits or not**: the factor crosses 1.
3. **Memory**, two ways, both recorded for every run:
   - `mem.log`: every 0.5 s, `free -m` (used, free, available) and the
     `VmHWM` (peak resident set) of the `pbr_run` process, so the run's
     peak RSS and the system's peak used memory are both known. ECA-m's
     clutter matrix is N x 120 complex per channel: 790 MB at 4 s, twice,
     on a 4 GB board shared with the GPU. It may not build at 3.5 s or
     4 s: that is a result.
   - `tegrastats.log` (one for the sweep, `runs.log` gives each run's
     start and end time): RAM, GPU load, CPU load and power per second.
4. **Frame size and output cost**: 1.5 MB at 1 s to 6 MB at 4 s;
   `ardWriteMs`.
5. **Power** from tegrastats, averaged over each run.

## 4. What is collected

On the board, `/data/results/cpi_sweeps/`, pulled to
`results/cpi_sweeps/` on the Mac (frames included; about 1.5 GB):

```
results/cpi_sweeps/
  README.md                   the sweep table and figures, written last
  tegrastats.log              the whole sweep, one line per second
  runs.log                    one line per run: T, canceller, start, end, exit
  T0.5/ECA-CD/                one directory per (length, canceller)
    config.conf               the exact configuration of the run
    run.log                   pbr_run's console output and summary
    telemetry.csv  detections.csv  plots.csv
    mem.log                   free -m and VmHWM every 0.5 s
    ard/                      frames, ECA-CD runs only
    score.csv  summary.txt    from score_run.m, on the Mac
  T0.5/ECA-B/ ...
  ...
  T4.0/CGLS/
  sweep.csv                   one row per run: every number in §3
```

Figures (MATLAB, `matlab/cpi_sweep_table.m`): target SNR and AoA error
against T on log axes with the 3 dB per doubling line; false plots per
second against T; real-time factor against T per canceller; peak memory
against T per canceller; Doppler width against time at 1 s and 4 s.

## 5. Procedure

1. Record the 600 s stream on the Mac (`scripts/cpi_sweep.sh record`),
   copy it and the truth files to the board.
2. Author: `sudo jetson_clocks`; `sudo tegrastats --interval 1000
   --logfile /data/results/cpi_sweeps/tegrastats.log` in a tmux.
3. `scripts/cpi_sweep.sh run` on the board, inside tmux: generates each
   config, runs the 56 replays in order of length then canceller, with
   the memory sampler, appending to `runs.log`. About two hours; CGLS
   is most of it.
4. Pull to `results/cpi_sweeps/`; `score_run.m` on each run;
   `cpi_sweep_table.m` writes `sweep.csv`, the figures and the README
   table.

## 6. Expected, so the results mean something

- SNR +3 dB per doubling until decorrelation; AoA error falling with it.
- Doppler width 1/T away from the crossing, smeared at 4 s near it.
- False plots per second flat with T at fixed Pfa.
- Real-time factor flat with T for ECA-CD, ECA-B, ECA-S, ECA-LD; FBLMS
  and CGLS have their own slopes; ECA-m's memory grows linearly with T
  and may not fit at the top of the range.
- Information age grows as T/2: the price of the gain, for a tracker.
