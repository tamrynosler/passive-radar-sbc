# Results chapter: the remaining test campaign

Written 2026-10-18 for the agent that picks this up next. It turns the
gap analysis of `Report/7_results.tex` against `results/` into ordered
tasks. Read `CLAUDE.md` first, in full, then this file, then
`Report/docs/plan_7_results.md` (the chapter plan the author agreed on
2026-10-17). The `.tex` carries a `% Sources:` comment above every
section saying which file each number must come from; those comments
are the contract.

## Rules that apply to every task

- **Every number in the thesis cites a file in the repo.** Nothing is
  typed from memory. Runs land under `results/`, scored by
  `matlab/score_run.m`, and the `.tex` names the path.
- **Sections 7.3 to 7.5 cite only the new runs** ("the rerun"). The
  older directories (`prelim/`, `automated_tests/`, `cpi_sweeps/`,
  `bandwidth_detections_tests/`) are for checking and for deciding, not
  for citing, unless the author says otherwise for a given table.
- **No central code changes.** These are runs, scripts and MATLAB
  scoring. If a task needs a change to DSI, CAF, CFAR or buffers, stop
  and ask.
- **Thesis text is one paragraph at a time with approval.** Tables,
  figure scaffolding and captions marked TODO may be filled freely once
  the data exists; say which file each column came from.
- **Clock-pinned runs need the author.** `jetson_clocks` needs sudo over
  `ssh -t`, so no run that must be cited as clocks-pinned can happen
  unattended. Prepare everything, then ask for a session.
- **Captures come from FERS on the Mac.** FERS lives in `~/FERS`; see
  `CLAUDE.md` "FERS" and `scenarios/S0_clean/README.md` "Regenerate".
  `--vita49-fullscale 0.005` must match `config/fers.conf`.
- **Data does not sync.** Captures, truth and goldens must be `scp`'d to
  the board explicitly (`data/` is gitignored and protected from rsync).
- Long board jobs run inside tmux on the board, or in the foreground of
  an ssh call backgrounded on the Mac. See `scripts/cpi_sweep.sh run` for
  the pattern (resumable, memory sampler beside each run, `runs.log`).
- The Jetson clock, ping, sudo and rsync traps are all in `CLAUDE.md`
  "Operating the kit". Read it before the first ssh.

## State on 2026-10-18

| Chapter block | Exists | Missing |
|---|---|---|
| 7.1 Method | Text, both annotated maps in `Report/figs/ch7/` | Captions |
| 7.2 Verification (S0_clean_wgn) | Nothing: no scenario, no data, no `results/L1_correctness/` | Everything (Task 5) |
| 7.3 Parameter table | `config/fers.conf` | Fill it (Task 8) |
| 7.3.1 CFAR sweep | `results/automated_tests/summary.md`, `FINDINGS.md`: six cancellers, Pfa 1e-5/1e-7/1e-9, 1-D and 2-D, on S0_long | ECA-m rows; the chosen setting written down (Decision A) |
| 7.3.2 Latency, memory | All seven at 1 s in `results/cpi_sweeps/sweep.csv` with `mem.log`, but with the third channel on | Two-channel figures if the table is meant two-channel (Decision C) |
| 7.3.3 Quality, noise | Nothing | Seven runs on S0_clean_wgn (Task 6) |
| 7.3.3 Quality, FM | 3-CPI prelim runs only | Seven full runs on S0_clean (Task 6) |
| 7.3.3 Quality, clutter | S1_clutter not built | Scenario, capture, seven runs (Task 6) |
| 7.3.3 Notch column | Derivable from `automated_tests/*/score.csv` (six cancellers) | ECA-m on S0_long; a script for the span in Hz (Task 3) |
| 7.3.4 CPI sweep | Complete: `results/cpi_sweeps/` | Placement decision only (author) |
| 7.4.1 Bandwidth | `bandwidth_detections_tests/*/per_cpi.csv` has refBwHz and false plots, no target SNR | Chosen-canceller S0_long run with frames on (Task 3) |
| 7.4.2 Zero Doppler | `automated_tests/*/score.csv` has per-CPI hit and hitSnrDb | Same run as 7.4.1 (Task 3) |
| 7.4.3 Two targets | S5_two not built | Scenario, capture, one run (Task 7) |
| 7.5.1 AoA accuracy | `results/cpi_sweeps/T1.0/<ALG>/score.csv` has aoaErrorDeg, plotCoherence; telemetry has directPhaseDeg; `data/S0_aoa/truth.csv` has bearingDeg, phaseDeg | Summary numbers, two figures (Task 1) |
| 7.5.2 Third-channel cost | Three-channel runs | One two-channel run on the same capture (Task 2) |

## Decisions to get from the author before Task 2

Ask these in one message at the start of the session. Do not guess.

- **A. The CFAR setting** for every new run. `config/fers.conf` has
  `CFAR_PFA = 1e-5`, 1-D kernel (0x4 / 0x8). The study in
  `results/automated_tests/FINDINGS.md` found each decade of Pfa halves
  the false plots and the 2-D kernel (1x4 / 2x8) recovers the target in
  the notch. Section 7.3.1 must name one setting and keep it.
- **B. The provisional canceller** for 7.4 and 7.5, so those can run
  before the clutter results exist. ECA-CD is the current default and the
  cheapest (real-time factor 0.027 in the sweep). If the clutter runs
  later change the choice, 7.4 and 7.5 are rerun with the winner.
- **C. Two or three channels for Table 7.3.2.** The sweep timings
  include the second surveillance chain. If the table is meant for the
  plain two-channel chain, the seven quality runs of Task 6 must be
  two-channel and their telemetry feeds 7.3.2.
- **D. The noise waveform file.** `~/FERS/Waveforms/WGN.h5` exists but
  was made for the old pulsed scenario; check its length and rate. If it
  is not 180 s at 204800 Hz, a new one must be written (a writer in the
  style of `matlab/rcf_to_fers_h5.m`); ask before adding a script.
- **E. The clutter and two-target geometries** (Tasks 6 and 7). Propose
  a `scenario.fersxml` diff for each; the author approves the design.

## Tasks, in order

Each task says what it feeds in the `.tex` (label), what to run, where
the output goes and when it is done. Tasks 1 and 3 need no board session
beyond what already exists or one short run; do them first.

### Task 1: AoA accuracy from existing runs (7.5.1, no new run)

Feeds `tab:results.aoa.accuracy`, `fig:results.aoa.track`,
`fig:results.aoa.planview`.

1. Input: `results/cpi_sweeps/T1.0/<ALG>/score.csv` and `telemetry.csv`
   for the provisional canceller (Decision B), `data/S0_aoa/truth.csv`.
   The score.csv was made with gate 2 km / 3 Hz (see its `summary.txt`).
2. Write one MATLAB script, `matlab/aoa_report.m`, peripheral: reads the
   run directory and truth, prints the table rows (direct-path phase
   computed vs measured mean, bearing span, error RMS / mean / max, CPIs
   with a bearing, median coherence) to `results/.../aoa_summary.txt`
   beside the run, and saves the two figures to `Report/figs/ch7/`.
   `directPhaseDeg` is in telemetry.csv; the computed value is printed by
   `fers_truth.m` (about -176 deg, `scenarios/S0_aoa/README.md`) and is
   the `phaseDeg` column of truth.csv.
3. Done when the summary file exists and the two PNGs are in
   `Report/figs/ch7/`. Fill the table with the file path in a comment.

Note the sweep found the error floor is a bias of about -0.3 deg
(`results/cpi_sweeps/README.md`); the chapter's notes ask for that.

### Task 2: the two-channel run (7.5.2, one board run)

Feeds `tab:results.aoa.cost`.

1. Config: copy `results/cpi_sweeps/T1.0/<ALG>/config.conf` for the
   provisional canceller and set `SURVEILLANCE2_RECEIVER_NAME = none`.
   Nothing else changes, so the three-channel row of the table is that
   sweep run and the two-channel row is this one.
2. Capture: `data/S0_aoa/capture_600s.pbrcap` (on the board already;
   check with `ssh jetson ls -l ~/passive-radar-sbc/data/S0_aoa/`).
3. Run on the board, clocks pinned, the memory sampler beside it (copy
   the per-run block of `scripts/cpi_sweep.sh run`), output to
   `/data/results/L2_performance/S0_aoa/<ALG>/<stamp>_2ch/`, then
   `make pull`. Write `run.txt` (fields in `results/README.md`).
4. Score with `score_run.m` against `data/S0_aoa/truth_T1.0.csv`,
   `'gateKm', 2, 'gateHz', 3`, so the two rows use the same gate.
5. Done when both directories hold `score.csv`, `telemetry.csv`,
   `mem.log`, `config.conf`, `run.txt`. Datagram loss is the `datagrams`
   column of telemetry.csv against the capture's count.

### Task 3: one S0_long run with frames on (7.4.1, 7.4.2, notch column)

Feeds `tab:results.conditions.bandwidth`, both bandwidth figures,
`tab:results.conditions.zerodoppler`, `fig:results.conditions.zerodoppler`,
and the Notch column of `tab:results.dsi.quality` for the provisional
canceller.

1. Config: `config/fers.conf` with the chosen CFAR (Decision A), the
   provisional canceller, `SURVEILLANCE2_RECEIVER_NAME = none`,
   `ARD_WRITE_FILES = 1` (score_run needs frames for `targetSnrDb` and
   `ridgeDb`), `ARD_STREAM = 0`, `DSI_BACKEND = GPU`.
   `results/automated_tests/run_test.sh` is the model but it forces
   frames off and writes under `automated_tests/`; do not reuse it as is.
2. Capture: `data/S0_long/capture.pbrcap` (600 s). Output to
   `/data/results/L2_performance/S0_long/<ALG>/<stamp>/`. Clocks pinned,
   memory sampler, `run.txt`. Frames for 600 CPIs are large; pull
   `score.csv`, `telemetry.csv`, `mem.log`, `config.conf`, `run.txt`,
   `plots.csv`, and leave the frames on the board unless a figure needs
   one.
3. Score against `data/S0_long/truth.csv`, gate 2 km / 3 Hz.
4. Bandwidth: join score.csv and telemetry.csv on `cpi`; telemetry has
   `refBwHz` and `refCentreHz` per CPI in the current build. Bin by
   bandwidth for the table; the two figures are a time series and
   histogram of `refBwHz`, and false plots and target SNR against it.
   The noise reference line comes later from Task 6.
5. Zero Doppler: the crossing is `|truthDopplerHz| < 5` (264 s to
   334 s, `scenarios/S0_long/README.md`). Pd inside and outside, CPIs
   lost, gap in seconds, ridge from `ridgeDb`.
6. Notch width: the Doppler span over which `hit` is 0 around the
   crossing, from the truth Doppler at the first and last missed CPI.
   Put the measurement in a small MATLAB function, `matlab/notch_width.m`,
   so the same rule gives the column for every canceller in Task 6.
7. Done when the scored run exists and the four figures are in
   `Report/figs/ch7/`.

Later, the Notch column for the other six cancellers comes from their
S0_long runs in Task 6 (same script). The author has not asked for
S0_long runs of all seven; confirm before running them.

### Task 4: fill what Tasks 1 to 3 allow (7.4, 7.5, part of 7.3)

Tables only, each number with its file in a `%` comment. Captions and
paragraphs are drafted in chat one at a time.

### Task 5: S0_clean_wgn and the whole of 7.2

Feeds every label in `sec:results.verify`.

1. **Scenario.** `scenarios/S0_clean_wgn/`: copy `S0_clean/scenario.fersxml`
   and swap both `cw_from_file` waveforms for the noise file (Decision D).
   Everything else identical, so `truth.csv` is the same as S0_clean's;
   still regenerate it from the new fersxml. Write its `README.md` in
   the style of the others.
2. **Capture and frozen CPI**, on the Mac, as in
   `scenarios/S0_clean/README.md` "Regenerate": `pbr_run --live
   --config config/fers.conf --seconds 185 --capture
   data/S0_clean_wgn/capture.pbrcap --dump-cpi data/S0_clean_wgn/cpi_0000.bin`,
   then FERS at `127.0.0.1:4991`. Count datagrams on the receiving end;
   FERS logs nothing.
3. **Truth.** `fers_truth('scenarios/S0_clean_wgn/scenario.fersxml',
   'config/fers.conf', 'data/S0_clean_wgn/truth.csv')`.
4. **Goldens**, all reading `config/fers.conf` (never hand-copied):
   `eca_golden` (golden_ecam.bin, golden_ecab.bin), `eca_ld_golden`,
   `fblms_golden`; then `dsi_verify --dump-output
   data/S0_clean_wgn/cleaned_0000.bin` with ECA-B to feed `ard_golden`
   (golden_ard.bin). Caveat from PROGRESS.md 2026-10-07: golden_ecab.bin
   matched only with `ECA_UPDATE_INTERVAL_s = 0.025`; the current
   fers.conf says 0.0025. dsi_verify refuses a golden whose header
   disagrees with the config, so if it refuses, report it and ask rather
   than editing either.
5. **Level 1 on the board**, CPU FP64 backend: `dsi_verify` against each of
   the four cancellation goldens, `ard_verify --stage caf` and `--stage
   cfar`. Write each output to
   `results/L1_correctness/S0_clean_wgn/<alg>_cpu_fp64.txt`. These fill
   `tab:results.verify.golden`.
6. **Level 1 GPU**, all seven cancellers: `dsi_verify --backend GPU`
   (suppression against the golden; for ECA-S, ECA-CD and CGLS, which
   have no MATLAB golden, use `--against CPU` as `prelim/` did) and
   `ard_verify --chained --backend GPU` for the detections. Output to
   `results/L1_correctness/S0_clean_wgn/<alg>_gpu.txt`. These fill
   `tab:results.verify.fp32`. Target SNR per canceller comes from
   scoring the chained map; if `ard_verify` cannot write that map, say
   so and ask.
7. **The 74-of-83 figure** in the 7.2 lead-in: score
   `data/S0_clean/golden/golden_ard.bin` (the FM golden) with the 7.1.2
   gate and write the result to
   `results/L1_correctness/S0_clean/golden_ard_scoring.txt`. A small
   MATLAB script is fine. If the count differs from 74 of 83, report it;
   the text changes, not the file.
8. **Figure 7.2.1.** `plot_ard` on the new golden_ard.bin, dB re peak,
   +-200 Hz, truth as a cross, detections as dots, saved to
   `Report/figs/ch7/s0_clean_wgn_cpi0_matlab_map.png`.
9. **The 180 s live run** (7.2.4), with the author: `pbr_run --live` on
   the board, clocks pinned (`scripts/run_live.sh` is the model), FERS on
   the Mac pointed at the board's IPv4 (`ssh jetson hostname -I`), the
   canceller from `fers.conf`, chosen CFAR, `--out
   /data/results/L2_performance/S0_clean_wgn/<ALG>/<stamp>/`. Pull, write
   `run.txt`, score against `data/S0_clean_wgn/truth.csv`. Fill
   `tab:results.verify.fullrun`; the figure is detections against the
   truth track in range and Doppler over time
   (`matlab/plot_detection_history.m` is the starting point).

### Task 6: the quality runs (7.3.3, 7.3.2, 7.3.5)

Feeds `tab:results.dsi.quality`, `tab:results.dsi.realtime`,
`fig:results.dsi.latency`, `fig:results.dsi.cluttermaps`,
`fig:results.dsi.ridge`, and the choice in 7.3.5.

1. **S1_clutter design** (Decision E): `scenarios/README.md` says static
   scatterers over range bins 2 to 60 on the 600 s waveform; "every
   scenario after S0 uses the 600 s waveform". Propose the fersxml (how
   many scatterers, RCS, spacing, which platform entries) in chat; wait.
   Then `scenarios/S1_clutter/`, capture on the Mac, truth, scp to the
   board.
2. **Runs**: seven cancellers on each of S0_clean_wgn, S0_clean and
   S1_clutter, chosen CFAR, channels per Decision C, `ARD_WRITE_FILES = 1`
   on S1_clutter (the maps figure and the zero-Doppler cut need one
   frame per canceller; score_run needs frames for SNR and ridge on every
   input). Write a sweep script in the style of `scripts/cpi_sweep.sh`
   (configs, run, resumable, memory sampler, `runs.log`); propose it
   before writing it. Output to
   `results/L2_performance/<scenario>/<ALG>/<stamp>/`. Clocks pinned,
   with the author.
3. **Score** every run with the scenario's truth, gate 2 km / 3 Hz.
   Noise, FM and clutter columns are `suppressionDbMean`,
   `targetSnrDbMean`, false plots shown per second, `pd` from each
   `summary.txt` (the sweep's `cpi_sweep_table.m` shows how to gather
   them; a sibling script for this layout is fine).
4. **7.3.2** comes from the same runs' `telemetry.csv` (`dsiMs`,
   `ardMs`, `totalMs`: median and max) and `mem.log`; the chapter says
   so. State whether the three inputs give the same latency.
5. **Notch column**: `matlab/notch_width.m` from Task 3 on each
   canceller's S0_long run (confirm with the author that those six extra
   runs are wanted; they are 1 to 4 min each on the board).
6. **Figures**: seven-panel clutter maps, one CPI, same colour scale,
   truth marked (`plot_ard`); zero-Doppler cut per canceller with the
   uncancelled map as reference.
7. Then the author chooses the canceller (7.3.5). If it is not the
   provisional one, Tasks 2 and 3 are rerun with the winner.

### Task 7: S5_two (7.4.3)

Feeds `tab:results.conditions.twotargets`, `fig:results.conditions.twotargets`.

1. Design (Decision E): a second target platform in the S1_clutter
   fersxml (S3 to S5 build on S1, `scenarios/README.md`), with a path
   whose separation from the first falls through one resolution cell
   (Table `tab:ch3.parameters`) during the run. Propose, wait.
2. Capture, truth (`fers_truth.m` already writes one row per target per
   CPI), one run with the chosen canceller, frames on, score.
3. `score_run.m` scores per target but does not report "two targets
   merged into one plot" or "smallest separation resolved". Propose a
   small addition or a separate script before writing it.

### Task 8: the chapter

- `tab:results.dsi.params` from the saved `config.conf` of the Task 6
  runs (not from `config/fers.conf`, which may change).
- `tab:results.dsi.cfarsweep` from `results/automated_tests/summary.md`
  unless the author wants the sweep rerun at the chosen setting
  (listed as an open item in the chapter plan).
- Every other table from the runs above. Every row's source in a `%`
  comment. Captions and paragraphs one at a time in chat.
- Update `PROGRESS.md` and `PLAN.md` at the end of each session, with
  dates from the Mac's `date`, not the board.

## Results layout and manifest

`results/README.md` is the contract: `L1_correctness/<scenario>/<alg>_<backend>.txt`,
`L2_performance/<scenario>/<ALG>/<stamp>/` with `config.conf`, `run.txt`,
`run.log`, `telemetry.csv`, `detections.csv`, `plots.csv`, `score.csv`,
`mem.log`. `run.txt` holds scenario, git hash, host, date, command line,
SHA-256 of the capture and the config, clocks pinned or not, L4T release.
A rerun gets a new stamp; the chapter names the stamp it used.

## Tools the tasks rely on

- `scripts/cpi_sweep.sh`: the sweep pattern (configs, truth, run).
- `scripts/run_live.sh`, `scripts/pin_clocks.sh`: a live clock-pinned run.
- `results/automated_tests/run_test.sh`: one replay with keys replaced.
- `matlab/fers_truth.m`, `matlab/score_run.m`, `matlab/cpi_sweep_table.m`,
  `matlab/plot_ard.m`, `matlab/plot_detection_history.m`.
- `matlab/eca_golden.m`, `matlab/eca_ld_golden.m`, `matlab/fblms_golden.m`,
  `matlab/ard_golden.m`; `src/app/dsi_verify.cpp`, `src/app/ard_verify.cpp`.
- `pbr_run --file | --live, --config, --out, --capture, --dump-cpi,
  --seconds, --save-config`.
