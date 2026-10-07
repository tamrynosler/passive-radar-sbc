# ECA Gram matrix by shift recurrence

Date: 2026-10-07. Branch pr-gram. Board: Jetson Nano, clocks as found (not
re-pinned by me), shared with other agents' work; `pgrep -a "dsi_bench|pbr_run"`
showed nothing before and after the runs. Inputs: `data/S0_clean/cpi_0000.bin`,
`config/fers.conf` (copies in /tmp with only the flag, and for ECA-B
`ECA_UPDATE_INTERVAL_s = 0.025`, changed). 5 timed iterations after one warm-up.

## What cherk was doing

ECA builds the delayed-reference matrix R (column k is the reference delayed by
k samples) and forms G = R^H R. cuBLAS cherk does this as a general matrix
product: K^2 N / 2 complex multiply-adds per CPI (K = 120 taps, N = 204800
samples). That was about 112 ms of ECA-B's 153 ms. cherk does not know that
every column of R is the same signal shifted.

## Why the first column is enough

For a window of samples n in [n0, n1), with r[negative] = 0:

    G[i][j] = sum_n conj(r[n-i]) r[n-j]

Substitute m = n - 1. The sum for (i+1, j+1) is the sum for (i, j) with the
sample before the window added and the last sample of the window removed:

    G[i+1][j+1] = G[i][j] + conj(r[n0-1-i]) r[n0-1-j] - conj(r[n1-1-i]) r[n1-1-j]

This is exact. So every diagonal of G can be walked down and to the right from
its first entry, and those first entries are the first column
c[i] = G[i][0] = sum_n conj(r[n-i]) r[n], which is R^H r: one cgemv, the same
call that already gives the right-hand side R^H s. Only the lower triangle is
built (cholesky reads only that). One GPU thread walks each diagonal, at most
K-1 steps of two complex products each. Work falls from K^2 N / 2 to about
K N + K^2.

ECA-S: the chunk first columns are summed over the M chunks of each estimation
window (fixed order, like the right-hand sides), and the recurrence then uses
the WINDOW's n0 and n1, since the edge terms belong to the whole window. The
per-chunk Gram arrays are not allocated at all (ECA-S memory drops by one
windows x K x K array, 115 MB at 2.5 ms updates).

## FP32 drift

A diagonal accumulates up to 119 add/subtract steps, so roughly 119 x 6e-8 =
1e-5 relative to the entry was expected. Measured (below), the result is no
worse than cherk's, and against the FP64 MATLAB goldens it is better: the
products in cherk are rounded in a K^2 N sum, the recurrence's column is a
length-N dot product and the edge terms are only a few samples.

## Accuracy (relative L2, GPU FP32; recurrence = flag 1, cherk = flag 0)

| Check | recurrence | cherk |
|---|---|---|
| ECA-m vs MATLAB golden (FP64) | 8.4e-05 (40.1839 dB) | 2.1e-04 (40.1846 dB) |
| ECA-B vs MATLAB golden (FP64) | 5.9e-04 (51.6193 dB) | 6.5e-03 (51.6191 dB) |
| ECA-B vs CPU FP32 | 7.2e-03 | 1.0e-02 |
| ECA-S vs CPU FP32 | 7.1e-03 | 7.3e-03 |

Suppression matches to 0.001 dB in every row. The GPU-vs-CPU rows on the real
CPI are dominated by the CPU's own FP32 rounding amplified by 50 dB of
suppression (as the header of EcaGpu.cu explains). The structural check on
uncorrelated noise (tests/test_eca.cpp, GPU vs CPU-FP32) was run for both paths:

| Case | recurrence | cherk |
|---|---|---|
| ECA-B | 2.4e-07 | 3.4e-07 |
| ECA-S | 2.4e-07 | 2.5e-07 |
| ECA-m | 1.6e-07 | 2.5e-07 |

## Time per CPI (median of 5, ms)

| Algorithm | before (baseline) | cherk, this branch | recurrence |
|---|---|---|---|
| ECA-B | 153.25 | 154.85 | 55.48 |
| ECA-S | 224.91 | 227.11 | 120.44 |

ECA-B is 2.8x faster, ECA-S 1.9x. What is left is mostly buildSubspaceKernel,
the two cgemv calls (one now over the reference) and, for ECA-S, 1600 chunk
slices of small cuBLAS calls. ECA-m was not timed.

## The key, and why both paths stay

`ECA_GRAM_RECURRENCE` (0 or 1, required, 1 in config/prb.conf and
config/fers.conf, a yes/no row in the viewer for ECA-m, ECA-B and ECA-S). 1 is
the recurrence, 0 is cherk. Both are kept so the thesis can show the two rows
side by side and so cherk remains the plain library reference to check the
recurrence against. The CPU canceller ignores the flag. With 0 the code runs
exactly as before.
