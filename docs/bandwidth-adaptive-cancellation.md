# Tailoring the canceller to the broadcast's bandwidth

Written 2026-10-08. Three ways to make the DSI cancellation adapt to the
FM programme's instantaneous bandwidth, so that narrow-bandwidth seconds
(speech, pauses) stop producing the messy maps seen in the S0_long study
(`results/automated_tests/FINDINGS.md`, `docs/canceller-comparison-plan.md`).
None of this is implemented. Every item is central code (DSI) and takes the
propose / approve / write / walk-through process of CLAUDE.md.

The per-CPI bandwidth measure that all three need as their x-axis is
implemented (Section 0).

## The problem in one paragraph

The canceller models the surveillance clutter as a sum of delayed copies of
the reference and solves a least-squares problem for the weight of each
delay. The matrix of that problem (the Gram, K x K, K = 120 taps) is built
from the reference's autocorrelation at lags 0 to K - 1. A wideband reference
has an autocorrelation that drops to nothing after a few lags, so the
delayed copies are distinct and the Gram is well conditioned. A narrowband
reference (a quiet passage is nearly a sine wave) has an autocorrelation
that barely drops, so neighbouring delayed copies look the same, the Gram
has eigenvalues spanning five orders of magnitude (condition number 1.8e5
measured on S0_long CPI 0), and the solution is dominated by whichever
direction is least determined. The symptoms we have seen are all this:
ECA-B's FP32 Cholesky reporting "not positive definite" on 106 of 600 CPIs
at loading 1e-6, ECA-LD's Toeplitz approximation turning indefinite, and
ridge residue that comes and goes with the programme.

Regularisation is the general cure: do not try to fit the ill-determined
directions. Options 1 and 3 regularise more when the second is bad and less
when it is good. Option 4 removes the badness at the source by flattening
the reference's spectrum before anything else sees it.

## 0. The bandwidth measure (implemented 2026-10-08)

`spectrumRmsBandwidthHz()` in `src/ard/Spectrum.cpp`, computed on the
reference channel of every CPI from the same Welch spectrum the viewer
shows (512 bins, 400 Hz per bin at 204.8 kHz, Hann, every tenth segment).

1. Convert the dB bins to linear power.
2. Floor = mean of the quietest tenth of the bins. Subtract it and clip at
   zero, so the noise floor across the whole 204.8 kHz does not count as
   signal (uniform noise alone would read fs / sqrt(12) = 59 kHz).
3. Centroid f0 = sum(w f) / sum(w) and RMS width
   B = sqrt( sum(w (f - f0)^2) / sum(w) ), one-sided, in Hz.

A flat band of two-sided width W reads W / sqrt(12), so a full 180 kHz FM
channel reads about 52 kHz and a nearly unmodulated carrier reads a few
hundred hertz. The RMS width is the right measure here because it is the
curvature of the autocorrelation at zero lag (the Gabor bandwidth): it sets
how fast the autocorrelation falls away from lag zero, which is exactly
what the Gram's conditioning depends on.

It is in the telemetry CSV as `refBwHz` and `refCentreHz`, in the status
JSON under the same names, in the viewer header, and as two black dots per
row on the before-processing spectrogram at f0 - B and f0 + B.

First use: plot suppression, false plots per CPI and the ECA-B Cholesky
failures against `refBwHz` over the S0_long run. If they line up, the
explanation above becomes a measured result for the report, and the
thresholds for Option 1 can be read off the plot.

**Result (2026-10-08, `results/bandwidth_detections_tests/README.md`):**
they do not line up the expected way. False plots grow with bandwidth
(correlation 0.36 to 0.45 for every canceller), and the whole effect is in
the off-ridge sidelobe plots; the ridge plots do not track bandwidth. ECA-B
at 1e-6 had no Cholesky failure in 600 CPIs once the Gram came from the
shift recurrence (the study's 106 failures were cherk's FP32 accumulation).
So on this capture the conditioning problem is not what makes the maps
messy; the target's sidelobes are, and they widen with bandwidth. That
favours Option 4 (and the CLEAN step) over Option 1.

## 1. Adaptive diagonal loading (ECA-B, ECA-S, ECA-LD)

### What loading does

Diagonal loading adds lambda to every diagonal entry of the Gram before the
solve, so the problem becomes "fit the clutter, but keep the weights small":
minimise |s - R w|^2 + lambda |w|^2. In the Gram's eigen-directions, a
direction with eigenvalue sigma_k gets its weight scaled by
sigma_k / (sigma_k + lambda). Strong directions (sigma_k >> lambda) are
untouched; weak ones are suppressed. The cost is bias: the clutter in the
suppressed directions is not cancelled, and shows up as residue on the
zero-Doppler ridge. That is the "one ridge plot per CPI" that 1e-4 costs in
the study. Today lambda is one fixed fraction of the mean diagonal
(`ECA_DIAGONAL_LOADING`, 1e-6; `addLoadingKernel`, `src/dsi/gpu/EcaGpu.cu`),
chosen once at `init()` and the same for every CPI.

The trade-off is per second of broadcast: a wideband second needs almost no
loading and loses nothing with 1e-6; a narrowband second needs 1e-4 or more
and the ridge cost is the price of getting any answer at all. One number
for both is wrong for one of them.

### Design A: retry on failure (the minimum)

When `cusolverDnCpotrfBatched` reports a window that is not positive
definite, the whole CPI currently returns an error and a blank frame goes
out. Instead:

```
for attempt in 0 .. 3:
    build the Gram and right-hand side          (buildNormalEquations, shareChunks)
    add loading  lambda = lambda0 * 10^attempt  (addLoading)
    Cholesky                                    (solve)
    if every window factorised: break
record attempt in DSIStats (new field uLoadingRetries)
```

The Gram must be rebuilt because the factorisation overwrites it in place;
with the shift recurrence that is 50 ms for ECA-B, paid only on the failing
CPIs (18% on S0_long at 1e-6). Alternative: keep a copy of the Gram (K x K x
windows x 8 B = 4.6 MB for ECA-B's 40 windows, 46 MB for ECA-S's 400) and
reload it; faster but more memory, and ECA-S never fails, so the rebuild
route is enough. Only the failing windows need the higher loading in
principle, but giving the whole CPI one lambda keeps the semantics simple
("this CPI used 1e-5") and matches the CPU twin, which does the same loop.

What it changes in the comparison: loading becomes a per-CPI quantity. The
report gives the distribution (fraction of CPIs at 1e-6, 1e-5, 1e-4) next
to the suppression, which is a more honest number than one loading for all.
ECA-S, which never fails, is unaffected; ECA-LD gets the same loop around
its Levinson recursion (its degenerate-batch guard is the equivalent of the
Cholesky failure).

Verification: the S0_clean golden never triggers the loop, so it is
untouched; a new test in `test_eca.cpp` with a near-singular reference (a
pure tone) must take at least one retry and still cancel; the CPU twin must
pick the same attempt count on the capture.

Effort 2 to 3 hours. Limitation: it reacts only to outright failure. A CPI
that factorises but is badly conditioned gets no help and still leaves
residue.

### Design B: loading from the measured bandwidth

Once Section 0's plot exists, pick lambda per CPI from `refBwHz` before
the solve: a two- or three-step table (for example 1e-6 above 30 kHz, 1e-5
between 10 and 30, 1e-4 below 10), with the thresholds read off the plot of
Cholesky failures against bandwidth. This needs a per-CPI setter on the
canceller (`setLoading()` ahead of `cancel()`, applied in `addLoading`),
which is small, and Design A as the safety net underneath it. Do B only if
A's retries cluster at low bandwidth, which the telemetry will show.

## 3. CGLS with an adaptive stop

### Why CGLS already tolerates bad seconds

CGLS builds the solution one Krylov direction per iteration, and those
directions arrive in order of the Gram's strong eigen-directions first.
Stopping after a fixed number of iterations is therefore a regulariser of
the same family as loading: a spectral filter that leaves the weak
directions unfitted (semiconvergence). This is why primed CGLS does not
fail on the seconds that break ECA-B. The fixed count of 30
(`CGLS_ITERATIONS`, tuned on S0_clean in `scripts/cgls_tuning/`) is the
knob, and it is one number for every CPI, with the same objection as one
loading for every CPI: on a wideband second 30 may stop before the strong
directions are fully fitted (residue), on a narrowband second 30 may run
into the weak directions (a wider notch and ridge residue, which the study
measured as low-Doppler Pd 0.67 to 0.75 and the second-most ridge plots).

### The stopping rule

Both backends already compute, every iteration, gamma_k = |R^H r_k|^2, the
squared norm of the gradient of the least-squares cost (`reduceKernel` in
`src/dsi/gpu/CglsGpu.cu`; `CglsCpu.cpp` line 268). They do not track the
residual norm itself, but it is free: with the step length
alpha_k = gamma_k / delta_k (delta_k = |R p_k|^2, also computed), the
residual norm obeys

    |r_{k+1}|^2 = |r_k|^2 - alpha_k * gamma_k

exactly in exact arithmetic (r_k^H R p_k = gamma_k because p_k = s_k +
beta p_{k-1} and s_k is orthogonal to p_{k-1}). So the energy removed by
iteration k is alpha_k gamma_k, and the rule is:

    stop when  alpha_k gamma_k  <  tau * |r_0|^2      (the step no longer
                                                        buys anything)
    or when    k == CGLS_ITERATIONS                   (the existing cap)

|r_0|^2 is the surveillance power in the window (computed once; it is the
suppression denominator anyway). tau is a relative tolerance, something like
1e-4 (a step that removes less than a ten-thousandth of the input power is
fitting noise or ill-determined directions). The number of iterations
actually taken is already reported (`oStats.uIterations`), so the telemetry
shows it per CPI with no further change.

Why this rule and not a residual threshold: the residual's final level is
the uncancelled target plus noise plus whatever the model cannot represent,
which varies by CPI, so "stop at residual < X" needs a noise estimate. The
decrease-per-step rule needs nothing and is the one the fixed count
approximates on the tuning capture.

Per-window stopping already exists (the `d_bStopped[w]` flag when delta is
not positive), so the new rule reuses that mechanism: a window that has
converged stops and the others carry on; the CPI's time is set by the
slowest window. Priming interacts well: a primed start is already close, so
the rule stops early on an unchanged second and runs longer when the clutter
has moved.

What it changes in the comparison: CGLS's time per CPI becomes a
distribution instead of a constant (fewer iterations on easy seconds, up to
the cap on hard ones), and so does its notch. The report gives both as
distributions against bandwidth, which is the same shape of result as
Option 1's loading distribution. Side by side, the two are the explicit
(Tikhonov) and the implicit (Krylov) forms of the same idea, which is a
clean thesis point.

Verification: the CPU twin takes the same rule; the test is that both
backends stop at the same iteration on the same window to within one (FP32
reordering can move the last step). CGLS is one of the two line-level areas
in CLAUDE.md (FP32 semiconvergence), so this goes in small increments: the
residual recurrence and its logging first, with the stop disabled; then the
stop.

Effort 3 to 4 hours plus a tuning pass for tau on S0_long.

## 4. Flattening the reference before cancelling (reciprocal filter)

### The idea

Everything that goes wrong on a narrowband second comes from the shape of
the reference's spectrum. So reshape it. Take the CPI's reference spectrum
X_ref(f) and build a zero-phase filter

    W(f) = 1 / max( |X_ref(f)|, epsilon )

and apply the same filter to both channels:

    ref' = IFFT( W(f) X_ref(f) )
    sur' = IFFT( W(f) X_sur(f) )

The filtered reference has a flat magnitude spectrum, so its
autocorrelation is a spike at lag zero and nearly nothing elsewhere,
whatever the programme was doing. The literature calls this a reciprocal
filter (the CAF then uses 1/X_ref instead of the matched filter's
conj(X_ref)); it is standard for DVB-T and DAB passive radar, where the
pilot structure gives bad sidelobes, and less used for FM because FM's
spectrum changes with time. The references I know are Searle, Palmer and
Davis on reciprocal filtering for OFDM (around 2014) and Glende's
mismatched filter for sidelobe reduction (2006); both need to be found and
read before anything is cited.

### Why it is legitimate for the canceller

The clutter model is sur(n) = sum_k h_k ref(n - k) + noise + target. W is a
linear time-invariant filter, and LTI filters commute with delays, so after
filtering both channels the same model holds with the same h_k:
sur'(n) = sum_k h_k ref'(n - k) + filtered noise + filtered target. The
canceller solves the same problem on a reference whose Gram is now close
to a multiple of the identity. The target's Doppler is unchanged (W is
time-invariant); the target's echo spectrum is X_ref(f - f_d) and is
whitened by W(f) instead of W(f - f_d), a mismatch of order f_d / B, which
for f_d below 100 Hz against tens of kHz is nothing.

### What it costs

1. Noise gain. Where |X_ref(f)| is small, W is large, and the surveillance
   noise at those frequencies is amplified. The floor epsilon caps that. The
   knob is epsilon relative to the mean magnitude of |X_ref|: a high floor
   leaves the spectrum partly unflattened (less noise gain, less
   conditioning benefit); a low floor flattens fully and costs sensitivity.
   Expect a few dB of SNR loss on narrowband seconds, which is the price of
   a clean map on those seconds; on wideband seconds the filter does nearly
   nothing and costs nearly nothing. This becomes a measured curve.
2. One spectrum per CPI is an average over the second. FM's instantaneous
   bandwidth changes inside the CPI, so a 25 ms batch's local spectrum is
   narrower than the 1 s average, and its Gram is improved but not made
   diagonal. Blockwise whitening (overlap-save with 100 ms blocks) would
   follow the programme more closely at more complexity; start with the
   full-CPI filter and measure the per-batch conditioning before deciding.
3. Compute. Two forward FFTs of 204800 points, one elementwise kernel, two
   inverse FFTs, on buffers already on the device: about 5 to 8 ms per CPI
   on the Nano. The MATLAB twin is ten lines.
4. The reference's phase is kept (W is magnitude only), so the CAF's range
   axis is still the delay axis; nothing downstream changes.

### What it would do to the comparison

This touches every result, which is why it is a thesis section rather than
a fix:

- ECA-B's Cholesky should stop failing at 1e-6, and the ridge residue from
  loading should go.
- ECA-LD's Toeplitz approximation error is still about K/L = 2.3% of the
  entries, but the smallest eigenvalue of a whitened Gram is of the order
  of the diagonal rather than 1e-4 of it, so the approximation should hold
  at 25 ms. That would make ECA-LD usable at the batch length the paper
  intends, and the ECA-B / ECA-LD pair becomes a clean exact-against-
  approximate result.
- The false plots, which sit on the FM ambiguity function's range
  sidelobes, should thin out for every canceller, because the CAF now sees
  a spike-like autocorrelation. This is the only one of the three options
  that reduces the common-mode false plots.
- The cost side is the noise gain, measured as target SNR against epsilon
  on the fly-by.

The study would be repeated with the filter on and off at the settled CFAR
(2-D kernel, Pfa 1e-9), six cancellers, on S0_long: twelve runs, two hours
of board time with the existing harness.

### Where it goes and how it is verified

A stage between CPI extraction and the canceller, on the device, taking
`d_cRef` and `d_cSur` in place: `src/dsi/gpu/WhitenGpu.cu` with a CPU twin
and `whiten.m` in `matlab/`, a config key (`REF_WHITEN_FLOOR`, 0 = off) in
both conf files, a golden file from MATLAB on S0_clean compared at 1e-4,
and a conditioning check: the Gram's condition number of CPI 0 of S0_long
before and after (MATLAB, the 1.8e5 figure is the baseline). Memory: two
complex buffers of 204800 samples already exist; the FFT workspace is the
new allocation, so this is also a memory-management change under CLAUDE.md.

Effort about one day of code, MATLAB and golden, plus the twelve-run
comparison and a reading pass for the citations.

## How the three relate, and an order

| | Fixes | Leaves | Cost | Effort |
|---|---|---|---|---|
| 1. Adaptive loading | ECA-B blank CPIs; ridge cost confined to bad seconds | Sidelobe false plots; ECA-LD at 25 ms | None on good seconds | 2-3 h |
| 3. CGLS adaptive stop | CGLS over/under-fitting per second; time on easy seconds | Sidelobe false plots | A tolerance to tune | 3-4 h |
| 4. Reciprocal filter | The conditioning itself, for every canceller; sidelobe false plots | Nothing structural | A few dB SNR on bad seconds; 5-8 ms | ~1 day + 12 runs |

Suggested order, revised after the first data (Section 0, Result):

1. Done: the bandwidth measure and seven runs against it
   (`results/bandwidth_detections_tests/`). The bandwidth story is not
   the one expected: false plots grow with bandwidth and are sidelobe
   plots; the ECA-B blank CPIs were cherk's accumulation and are gone.
2. Option 4 (or the CLEAN step) is now the option that addresses the
   measured problem, since it is the only one that lowers the sidelobes.
   It still needs the reading pass before design. The ECA-LD outcome at
   25 ms remains a reason on its own.
3. Option 1 Design A only as a safety net (a retry costs nothing when it
   never triggers); no longer a priority.
4. Option 3 as a thesis point on regularisation, not as a fix.
