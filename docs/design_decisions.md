# Design Decisions

Rationale for implementation choices, kept as source material for the thesis
report. Each entry records what was decided, why, and what was rejected.

Performance figures are labelled either **derived** (calculated from published
hardware specifications and known data rates) or **measured** (observed on
hardware). Nothing in this document has been measured on the Jetson yet.

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
