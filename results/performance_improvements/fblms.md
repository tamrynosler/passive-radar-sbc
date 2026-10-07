# FBLMS on the GPU: one kernel for the weight loop

## Before

`runBlocks()` looped over the 1600 blocks of a CPI (N = 128 taps, M = 256).
Each block issued 4 single-transform cuFFT calls and 5 small kernels, 9
launches per block, about 14,400 per CPI. Each launch worked on 256 points.
The GPU did only about 35 ms of work (cuFFT 22 ms, five small kernels); the
other ~330 ms of the 364 ms wall time was the host issuing launches one after
another. The canceller was launch-bound, not compute-bound.

## Now

Two launches per CPI besides the copies:

1. `gatherKernel` and one batched cuFFT of all reference blocks into
   `d_cXAll` (unchanged).
2. `fblmsBlocksKernel<<<1, M>>>`: one thread block of M = 2N threads runs all
   B blocks in turn. W, P and two work buffers live in shared memory (loaded
   from `d_cW`/`d_fP` at the start, stored back at the end, so the state
   across CPIs is as before). Thread m owns bin m. Per block: power update,
   Y = XW, inverse FFT, error and output, forward FFT of [0 ; e], gradient,
   inverse FFT, constraint, forward FFT, W += 2 Gc, with `__syncthreads`
   between stages. The arithmetic order is that of the old kernels.

The four in-loop transforms use `fftBlock`, a radix-2 FFT in shared memory
(bit-reversal, then log2(M) butterfly stages, half the threads active per
stage; inverse uses conjugate twiddles, unnormalised like cuFFT). The twiddle
table is computed on the host in double and copied to `__constant__` memory
once in `setUp`. The single-transform cuFFT plan and the buffers `md_cY`,
`md_cEPad`, `md_cG`, `md_cGc` are gone.

## Numbers (Jetson Nano, `config/fers.conf`, `data/S0_clean/cpi_0000.bin`)

No other `dsi_bench`/`pbr_run` was running before or after. Clocks were not
pinned, so the timing is indicative.

| | before | after |
|---|---|---|
| `dsi_bench` FBLMS GPU, median of 5 | 364 ms | 36.3 ms (min 36.27, max 36.45) |

Verification:

| check | before | after |
|---|---|---|
| `dsi_verify` vs MATLAB golden (`golden_fblms.bin`), relL2 | 2.62e-6 | 2.54e-6 |
| `dsi_verify` GPU vs CPU FP32, relL2 | 2.23e-6 | 2.01e-6 |
| `test_fblms` GPU vs CPU-FP32 (noise), relL2 | | 1.95e-07 (bound 1e-3) |
| `test_fblms` GPU known clutter | | 18.6 dB first CPI, 47.5 dB second |
| `test_fblms` bit-identical fresh cancellers | | ok |

`make test`: 16 of 16 suites pass. Two runs of `dsi_verify` against the golden
gave identical figures, and the in-test bit-identity check passes, so repeated
runs are bit-for-bit identical (no atomics, fixed order).

## Rounding

The in-loop FFTs are no longer cuFFT's, and FP32 radix-2 sums in a different
order. The effect is small: against the golden, relL2 moved from 2.62e-6 to
2.54e-6, and against the CPU FP32 twin from 2.23e-6 to 2.01e-6. Both are far
inside the 1e-4 tolerance. The reference-block FFT is still cuFFT.

## Decisions

- The brief said to refuse anything but 2N = 256. `test_fblms` runs the GPU
  with 16, 32 and 64 taps, so a refusal would fail it. Instead `fftBlock` takes
  the point count at run time (any power of two, M <= 1024, one thread per
  point, shared arrays sized for 1024); the host fills only M/2 twiddles. The
  check in `setUp` refuses 2N > 1024. The configured case (N = 128) is the same
  code path as the plan.
- Idle half of the threads in each butterfly stage: simplest correct form.
- Used `__brev`, `__ffs` for the bit reversal and log2.
