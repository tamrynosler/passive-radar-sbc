# Benchmarking methodology

Notes underpinning the performance claims in the thesis. The goal is that every
latency number is reproducible and free of hidden variables (throttling, DVFS,
memory pressure).

## Controlled conditions

- **Clocks pinned** with `jetson_clocks` so GPU/CPU frequencies are fixed for the
  duration of a run; DVFS is removed as a variable.
- **Fan pinned** to maximum during runs; a `tegrastats` trace is captured
  alongside every run so the temperature record can be inspected for throttling.
- **Headless over SSH** so the GPU kernel watchdog ("Run time limit on kernels:
  Yes" in `deviceQuery`) does not apply and the display does not consume the
  shared memory pool.
- Timing uses **CUDA events** around the transform only, after a 3-iteration
  warmup that absorbs lazy context/plan initialisation.

## What is measured

- Plan creation time (one-time cost, reported separately from execution).
- Per-transform execution: mean, min, standard deviation over N iterations.
- Effective GFLOP/s using the 5·N·log₂N per-transform model.
- Sample throughput (MS/s).
- Workspace and data memory footprint, and free memory after allocation, via
  `cufftGetSize*` / `cudaMemGetInfo`.

## Design questions this informs

1. **Achievable CPI / FFT length.** Largest N whose per-transform latency stays
   inside the real-time budget for one coherent processing interval.
2. **Batched Doppler capacity.** Largest batch (≈ range-bin count) that fits in
   memory and meets budget at a chosen Doppler length.
3. **Size selection.** Prefer 7-smooth FFT lengths; the harness flags sizes with
   a prime factor > 7, which cuFFT handles far less efficiently.

## Profiling tools

Under CUDA 10.2 on the Nano: CUDA events (in-harness), `nvprof`, and `tegrastats`.
Nsight Systems/Compute are unavailable at this toolkit version. Record the tool
and version used for each figure.

## To expand

- Table: size sweep results (N, latency, GFLOP/s, footprint) with the ceiling row.
- Table: batch sweep at the chosen Doppler length.
- Plot: latency vs N with the real-time budget line overlaid.
- Cross-reference the chosen operating point to the DSI cancellation benchmarks.
