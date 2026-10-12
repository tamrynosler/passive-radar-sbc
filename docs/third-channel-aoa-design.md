# Third channel: a second surveillance antenna at λ/2 (design, draft)

Written 2026-10-09, before any code. Last feature of the system. The end
goal is the live chain on the Jetson with three channels; no goldens, no
CPU reference.

**Status 2026-10-11: steps 1 to 5 of §8 built and working on the laptop
chain** (`scenarios/S0_aoa/README.md` has the numbers): the five config
keys in both `.conf` files, the three-channel ingest, a second canceller
and map former run after the first, the direct-path phase and a bearing
per plot (`src/ard/Bearing.cpp`), `plots.csv` and `telemetry.csv` columns,
and the frame's AoA (the strongest plot's bearing), the direct-path phase
and channel 2's timing in the status JSON and the viewer header. The
author's choices (2026-10-11): the keys live in `fers.conf`; "an AoA value
for every CPI frame, nothing fancy". Decisions 2, 3, 5, 6, 7 of §2 taken as
recommended; decision 4 deferred: the frame stays `PBRARD01` and the
per-frame AoA travels in the status JSON, so the per-plot bearing is in
`plots.csv` only, not drawn. The bearing is read over the 3 x 3 cells
around the plot's peak (coherence needs more than one cell). **2026-10-12: step 6 done on the
Jetson** (PROGRESS 2026-10-12): live from FERS on the Mac over a direct
cable, ECA-CD, 70 CPIs in real time at a median 114 ms for both channels,
AoA RMS 0.34 deg; every canceller replayed on the GPU (unpinned), the
three batched ECAs at 0.4 deg RMS; ECA-m peaks at 962 MB used of the
board's 3963 MB with two sets of stages. Next: §6.2 and §7 as the author
decides, and the §2.3 question raised by the results: the strongest plot
is the residue when cancellation is weak.

## 1. What the third channel buys

Two surveillance elements half a wavelength apart (λ = 3.368 m at 89 MHz,
so d = 1.684 m) form a two-element interferometer. The phase difference
between the two range-Doppler maps at a target's cell is

    Δφ = 2π (d / λ) sin θ            θ measured from the array broadside

and with d = λ/2 it is unambiguous over the whole ±90° in front of the
array. One bearing per plot, read off the two complex maps, costs
microseconds. With the bistatic range the chain already has, bearing plus
range places the target on the ground: a plan view, the first output that
looks like a radar picture rather than a range-Doppler map.

Rejected uses of the pair: beamforming before cancellation (two elements
give ~3 dB and no useful null steering) and non-coherent integration of the
two maps for detection (1.5 to 3 dB; cheap to add later, §7, but it changes
the plots the thesis comparison is built on).

What it costs: the DSI and the map former run once per surveillance
channel. That is itself a thesis point: the ECA family's Gram matrix and
its factorisation depend on the reference only, so a second channel is an
extra right-hand side for them, while CGLS and FBLMS pay the full price
again (§7).

Expected accuracy: at 20 dB SNR the phase error is about 0.07 rad per cell,
so the bearing error is about 1.3° at broadside and 1.8° at 45° off it.
Range and Doppler sidelobe plots carry the target's phase relation, so they
get the target's bearing; noise plots get a low coherence (§5.3), which is
a second way to tell them apart.

## 2. Decisions for the author

1. **Value: AoA per plot, then a plan view.** As above. Alternatives
   rejected in §1.
2. **When channel 2 is processed: in the same CPI, after channel 1, in the
   same single-threaded loop**, timed separately. Budget on the Nano,
   clocks pinned (results/performance_improvements/README.md): ECA-CD
   10 ms + Batches 11 ms per channel, so ~45 ms of 1000 ms for both.
   ECA-B 153 ms doubles to ~310 ms; CGLS at 482 ms doubles to ~970 ms and
   no longer fits a 1 s CPI. No CUDA streams, no second thread: the budget
   does not call for them and the comparison is clearer without them.
3. **CFAR runs on channel 1's map only**, exactly as today. Channel 2's map
   is only read at the plots' cells. The plots, the detections.csv and
   every existing result stay comparable. (Option: detect on
   |M1|² + |M2|², §7.)
4. **Frame format: bump PBRARD01 to PBRARD02** with 64-byte plot records
   (the 48 bytes today plus bearing and coherence as two float64). One
   format, one serialiser, a frame off the wire is still a .bin. Readers
   accept both magics. Alternative: a separate per-frame bearing datagram
   and the frame untouched; more moving parts for the same bytes.
5. **Array geometry in the config**, not derived from anything: see §4.1.
   `SURVEILLANCE2_RECEIVER_NAME = none` keeps the USRP configuration a
   two-channel chain (the USRP2 BasicRX has two inputs; the driver does not
   change).
6. **Ring buffer: three buffers in an array and a loop**, instead of a
   third named member next to `mRefBuffer` and `mSurBuffer`. This is the
   one rewrite rather than extension in the plan, and it is in ring-buffer
   code (line-level process). Say if the named-member version is preferred.
7. **Scenario: S0_long plus a third receiver** (§3), baseline perpendicular
   to the surveillance boresight. With that baseline the transmitter sits
   78° off broadside and its direct path reads Δφ ≈ 176°, four degrees
   from the wrap. Fine for a noiseless check in FERS; a spacing of 0.45 λ
   would give margin if the author prefers it to the textbook λ/2.

## 3. The FERS scenario: `scenarios/S0_aoa/`

Copy of S0_long with one more platform:

- `ArmasuisseSur2RxPlatform`, receiver `ArmasuisseSur2RxClean`, the same
  antenna (`RxYagiAntenna`), the same rotation (azimuth 125°, elevation
  10°), the same `timing="RxClock"`, the same noise temperature.
- Position: surveillance element + d along the baseline. FERS azimuth is a
  compass bearing (checked against the S0_long README: the target at t = 0
  is 43° off a 125° boresight), so a baseline perpendicular to the
  boresight lies along 35°, and element 2 is at
  (287946.01 + 0.966, 6297267.09 + 1.379, 241) for d = 1.684 m.
- Carrier phase is modelled per path in FERS (`channel_model.cpp`:
  `phase = -2π f_c τ`), and receivers naming the same `timing` share one
  `Timing` instance (`xml_parser_utils.cpp`, `resolve_timing_instance`),
  so the `random_freq_offset_stdev` on `RxClock` is common to both elements
  and cancels in Δφ. The inter-element delay (5.6 ns) is far below a
  sample (4.9 µs), so only the phase differs, as it should.
- `config/fers_aoa.conf`: fers.conf plus the §4.1 keys. Or the keys go into
  fers.conf with `none` for the two-channel scenarios; author's call.

Truth: `matlab/fers_truth.m` gains, per CPI, the target's bearing from the
array (compass and from broadside), the expected Δφ, and the direct path's
expected Δφ (a constant). It reads the two surveillance names and the
array keys from the config, never the geometry by hand.

The bearing sweep over the 600 s run: from 43° off boresight at t = 0,
through 1° at t = 420 s, to 39° the other side at the end. Good coverage
of the arcsine.

## 4. The chain

### 4.1 Config keys

| Key | fers_aoa.conf | prb.conf | notes |
|---|---|---|---|
| `SURVEILLANCE2_RECEIVER_NAME` | `ArmasuisseSur2RxClean` | `none` | restart-only |
| `CARRIER_FREQ_Hz` | 89e6 | 89e6 | λ; the driver already wants it (PLAN, geometry) |
| `ARRAY_SPACING_m` | 1.684 | 1.684 | element 2 relative to element 1 |
| `ARRAY_BASELINE_AZ_deg` | 35 | tbd | compass bearing from element 1 to element 2 |
| `PHASE_CAL_deg` | 0 | tbd | added to Δφ before the arcsine; the real array's cable and LO offset |

All five present in every config (a missing key is fatal, as always);
`none` is the documented "no third channel". The three array keys are
live-changeable (the AoA step reads them per CPI; nothing is rebuilt).
The plan view (§6.3) adds the transmitter and receiver positions, which
PLAN.md already lists as the geometry keys.

### 4.2 Ingest (central: ring buffers, line-level process)

- `ChannelRole` gains `Surveillance2`. `ChannelIngestConfig` gains
  `sSurveillance2Name` (empty = absent).
- Three `CircularBuffer`s in an array indexed by role; the first-index
  tracking, the oldest-valid and write-end checks, the coverage test and
  the reads loop over the channels present. `tryExtractCpi` takes a third
  pointer, `nullptr` in a two-channel run.
- `rolesResolved()` requires the third stream when it is configured.
  `streamSummary()` names it.
- Nothing changes for the USRP driver, the capture format (datagrams,
  so a three-stream FERS run records and replays as it is), or the
  two-channel configs.

Checkpoint, before any GPU work: a replay of the S0_aoa capture prints,
per CPI, each channel's power and the direct-path phase difference
`arg Σ sur2[n] · conj(sur1[n])` over the CPI (a host dot product, well
under a millisecond, dominated by the direct signal). It must equal the
truth file's direct-path Δφ, sign included. This is the one place the
sign convention of the whole feature is settled, against a known bearing,
before anything downstream exists. It stays in the telemetry afterwards
as `directPhaseDeg`: on the real array it is the phase calibration check
against the known transmitter bearing.

### 4.3 Cancellation and map formation (central, function-level process)

- `Chain` holds a second `DSICanceller` and a second `ArdProcessor`,
  built from the same parameters, only when the third channel is
  configured. Two instances, no change to either interface: every one of
  the six cancellers works unchanged on channel 2.
- `processCpi` runs channel 2 after channel 1: cancel, then map. Channel
  2's CFAR output is discarded (the processor runs it anyway; a flag to
  skip it is a later saving of ~3 ms). Channel 2's map always comes to the
  host (1.5 MB, about 1 ms): the AoA step reads it there.
- Telemetry and the status JSON gain `dsi2Ms`, `ard2Ms`, `suppression2Db`,
  `aoaMs`, `directPhaseDeg`. `totalMs` includes them. The viewer's header
  shows them.
- Memory: two sets of device buffers. ECA-m's whole-CPI clutter matrix is
  the largest (204800 × 120 complex, ~200 MB per channel). Measure with
  `tegrastats` at the checkpoint; the Nano's 4 GB is shared.
- Warm-up runs both channels; `resetState()` on both.

Checkpoint: both maps written to disk for one CPI; the target cell's
magnitude within a few dB between them; suppression within a dB.

### 4.4 Bearing per plot (new, peripheral: host arithmetic)

New file `src/ard/Bearing.cpp`, header `pbr/Bearing.hpp`. For each plot:

    z   = Σ_cells M2[c] · conj(M1[c])       over the plot's cells
    Δφ  = arg z + PHASE_CAL
    coh = |z| / sqrt(Σ|M1|² Σ|M2|²)          in [0, 1]
    θ   = asin( Δφ · λ / (2π d) )             from broadside, NaN if |·| > 1
    bearing = ARRAY_BASELINE_AZ_deg − 90 + θ    compass, sign fixed at §4.2

First version uses the peak cell only (`uPeakRangeBin`, `uPeakDopplerBin`
are already in `Plot`); the sum over the group's cells is the second
version, if the first is noisier than the §1 estimate. `Plot` gains
`dBearingDeg`, `dPhaseDeg`, `dCoherence`; `plots.csv` gains the three
columns; `plotAsDetection` carries bearing and coherence into the record.

Checkpoint: a replay of S0_aoa scored against `truth.csv` (an extension of
`score_run.m`): bearing error per CPI against SNR, RMS over the run. The
§1 numbers say 1 to 2°.

## 5. Frame and stream (peripheral)

- `ArdFile`: magic `PBRARD02`, `kArdDetectionBytes` 64: the 48 today, then
  float64 bearing (degrees, NaN if none) and float64 coherence. The reader
  and `parseArdFrameHeader` accept `PBRARD01` with 48-byte records, so
  old frames and the goldens still read. `Detection` gains the two fields
  (zero from the CFAR, set from the plot).
- `ArdStream`: unchanged (it carries bytes). `ard_view`: unchanged.
- `matlab/read_ard.m`: both magics.
- The status JSON (1384 bytes) is unchanged except the §4.3 fields.

## 6. The viewer (peripheral)

6.1 Header: `dsi2`, `ard2`, `supp2`, `aoa`, `direct φ`.

6.2 Detections panel: each plot's dot coloured by bearing (a cyclic scale
with a small legend), and the plot list on hover gains bearing and
coherence. A new panel **Bearing against time**: x = CPI time, y =
bearing, one dot per plot, newest in red, the same history depth as the
Detections panel. The target's bearing track is the thing to look at;
sidelobe plots fall on the same line, noise plots scatter.

6.3 **Plan view**: a map-like panel with the receiver at the origin, the
transmitter drawn from the geometry keys, the baseline, and each plot
placed by range and bearing. In the ground plane, with φ the angle at the
receiver between the target bearing and the transmitter bearing and
L the baseline,

    Rr = ((Rb + L)² − L²) / (2 ((Rb + L) − L cos φ))

places the target at distance Rr along the bearing. Needs the transmitter
and receiver positions in the config (PLAN's geometry keys; the FERS
scenario's projected metres serve as the frame, x east, y north). History
as grey dots, like the Detections panel. This is the last step; the
feature is useful without it.

## 7. Later, if wanted

- **Multi-RHS ECA**: one Gram and one factorisation per reference window,
  two solves. Saves most of channel 2's DSI time for ECA-m/B/S/LD and is
  the natural second benchmark column. Needs a `cancel2()` or a
  two-channel `cancel()` on the interface: ask first.
- Skip channel 2's CFAR (~3 ms).
- Detection on |M1|² + |M2|².
- Channel 2's before-processing spectrum (`kArdSpectrumChannels` 3).
- Bearing from the sum over the plot's cells (§4.4).
- Pinned host memory for the third CPI copy (listed already).

## 8. Order of work and checkpoints

Each step runs from a file before the next starts (CLAUDE.md).

1. This note agreed; §2 answered. Config keys and loader (peripheral).
2. `scenarios/S0_aoa/`, `fers_truth.m` with bearings, a recorded
   three-stream capture in `data/S0_aoa/` (FERS on the Mac, chain
   recording). Check: three streams named in `streamSummary()`.
3. Ingest: three buffers, three-channel extraction (line-level process).
   `tests/test_cpi_extraction.cpp` extended for a third stream.
   Check: `directPhaseDeg` equals the truth, sign included.
4. Chain: second canceller and map former (function-level proposals).
   Check: two maps, timings, `tegrastats` memory, on the Jetson.
5. Bearing per plot, `plots.csv`, score against truth.
6. Frame `PBRARD02`, viewer header, bearing colour, bearing-against-time
   panel. Live on the Jetson with FERS streaming from the Mac.
7. Geometry keys and the plan view.
8. Benchmark rows: every canceller, three channels, clocks pinned;
   `docs/benchmarking-methodology.md`, PLAN, PROGRESS, this note's status.
9. §7 items as the author decides.

Files touched, by step: (1) `Config.hpp/.cpp`, both `.conf`; (2)
`scenarios/`, `matlab/fers_truth.m`; (3) `ChannelIngest.hpp/.cpp`,
`pbr_run.cpp`, `tests/test_cpi_extraction.cpp`; (4) `pbr_run.cpp` only;
(5) `Bearing.hpp/.cpp`, `DetectionGrouper.hpp/.cpp`, `pbr_run.cpp`,
`matlab/score_run.m`; (6) `ArdFile.hpp/.cpp`, `ArdProcessor.hpp`,
`ard_viewer.html`, `matlab/read_ard.m`, `tests/test_ard_stream.cpp`;
(7) `Config`, `ard_viewer.html`.
