# Canceller findings, 2026-10-19 to 2026-10-21

What the results campaign found about the DSI cancellers, for the author
to use in Chapter 7. The thesis is not updated from this file yet. Every
number names the file it came from; `results/` is gitignored, so the runs
live on the Mac and on the board under `/data/results/`.

All runs: Jetson Nano, clocks pinned (`clocks pinned` in each `run.txt`),
GPU, 1 s CPI unless stated, 1-D CFAR along Doppler (guard 4, training 6,
Pfa 1e-5), two channels unless stated, scored by `matlab/score_run.m` with
a 2 km / 3 Hz gate. Settings shared by every run are in the header of
`scripts/results_run.sh`; ECA-LD solves the whole CPI in one batch.

## 1. ECA-CD is not the right canceller once there is clutter

S1_clutter (S0_long plus 28 static scatterers over bins 0 to 60 and 4 slow
movers at 1 to 2 Hz; `scenarios/S1_clutter/README.md`), FM waveform, 600 s.
Source: `results/L2_performance/quality.csv`, rows `S1_clutter`, runs
`results/L2_performance/S1_clutter/<ALG>/20261021_*_2ch/`. Notch:
`results/L2_performance/S0_long/notch.csv`.

| Canceller | Suppression (dB) | Target SNR (dB) | Pd | False plots per s | Notch (Hz) | Latency, median (ms) |
|---|---|---|---|---|---|---|
| ECA-S | 50.3 | 35.2 | 0.97 | 5.3 | 1.1 | 125 |
| ECA-B | 50.0 | 35.0 | 0.95 | 11.9 | 4.2 | 63 |
| FBLMS | 48.4 | 38.4 | 1.00 | 13.5 | 0 | 43 |
| CGLS | 47.5 | 38.3 | 0.97 | 7.9 | 3.1 | 220 |
| ECA-LD | 46.1 | 36.7 | 1.00 | 13.0 | 0 | 25 |
| ECA-m | 38.3 | 31.3 | 0.99 | 13.0 | 0 | 67 |
| ECA-CD | 25.3 | 17.2 | 0.38 | 10.2 | 1.1 | 13 |

ECA-CD's per-carrier model cannot represent clutter spread over 60 range
bins: 25 dB with either waveform, at any CPI length, and a longer batch
(20 ms) buys 9 dB but adds batch-rate ambiguity lines that become plots
(`results/L2_performance/S1_clutter{,_wgn}/ECA-CD/20261020_*_recap/`).
CGLS is dominated by FBLMS (same SNR, five times the cost, a 3 Hz notch);
ECA-B is dominated by ECA-S.

## 2. The three contenders through every condition test

ECA-S, FBLMS and ECA-LD run on S0_long with frames (bandwidth and zero
Doppler) and on S0_aoa with three and with two channels (angle accuracy and
the cost of the third channel), 2026-10-21. Sources: each run's
`conditions_zerodoppler.txt`, `conditions_bandwidth.txt`, `aoa_summary.txt`
and `summary.txt` under `results/L2_performance/S0_long/<ALG>/20261021_14*_2ch/`
and `results/L2_performance/S0_aoa/<ALG>/20261021_14*`. ECA-CD's row is
from its 2026-10-19 runs.

| | ECA-CD | ECA-LD | FBLMS | ECA-S |
|---|---|---|---|---|
| Zero-Doppler crossing: CPIs lost of 72 | 6 | 0 | 0 | 4 |
| Notch (Hz) | 1.1 | 0 | 0 | 1.1 |
| Pd per bandwidth quartile | 0.98 to 1.00 | 1.00 in all four | 0.99 to 1.00 | 0.98 to 1.00 |
| AoA error, RMS (deg) | 0.32 | 0.32 | 0.49 | 0.41 |
| Latency, median, 2 / 3 channels (ms) | 12 / 26 | 24 / 51 | 42 / 86 | 124 / 249 |
| System memory max, 3 channels (MB) | 612 | 641 | 627 | 948 |

ECA-LD gives the most dependable detections: Pd 1.00 on every test, no
notch, ECA-CD's angle accuracy, at the second-lowest cost. FBLMS is the
runner-up (slightly higher SNR on clutter, poorer bearing, twice the cost).
ECA-S gives the cleanest maps and most suppression but loses CPIs at zero
Doppler, costs five times more and holds the most memory. The choice for
Section 7.3.5 is the author's; if it is ECA-LD, its runs already exist for
every table in Sections 7.4 and 7.5.

## 3. Noise with clutter: a simulator effect, not a canceller one

On S1_clutter_wgn (the same clutter, full-band white Gaussian noise as the
illuminator) the batched ECAs, CGLS and FBLMS suppress 8 to 10 dB less
than on FM (`quality.csv`, rows `S1_clutter_wgn`). Least-squares ceiling
on CPI 0, 25 ms windows, computed in MATLAB from the frozen CPIs:

| Input | 60 taps | 120 taps | 180 taps | 240 taps |
|---|---|---|---|---|
| FM + clutter | 44.5 | 52.8 | 52.8 | 52.9 |
| Noise + clutter | 33.2 | 40.2 | 40.3 | 40.3 |
| FM, no clutter (S0_long) | 52.9 | 53.0 | 53.0 | 53.1 |

Filtering both channels with the same ideal low-pass before the fit, 120
taps:

| Input | Whole band | Inner 80 % | Inner 50 % |
|---|---|---|---|
| Noise + clutter | 40.2 | 52.5 | 52.3 |
| FM + clutter | 52.8 | 53.5 | 54.6 |
| Noise, no clutter (S0_clean_wgn) | 48.6 | 51.5 | 51.6 |

More taps do nothing; removing the band edges recovers the FM ceiling. The
clutter points sit at fractional delays, and a full-band waveform has
energy at the band edge, where neither integer delay taps nor FERS's
fractional-delay filter represent a fractional delay well. FM has little
energy there. A noise waveform band-limited to about 80 % of the band
would remove the effect (a new `WGN_600s` file and a 30 min recapture).
Scripts: `ls_ceiling.m`, `ls_ceiling_band.m` (session scratchpad,
2026-10-21; not in the repo).

## 4. CGLS: correct, and one bug fixed

- An independent MATLAB CGLS (explicit delay matrix, Tong's update order
  from `matlab/legacy/MatlabProcFM-master 2/Cancellation/CGLS_Cancellation.m`)
  matches our CPU FP64 output to 2.5e-8 to 3e-7 on noise, FM, clutter and
  Tong's 210 taps / 0.5 s windows. Our step length and Tong's agree to 1e-7.
- Ten cold iterations reach 18 to 21 dB on FM against 49 to 53 dB for the
  exact least squares: under-convergence, not a bug.
- GPU against CPU FP64 differs by 21 to 41 % at 120 taps, 0.25 s windows,
  10 iterations on FM; the CPU in FP32 differs from its own FP64 by the
  same order. An unconverged CG in FP32 amplifies rounding, about 1 dB of
  suppression. Gone once converged or with 0.5 s windows.
- Bug (fixed 2026-10-21): CGLS had no `resetState()`, so primed CGLS began
  CPI 0 from weights fitted to pbr_run's synthetic warm-up signal (-3.4 dB
  CPU, -6.4 dB GPU instead of about +21). Now primed CPI 0 equals cold CPI 0.
  Every CGLS result used in the thesis is from a run after the fix.

## 5. FP32 on the GPU costs nothing measurable

Chained canceller + map on the GPU (FP32) against the same chain on the
CPU in FP64, CPI 0 of S0_clean_wgn, all seven cancellers: target SNR within
0.001 dB, identical CFAR cells, device coupling exact (0 of 192000 cells).
Source: `results/L1_correctness/S0_clean_wgn/maps/fp32_vs_fp64.csv`
(`ard_verify --dump-map`, scored by `golden_ard_score.m` with score_run's
SNR definition).

## 6. Tong (2014) comparison, after the CGLS fix

CGLS with Tong's TK1 parameters (4 s CPI, 0.5 s windows, 10 iterations,
210 taps) on S0_long: cancellation 512 to 517 ms median against Tong's
3059 ms; total 530 to 535 ms against 4690 ms. Primed after the fix:
suppression 49.1 dB, Pd 0.96, target SNR 45.8 dB, 12.6 false plots per CPI.
Sources: `results/L2_tong/S0_long/CGLS/20261020_203609_2ch` (cold) and
`20261021_122152_2ch` (primed); brief in `docs/tong-comparison-run.md`.

## 7. Board power: over-current alarms during every run

`dmesg` on 2026-10-21 shows 2723 `soctherm: OC ALARM` events, about one per
second during every sustained run from 12:07 onward and none while idle;
the board also rebooted unprompted twice on 2026-10-19 and 2026-10-20. OC
alarms are the Tegra's supply over-current events and can throttle the
clocks. The latencies measured during the alarms match the 2026-10-19 runs
within 1 to 2 ms (e.g. ECA-S 116 against 117 ms median), with no step
changes inside a run, so no throttling is visible in the timing; proving
it needs `tegrastats` under sudo. The supply is likely undersized: the Nano
wants a 5 V 4 A barrel supply with the J48 jumper fitted.

## 8. Batch length: ECA-B, ECA-LD and ECA-S on S1_clutter, FM and noise

32 runs, 2026-10-21, clocks pinned, every one exit 0, frames on. ECA-B and
ECA-LD at 25, 50, 100, 250, 500, 1000 ms (ECA-B updates once per batch);
ECA-S at 10, 25, 50, 100 ms with updates every 2.5 ms. Source:
`results/L2_batch/batch_sweep.csv` (`matlab/batch_sweep_table.m`), runs under
`results/L2_batch/B<len>/<scenario>/<ALG>/`, figures in
`results/L2_batch/figures/`. FM = S1_clutter, noise = S1_clutter_wgn.

| Batch (ms) | ECA-B FM supp / SNR / Pd | ECA-LD FM supp / SNR / Pd | ECA-B noise supp / SNR | ECA-LD noise supp / SNR |
|---|---|---|---|---|
| 25 | 50.0 / 35.0 / 0.95 | 15.8 / 10.7 / 0.08 | 40.0 / 28.9 | 39.7 / 29.0 |
| 50 | 49.5 / 38.3 / 0.96 | 42.6 / 30.9 / 0.77 | 39.9 / 33.3 | 39.9 / 33.5 |
| 100 | 48.9 / 39.5 / 0.88 | 48.2 / 38.2 / 0.88 | 39.8 / 35.7 | 39.8 / 35.7 |
| 250 | 47.6 / 38.3 / 0.97 | 47.5 / 38.2 / 0.97 | 39.6 / 37.0 | 39.6 / 37.0 |
| 500 | 46.3 / 36.8 / 1.00 | 46.3 / 36.8 / 1.00 | 39.4 / 37.3 | 39.4 / 37.3 |
| 1000 | 46.1 / 36.8 / 1.00 | 46.1 / 36.7 / 1.00 | 39.4 / 37.5 | 39.4 / 37.5 |

- **The Toeplitz approximation is free from 250 ms up and breaks on FM below
  100 ms.** ECA-LD equals ECA-B to 0.1 dB at 250, 500 and 1000 ms on both
  waveforms. On FM it falls 0.7 dB short at 100 ms, 7 dB at 50 ms, and
  collapses at 25 ms (15.8 dB, Pd 0.08: 551 of 600 CPIs missed at every
  Doppler). On noise it stays within 0.3 dB of ECA-B at every length. The
  approximation drops the Gram's edge terms, of relative size about K / N_B
  (120 / 5120 at 25 ms); white noise has small off-diagonal correlation so
  the error stays small, while FM's narrow spectrum correlates the delayed
  copies strongly, so the same edge error is large relative to a badly
  conditioned matrix (an explanation consistent with the data, not proven).
  This is why the 2026-10-12 CPI sweep, at a 0.1 s ECA-LD batch, gave
  Pd 0.85 to 0.92.
- **The 100 ms Pd dip is geometry.** Every missed CPI at 100 ms (70 for
  both ECA-B and ECA-LD) has the target within 10 Hz of zero Doppler: the
  batch's notch catches the slow crossing (|Doppler| < 5 Hz for 70 s).
  At 1000 ms both miss 2 CPIs.
- **Shorter batches trade target SNR for suppression in ECA-B.** 50 dB at
  25 ms against 46 dB at 1000 ms; target SNR peaks at 50 to 250 ms; Pd is
  best at 500 to 1000 ms.
- **ECA-S suppression barely depends on its batch** (49.2 to 50.9 dB on
  FM), because it updates every 2.5 ms whatever the batch; its target SNR
  rises with the batch (30.8 dB at 10 ms to 40.2 dB at 100 ms, the best of
  any run on FM + clutter), at Pd 0.94 to 0.98.
- **Latency does not depend on the batch length** for any of the three:
  ECA-B 62 to 68 ms, ECA-LD 24 to 25 ms, ECA-S 125 to 133 ms median.
- **On noise every canceller sits at 39.4 to 40.2 dB at every batch
  length**: the band-edge ceiling of section 3, not a canceller limit.
