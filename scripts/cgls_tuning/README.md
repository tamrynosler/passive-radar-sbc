# scripts/cgls_tuning/

Manual CGLS parameter tests on the laptop CPU, driven by the author. Each
test runs the whole chain on `data/S0_clean/capture.pbrcap` (3 CPIs) with
the parameters set at the top of `run_cgls.sh`, and returns suppression,
time per CPI, detections and a range-Doppler map per CPI.

```bash
# 1. edit the PARAMETERS block in run_cgls.sh
# 2. run, with a label for the test
bash scripts/cgls_tuning/run_cgls.sh it60_fp64
# 3. plot (map, zero-Doppler cut, Doppler cut at the target; prints telemetry and score)
/Applications/MATLAB_R2025a.app/bin/matlab -batch "addpath('scripts/cgls_tuning'); plot_cgls('it60_fp64_<stamp>')"
```

A sweep: set `CGLS_BATCH_SWEEP` in `run_cgls.sh` to a list of batch
lengths and `./run_cgls.sh NAME` runs each as `NAME_b<batch>`; then
`plot_cgls_sweep('NAME')` in MATLAB draws every map side by side and the
suppression / detections / cost against batch length.

Findings so far are in `RESULTS.md` (2026-10-02 afternoon): cost linear in
iterations, flat in batch length; the warm-up primes CGLS from garbage when
`CGLS_PRIME = 1`; the Doppler streak through the target is the FM
illuminator's envelope, not the canceller.

Output goes to `results/CGLS_tuning/` (gitignored): one directory per test,
and `index.csv` with one row per test (parameters and headline numbers), so
a sweep can be compared without opening each directory.

Needs `bin/pbr_run` (`make host`) and the capture in `data/S0_clean/`.

The baseline this is tuning from: `RESULTS.md`, 2026-10-02 (cold CGLS at 30
iterations: 41.8 dB, +16 dB zero-Doppler ridge, GPU and CPU outputs 22 %
apart against FP64). Questions the sweep should answer:

- how suppression and the ridge change with iterations (30, 60, 120, 240)
- what priming buys at a given count (CPI 1 and 2 vs CPI 0)
- whether FP32 and FP64 converge to the same answer as iterations grow, or
  diverge (the GPU is FP32 only)
- the window length (0.0625 s as Schuepbach, 0.25 s, 1 s)
