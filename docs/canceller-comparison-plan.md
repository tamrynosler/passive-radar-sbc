# Canceller comparison: what the 2026-10-08 study showed, and the plan

Written 2026-10-08 evening for the work of 2026-10-09. The study itself is
in `results/automated_tests/` (`FINDINGS.md`, `summary.md`, one hold map
per test); this note says what it means for the thesis comparison and what
to do next. The thesis question is not "does each algorithm work" but "how
do they compare on the same input, on the same hardware, with the same
outputs" (CLAUDE.md: suppression against latency, now also detection
quality).

## 1. What the study established

Input: one 600 s FERS capture of S0_long (a target flying through zero
Doppler at t = 300 s), replayed on the Jetson GPU 34 times, one config per
run; scored on the plots the viewer shows, against the truth.

1. **CFAR.** Each decade of Pfa halves the false plots (6-12 per CPI at
   1e-5, 1.7-3 at 1e-9) and costs the target inside the canceller's notch,
   where it is weak. The 2-D kernel (range training on) buys that target
   back for about 30% more false plots. Combined, the 2-D kernel at Pfa
   1e-9 gives ECA-CD Pd 0.998 / 1.000 through the crossing at 4.9 false
   plots per CPI and ECA-S 0.980 / 0.889 at 2.5. This is settled; no more
   CFAR sweeping is needed.
2. **The false plots are common-mode.** On every hold map, whatever the
   canceller, they lie on the target's own sidelobe arcs (the FM
   ambiguity function), densest when the target is strongest. They do not
   distinguish cancellers. What does: the residue on the zero-Doppler
   ridge, the width and depth of the notch, and the suppression.
3. **The cancellers.**
   - ECA-S: deepest suppression (52.5 dB), fewest false plots, quietest
     ridge (0.01 per CPI); its notch loses the target below about 5 Hz
     at the lower Pfa unless the 2-D kernel is used. 231 ms per CPI.
   - ECA-CD: Pd 0.98-1.0, twice ECA-S's false plots, 12 ms.
   - FBLMS: the only one that never loses the target through zero
     Doppler (notch 1-3 Hz); leaves about one plot per CPI on the ridge
     at every setting; mu makes no difference in 0.005-0.02. 364 ms.
   - CGLS primed: the widest notch of the working cancellers (low-Doppler
     Pd 0.67-0.75), second-most ridge residue, slowest (486 ms). Unprimed
     it leaves 15.6 ridge plots per CPI: priming is not optional.
   - ECA-B: fails the GPU's FP32 Cholesky ("not positive definite") on
     106 of 600 CPIs at the config's relative loading 1e-6; clean at 1e-4,
     but 1e-4 costs about one ridge plot per CPI (and does the same to
     ECA-S). ECA-S's estimation Grams are the same 25 ms matrices and
     never fail, which is not yet explained (Section 3).
   - ECA-LD: at a 25 ms batch the Toeplitz approximation of a Gram with
     condition number 1.8e5 is not positive definite (the approximation
     error, ~K/L = 2.3% of the entries, is a hundred times the smallest
     eigenvalue), so the Levinson recursion breaks in a few batches per
     CPI and the output is 10-16 dB. Not precision: FP64 fails
     identically; exact ECA-B in FP64 is clean on every CPI. The paper's
     own biased estimator is positive definite by construction but still
     gives only 29 dB at 25 ms. From a 0.1 s batch up, ECA-LD matches
     ECA-B's suppression exactly (52.8 dB) at a tenth of its time, with
     the long-batch notch that implies (low-Doppler Pd 0.04 at 0.1 s).
     The method is valid for N >> K by a larger factor than 43; the
     paper's examples use K = 10. On S0_clean's cut it happened to hold
     at 25 ms. Not a bug; a limit of the approximation at K = 120 on FM.

   | Batch | K/L | ECA-LD, our estimator | ECA-LD, paper's estimator | exact ECA-B |
   |---|---|---|---|---|
   | 25 ms | 2.3e-2 | 9.9 dB (4/40 batches lost) | 29.0 dB | 53.0 dB |
   | 0.1 s | 5.9e-3 | 52.8 dB | 41.6 dB | |
   | 0.25 s | 2.3e-3 | 52.7 dB | 49.1 dB | |
   | 1 s | 5.9e-4 | 52.7 dB | 52.7 dB | |

   (S0_long CPI 0, MATLAB FP64, K = 120, loading 1e-6.)

## 2. Why a single shared setting is the wrong comparison

"25 ms for everyone" compares ECA-LD's broken approximation and
CGLS's tuning choice against ECA-S's best point. Each algorithm has one
knob that sets its notch and its cost, and the knobs mean different
things. The defensible comparison is each algorithm along its own knob,
on the same capture, with the same four outputs: suppression, the notch
(low-Doppler Pd, and the notch profile of Section 4), ridge residue, and
time per CPI with pinned clocks. The report then compares trade-off
curves, and the reader can pick the operating point.

| Algorithm | Its knob | Points to run |
|---|---|---|
| ECA-B | batch length | 25 ms, 0.1, 0.25, 1 s (1 s is ECA-m) |
| ECA-LD | batch length | the same four, paired with ECA-B |
| ECA-S | update interval, estimation fixed at 25 ms | 1.25, 2.5, 5 ms |
| ECA-CD | batch and Doppler taps | 2.5 ms with P = 0, 1, 3 |
| CGLS | iterations, primed | 10, 30, 60 |
| FBLMS | mu | done: 0.005, 0.01, 0.02 are the same |

The ECA-B / ECA-LD pair at identical batch lengths is the thesis-grade
result for ECA-LD: the same least-squares problem, exact against
approximate, at what batch length the approximation holds and what it
buys in time. Already known in FP64 (above) and in GPU time (26 ms
against about 160 ms at 25 ms); it needs the four GPU runs each.

All runs at the settled CFAR (2-D kernel 1x4 / 2x8, Pfa 1e-9), on the
S0_long capture, with `results/automated_tests/run_test.sh`.

## 3. Two fairness problems to settle first

1. **ECA-B's Cholesky failures against ECA-S's none.** The two estimate
   the same 25 ms Grams; ECA-S never reports a non-positive-definite
   window, ECA-B does on 18% of CPIs. Either FP32 accumulation order
   differs enough (ECA-S sums partial chunk Grams) or the ECA-B path has a
   defect. Check: the CPU twin in FP32, ECA-B, on the capture
   (`pbr_run --file` on the Mac, `DSI_BACKEND = CPU`,
   `DSI_MATH_PRECISION = FP32`, about 2 min per 100 CPIs). If it fails
   the same way it is rounding, and the fair comparison uses ONE loading
   for the whole ECA family (1e-4, with its ridge cost stated); if it
   does not, the GPU ECA-B is to be fixed before anything is cited.
   Central code: report, then the author decides.
2. **Sidelobe plots.** Either implement the CLEAN step before the final
   comparison, or state that sidelobe plots are a constant of the
   illuminator and compare cancellers on the canceller-specific metrics.
   Proposal: the latter now, CLEAN later (PLAN.md).

## 4. The notch profile: the figure to build, no new runs

The S0_long crossing sweeps the target from +50 to -50 Hz through every
canceller's notch. For each run, the hit plot's `peakSnrDb` (the CFAR
margin) against the truth Doppler, from `plots.csv` and `truth.csv`, is
that canceller's notch profile, already measured. One figure, six curves:
ECA-S and ECA-B at their 1/T_B, FBLMS at 1-3 Hz, ECA-CD, CGLS. This is
the centrepiece of the detection comparison and costs an hour of MATLAB
(a function next to `score_plots.m`; average over the runs that share a
canceller and CFAR setting). A map-based version (target SNR over the
floor, `score_run.m`'s `targetSnrDb`) needs frames written
(`ARD_WRITE_FILES = 1`, about 900 MB per run) and can follow for the two
or three settings the report plots.

## 5. Timing under the methodology

The study's timings mix pinned and unpinned clocks and include the map.
The citable column is one `dsi_bench` pass per algorithm at its chosen
settings with `jetson_clocks` and `tegrastats` logged
(`docs/benchmarking-methodology.md`), 10 iterations after warm-up: about
15 minutes for all six, done once the settings of Section 2 are fixed.

## 6. A second waveform cut

Everything above is one 10-minute cut of one recording, and the
conditioning results (ECA-B, ECA-LD) depend on programme content: the
S0_clean cut behaved differently. Repeat the key rows (each algorithm at
its chosen point, the ECA-B / ECA-LD pair) on a second capture: the
0-180 s Malmesbury cut (S0_clean's waveform, rendered with the S0_long
fly-by), or the real USRP recording once it exists. Two hours of board
time; the harness runs it unattended (`run_test.sh` takes the capture
path as a variable to add).

## 7. Order of work for 2026-10-09

1. Section 3.1, the ECA-B check (15 min). Decides the loading rule.
2. Section 4, the notch-profile figure from existing runs (1 h).
3. Section 2, the batch-length pair ECA-B / ECA-LD on the GPU (8 runs,
   about 20 min of board time) and the ECA-S / ECA-CD / CGLS knob points
   (about 10 runs, 40 min).
4. Section 5, the pinned timing pass (15 min).
5. Section 6, the second capture, when there is a free two hours.

Not planned: making ECA-LD work at 25 ms (a biased-estimator fallback
only lifts it to about 29 dB), more CFAR sweeps, or changes to central
code before the Section 3.1 result is in.
