# data/

Recorded test data. **Gitignored — never committed** (this README is the
one exception). Regenerate from FERS.

> **2026-10-01.** Laid out per scenario: `data/<scenario>/` holds the
> capture, frozen CPIs, `truth.csv` and `golden/` for the matching
> `scenarios/<scenario>/`; `data/waveforms/` holds the FM cuts from the
> Malmesbury recording. `scenarios/README.md` defines it. The 2026-09-16
> capture and CPI 0 are now `data/S0_clean/capture.pbrcap` and
> `data/S0_clean/cpi_0000.bin`; the old goldens and live-run leftovers went
> to `~/passive-radar-archive/old-results/data-2026-09/`. Paths below that
> say `data/fers_capture.pbrcap` or `data/cpi_0000.bin` mean those.

> **2026-09-29.** Everything below describes the FERS scenario at 204800 Hz,
> which now lives in `config/fers.conf`; `config/prb.conf` is the USRP
> configuration at 200000 Hz. Replay the capture with `--config
> config/fers.conf` (`make replay` does). Live runs no longer write `ard/`
> frames unless `ARD_WRITE_FILES = 1`; frames go to the viewer over
> multicast instead (`docs/ard-stream-design.md`). Golden files here predate
> the current parameters and are not a trusted gate (PLAN.md).

Every fact below is taken from the MATLAB reference chain in `matlab/`
(cited per item). Nothing here is guessed. Where FERS behaviour is not
documented in `matlab/`, it is marked TODO rather than invented.

## Signal parameters

From `matlab/fm_passive_radar_process.m`:

| Parameter | Value |
|---|---|
| Sample rate | 204800 Hz |
| Format | complex IQ, already mixed to baseband, one FM channel |
| Length | 36,864,000 samples = 180 s |
| Receivers | `ArmasuisseRefRxClean` (reference), `ArmasuisseSurRxClean` (surveillance) |

Reference and surveillance come from the same FERS run, so they share a time
base and need no inter-channel alignment.

## Recorded files (HDF5 playback path)

```
ArmasuisseRefRxClean_results.h5
ArmasuisseSurRxClean_results.h5
```

Written by FERS `--out-dir`. Read by `read_fers_iq()` in
`matlab/fm_passive_radar_process.m` and `matlab/fm_passive_radar_movie.m`:
the sample chunks are listed in the root attribute `fers_metadata_json`, and
concatenating them yields one complex vector.

⚠ Data is **zero-padded past the simulated interval**. Find the true end with
the last non-zero sample before using it (`validLen` in the processing
script) — do not trust the allocated file length.

⚠ HDF5 is on the ask-first dependency list in `CLAUDE.md`. The C++ playback
path is unresolved: either HDF5 gets approved, or MATLAB exports raw IQ for
C++ to consume. See `PLAN.md`.

## Live UDP path (VITA 49.2)

From the workflow notes in `matlab/fm_passive_radar_stream.m`. Start the
listener first, then FERS:

```
~/FERS/build/release/packages/fers-cli/fers-cli \
  scenario.fersxml --out-dir=./results \
  --vita49 127.0.0.1:4991 --vita49-fullscale 1.0
```

- Port `4991` must match the listener's port.
- `--vita49-fullscale` must match the consumer's `fullscale` constant. Raw
  int16 ADC codes are recovered as `code / 32767 * fullscale`, so the codes
  must span ±32767, not ±1.
- Receiver names must match the `<receiver name="...">` values in the
  `.fersxml`.
- FERS must be in **continuous** mode; pulsed mode defers UDP output.

### Packet contract

Documented at
<https://github.com/stpaine/FERS/wiki/VITA49-Streaming-Implementation> and
implemented in `matlab/parse_vita49_packet.m`. All multi-byte fields are
**big-endian**. Class ID `0xFA52530001000101` (a FERS-internal placeholder
VRT profile).

- **Signal-data packets:** 32 bytes fixed overhead — 4 header + 4 stream ID
  + 8 class ID + 4 UTC integer seconds + 8 UTC fractional picoseconds
  + 4 trailer. Timestamp is the time of the **first** sample in the packet.
- **Context packets:** 92-byte header followed by NUL-terminated ASCII JSON.
  Carry sample rate and `receiver.name`.
- Trailer bit positions are **not** documented on the wiki and are left
  un-decoded in the MATLAB parser. TODO if needed: read
  `packages/libfers/src/serial/vita49/` in the FERS source.

### Streaming behaviour

- Channel roles (ref/sur) are learned from context-packet JSON, so a context
  packet must arrive before a channel's data is usable. FERS emits one at
  stream open and then roughly every 1 s.
- Place samples by **absolute sample index derived from the VRT timestamp**,
  not arrival order. This tolerates minor UDP reordering; it assumes no
  dropped packets. Drops appear as a gap that stalls CPI processing.
- FERS marks VITA 49 output "Alpha and Unverified" — treat the format as
  provisional.

## Captured UDP streams (`.pbrcap`)

Raw datagram captures for replaying a FERS stream without FERS. Format is in
`include/pbr/CaptureFile.hpp`: a 16-byte header then length-prefixed records.

Record one with `pbr_run`, which can also freeze the first CPI it
extracts. Start it before `fers-cli`:

```
./bin/pbr_run --live --config config/fers.conf --seconds 5 \
    --capture data/S0_clean/capture.pbrcap --dump-cpi data/S0_clean/cpi_0000.bin
# then start fers-cli as above
```

Replay it with `pbr_run --file data/S0_clean/capture.pbrcap --config config/fers.conf`.

About 1.7 MB per second of capture. A 3 s capture is ample for regression use.

**Record the fullscale a capture was made with.** `pbr_run` descales with
`VITA49_FULLSCALE` from the config it is given, so the config must carry the
value the capture was sent with, or every sample comes out scaled wrongly with
no indication that anything is amiss. `config/fers.conf` is set to 0.005, the
value the capture in `data/S0_clean/` was recorded at.

## TODO

- FERS version / commit used for the recordings.

(The scenario file is committed as `scenarios/S0_clean/scenario.fersxml`
since 2026-10-01.)
