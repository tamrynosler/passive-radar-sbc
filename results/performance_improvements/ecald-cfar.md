# ECA-LD Levinson kernel and CFAR kernel

Board: Jetson Nano, clocks pinned (GPU 921.6 MHz). Other agents share the GPU, so
every "before" below was re-measured in the same session by alternating the old
binary (~/passive-radar-sbc/build) with the new one (~/pr-ldcfar/build), 5 iterations
each, median. Absolute numbers drift with board load; the pairs are what to compare.
Inputs: data/S0_clean/cpi_0000.bin and cleaned_0000.bin, config/fers.conf, and a /tmp
copy with CFAR_GUARD_RANGE=1 CFAR_GUARD_DOPPLER=4 CFAR_TRAIN_RANGE=2 CFAR_TRAIN_DOPPLER=8.

## Item 1: levinsonKernel (src/dsi/gpu/EcaLdGpu.cu)

Was: one block per batch, one thread per tap, K = 120 steps each with two block
reductions and three barriers. The real work is only 40 x about 14k complex
multiply-adds, so the time went on the reductions and barriers (10.1 ms).

Now: one thread per batch runs the recursion serially, in the same loops and the
same order as EcaLdCpu::levinson. rr, bvec, x, y and yNew are per-thread local
arrays of kMaxLdTaps = 256. setUp checks K <= kMaxLdTaps as well as K <= kBlock.
The degenerate guard and its atomic counter behave as before.

Result (quiet board, ECA-LD, dsi_bench, 5 runs of 5 iterations):
- old 24.5 ms, new 20.7 ms per CPI. Saving about 3.8 ms.
- Measured with cudaEvents (temporary, removed): the new kernel takes 6.0 ms, not
  the hoped 0.2 ms. It is a single SM (the Nano has one) with 2 warps of 40 active
  threads, so there is nothing to hide latency, and the local arrays (about 5 KB per
  thread, 200 KB in all) do not fit in L1, so most reads wait on L2. #pragma unroll 8
  on the inner loops changed nothing. Further gain would need more threads per batch
  (and a different summation order) or shared-memory arrays; not done.

Accuracy (before -> after):
- dsi_verify against golden_ecald.bin: relative L2 5.2e-4 -> 6.8e-4 (tolerance 1e-4, as before)
- dsi_verify --against CPU --precision FP32: 7.9e-4 -> 8.6e-4
- Suppression 51.6187 dB, identical to the reference.
So making the order equal to the CPU twin did NOT shrink the GPU-vs-CPU difference.
The difference comes from elsewhere (cuFFT against HostFft in the correlations, then
amplified by the ill-conditioned Toeplitz solve; the GPU also fuses multiply-adds),
not from the Levinson summation order. Not a regression in kind, but it is slightly
larger, not smaller.

## Item 2: CFAR (src/ard/gpu/ArdGpu.cu)

BatchesArdGpu.cu has no CFAR of its own; the Batches route shares ArdGpu's
runCfar, so one edit covers both.

Was: each cell walked its whole window from global memory: 16 reads (1-D window),
148 reads (2-D window).

Now: dopplerPrefixKernel (one thread per range bin, serial over Doppler) writes a
running sum S[r][d] into a new buffer md_fPrefix, in the layout of the power map.
cfarKernel adds, for each range row of the window, S at the span's end minus S just
before its start, and subtracts the guard block's rows the same way. Rows off the
map count zero, the divisor is the full training count: unchanged, as is everything
after the threshold test. Reads per cell: 4 (1-D), 20 (2-D).

Before / after, ard_bench, whole CPI, median (same-session pairs, quiet board):

| case                | old ms | new ms | detections old / new |
|---------------------|--------|--------|----------------------|
| Direct, 1-D CFAR    | 16.06  | 14.17  | 83 / 83              |
| Direct, 2-D CFAR    | 19.95  | 15.98  | 95 / 95              |
| Batches, 1-D CFAR   | 15.39* | 9.37   | 83 / 83              |
| Batches, 2-D CFAR   | 14.98  | 10.39  | 95 / 95              |

(*the old Batches 1-D run was noisy, min 14.1 ms; other agents were using the GPU.
Border counts also match: 2 and 10.)
Gain is about 2 ms (1-D) and 4 ms (2-D) per CPI, in line with cfarKernel having been
2.84 ms and 6.62 ms.

Rounding: the prefix is a float, so a window sum is a difference of two running
totals. The error is a few 1e-7 of the running total of that range row, so a row that
holds a very strong target makes the sums of quiet cells further along that row less
accurate (the running total includes the target). The test data showed no effect:
- ard_verify (CFAR stage, GPU, golden_ard.bin): 83 detections, missed 0 / extra 0
  against MATLAB, map relL2 3.2e-7, same as the old binary.
- Detection counts and border counts equal in all four timing cases.
- No detection flipped, so there is no cell/margin to report. A cell within about
  1e-5 of its threshold could still flip on other data.
- tests/test_ard.cpp compares the GPU and CPU detection lists exactly (same count,
  same cells, sorted) on its synthetic scene, and it passed.

## make test (REMOTE=~/pr-ldcfar)

16 of 16 passed, including:
- test_ard: GPU CFAR finds the target (6 detections), GPU vs CPU map relL2 1.82e-7, detections 4 vs 4
- test_batches_ard: GPU K=32/129/130 relL2 3.4e-7 / 4.1e-7 / 6.6e-7, detections 4/6/5 vs CPU the same
- test_ecald: GPU vs CPU-FP32 relL2 1.30e-7 (noise), GPU cancels clutter 49.2 dB, bit-identical across runs
