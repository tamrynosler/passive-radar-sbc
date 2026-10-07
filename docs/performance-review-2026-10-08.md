# Processing-speed review, 2026-10-08

A read-through of the chain for places where processing time can be cut,
with the measured per-stage times of the 34-run study in
`results/automated_tests/` (Jetson GPU, clocks pinned from the second run,
600 CPIs each, medians) as the evidence. Nothing here was changed; this is
the list to pick from. Every item in Sections 2 and 3 is central code (DSI,
CAF, CFAR, memory) and takes the propose / approve / write / walk-through
process of CLAUDE.md.

## 1. Where the time goes

| Stage | ms per 1 s CPI | Note |
|---|---|---|
| DSI, ECA-CD | 12.1 | three cuFFT passes, ~9 launches: the floor |
| DSI, ECA-LD | 26.0 | ~14 ms of it is the shared `convolveKernel` |
| DSI, ECA-B | 161 | ~140 ms is the Gram (`cherk`), 40 windows |
| DSI, ECA-S | 231 | 400 chunk Grams, ~1200 launches, 400 Cholesky solves |
| DSI, FBLMS | 362 | 1600 sequential blocks x 9 launches: launch-bound |
| DSI, CGLS | 487 | 30 iterations x 16 ms; 4 windows = 4 blocks on 1 SM |
| ARD (map + CFAR), 1-D kernel | 12.7 | ~7-8 ms lag product, ~1 ms FFT, ~0.7 ms map copy, **CFAR ~0.3 ms** |
| ARD, 2-D kernel (1x4 / 2x8) | 16.5 | **the 2-D CFAR adds 3.8 ms**, the same for every canceller |
| between CPIs (file read, decode, ring, extract, CSV) | 9.2 | 7.7 us per datagram; 13-16 ms when flushing large CSVs |
| frame serialise + stream + spectrum | not measured | `ardWriteMs` was 0 in every run (streaming off); estimate below |

`totalMs` is `dsiMs + ardMs` by construction (`processCpi`), so the host
does no work inside the timed region. The detection count does not move
`ardMs` (slope -0.001 ms per detection): grouping, serialising and the
detection copy are noise. Every algorithm is inside the 1 s budget (CGLS,
the worst, uses 49%), so none of this is about keeping up; it is about the
benchmark rows, headroom for an overlapping hop, and the board's heat.

Nothing in `src/` uses pinned host memory, `cudaMemcpyAsync` or streams:
every copy is synchronous on the default stream (3.3 MB up, 1.5 MB down per
CPI, ~2 ms in all).

## 2. The CFAR question

The CFAR kernel (`src/ard/gpu/ArdGpu.cu`, `cfarKernel`) gives one thread
per cell and walks the window directly from global memory, recomputing the
training sum for every cell: 16 reads per cell for the config's 1-D window
(0x4 / 0x8), 148 for the 2-D window (1x4 / 2x8). The reads are coalesced
(adjacent threads are adjacent range bins). Detections are compacted on
the device and only the written records come back.

- With the 1-D window the CFAR is about 0.3 ms of the 12.7 ms ARD stage.
  A summed-area table cannot beat 16 reads by enough to notice.
- With the 2-D window it is about 4 ms, and that is where a faster
  formulation pays: separable prefix sums along Doppler (one scan of the
  1600-long columns, then 7 range rows x 2 reads minus the guard block,
  ~20 reads per cell) or a full summed-area table (one 2-D scan, then 8
  reads per cell). Either brings the 2-D CFAR to ~1 ms: **saving ~3 ms per
  CPI, only when the 2-D kernel is used.** The full SAT accumulates
  192,000 cells of power in FP32, which puts its rounding at about 1e-4 of
  a 150-cell window sum next to a strong peak, borderline against the
  golden tolerance; the separable form (1600-long sums) is safe. Keep the
  zero-padded edges and the full training count as the divisor, as the
  CPU reference and MATLAB do. Effort 3-4 h, low-medium risk, golden
  check required.

So: yes, there is a faster CFAR, and it is worth doing now that the 2-D
kernel at Pfa 1e-9 is the recommended setting, but it is a 3 ms item. The
ARD stage's time is the Direct CAF's lag product, and the cheaper change
there is `ARD_ALGORITHM = Batches`: the batches route already exists, is
golden-verified, and measured 11 ms against 15.5-17.5 ms unpinned
(`docs/design_decisions.md` 1e.8). **~4.5 ms per CPI from one config
line.**

## 3. The cancellers: ranked by expected saving

1. **FBLMS, 362 ms -> ~25 ms.** `FblmsGpu.cu` runs 1600 sequential blocks
   of 256-point work with 4 cuFFT calls and 5 kernels each: 225 us per
   block, ~20 us per launch, host-launch-bound. One persistent kernel
   (one block of 256 threads, a shared-memory radix-2 256-point FFT, the
   elementwise steps inlined, looping over the 1600 blocks) removes the
   launches: ~5 us per block. Hand-written FFT, so it must reproduce the
   cuFFT scaling and pass the golden check; the Nano has one SM, so a
   single block loses nothing. Effort 8-12 h. Cheaper fallback: fuse the
   five kernels into two (saves ~120 ms, 3 h). This is also the "fused
   kernel" row already listed in `design_decisions.md` 1d.11.
2. **CGLS, 487 ms -> ~200-280 ms.** Two causes. (a) Occupancy: 4 windows of
   0.25 s give 4 blocks per launch, and the adjoint kernel has only K = 120
   active threads, so ~15 of 64 warps are resident and each thread runs a
   51,200-step serial loop: ~25 GFLOPS against a 236 GFLOPS peak. Split
   each window into ~16 segments with a partial-sum reduction. (b) The
   shared `convolveKernel` (item 4). Measure first: there is no per-kernel
   profile in the repo; one `nvprof` run of `dsi_bench` for CGLS and ECA-B
   (1 h, needs sudo) sizes (a) against (b) before anyone spends a day on
   it. Effort 6-10 h, medium risk (reduction order changes; CPU-twin
   check at the existing tolerance).
3. **ECA-B 161 -> ~35 ms and ECA-S 231 -> ~100 ms: the exact Toeplitz
   recurrence for the Gram.** The Gram of delayed copies of one sequence
   obeys G[i+1,j+1] = G[i,j] + conj(r[-1-i]) r[-1-j] - conj(r[L-1-i])
   r[L-1-j] exactly, so only the first column (120 lag correlations, which
   ECA-LD already computes by batched FFT) plus a per-diagonal recurrence
   is needed instead of `cherk` over K x K x L. This is the "exact 60x
   saving" deferred in `design_decisions.md` 1d.7, and it is exact, unlike
   ECA-LD's Toeplitz approximation. FP32 drift along a 120-step diagonal
   must be checked against the golden file. For ECA-S the chunk Grams and
   `accumulateWindowsKernel` go away too; the 400 batched Cholesky solves
   remain and are unmeasured, which bounds the ECA-S figure. Effort
   8-12 h, low-medium risk. **Methodology note:** this changes what the
   ECA-B row measures. Keep the `cherk` build selectable and report both
   ("ECA-B, direct Gram" and "ECA-B, recurrence"), or the thesis loses its
   like-for-like against the literature's cost model.
4. **Shared `convolveKernel` (`DsiGpu.cuh`): ~10 -> ~3 ms per call.** One
   thread per output sample, 120 taps, two loads per MAC, no tiling. Four
   outputs per thread with the weights and a reference tile in shared
   memory, `const __restrict__` for the read-only cache. Used by ECA-B,
   ECA-S, ECA-LD (26 -> ~20 ms) and 35 times per CPI by CGLS. Effort 3-4 h,
   low risk. Estimate comes from ECA-LD (26 ms) minus ECA-CD (12 ms), which
   differ by one convolve plus small kernels.
5. **ECA-S launch count** (if item 3 is not done): fuse the 400 x
   (build, cherk, cgemv) triples; ~25-35 ms at ~25 us per launch. 4 h.
6. **Pinned host buffers and async copies** (`vcRef`, `vcSur`, `vcMap`,
   `cudaHostAlloc` + `cudaMemcpyAsync`): ~1.5-2 ms per CPI. Memory
   management, so central. Not zero-copy mapped memory: on Tegra the GPU
   reads it uncached and the kernels reread the reference ~120 times.
   2-3 h.
7. **ARD round trips** (`runCfar`): the `cublasIsamax` peak, its 4-byte
   copy, the counters copy and the detection copy are four synchronous
   trips; ~0.3 ms. Low priority.

## 4. The host side

Not a bottleneck, but three items matter when streaming to the viewer,
because they sit between one CPI and the next and are not in the telemetry
(`ardWriteMs` was 0 in the study):

1. **The before-processing spectrum** (`src/ard/Spectrum.cpp`): 2 channels
   x 400 segments x 512-point hand-rolled FFTs in `std::complex<float>` on
   the loop thread, estimated 20-55 ms per CPI. Average every 10th segment
   (40 Welch segments is plenty for a 512-bin display) or batch it through
   cuFFT on the buffers already on the device. Display only, not
   golden-checked, peripheral. ~20-50 ms, 2 h. Measure it first with a
   clock around the two `compute()` calls.
2. **Frame stream** (`ArdStream.cpp`): 1.54 MB per frame as ~1130 `sendto`
   calls of 1368 B, back to back. `sendmmsg` or `ARD_SNAPSHOT_EVERY = 2`
   halves it; ~3-6 ms. Peripheral, 2 h.
3. **Frame file write** (`ARD_WRITE_FILES = 1`, as `config/fers.conf` now
   has it): 1.54 MB `fwrite` per CPI, 5-20 ms on the SD card with
   occasional `fclose` stalls. Write every Nth or hand to a writer thread.
   Peripheral, 1 h.

Smaller: the receive loop spins on `recvfrom` + `poll(0)` at 100% of a
core while idle (use `poll` with a 5-10 ms timeout over both sockets, or
`recvmmsg`; frees a core, ~2-5 ms); `setvbuf` 64 KB on the three CSV files
removes the 10-16 ms p99 flush spikes; the HTTP first-byte timeout can
hold the loop for 250 ms once per idle browser connection (serve only when
bytes are already readable, or move HTTP to a thread). The ring buffer and
extraction are two `memcpy`s per channel and need nothing.

**Correctness under load, checked:** the loop is single-threaded, so
nothing is received while a CPI is processed; the 4 MB socket buffer holds
2.4 s of input against a worst tail of ~1.3 s (CGLS 0.5 s + a 0.7 s
rebuild), and the 2.0 s ring (`BUFFER_LENGTH_s`) holds the window plus
0.5 s of backlog. A rebuild and a CGLS CPI back to back would overtake one
CPI (counted, not lost); `BUFFER_LENGTH_s = 2.5` removes even that. No
skipped CPIs in any of the 34 runs.

## 5. Suggested order

1. Config only, today: `ARD_ALGORITHM = Batches` (-4.5 ms), `BUFFER_LENGTH_s
   = 2.5`, `ARD_WRITE_FILES = 0` unless frames are wanted.
2. Measure before building: `nvprof` on `dsi_bench` for CGLS, ECA-B and
   ECA-S (1 h); a clock around the spectrum and the frame send in a
   streaming run (30 min). The two biggest estimates (CGLS split, spectrum)
   rest on these.
3. Peripheral, no approval needed: spectrum decimation, CSV buffers,
   `sendmmsg`, the poll loop (1 day in all).
4. Central, in value order, each proposed first: the separable 2-D CFAR
   (3 ms, 4 h); the convolve kernel (shared by four cancellers, 4 h);
   FBLMS persistent kernel (330 ms, 1-2 days); CGLS segmentation (200+ ms,
   1-2 days); the ECA Gram recurrence (120-140 ms, 1-2 days, with the
   methodology decision above).

The benchmark rows should be regenerated after any central change, with
pinned clocks and `tegrastats`, per `docs/benchmarking-methodology.md`.
