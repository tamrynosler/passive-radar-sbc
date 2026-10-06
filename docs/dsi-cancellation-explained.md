# DSI Cancellation, Explained From Zero

A plain-English companion to `src/dsi/`. It explains what the code does and
why, assuming no prior knowledge of DSI cancellation, and then traces the
program flow exactly. `docs/design_decisions.md` §1d records the *decisions*;
this document teaches the *mechanism*. Written 2026-09-17, when the code was
ECA-m and ECA-B on the CPU; the trace refers to `src/dsi/cpu/EcaCpu.cpp` and
`bench/dsi_bench.cpp`.

---

## 1. The physical problem

You have two antennas. The **reference** antenna points at an FM radio
transmitter and hears it loudly and clearly. The **surveillance** antenna
points away, at the sky, hoping to hear a faint echo bouncing off an aircraft.

The problem is that the surveillance antenna still hears the transmitter.
Three ways:

- **directly** — it leaks around the back of the antenna
- **off buildings, hills, water towers** — stationary things that reflect
- and, somewhere underneath all that, **off the aircraft you actually want**

The direct signal and the building reflections are roughly a million times
stronger than the aircraft echo. Looking for the aircraft without removing
them is like trying to hear a whisper next to a jackhammer. That interference
is what "DSI" means — direct signal interference — and removing it is what
`src/dsi/` does.

## 2. The key insight

Every unwanted thing in the surveillance channel is **a delayed copy of what
the reference antenna heard.**

A reflection off a building 15 km away is just the reference signal, arriving
a bit later and a bit quieter. Radio travels at 300,000 km/s, so 15 km of
extra path is 50 microseconds of delay. At our sampling rate of 204,800
samples per second, that is about 10 samples.

So: take the reference signal, delay it by 0 samples, 1 sample, 2 samples, …
up to 120 samples. Scale each delayed copy by some amount. Add them up. If you
pick the scale factors well, that sum should look almost exactly like the
interference in the surveillance channel — and you can subtract it.

120 taps at our sample rate covers about 176 km of bistatic range. Anything
reflecting from within that range gets cancelled.

In one line, where `r` is reference, `s` is surveillance and `w` are the scale
factors we need to find:

```
out[n] = s[n] − ( w[0]·r[n] + w[1]·r[n−1] + w[2]·r[n−2] + … + w[119]·r[n−119] )
```

The aircraft survives because a moving target shifts the signal's frequency
(Doppler). A delayed copy of the reference can imitate a stationary reflector
perfectly, but it cannot imitate something moving. So the subtraction removes
the clutter and leaves the target behind.

## 3. Finding the weights

We want the `w` values that leave the least energy behind. That is a
**least-squares** problem, and it has a closed-form answer. Two ingredients:

**The Gram matrix, `G`** (120 × 120). Entry `G[i][j]` answers: *how similar is
the reference delayed by i to the reference delayed by j?* It is a table of
the signal compared against shifted versions of itself.

**The right-hand side, `v`** (120 long). Entry `v[i]` answers: *how similar is
the reference delayed by i to the surveillance signal?* This is effectively a
map of where the clutter is.

"How similar" means: multiply the two signals sample by sample and add up the
results, conjugating the first one. Conjugating flips the sign of the phase,
so the product measures both how big the overlap is and how the phases line
up. If two signals are identical, every product adds up in the same direction
and you get a big number. If they are unrelated, the products point every
which way and cancel to near zero.

Then the answer to the whole problem is the solution of:

```
G · w = v
```

A 120×120 system of equations. That is the entire algorithm.

## 4. Where ECA-m and ECA-B differ

**ECA-m** does the above once, using all 204,800 samples of the one-second
CPI. One set of weights for the whole second. It is defined as "whatever the
MATLAB reference chain does" and exists to be the golden-file baseline.

**ECA-B** chops the second into 40 fragments of 25 ms each and does the whole
thing separately in every fragment. 40 Gram matrices, 40 solves, 40 sets of
weights.

Why bother? Because the clutter *changes* during a second. Trees move. The
transmitter drifts. A single set of weights averaged over the whole second is
a compromise that fits no individual moment well. Per-fragment weights track
the change.

On our real captured FERS data this is worth **11.4 dB** — ECA-m gets
40.18 dB of suppression, ECA-B gets 51.62 dB (see design_decisions.md §1d.6).

The price of batching: the independent per-batch weights are themselves a
modulation, which puts false peaks at 1/T_B spacing in Doppler around any
target inside the cancellation notch (Colone et al., TAES 52(3) 2016, §III).
At 25 ms batches that is every 40 Hz, inside our ±200 Hz display. They are a
property of the algorithm, not a bug — and their presence at exactly 1/T_B is
a physical correctness check the golden file cannot provide.

## 5. The exact program trace

### Starting up — `bench/dsi_bench.cpp`

1. Parse the command line. `--cpi data/S0_clean/cpi_0000.bin` is required
   (written by `pbr_run --dump-cpi`).
2. `loadPbrConfig()` reads `config/prb.conf`. Any missing key is fatal — no
   silent defaults.
3. If `--algorithm` or `--backend` was given, overwrite those two config
   values. This happens *before* anything else, so the printout always
   matches what actually ran.
4. `dsiParamsFromConfig()` turns the config into a `DSIParams`. For ECA-B it
   copies the window lengths straight across: 5120 samples estimate, 5120
   samples update. **For ECA-m it ignores them** and forces both to 204800,
   and switches the diagonal loading to MATLAB's absolute `1e-3`.
5. `readCpiFileHeader()` checks the frozen CPI file's magic bytes and sample
   count against the config. A mismatch stops the run rather than
   benchmarking the wrong data.
6. `readCpiFile()` loads 204,800 reference samples and 204,800 surveillance
   samples.
7. `makeDSICanceller(EcaB, Cpu)` returns an `EcaCpu` object.
8. `init()` validates everything and allocates: the Gram (120×120), a copy
   for the Cholesky factor, the right-hand side and the weights. **Nothing
   allocates after this point** — otherwise the benchmark would be timing the
   memory allocator.
9. One **warm-up** call to `cancel()`, untimed. The first call touches memory
   pages for the first time and fills cold caches; timing it would report a
   number that never happens again.
10. Then `--iters` timed calls, with a clock around `cancel()` and nothing
    else.

### Inside `EcaCpu::cancel()`

Checks the buffers are not null and that the output does not overlap either
input (the original surveillance signal is still needed at the end to measure
suppression). Then loops over the 40 windows. Window `w` covers samples
`w × 5120` to `w × 5120 + 5119`.

For each window, three steps.

### Step 1 — `buildNormalEquations()`

Two nested loops over `i` and `j`, from 0 to 119, with `j` starting at `i`.
That is 7,260 pairs rather than 14,400, because `G` is symmetric in a
mirror-image way (`G[j][i]` is `G[i][j]` with its phase flipped), so half the
table comes free.

For each pair, an inner loop runs across every sample in the window,
multiplying `conj(r[n−i])` by `r[n−j]` and adding it up.

Two details worth pointing at:

- **The `uFirst` clamp.** The sum cannot start until both delayed copies have
  real samples underneath them. In the very first window, `r[n−119]` does not
  exist until `n` reaches 119, so the sum starts there. That is exactly what
  MATLAB's leading zero-padding of each column amounts to. In later windows
  it starts at the window boundary — and `r[5120−119]` is real data from the
  *previous* window, which is correct and free.
- **The accumulator is `double`** while the samples are `float`. We are
  adding up 204,800 products; in single precision, rounding errors pile up
  and eat about four decimal digits — which alone would fail the 1e-4
  agreement with MATLAB. The GPU has no fast double at all; `EcaGpu.cu`
  explains why it gets away with float.

Then a much shorter loop builds `v`: 120 sums instead of 7,260.

**One thing that is deliberately absent: the matrix `R`.** MATLAB physically
builds a 204800 × 120 table of delayed copies. That would be 197 MB — most of
the Jetson's memory — to produce something we only ever need as a 115 KB
summary. So each Gram entry is computed directly from two shifted slices of
the reference. Same arithmetic, 1700× less memory.

### Step 2 — `solveWeights()`

First, **diagonal loading** (`loadingLambda()`). The diagonal of `G` holds the reference signal's
energy under each delay, so its average is the natural scale of the whole
matrix. We add a tiny fraction of that (1e-6) to each diagonal entry. This is
insurance: if the matrix is nearly singular, the solution can blow up into
huge weights that cancel the clutter beautifully and destroy the targets with
it. A small nudge prevents that.

Expressing it as a *fraction* rather than a fixed number is the point — it
then means the same thing whatever amplitude the samples happen to arrive at.
ECA-m is the exception and uses MATLAB's fixed `1e-3`, because reproducing
that chain exactly is its only job.

Then **Cholesky decomposition**. This factors `G` into `L × L^H`, where `L`
is triangular — all zeros above the diagonal. Triangular systems are trivial
to solve by substitution, so this turns one hard problem into two easy ones.
It only works on matrices of this specific well-behaved type (Hermitian
positive definite), which `G` is, and it is about twice as fast as the
general method.

If a diagonal value comes out zero or negative, the matrix is not
well-behaved after all, and the code stops and says so. The usual cause is an
all-zero window from a dropped packet — no signal means no clutter to cancel
and no way to solve for anything.

Then **forward substitution** solves `L·y = v` top-down, and **back
substitution** solves `L^H·w = y` bottom-up. Out comes `w`: 120 complex
weights.

All of this is about 576,000 operations — completely negligible next to the
37 million that built the Gram.

### Step 3 — `applyWeights()`

For each of the 5,120 samples in the window, add up the 120 weighted delayed
reference values and subtract from the surveillance sample. Write to the
output buffer.

The `uTaps` clamp handles the same edge as before: near sample 0, taps that
would reach before the start of the CPI contribute nothing.

### After all 40 windows

Sum the power of the original surveillance signal, sum the power of the
output, and report the ratio in decibels (`hostSuppressionDb()`, shared by
the three CPU cancellers).

### Where the time goes

Per 5120-sample window, roughly:

| Step | Operations | Share |
|---|---|---|
| Build `G` | 7,260 sums × 5,120 samples ≈ 37 M | ~97% |
| Build `v` | 120 × 5,120 ≈ 0.6 M | ~2% |
| Cholesky + substitutions | ≈ 0.6 M | ~1.5% |
| Apply and subtract | 5,120 × 120 ≈ 0.6 M | ~2% |

The Gram build is essentially the whole cost. Any speed-up effort that does
not attack the Gram is attacking the wrong thing. This also explains why
ECA-m and ECA-B take identical time: 40 windows of 5,120 samples sum to the
same total work as 1 window of 204,800.

## 6. Reading the result honestly

**51.62 dB means the cancellation removed about 99.999% of the energy.**
Colone's FM measurements put the theoretical ceiling around 52 dB, so this is
essentially as good as the method gets.

But **suppression alone is a misleading score.** An algorithm scores well on
it whether it removed the clutter or removed the targets. Laptev et al.
(DSPA 2026) measured exactly that failure: ECA-B scoring well on suppression
while attenuating a slow-moving target by 21 dB. Measuring how much of the
target survives needs the range-Doppler map. Until the CAF exists, treat a
suppression figure as encouraging rather than conclusive.

And **host timings prove nothing about the Jetson.** They say the code runs
and give the order of magnitude. Citable figures come from the Jetson with
clocks pinned (`nvpmodel -m 0 && jetson_clocks`) and tegrastats logged.

## 7. Two things measured later that this document should warn about (2026-10-02)

- **The vertical Doppler streak through every strong return is not the
  canceller.** At the matched delay the Doppler FFT sees |s(t)|^2 times the
  target's tone, so each return is convolved with the spectrum of the
  illuminator's power envelope. A band-limited recording of a real FM station
  has an envelope with programme content in it (lines at 40, 75 and 145 Hz
  in `cpi_0000.bin`), which prints a streak 25-30 dB down on the target and
  on the direct signal alike, identical for all five cancellers. The
  cancellers act only at zero Doppler. `RESULTS.md`, 2026-10-02 afternoon.
- **A stateful canceller must be reset after the chain's warm-up.** CGLS
  with priming keeps the weights from the warm-up's random signal and starts
  the first real CPI from them (17.8 dB instead of 42). Fix pending.

## References

- Colone, O'Hagan, Lombardo, Baker, TAES 45(2) 2009 — ECA and ECA-B.
- Colone, Palmarini, Martelli, Tilli, TAES 52(3) 2016 — ECA-S, the two-window
  formulation this implementation is built on, and the efficient
  (FFT/Toeplitz/Levinson) implementation in its §VI.
- Schüpbach, Paine, O'Hagan, RadarConf 2020 — the FM algorithm comparison
  this thesis reproduces on embedded GPU hardware.
- Laptev, Konovalov, Vorobev, DSPA 2026 — ECA-B/S/CD comparison; source of
  the target-attenuation metric.
