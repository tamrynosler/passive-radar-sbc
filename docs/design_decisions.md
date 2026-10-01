# Design Decisions

Rationale for implementation choices, kept as source material for the thesis
report. Each entry records what was decided, why, and what was rejected.

Performance figures are labelled either **derived** (calculated from published
hardware specifications and known data rates) or **measured** (observed on
hardware). DSI cancellation figures in 1d.6c ARE measured on the Jetson,
though with clocks unpinned and therefore provisional.

---

## 0. Source tree layout

**Decision (2026-09-17).** `src/` is grouped by **pipeline stage**, not by
library: `ingest/`, `dsi/`, `ard/`, `display/`, `app/`. The earlier
placeholders `dsp/`, `fft/` and `linalg/` were removed before any code landed
in them.

**Why by stage.** The thesis contribution is a comparison of DSI cancellation
algorithms, so cancellation deserves a directory of its own rather than being
one third of a `dsp/` bucket. Each directory also matches a validation
checkpoint — a stage is proven in isolation before the next one starts — so
the tree mirrors the order the work is done in.

**Why no `fft/` or `linalg/`.** Those directories would have held wrapper
classes over cuFFT and cuBLAS with no caller yet to shape them. Both libraries
are called directly from the stage that needs them: the CAF owns its cuFFT
plan, ECA-B its cuBLAS calls. A shared wrapper can be extracted later, once
two callers exist and their real requirements are known, rather than guessed.

**Why headers stay flat in `include/pbr/`.** Mirroring the `src/`
subdirectories in `include/` would spend a round of include-path churn on a
grouping that filenames already carry, and would make moving a file between
stages an API change for every caller. The directory a file is implemented in
is an internal detail; the header name is the interface.

**`display/` is reserved, not started.** The current milestone ends at "basic
range-Doppler output", so serialising the ARD map and detection list to disk
belongs with the stage that produces them, in `ard/`. `display/` exists for a
later on-screen viewer, which would be the first GUI dependency in the project
and needs a decision of its own.

---

## 1. Ingest circular buffer (`pbr::CircularBuffer`)

Files: `include/pbr/CircularBuffer.hpp`, `src/ingest/CircularBuffer.cpp`,
`tests/test_circular_buffer.cpp`.

The ingest stage receives VITA 49.2 UDP datagrams from FERS — 342 IQ samples
per datagram, 599 datagrams/s per channel at fs = 204800 Hz — and must present
the processing chain with coherent 1-second CPI windows of 204,800 samples per
channel.

### 1.1 Addressing by absolute sample index, not arrival order

**Decision.** Every sample is addressed by its absolute sample index, derived
from the packet's own VITA 49 timestamp: `index = round(timestamp_s × fs)`.
Writes specify where their data belongs; the buffer never infers position from
arrival order.

**Why.** Three properties follow directly:

1. **Out-of-order packets land correctly.** UDP does not guarantee ordering.
   Because each write carries its own index, reordering is a non-issue rather
   than something to detect and correct.
2. **The two channels align by construction.** Reference and surveillance share
   one un-anchored global timeline, so no inter-channel synchronisation step is
   required.
3. **Late packets are rejected rather than misplaced.** A packet whose samples
   have already been overwritten is refused, not written into the wrong lap.

**Rejected alternative: anchoring to the first packet seen.** The MATLAB
reference originally computed indices relative to whichever packet the receiver
happened to parse first. When that packet was not the chronologically earliest,
every packet from the other channel produced a negative index and was silently
dropped for the remainder of the run. The absolute-index design makes this class
of bug unrepresentable.

### 1.2 Written from scratch rather than using a third-party ring buffer

**Decision.** No external dependency.

**Why.** The required semantics are not those of a queue. Off-the-shelf ring
buffers (`boost::circular_buffer`, `boost::lockfree::spsc_queue`,
`rigtorp::SPSCQueue`, `cameron314::readerwriterqueue`, `jnk0le/Ring-Buffer`) all
implement FIFO push/pop. This design needs *random-access writes by absolute
index into a sliding window*, with gap detection — an operation none of those
libraries expose. Building it on top of a FIFO would mean working against the
abstraction.

Two supporting reasons: the buffer is thesis-critical code whose indexing must be
fully understood and defensible; and the flexibility that will actually be needed
later is the **memory kind** (pageable → pinned → possibly device-mapped), which
is an allocation concern that no FIFO library parameterises.

### 1.3 Concrete type, not templated

**Decision.** Stores `ComplexF` (`std::complex<float>`) only.

**Why.** Exactly one sample type exists in the pipeline. Templating would force
the implementation into the header, where the index arithmetic is recompiled per
translation unit and is harder to step through under a debugger. The plausible
future need is a different *memory kind*, not a different sample type, and a type
parameter does not address that. Converting to a template later is a mechanical
change with the test suite already in place.

### 1.4 Capacity is not constrained to a power of two

**Decision.** Capacity is `bufSeconds × fs` = 409,600 samples, matching the
MATLAB reference. It is not padded to 524,288 to permit bitmask indexing.

**Why.** The usual argument for power-of-two ring capacities — replacing `%` with
`&` — assumes the modulo runs per sample. Here it runs **once per packet**: the
slot of the first sample is computed once, and the packet body is then copied as
at most two contiguous `memcpy` spans. At 1,198 packets/s combined that is ~1,200
integer divisions per second, which is not a measurable cost (derived: ~0.002% of
one Cortex-A57 core at 1.43 GHz). Padding the capacity would have cost 28% more
memory and, more importantly, would have broken numerical parity with the MATLAB
reference's buffer length.

### 1.5 Gaps are zero-filled — a deliberate divergence from the MATLAB reference

> **Amended 2026-09-25.** Coverage is no longer judged from the zeros. Each
> slot carries a validity flag set by `write()` and cleared by the zero-fill,
> and `validCount()` gives the fraction of a window that came from packets.
> A quiet input (ADC codes of -2..2 with no antenna) was being rejected as
> 0% covered under the value test. The zero-fill stays as a second guarantee.

**Decision.** A write whose index is beyond the current write end zero-fills the
skipped span before writing, clamped to one capacity.

**Why.** Without this, a slot not rewritten on the current lap still holds the
sample written to it one full buffer-length ago. After a dropped packet, that
stale data reads back as if it were current — in radar terms, a two-second-old
echo presented as a live return.

The MATLAB reference does not zero-fill; its vectorised write "only ever touches
the samples being written." It works in practice only because packets normally
arrive contiguously and every slot is rewritten each lap. Its loss-detection
mechanism (reject a CPI window that is under 99% non-zero) cannot see the
problem, because stale data is non-zero and therefore reads as covered.

Zero-filling makes that same 99% check trustworthy rather than replacing it, so
the C++ implementation stays comparable with the reference while being correct
under packet loss. Cost is zero in the normal case: the fill span is empty unless
a gap actually occurred.

### 1.6 Reads are all-or-nothing

**Decision.** `read()` requires the entire requested window to be resident and
refuses otherwise, rather than returning a partial count.

**Why.** A silently truncated CPI produces a plausible-looking but meaningless
range–Doppler map. Feeding a partially-populated window into clutter cancellation
yields a near-zero, noise-like output indistinguishable from a genuine
no-detection frame. An explicit refusal surfaces as a skipped CPI, which is
diagnosable.

### 1.7 Separation of concerns: the buffer is scale- and role-agnostic

**Decision.** The buffer knows nothing about int16 ADC codes,
`--vita49-fullscale`, channel roles, CPIs or coverage fractions. It is purely a
window over a sample timeline. Descaling, role resolution from context-packet
JSON, and the CPI coverage check all belong to the ingest layer above it.

**Why.** It makes the component testable with no sockets, no FERS and no CUDA —
the whole suite runs in milliseconds on any host. It also keeps `fullscale`
policy out of the buffer, which matters because that value is a run-time property
of the FERS command line, not of the buffer.

### 1.8 No exceptions: `nothrow` allocation plus an `isValid()` query

**Decision.** Allocation uses `new (std::nothrow)`; construction failure is
reported through `isValid()`, which callers must check.

**Why.** The project prohibits exceptions, so a constructor has no other channel
for reporting failure. The alternative — a separate `init()` method — was rejected
as more state to get wrong for no benefit.

### 1.9 Single-threaded and non-copyable

**Decision.** No internal locking; copy constructor and assignment deleted.

**Why.** The step-1 receive loop drains available datagrams and then processes,
on one thread, so no synchronisation is required and none is paid for. Deleting
the copy operations prevents double-free of the owned allocation. If profiling
later justifies a receive thread separate from a processing thread, the change is
localised — and would be a single-producer/single-consumer problem, for which the
absolute-index design is already well suited (writers and readers touch disjoint
regions except during overwrite).

---

## 1b. Configuration file (`config/prb.conf`)

**Decision.** One `KEY = VALUE` text file, in the legacy Commensal Radar
ProcServer format, loaded into a typed struct at startup. No dependency, no
sections, `#` to end of line for comments.

**Why a file rather than compiled-in constants.** Every parameter in it is one
that has to agree with something outside the program: the `.fersxml` scenario,
the `fers-cli` command line, or the MATLAB reference chain. Compiled-in
constants drift silently. That already happened once —
`fm_passive_radar_stream.m` still carried `fullscale = 1.0` long after runs
moved to 0.005, which would make any amplitude comparison meaningless. A
config file also makes a result traceable: the file is tracked in git, so a
thesis figure can be tied to the parameters that produced it.

**Two policies that matter more than the parsing.**

*A required key that is absent is a fatal startup error, naming the key.*
Never a silent default. A silent default is exactly how a 200x amplitude error
survives unnoticed.

*An unrecognised key is ignored.* One file then serves tools that read
different subsets of it, and adding a parameter for one tool cannot break
another. Same instinct as skipping unknown VITA 49 packet types.

**Derived values are computed, never stored.** Samples per CPI, samples per
hop, buffer capacity in samples and the Doppler bin count all come from the
values they depend on, so they cannot fall out of step with them.

**Validation at load** catches three mistakes that are otherwise painful to
diagnose: `N_CLUTTER_TAPS < N_RANGE_BINS` (the ECA formulation requires at
least as many taps as bins); a `DOPPLER_DECIMATION` that does not divide the
CPI exactly, which leaves a remainder and a subtly wrong Doppler axis; and a
buffer shorter than one CPI, which makes extraction impossible rather than
merely tight.

**Rejected: making the file valid MATLAB** so both chains could read it
directly. MATLAB cannot `run()` a file that does not end in `.m`, so the
MATLAB side would need a reader either way, and the author preferred to leave
the MATLAB chain untouched.

## 1c. CPI extraction (`ChannelIngest::tryExtractCpi`)

**Decision.** CPI extraction is a method on `ChannelIngest`, not a separate
class. It already owns both channel buffers and knows the sample rate, so a
separate extractor would only pass references around. "Packets in, CPIs out"
is one job.

**One shared start pointer, not one per channel.** Both channels are addressed
on a single absolute timeline, so a single `muCpiStart` serves both and
inter-channel alignment needs no separate step. This is the payoff from the
absolute-index addressing decision in section 1.1.

**Ready requires all three of:** both channels have written past the end of the
window; neither has yet overwritten its start; and the window is at least
`MIN_CPI_COVERAGE` non-zero. The coverage test is trustworthy here in a way it
is not for the MATLAB reference, because the zero-fill of section 1.5
guarantees a gap really is zeros rather than stale samples from the previous
lap.

**An overtaken window advances by whole hops, not by snapping** to the oldest
resident index. That keeps CPI starts on a fixed lattice, so an index still
maps predictably back to a time, and the number of hops skipped is itself the
count of CPIs lost.

**Three-way return rather than a bool.** `Ready`, `NotYet` and `Skipped` are
genuinely different situations for the caller: wait, versus a window that is
gone and will not come back. Collapsing them would hide data loss.

## 1d. DSI cancellation: ECA-m and ECA-B (`pbr::DSICanceller`)

Files: `include/pbr/DSICanceller.hpp`, `src/dsi/DSICanceller.cpp`,
`src/dsi/cpu/EcaCpu.cpp`, `tests/test_eca.cpp`, `bench/dsi_bench.cpp`.

A from-zero explanation of the mechanism, with an exact program trace, is in
`docs/dsi-cancellation-explained.md`. This section records only the decisions.

### 1d.1 Three named algorithms, one implementation

Colone et al. (TAES 52(3), 2016, eq. 18–20) define ECA-S with two independent
windows: **T_A**, the stretch of signal the weights are estimated from, and
**T_S**, the stretch they are applied to. The paper then states that ECA-B is
the case T_B = T_S = T_A.

That collapses the family:

| | estimate from | apply to |
|---|---|---|
| ECA-m | whole CPI | whole CPI |
| ECA-B | batch | same batch |
| ECA-S | long, overlapping | short |

One implementation covers all three, so there is one thing to validate, one to
optimise for the GPU, and no possibility of the variants disagreeing through
separate code. ECA-m stops being a special case that happens to agree with
ECA-B and becomes a genuine cross-check on it.

It also resolves a question the earlier reading could not: ECA-S output
fragments do **not** overlap and are never blended. Only the estimation windows
overlap; each output sample is written exactly once.

### 1d.2 ECA-m is a separate algorithm, not a configuration

The MATLAB reference chain is labelled ECA-B in its comments but performs one
least-squares solve over the whole CPI with no batching. Rather than quietly
treat that as ECA-B with B = 1, it is a fifth named algorithm, ECA-m, defined
as "whatever `matlab/fm_passive_radar_process.m` does".

The gain is that ECA-m can inherit the reference's quirks without infecting the
others. Specifically the diagonal loading: MATLAB adds an **absolute** `1e-3`,
which is not scale invariant — the same number is meaningful regularisation at
one signal amplitude and nothing at all at another, and the two sides of the
golden-file comparison do not currently agree on amplitude. ECA-m keeps the
absolute value so it reproduces the reference exactly; every other algorithm
uses a loading expressed as a **fraction of the mean Gram diagonal**, which
means the same thing at any amplitude. Nothing in `matlab/` had to change.

### 1d.3 The subspace matrix is never formed

MATLAB builds the N x K matrix `R` of delayed reference copies explicitly. At
204800 samples and 120 taps that is 197 MB — most of the Jetson's shared
memory, for something only ever needed as a 120x120 Gram summary (115 KB).

Each Gram entry is instead computed directly as a dot product of two shifted
slices of the reference. The arithmetic is identical; only the storage differs.
This is what makes the GPU port viable at all on a 4 GB shared-memory board.

### 1d.4 Accumulation in double, samples in float

A Gram entry sums 204800 products. Accumulating that in FP32 loses roughly four
decimal digits, which on its own would breach the 1e-4 relative agreement the
golden-file check demands. The accumulators are therefore `std::complex<double>`
while the samples stay `ComplexF`.

**This is an open question for the GPU backend**, not a settled one. cuBLAS
accumulates in the precision it is given, and a `cherk` in FP32 would hit
exactly the error described above. Resolving it — mixed precision, splitting
the sum, or accepting a looser tolerance — is a decision the GPU port has to
make explicitly rather than discover.

### 1d.5 Not implemented is refused, not ignored

`ECA_DOPPLER_TAPS > 0`, ECA-S, ECA-CD, CGLS and the GPU backend in a build
without CUDA all fail loudly. The GPU case matters most: a silent fallback to
the CPU would produce a benchmark row labelled GPU that was measured on the
CPU, and nothing downstream could tell.

### 1d.6 Measured, 2026-09-17 (Mac host, first CPI of `fers_capture.pbrcap`)

| Algorithm | Windows | Suppression | Time per CPI |
|---|---|---|---|
| ECA-m | 1 x 1000 ms | 40.18 dB | 971 ms |
| ECA-B | 40 x 25 ms | **51.62 dB** | 971 ms |

Two things to take from this. The 11.4 dB gain from batching is the entire
justification for ECA-B, and 51.62 dB sits near the ~52 dB theoretical
cancellation Colone reports for FM. And the two times are identical, which
confirms that batching does not reduce the arithmetic — the Gram cost is
K(K+1)/2 sums over the CPI either way. Batching buys adaptivity, bounded
memory and parallelism.

These are **host** figures and are not citable. They establish order of
magnitude only; the Jetson with pinned clocks is the measurement platform.

### 1d.6b GPU backend (`src/dsi/gpu/EcaGpu.cu`), 2026-09-17

**Single precision is forced, not chosen.** The Nano's Maxwell GPU runs FP64
at 1/32 the FP32 rate: ~0.2 s per CPI in single precision against 5+ s in
double, which would be slower than the CPU. So the GPU is FP32 throughout and
ignores `DSIParams::bFloat32Math`.

**The error is in the multiplications, not the summation.** Measured on the
real CPI, against MATLAB:

| | relative L2 | suppression |
|---|---|---|
| double throughout | 8.2e-10 | 51.6193 dB |
| float Gram, sequential sum | 7.2e-3 | 51.6191 dB |
| float Gram, tree sum | 5.0e-3 | 51.6192 dB |
| **float products, double accumulate** | **4.9e-3** | 51.6192 dB |
| FP32 Gram + FP32 solve + FP32 apply | 4.9e-3 | 51.6192 dB |

Keeping the additions in double and rounding only the products still gives
4.9e-3. That rules out tree reductions, Kahan summation and double
accumulators as fixes — they all address the addition. It also means the
FP32 batched solve is free: 4.935e-3 with it against 4.951e-3 without.

**Two-tier validation.** The MATLAB bar is relaxed to 1e-2 for the GPU,
because FP32 measures ~5e-3 and no amount of care brings that to 1e-4 on this
hardware. What keeps that safe is a second, tighter test:

- **GPU vs CPU in FP32 mode, on uncorrelated signals: 3.41e-07.** With nothing
  to cancel there is no amplification, so the two backends must agree almost
  exactly, and any indexing or race bug still shows at full size.
- On the deeply-cancelled real CPI the same comparison is only good to
  ~1e-2, because cuBLAS sums in a different order and the suppression ratio
  (~380x) amplifies the difference. **That tolerance is too loose to catch an
  off-by-one**, which is precisely why the low-suppression test exists.
- Plus: bit-identical output across repeated runs (catches races), and 125 dB
  suppression of synthetic known clutter (an indexing bug collapses this).

**What runs where.** Two custom kernels of about ten lines each (build the
delayed-reference matrix; subtract the clutter). Everything heavy is a
library call: cuBLAS `cherk` for the Gram, `cgemv` for the right-hand side,
cuSOLVER `Cpotrf/CpotrsBatched` for all 40 solves at once, `scnrm2` for the
suppression figure.

The solve stays on the GPU. It is 1.5% of the arithmetic, but on the A57 that
1.5% measures ~150 ms against a 145 ms total, with the GPU idle throughout.

**Memory: ~25 MB.** R is built one chunk at a time and the buffer reused;
building it for a whole CPI at once would cost 197 MB. Chunking with
`beta = 1` accumulation is also what lets ECA-m (one window over the whole
CPI) share the same code as ECA-B.

### 1d.6c Measured on the Jetson, 2026-09-17

Single session, first CPI of `fers_capture.pbrcap`. **Clocks NOT pinned** --
provisional, not citable.

| Algorithm | Backend | Suppression | Time per CPI | vs real time |
|---|---|---|---|---|
| ECA-m | GPU | 40.1846 dB | 151.8 ms | 0.15x |
| ECA-B | GPU | 51.6191 dB | **142.1 ms** | **0.14x** |
| ECA-m | CPU | 40.1834 dB | 9719.9 ms | 9.72x |
| ECA-B | CPU | 51.6193 dB | 9632.1 ms | 9.63x |

**The GPU is 68x faster than the CPU and runs at 7x real time.** The CPU
cannot keep up at all. ECA-m and ECA-B cost the same on both backends, which
confirms again that batching buys adaptivity rather than speed.

Golden-file agreement: ECA-B (GPU) 6.5e-3, ECA-m (GPU) 2.1e-4. ECA-m is the
tighter of the two because its 40 dB of suppression amplifies the FP32 error
less than ECA-B's 51.6 dB does.

### 1d.6d Two bugs this work exposed

**The Jetson test suite was not testing anything.** CMake's Release build
defines `NDEBUG`, which turns every `assert()` in `tests/` into a no-op. The
suite ran to completion and reported 5/5 PASS having checked nothing. Fixed
with `target_compile_options(... -UNDEBUG)` on test targets; the suite's run
time went from 0.22 s to 3.40 s, which is the checks actually executing.
This predates the GPU work and applies to every test written so far.

**`make build` was deleting the recorded test data.** `rsync --delete` with
the gitignore filter removed `data/` on the Jetson on every build. Fixed with
explicit `P` (protect) filter rules for `data`, `results` and `build`.

**The test data generator was broken in two ways**, and the first diagnosis of
it was wrong -- the real causes came out only under measurement.

The LCG returned `uint32_t(state >> 33) / 2^31 - 1`: 31 bits divided by 2^31
is [0,1), so the output range was **[-1, 0), not [-1, 1)**. Every sample
carried a -0.5 DC offset. A constant offset is precisely what a clutter
canceller removes perfectly, so signals that were supposed to be
uncorrelated cancelled by 6.3 dB where theory says 0.28.

The splitmix64 replacement then had its own flaw: seeded `state = seed *
GAMMA` and stepped `+= GAMMA`, seed s+1 yields **exactly stream s offset by
one output**. Adjacent seeds produced the same signal, shifted -- and seeds
51/52 in an existing test were that case.

Both are fixed (the seed is hashed through the finaliser), and
`testRngIsUsable()` now asserts zero mean and adjacent-seed decorrelation
before any test depends on them. The GPU-vs-CPU agreement is 3.41e-07 with a
correct generator, against 1.19e-5 with the broken one.

### 1d.7 Deferred: the efficient implementation

Colone Section VI shows the Gram is Hermitian positive-definite **Toeplitz** if
the border effect is neglected (valid when K << N), which turns the whole setup
into FFTs and the solve into Levinson at ~16K². For our parameters that is
about 1.0e8 FLOPs against 1.2e10 for the direct form — roughly 100x.

A smaller, **exact** saving of about 60x is also available, since successive
Gram entries share nearly all their terms.

Neither is implemented. The direct form is the correctness reference and has to
be right before it is fast, and both are legitimate separate benchmark rows
later rather than silent optimisations now.

The exact 60x saving is partly taken up for ECA-S in §1d.9. The Toeplitz/FFT
route remains unimplemented.

### 1d.8 Benchmark isolation

Timing the canceller inside the live pipeline would measure the socket, the
packet decode and the scheduler along with it. Instead `ingest_replay
--dump-cpi` freezes one extracted CPI to a self-describing file, and
`bench/dsi_bench` loads it once, calls `init()` once, and times only `cancel()`.

Consequences: every algorithm and every run sees byte-identical input, so the
comparison is fair by construction; no allocation happens inside the timed
region, so the numbers are not measuring the allocator; the first call is
discarded as warm-up; and the harness fails if the suppression figure changes
between identical runs, which would mean the canceller is carrying state it
should not.

The same frozen CPI is what the MATLAB golden-file check will read, so both
sides start from identical samples and the unresolved `.h5` amplitude scaling
(§5.2, and `PLAN.md`) is out of the critical path entirely.

### 1d.9 ECA-S: shared chunk Grams, and the half-chunk window quantisation

ECA-S estimates each update window's weights from a longer window centred on
it, so consecutive estimation windows overlap by (T_A - T_S)/T_A -- 90% at the
shipped 25 ms / 2.5 ms. Summing each one from scratch recomputes nearly every
product M = T_A/T_S times, measured at **1460 ms per CPI against ECA-B's 155**.

The fix is to cut the CPI into chunks of T_S, compute one Gram per chunk (total
sample-work then exactly equal to ECA-B's), and build each window's Gram by
adding the M chunk Grams it spans.

**Not** Colone's recursive slide (`G_{w+1} = G_w - leaving + entering`). That
reads two chunks per window instead of M, but it serialises the windows and
accumulates add/subtract rounding across all 400 steps in FP32, which then
needs periodic re-anchoring to bound. Summing M chunks is fully parallel, has
no drift, and is if anything slightly more accurate than the direct form, since
M partial sums combined pairwise beat one serial sum over T_A. The extra reads
cost ~12 ms against a Gram of ~143 ms. If that ever matters the recursion is
the fallback.

**The consequence, accepted deliberately (author, 2026-09-21):** chunk Grams
can only be shared if every estimation window starts on a chunk boundary, so
`dsiEstimationStart()` quantises the start to a multiple of T_S. When M is
even there is no chunk boundary at the centre of the update window, so the
estimation window sits **T_S/2 off-centre** -- 256 samples in 5120 at the
shipped settings, a 5% asymmetry. M odd centres exactly. The alternative,
exact centring with two partial chunks per window, gives back most of the
saving. The asymmetry has no physical meaning: it shifts which 5% of a 25 ms
stationarity window the weights favour.

M = 1 returns the update window itself, so **ECA-m and ECA-B are unaffected**,
bit for bit. That is asserted in `tests/test_eca.cpp` rather than argued.

The CPU backend stays direct. It sums the estimation window in one go while
the GPU sums it in M pieces, so the existing GPU-vs-CPU agreement test checks
the chunk grouping independently instead of comparing an implementation with
itself.

### 1d.10 Device memory budget on the Nano, measured 2026-09-21

The chunk Grams cost `C x K^2 x 8` bytes where `C = CPI/T_S`, so the footprint
scales as 1/T_S and a mistyped T_S could ask for gigabytes. What ceiling is
defensible? Measured on the board:

| | MB |
|---|---|
| Total RAM, shared CPU+GPU | 3963 |
| OS and daemons at idle | 268 |
| Page cache (reclaimable) | 3451 |
| MemAvailable | 3611 |
| zram swap, 4 x 495 MB compressed | 1981 |
| `pbr_run` peak RSS, ECA-B **or** ECA-S | 788 |
| Largest single `cudaMalloc` that succeeds | 2988 |
| `cudaMemGetInfo` free | 234 |

Three findings that set the policy:

1. **The CUDA context dominates.** ECA-B and ECA-S measured the *same* 788 MB
   peak RSS despite a 10x difference in Gram array size. Roughly 780 MB of
   that is context and runtime; our arrays are in the noise.
2. **`cudaMemGetInfo` is useless as a budget here.** It reports 234 MB free
   while a 2988 MB allocation succeeds, because the kernel evicts page cache
   under pressure. An init-time check against it would wrongly refuse every
   configuration.
3. **There is swap, and it is the real hazard.** 1981 MB of zram means a large
   allocation does not fail, it thrashes -- which during a live run means
   missed CPIs rather than a clean error. So the guard must be a policy
   ceiling, not an out-of-memory test.

True headroom is therefore about 3963 - 268 - 788 = **2900 MB**.

The cap is set at **512 MB for the canceller's total device allocation**, which
is not a hardware limit -- anything from 256 MB to 1.5 GB would be safe -- but
is chosen because it sits comfortably above the entire physically useful
parameter range and comfortably below where the system starts reclaiming
aggressively. At K = 120 it permits T_S down to 0.45 ms, while the useful range
is 1-5 ms: T_S must be under 5 ms to put the 1/T_S comb outside the +-200 Hz
shown, and well above the 0.6 ms clutter delay spread for the estimate to mean
anything. The guard therefore never fires in legitimate use; it exists to turn
a config typo into a named error instead of a thrash. ECA-S at the shipped
settings allocates 114 MB of the 512.

---

## 1e. Range-Doppler map and CFAR (`pbr::ArdProcessor`)

Files: `include/pbr/ArdProcessor.hpp`, `src/ard/ArdProcessor.cpp`,
`src/ard/cpu/ArdCpu.cpp`, `src/ard/gpu/ArdGpu.cu`, `include/pbr/ArdFile.hpp`,
`tests/test_ard.cpp`, `src/app/ard_verify.cpp`, `bench/ard_bench.cpp`,
`matlab/ard_golden.m`, `matlab/plot_ard.m`.

### 1e.1 Range on the x axis: the array is the picture

**Decision.** The map is stored `map[doppler * uRangeBins + range]`, range
varying fastest, which is exactly how it is displayed: range horizontal,
Doppler vertical.

**Why.** That is the convention of every paper this work is measured against
-- Schuepbach RadarConf 2020 Figs 3-7, Colone TAES 52(3) 2016 Figs 4-5,
Laptev DSPA 2026 Fig 4 all plot bistatic range horizontally.
`fm_passive_radar_process.m` stores the transpose and plots Doppler
horizontally; it is the outlier, and `ard_golden.m` flips once on the way out
so nothing downstream has to think about it.

Storing it in display orientation costs nothing. The batched FFT naturally
produces range-major output, and the kernel that converts complex to power has
to read and write anyway, so it does the transpose and the `fftshift` in the
same pass.

### 1e.2 Samples never leave the GPU between stages

`DSICanceller` gained `deviceReference()` and `deviceOutput()`, returning
opaque `const void*` so no CUDA types enter a public header, plus
`setHostOutputEnabled(false)` to skip a 1.6 MB download nobody wants.
`ArdProcessor::processDevice()` takes those pointers directly.

A chained run therefore copies nothing between cancellation and map formation.
`process()` is the same work with an upload in front and a download behind,
for standalone use and validation. No new buffer-management class was needed.

### 1e.3 A direct DFT in the CPU reference, on purpose

The Doppler length is 1600 = 2^6 * 5^2, so radix-2 does not apply and a
mixed-radix or Bluestein implementation is a lot of subtle code.

More to the point, the CPU version is a REFERENCE. Its job is to be obviously
right and to fail differently from cuFFT if either fails. A direct DFT is ten
inspectable lines sharing no algorithm with the thing it checks. It costs
120 * 1600^2 complex operations, seconds per CPI; that is the trade, not an
oversight.

### 1e.4 CFAR edges match MATLAB exactly, and the cost is reported

`conv2(..., 'same')` zero-pads. Cells near the border therefore build their
background estimate from fewer real neighbours while still dividing by the
full training count, so they under-estimate the background and over-detect.
Both implementations replicate this exactly, because a golden comparison
against a reference we had silently "improved" would prove nothing.

**Measured on the first CPI**: the border is 11.1% of the map but holds 21.7%
of detections -- about 2.2x over-detection, 20 of 92. `ArdStats::
uDetectionsInBorder` reports it every run so the effect is visible rather than
silent. Fixing it properly (normalise by the count of cells actually inside
the map) is deferred and will be a documented divergence when it happens.

### 1e.5 Validated, 2026-09-17

Three checks, because one end-to-end number cannot say which half is wrong.
`ard_verify --stage caf | cfar | both`; the `cfar` stage reads MATLAB's own
golden map as input, so the detector is isolated from map formation.

| Stage | CPU | GPU |
|---|---|---|
| CAF, map relative L2 | 5.299e-11 | **3.203e-07** |
| CFAR detections | 92 vs 92, 0 missed, 0 extra | 92 vs 92, 0 missed, 0 extra |

The GPU meets the **standard 1e-4 bar**, unlike the canceller which needed
1e-2. Two reasons, both predicted before measuring: the summation here is
hierarchical (128 samples per block, then 1600 blocks) rather than 204800 in
one go, and there is no ill-conditioned solve amplifying small errors.

### 1e.6 The ambiguity check the golden file could not give us

`PLAN.md` flagged that ECA-B must place Doppler ambiguities at exactly 1/T_B
spacing around a target ([2] eq. 17), and that no golden file could verify it
because MATLAB would reproduce the same artefact.

The first real CPI shows it. Brightest cell: range bin 46 (67.34 km) at
+99 Hz, with the detections in that same range bin at:

```
  +99 Hz   0.00 dB   the target
  +19 Hz -21.63 dB   offset -80 Hz  = -2 x 40 Hz
  -21 Hz -21.40 dB   offset -120 Hz = -3 x 40 Hz
```

T_B is 25 ms, so 1/T_B is 40 Hz, and the artefacts land on that comb exactly.
This simultaneously confirms that the batching is right, that the Doppler axis
is right, and that the two stages are coherent with each other.

### 1e.7 Measured on the Jetson, 2026-09-17

GPU, 20 iterations after a warm-up, clocks NOT pinned -- provisional.

| Stage | Time per CPI | vs real time |
|---|---|---|
| DSI cancellation (ECA-B) | 142.1 ms | 0.14x |
| **Range-Doppler + CFAR** | **19.6 ms** | **0.02x** |
| **Full chain** | **~162 ms** | **~0.16x** |

The ARD stage is about 7x cheaper than the cancellation, as predicted. The
whole chain runs at roughly six times real time on a $99 board.

Also observed: the interior false-alarm rate is 4.2e-4 against a configured
Pfa of 1e-5. That is not a bug in the detector -- the CFAR scale factor
assumes independent, exponentially distributed noise, and a real post-
cancellation map has neither (correlated neighbouring cells from the ambiguity
function, residual clutter structure, and genuine target smear all count).
Worth stating plainly when reporting detection results.

### 1e.8 The batches algorithm is exact, not an approximation (2026-09-22)

`ARD_ALGORITHM` selects how the correlation matrix that feeds the Doppler
transform is built. `Direct` is the original: for each delay, multiply by the
delayed reference and sum each block of `DOPPLER_DECIMATION` samples.
`Batches` does the same correlation one block at a time with a short FFT, so
all `N_RANGE_BINS` delays come out of one transform (Petri et al., TIWDC/ESAV
2011).

The decision worth recording is that these produce the **same map**, not
similar maps. Circular correlation of length L gives
`Z[r] = sum_i X[i] conj(Y[(i-r) mod L])`. Packing a block at `n0 = b*S` as

```
X[i] = cleaned[n0 + i]      i in [0, S),     zero elsewhere
Y[j] = ref[n0 + j]          j in [0, S)
Y[j] = ref[n0 + j - L]      j in [L-K+1, L), zero elsewhere
```

makes `Z[r]` equal the direct sum for every `r` in `[0, K)`. The wrapped tail
of Y supplies the K-1 reference samples from before the block, which is what
the direct form reaches back for; the zeros in X mean only S surveillance
samples contribute, so the **integration length is unchanged and so is the
map**. The condition is `L >= S + K - 1`, rounded up to a power of two: 256
for the shipped 128 and 120.

Two consequences that shaped the implementation:

- **L is derived, never configured** (`ardBatchTransformLength`). There is no
  tuning freedom: a longer transform gives the same answer and costs more.
  Exposing it would only let a user break the exactness.
- **The zero-padding is not a longer integration.** It is tempting to read
  L = 256 as "a 256-sample batch", which would lengthen the boxcar and add
  high-Doppler droop. It does not. The legacy `Batches_ARD.m` *does* overlap
  its blocks (500 samples at stride 100), which is a different algorithm with
  a real droop/anti-alias trade; that is deliberately not what this is.

`Batches` therefore adds no new parameter. `DOPPLER_DECIMATION` and
`N_RANGE_BINS` already determine everything.

The legacy reference correlates circularly with no padding
(`Batches_ARD.m:38`), so its short lags are contaminated by wrapped terms --
the same "no cyclic prefix" error ECA-CD has. Zero-padding to 256 buys
exactness for one doubling of the transform.

**Measured, 2026-09-22, Jetson, clocks NOT pinned -- provisional.**

Replay of `fers_capture.pbrcap`, the real `processDevice` path, 3 CPIs:

| `ARD_ALGORITHM` | ard stage | peak (CPI 0/1/2) | detections |
|---|---|---|---|
| Direct | 17.5 / 15.5 / 15.7 ms | 51.83 / 51.70 / 51.66 dB | 82 / 83 / 67 |
| Batches | 11.3 / 11.0 / 10.5 ms | 51.83 / 51.70 / 51.66 dB | 82 / 83 / 67 |

1.4x on the stage, ~4.7 ms of a ~229 ms CPI, so about 2% of the chain -- the
DSI stage is where the time is. Identical peaks and detection counts on every
CPI, which is the property that matters more than the speed.

`ard_bench` shows a much wider gap (median 33.4 vs 12.4 ms) because it uses
`process()` with host-to-device copies rather than `processDevice()`, and runs
30 iterations back to back on unpinned clocks. The replay figures are the
honest ones.

Agreement against the MATLAB golden map, `cleaned_0000.bin`, all four
combinations, tolerance 1e-4:

| | GPU | CPU |
|---|---|---|
| Direct | 3.20e-07 | 6.79e-07 |
| Batches | 3.54e-07 | 6.77e-07 |

### 1e.9 The CPU ARD reference is FP32 (2026-09-22)

`ArdCpu` accumulated in double until now. It does not any more: the lag
products, the direct DFT, the power map and the CFAR sums are all float, and
the CFAR scale factor is narrowed once at init exactly as the GPU passes it
into the kernel.

The reason is benchmark isolation, the same argument as 1d.8. A double CPU
reference makes every CPU-vs-GPU comparison measure two things at once -- the
backend and the arithmetic width -- and the thesis needs it to measure one.
The direct DFT is still an independent algorithm from cuFFT; only its width
changed. Cost: the golden-map agreement moved from ~3e-7 to ~6.8e-7, still
150x inside the 1e-4 tolerance.

Unlike the DSI stage there is no `ArdParams` precision flag. `DSI_MATH_PRECISION` exists because ECA's normal equations are genuinely
ill-conditioned and the author wanted to see the difference; nothing in the
ARD stage amplifies rounding that way, so a flag would be a knob with no
question behind it.

---

## 2. Memory layout on the Jetson Nano

One contiguous heap allocation per channel, made once at construction and never
resized or reallocated.

| Quantity | Value |
|---|---|
| `ComplexF` | 8 bytes (two IEEE-754 `float`) |
| Capacity | 409,600 samples (2 s at fs = 204800) |
| Per channel | 3.125 MiB |
| Both channels | 6.25 MiB |
| Jetson Nano RAM | 4 GB, shared between CPU and GPU |
| Fraction of RAM | ≈ 0.15% |
| One CPI window | 204,800 samples = 1.5625 MiB per channel |
| One FERS datagram | 342 samples = 2,736 bytes ≈ 43 cache lines |

The footprint is immaterial. What matters more is that **no allocation occurs
after startup**, so there is no allocator activity in the packet path to cause
intermittent stalls.

Cache behaviour (derived from Tegra X1 specifications: 32 KiB L1-D per core,
2 MiB shared L2): at 3.125 MiB per channel the buffer exceeds L2, so a full-CPI
read streams from DRAM rather than cache. Access is sequential, which the
hardware prefetcher handles well. Individual packet writes are ~2.7 KiB and
comfortably cache-resident.

---

## 3. Performance assessment

All figures **derived**, not measured.

Per-packet work: one 64-bit modulo, a small number of comparisons, and one or two
`memcpy` calls totalling 2,736 bytes. The zero-fill path does not execute unless
a gap occurred.

| Quantity | Derived value |
|---|---|
| Packet rate | 599/s per channel, 1,198/s combined |
| Write traffic | 3.28 MB/s (both channels) |
| Nano DRAM bandwidth | ~25.6 GB/s |
| Fraction of memory bandwidth | ≈ 0.013% |
| Modulo cost | ≈ 0.002% of one core at 1.43 GHz |

The buffer is therefore not expected to be a bottleneck at FM-band rates; the
cancellation and CAF stages will dominate. This should be confirmed by
measurement once the chain runs on hardware.

---

## 4. Deferred performance opportunities

Listed in expected order of value. Each was consciously deferred under
"start simple, prove it works, then restructure", and the current design keeps
each one cheap to adopt.

### 4.1 Pinned host memory — do this before benchmarking

The buffer currently uses ordinary pageable memory. Host-to-device copies from
pageable memory are staged through an internal CUDA pinned buffer, which roughly
halves effective bandwidth and prevents both asynchronous transfer and
overlap with kernel execution.

Derived impact: ~0.25 ms additional transfer time per CPI. Against a 1-second
CPI that is 0.025% of throughput and irrelevant — **but latency is the thesis
metric**, and this contaminates latency measurements directly. Allocation is
confined to the constructor specifically so switching to `cudaHostAlloc` is a
two-line change.

### 4.2 Zero-copy / mapped memory on Tegra

On the Nano, CPU and GPU share the same physical DRAM; there is no PCIe bus to
cross. `cudaHostAllocMapped` may therefore let the GPU read the ring directly
with no copy at all, and unified/managed memory is a further option. Whether
this beats an explicit pinned copy on Tegra X1 is a genuine empirical question
and a legitimate benchmarking result for the thesis, not a foregone conclusion.

### 4.3 Eliminate the host-side read copy

`read()` copies a CPI into a caller-supplied buffer: 1.5625 MiB per channel per
CPI. Since that data is GPU-bound anyway, the host intermediate could be removed
entirely by exposing the ring's two contiguous spans and issuing two
`cudaMemcpyAsync` calls straight to device memory. Saves ~3.3 MB/s of host
copying. Small in absolute terms, but free to reclaim, and the current API does
not preclude it.

### 4.4 Separate receive and processing threads

Currently one thread drains datagrams then processes. If CPI processing ever
exceeds the CPI duration, received packets accumulate in the kernel socket
buffer and are eventually dropped. Splitting into a receive thread and a
processing thread would decouple them, at the cost of introducing
single-producer/single-consumer synchronisation.

### 4.5 Rejected: power-of-two capacity

Would allow `&` instead of `%`, but the modulo is per-packet, not per-sample
(§1.4). Costs 28% more memory and breaks length parity with the MATLAB
reference. Not worth revisiting unless profiling contradicts the derivation.

### 4.6 Rejected for now: int16 storage

Storing raw ADC codes as interleaved `int16` instead of `ComplexF` would halve
the footprint to 1.5625 MiB per channel. On a 4 GB machine the 3 MiB saved does
not justify converting the buffer to a template and pushing a
conversion step into the read path.

---

## 5. Verification status

| Check | Status |
|---|---|
| 13 unit tests (`tests/test_circular_buffer.cpp`) | Pass |
| `-Wall -Wextra`, Release flags (`-O2 -DNDEBUG`) | Clean, clang 14 host and gcc 7.5 aarch64 |
| AddressSanitizer + UndefinedBehaviorSanitizer | Clean |
| Build and unit tests on Jetson Nano (gcc 7.5, CUDA 10.2) | Pass |
| Live ingest against FERS, 30 s, Jetson over LAN | Pass — see §5.1 |
| Validation against MATLAB golden files | Not applicable — buffer is a data structure, not a processing stage |

### 5.1 Live ingest results, 2026-09-16

FERS on the development Mac streaming VITA 49.2 UDP to the Jetson Nano over a
LAN link; 30 s run, `--vita49-fullscale 0.005`, scenario
`SingleTargetCleanNativeFileCw.fersxml`.

| Measure | Reference | Surveillance |
|---|---|---|
| Data packets | 18,000 | 17,999 |
| Index continuity (exact / ahead / behind) | 17,999 / 0 / 0 | 17,998 / 0 / 0 |
| Immediate read-back verified | 18,000 | 17,999 |
| Lag check windows verified | 30 | 29 |
| Declared packet size matched | 18,000 | 17,999 |
| Raw ADC code peak | 27.8% of full scale | 0.9% of full scale |

Totals: 36,059 datagrams, 0 parse failures, 0 rejected writes, 0 packet-count
gaps, 0 data packets arriving before their context packet, 12,287,666 samples
buffered, buffer fill steady at 2.00 s throughout.

### 5.2 Full-length run and ADC scaling, 2026-09-16

A 172 s run (the full 180 s scenario, loopback) measured the true peak through
the UDP path rather than a 30 s sample:

| Measure | Reference | Surveillance |
|---|---|---|
| Data packets | 103,201 | 103,201 |
| Index continuity (exact / ahead / behind) | 103,200 / 0 / 0 | 103,200 / 0 / 0 |
| Lag check windows verified | 172 | 172 |
| Raw ADC code peak at `fullscale` 0.005 | 11,381 (34.7%) | 326 (1.0%) |

206,748 datagrams, 0 parse failures, 0 rejected writes, 0 packet-count gaps.

The reference peak grew from 8,964 over 30 s to 11,381 over the full run,
which is why calibration needs a full-length measurement.

**Recommended `--vita49-fullscale` = 0.0022**, derived as
`0.005 x 11381 / (0.8 x 32767)`, putting the reference at about 79% of full
scale. At that setting surveillance reaches ~741 codes, roughly 9.5 effective
bits, up from ~8.3.

**The limitation is structural, not a misconfiguration.** The direct path sits
about 31 dB above the bistatic echo, and FERS applies one global full-scale
value to every receiver. Whatever headroom the reference channel needs is
taken out of the surveillance channel's dynamic range. A real passive radar
receiver applies independent gain control per channel, so this simulator
constraint *understates* the surveillance dynamic range a physical system
would achieve. The resulting quantisation floor is worth stating explicitly
when reporting cancellation depth, since after 30-40 dB of direct-path
suppression the residual is measured against it.

### 5.1 Live ingest results (continued)

Four facts established by the 30 s Jetson run, each previously unverified:

1. **FERS timestamps land exactly on sample boundaries.** Index continuity was
   exact on every one of 36,000 packets, across two independent derivations
   (timestamp arithmetic versus accumulated sample counts). The truncation /
   rounding hazard in the picosecond-to-sample conversion does not arise, so
   `absoluteIndex()` needs no rounding term.
2. **FERS populates the header's declared packet-size field correctly.** Zero
   mismatches over two runs and ~72,000 packets, so the parser now enforces it
   as a free corruption and truncation check.
3. **The mod-16 packet counter advances for every packet in a stream, context
   packets included.** An earlier implementation tracked it only on data
   packets and reported one spurious gap per second, exactly matching FERS's
   1 Hz context heartbeat.
4. **No packet loss over the LAN link** at ~1.7 MB/s, despite the Jetson's
   `SO_RCVBUF` being clamped to 426 KB (0.25 s of slack) by the stock
   `net.core.rmem_max`. That margin is thin and should be raised before the
   DSP stages start competing for CPU.

Test coverage: construction and invalid construction; zero-length operations;
in-order round-trip; writes straddling the wrap; out-of-order writes; wholly
stale writes rejected; partially stale writes trimmed with correct source-pointer
advance; forward-jump zero-fill; gaps not exposing the previous lap's data;
jumps longer than one capacity; a write of exactly capacity; out-of-bounds reads
rejected; and a FERS-shaped stream of 342-sample packets pushed past one wrap
with a full CPI read back across the seam.

Note on test construction: the test file `#undef NDEBUG` before including
`<cassert>`. Without this, the Release build (which defines `NDEBUG`) compiles
every assertion out, leaving a test that passes unconditionally. This applies to
every test file in the project.

---

## 6. Related open items

- `fm_passive_radar_stream.m` still sets `fullscale = 1.0` while the working FERS
  run used `--vita49-fullscale 0.005`, so MATLAB's descaled output is currently
  200× off physical units. Harmless for ECA-B, CAF and CFAR — all are invariant
  to a constant scale applied equally to both channels — but it invalidates any
  golden file expressed in physical amplitude.
- `bufSeconds = 2` with `Tcpi = 1.0` leaves exactly one CPI of slack. If CPI
  processing overruns one second, the window tail falls out mid-read. `read()`
  refuses rather than returning corrupt data, so the failure mode is skipped
  CPIs rather than bad detections.
- VITA 49 trailer indicator bits (over-range, sample loss) are documented to
  exist but their positions are unpublished, so packet loss cannot yet be
  detected from the trailer. The header's mod-16 packet count is the cheaper
  candidate and is now decoded on the MATLAB side, pending observation.

## 5. Later design notes (2026-09-25 to 2026-09-29)

Recorded in their own documents rather than here; the decisions list at the
top of `PROGRESS.md` indexes them:

- `UHD_VITA49_Driver_design.md` — the USRP2/N210 driver, the FERS VITA 49.2
  profile as a wire contract, exact timestamp arithmetic, multicast delivery.
- `ard-stream-design.md` — range-Doppler frames streamed as the file bytes
  over UDP multicast, the C++ viewer, blank frames with a status word.
- `live-parameters.md` — one in-memory parameter set seeded by a file that is
  never written; changes at CPI boundaries by rebuilding; the control port;
  sample-rate adaptation.
