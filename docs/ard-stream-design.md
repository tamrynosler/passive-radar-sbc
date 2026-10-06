# ARD output: stream to a port, write to disk only when asked (design, v1)

Written 2026-09-25, before any code. **Status: implemented and verified on
the Nano and from the Mac the same day** (PROGRESS.md 2026-09-25); what was
built matches this note, with `--iface` added to `ard_view` for a viewer on
a multi-homed laptop. Measured cost of streaming in `pbr_run`: 26 to 31 ms
per CPI (`ardWriteMs`), against the 6 to 12 ms estimated in §5, still 3% of
the 1024 ms budget. **Approved 2026-09-25** with
the author's answers to §11: (1) not answered, taken as verbatim file bytes;
(2) yes, the USRP config is the default and FERS stays for testing; (3)
peripheral, agreed; (4) `UdpReceiver::open` blocking flag approved; (5)
retire the Python viewer outright. Implementation starts after the
CPI-validity question raised the same day is settled (see PLAN.md). Supersedes the sidecar arrangement in `ard-viewer.md`
once built; that note stays as the record of v1 and of the page's rendering
rules, which do not change.

## 1. What the author asked for

Today `pbr_run --out DIR` writes one 1.5 MB `.bin` per CPI and a Python
sidecar tails the directory for a browser. Two problems: the disk fills at
~90 MB a minute, and a Python process left running is not something we want
on the board indefinitely.

Wanted instead:

1. A config flag that enables writing ARD frames to disk. Off for a normal
   run, on for test and verification runs.
2. A config flag that enables streaming ARD frames to a port. On for a
   normal run.
3. A GUI application, in C++, that picks the stream up and shows it in the
   web viewer. Python retired from the display path.

## 2. Design in one paragraph

`pbr_run` serialises each frame **once**, into the exact `PBRARD01` byte
layout `ArdFile.hpp` already defines, and then does either or both of: write
those bytes to `ard/ard_NNNNN.bin`; cut them into 1400-byte UDP datagrams
and send them to a destination, by default a multicast group, followed by
one small JSON status datagram. A C++ sidecar, `ard_view`, joins the group,
reassembles frames in memory, keeps only the newest complete one, and serves
the existing HTML page plus the same two endpoints the Python served. The
page does not change what it draws; the browser still does every pixel.
Nothing is written to disk anywhere unless the flag says so.

Why the stream carries the file bytes verbatim: one format, one serialiser,
one parser (the page's), and a frame captured off the wire *is* a `.bin`. It
costs 1.5 MB per CPI; on the switch that is nothing, and the fallback if it
ever is (a magnitude-only stream) is listed in §9, not built.

## 3. Config keys (all required, per the config policy)

| Key | Values | prb.conf (USRP) | fers.conf (replay) |
|---|---|---|---|
| `ARD_WRITE_FILES` | 0 / 1 | 0 | 1 |
| `ARD_STREAM` | 0 / 1 | 1 | 1 |
| `ARD_STREAM_DEST` | `host:port` | `239.192.0.2:4992` | `239.192.0.2:4992` |
| `ARD_SNAPSHOT_EVERY` | existing | 1 | 1 |

`ARD_SNAPSHOT_EVERY` keeps its meaning and now gates both outputs: every Nth
CPI is serialised; the others are neither written nor streamed. Both flags 0
means the map is never copied off the GPU, as today without `--out`.

`--out DIR` keeps its job as the run directory: `telemetry.csv`,
`detections.csv` and the config copy still go there (kilobytes per minute).
`DIR/ard/` is created only when `ARD_WRITE_FILES = 1`. So `make run` and
`make replay` change nothing on the command line; the config decides.

The ARD group `239.192.0.2:4992` is deliberately distinct from the sample
stream's `239.192.0.1:4991`. `127.0.0.1:4992` is the single-viewer loopback
alternative; a unicast or broadcast address works too (`UdpSender` handles
all three already).

## 4. Wire format: the ARD stream

Little-endian throughout, like the `.bin` (aarch64 and x86 agree; stated so
nobody assumes network order from the VITA side). Two datagram types, both
1400 bytes or less.

### 4.1 Frame chunk, magic `PBRARDS1`

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `PBRARDS1` |
| 8 | 4 | `uint32` frame sequence = CPI index mod 2^32 |
| 12 | 4 | `uint32` chunk index, 0-based |
| 16 | 4 | `uint32` chunk count |
| 20 | 4 | `uint32` frame bytes in total |
| 24 | 4 | `uint32` payload bytes in this chunk |
| 28 | 4 | reserved, 0 |
| 32 | ≤1368 | payload: bytes `[index·1368, …)` of the `PBRARD01` frame |

Since 2026-10-06 the frame's detection list holds **plots**, one per group
of touching CFAR detections, at the power-weighted centroid
(`DetectionGrouper.hpp`, `ArdFile.hpp`); the raw detections go to
`detections.csv`, the plots to `plots.csv`.

A 120 × 1600 frame with N plots is 1,536,064 + 48·N bytes, so
**1123 chunks**, the last one short. 1368 = 1400 − 32, the same datagram
size as the sample stream: no IP fragmentation on a 1500 MTU, so one lost
Ethernet frame costs one chunk and never a whole datagram of fragments.

### 4.2 Status, magic `PBRARDT1`

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `PBRARDT1` |
| 8 | 4 | `uint32` frame sequence it belongs to |
| 12 | 4 | `uint32` JSON bytes |
| 16 | ≤1384 | ASCII JSON, NUL-terminated |

Sent once per streamed CPI, **after** its last chunk, so a receiver that
holds a complete frame and its status has a consistent pair. Built with
`snprintf` in `pbr_run`, no JSON library, exactly like the VITA context JSON.
Its keys are the telemetry CSV's column names plus what the page's header
strip shows, so the page needs no new field names:

```json
{"runName":"live_20260925_1530","cpi":36,"status":0,"reason":"",
 "signalTimeS":36.864,"lagS":0.012,
 "dsiMs":89.0,"ardMs":31.2,"totalMs":120.2,"suppressionDb":19.8,
 "peakDb":-3.1,"detections":11,"plots":2,"borderDetections":2,"skipped":0,
 "config":{"DSI_ALGORITHM":"ECA-CD","DSI_BACKEND":"GPU",
           "ARD_ALGORITHM":"Direct","CFAR_PFA":"1e-05","CPI_LENGTH_s":"1.024"}}
```

### 4.2a Blank frames and the status word (added 2026-09-25)

The `PBRARD01` header's word at offset 56, formerly reserved, is now
`uStatus`: 0 processed, 1 null input, 2 a stage failed (`ArdFile.hpp`).
`pbr_run` guards each CPI before the solver (a channel that is all zeros or
holds a non-finite sample) and turns a guard hit or a stage failure into a
**blank frame**: all-zero map, no detections, `uStatus` set, telemetry row
with a `status` column, and the run continues. Ten consecutive stage
failures stop the run, since that is not the data. The status JSON (§4.2)
carries the same value as `"status"` and a `"reason"` string, and the page
shows a banner over the map: "receiving, nothing to process: <reason>".
This is the user-facing behaviour asked for: a dead front end looks like a
live but empty display, never a crash or a frozen last frame.

### 4.2b Before-processing spectra, magic `PBRARDP1` (added 2026-09-30)

Sent after the frame's last chunk and before the status, one per channel,
so the viewer's before-processing spectrogram shows the CPI exactly as the
canceller received it, blank frames included. Approved in chat 2026-09-30.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `PBRARDP1` |
| 8 | 4 | `uint32` frame sequence it belongs to |
| 12 | 4 | `uint32` channel: 0 reference, 1 surveillance |
| 16 | 4 | `uint32` bins N |
| 20 | 4 | `uint32` sample rate, Hz |
| 24 | 2N | `int16` dB × 100 per bin; bin 0 is −fs/2, bin N/2 the carrier |

N = 512 (`kSpectrumBins` in `pbr_run`), so one datagram of 1048 bytes and
no fragmentation. The values come from `SpectrumEstimator`
(`include/pbr/Spectrum.hpp`): a Welch average over the CPI's consecutive
512-sample segments, Hann-windowed, plain C++ radix-2 on the host so the
Mac build has it and the GPU is not involved; about a millisecond per
channel. The assembler attaches a spectrum to the frame of the same
sequence exactly as it attaches the status; `ard_view` serves the two
datagrams back to back at `GET /api/spectrum/latest`. Levels are relative
(mean |X|²/N), comparable between CPIs and channels, not absolute.

### 4.3 Reassembly rules (the receiver)

- A chunk whose sequence differs from the frame being assembled starts a
  new frame; whatever was incomplete is dropped and counted
  (`framesDropped`). UDP loss therefore costs one frame at 1 CPI/s, which a
  viewer tolerates; it is never a shifted or corrupted picture.
- A frame is complete when every chunk index has arrived once. Duplicates
  are ignored. Out-of-order arrival is fine.
- On completion the frame's first 64 bytes are checked as a `PBRARD01`
  header and its declared size must equal the frame bytes, the same
  completeness test the Python applied to files.
- A status datagram is attached to the frame with the same sequence; a
  status arriving for a frame that is not complete is kept until the frame
  completes or a newer sequence arrives.

## 5. Sender side: `pbr_run`

Changes, all in `src/app/pbr_run.cpp` and `src/ard/`:

1. `ArdFile.hpp/.cpp` gain `ardFrameBytes(header)` and
   `serialiseArdFrame(header, map, detections, pOut, uCapacity)`.
   `writeArdFile()` becomes serialise-then-one-`fwrite`, so the file and the
   stream cannot disagree. `readArdFile()` is untouched. The existing ARD
   tests still round-trip through the file.
2. New `include/pbr/ArdStream.hpp`, `src/ard/ArdStream.cpp`: the constants
   of §4, `ArdStreamSender` (owns a `UdpSender`; `sendFrame(bytes, seq)`,
   `sendStatus(json, seq)`) and `ArdFrameAssembler` (the receiver's half:
   `feed(datagram) -> bool complete`, `frameBytes()`, `statusJson()`,
   counters). Both halves in one file so the format has one home and one
   test.
3. `pbr_run`: `bWantMap = (bWriteFiles || bStream) && (cpi % every == 0)`.
   One `std::vector<uint8_t>` frame buffer allocated at start for the
   maximum size (`kMaxDetections` detections). After CFAR: serialise; if
   writing, `fwrite`; if streaming, `sendFrame` then `sendStatus`. The
   `ardWriteMs` telemetry column keeps its name and now times both.
4. `Config.hpp/.cpp`: the three keys; `ARD_STREAM_DEST` parsed as
   `host:port` at load. `tests/test_config.cpp` fixture gains the keys.

Cost on the timed path, to be **measured**, not assumed: 1123 `sendto`
calls per CPI. On the Nano a UDP `sendto` is 5 to 10 µs, so 6 to 12 ms per
CPI against a 1024 ms budget in which the chain currently spends 80 to
230 ms. `ardWriteMs` in the telemetry shows it every run. Sends are blocking
on a 4 MiB send buffer, so a burst paces itself at wire speed and never
returns short.

## 6. Receiver side: `ard_view` (C++, replaces `ard_viewer.py`)

```
ard_view [--group 239.192.0.2] [--port 4992] [--http 8080]
         [--page src/display/ard_viewer.html] [--file FRAME.bin]
```

- **Two threads**, `std::thread`, standard library. The receive thread
  blocks in `recvfrom` and feeds the assembler; when a frame completes it
  swaps it into the published slot under one `std::mutex`. The main thread
  runs the HTTP accept loop. Shared state is three things: the newest
  complete frame's bytes, its status JSON, and the counters.
- **Memory**: the assembler's working buffer and the published buffer are
  allocated once, when the first chunk says how big a frame is, and
  reallocated only if that size changes. An HTTP request copies the
  published frame (1.5 MB, well under a millisecond) so the receiver is
  never blocked by a slow client. No per-datagram allocation. This is a
  display program with two fixed buffers, not a ring buffer; I am
  classifying it as peripheral and ask the author to confirm.
- **Socket**: `UdpReceiver` as it stands (`open`, `joinMulticastGroup`),
  but `ard_view` needs it **blocking** and with an 8 MiB receive buffer: a
  frame arrives as a 1.5 MB burst. The Jetson's `rmem_max` is already 8 MB.
  `UdpReceiver::open` sets `O_NONBLOCK` unconditionally today, so it gains a
  `bool bBlocking` argument, defaulting to the current behaviour. One line
  in `src/ingest/`, flagged here because that directory is "do not disturb".
- **HTTP**: a minimal GET-only HTTP/1.1 server on POSIX sockets, in
  `src/display/HttpServer.cpp` with `include/pbr/HttpServer.hpp`: `listen`,
  `accept`, read the request line, dispatch on the path, write
  `Content-Type`, `Content-Length`, `Cache-Control: no-store`,
  `Connection: close`, body. One request per connection, handled inline;
  5 s send and receive timeouts on the accepted socket so a stalled client
  cannot hang the loop. About 150 lines, no dependency. Endpoints exactly as
  the Python served them, minus per-CPI history:
  - `GET /` the page, read from `--page` at each request (so editing the
    HTML needs no restart);
  - `GET /api/status` → `{"runName","latestCpi","telemetry":{…},
    "config":{…},"framesDropped","chunksReceived","serverTime"}`, where
    `telemetry` and `config` are relayed from the status datagram;
  - `GET /api/frame/latest` → the frame bytes, `X-Pbr-Cpi` header; 404 until
    the first frame completes.
  Anything else is 404. No file system access beyond `--page`.
- `--file FRAME.bin` serves one recorded frame with a synthetic status, for
  working on the page on the Mac without a stream (`make host-view`).
- **Where it runs**: on the board, as now, with the browser reached over
  the ssh port forward or directly; **or on the Mac**, which joins the
  group across the switch and serves `localhost:8080`. Same binary; `make
  host` builds it (no CUDA in it). Several viewers may join the group at
  once, which is the multiple-clients requirement applied to the output.

## 7. The page

`scripts/ard_viewer.html` moves to `src/display/ard_viewer.html`, since
`src/display/` is the ARD viewer stage and that is what it now is. Changes
are confined to the header strip: `runName` comes from the status, the
"stale" banner keys off `latestCpi` as before, and a "frames dropped"
count is added. Parsing, colour scale, axes, detections, polling: unchanged.

## 8. Retired, renamed, added

| | |
|---|---|
| retire | `scripts/ard_viewer.py`, `tests/test_ard_viewer.py`, the Python hook in `make host-test` |
| move | `scripts/ard_viewer.html` → `src/display/ard_viewer.html` |
| add | `include/pbr/ArdStream.hpp`, `src/ard/ArdStream.cpp`, `include/pbr/HttpServer.hpp`, `src/display/HttpServer.cpp`, `src/app/ard_view.cpp`, `tests/test_ard_stream.cpp` |
| change | `ArdFile.hpp/.cpp`, `pbr_run.cpp`, `Config.hpp/.cpp`, `UdpReceiver` (blocking flag), `config/prb.conf`, `config/fers.conf`, `tests/test_config.cpp`, `Makefile` (`view`, `host-view`, host lists), `docs/ard-viewer.md` (pointer to this note) |
| keep | `scripts/replay_udp.py` (a test tool; removed 2026-10-06 with the other test scripts) |

`make view` becomes: stop any old `ard_view` on the board, start
`./build/ard_view --group 239.192.0.2`, forward 8080. `make host-view`
becomes `bin/ard_view --file data/ard/ard_00000.bin`. New: `make host`
builds `ard_view`, so a Mac on the switch can run `bin/ard_view --group
239.192.0.2` and watch the board directly.

## 9. Tests and acceptance

1. `tests/test_ard_stream.cpp` (Mac and Jetson, plain asserts):
   serialise a frame and byte-compare it with what `writeArdFile` puts on
   disk; chunk it and reassemble in order, in reverse order, with a
   duplicate, with one chunk missing (frame dropped, counted), and with a
   newer sequence interrupting (old one dropped); status attaches to the
   right sequence; a chunk with a bad magic or an index past the count is
   rejected.
2. Mac: `bin/ard_view --file data/ard/ard_00000.bin`, browser shows the
   fixture frame (proves HTTP server and page).
3. Jetson: `make replay` with `fers.conf` (files **and** stream on) while
   `make view` runs: browser follows the replay; `ard/` files appear;
   `ardWriteMs` in `telemetry.csv` records the cost.
4. Jetson: `make run` with `prb.conf` (stream only) from the synthetic
   driver: browser shows the planted target; **no `ard/` directory is
   created**; disk usage of the run directory stays in kilobytes.
5. Mac: `bin/ard_view --group 239.192.0.2` on the switch during step 4, a
   second viewer of the same run.

Accept when 1 to 4 pass and the worst `totalMs + ardWriteMs` stays inside
the CPI budget with the lag flat, as `pbr_run`'s own PASS line requires.

## 10. Escalations, not built now

- If `ardWriteMs` is material: stream magnitude only as `uint16` dB
  (a quarter of the bytes, 281 chunks) under a new magic, and teach the
  page a second parser. Needs a measurement first.
- If Wi-Fi viewers lose frames: run `ard_view` on the board and let the
  browser pull over HTTP, which is what the design already does; the
  multicast hop stays on the wired switch.
- Frame history in `ard_view` (a small ring of the last N frames) if
  scrubbing back is ever wanted. That would be a ring buffer, hence central,
  hence its own proposal.

## 11. Questions for the author

1. Stream the file bytes verbatim (1.5 MB per CPI, one format) as proposed,
   or a magnitude-only stream from the start?
2. Defaults as in §3: USRP config stream-only, FERS config both?
3. Confirm `ard_view`'s two fixed buffers count as peripheral.
4. Confirm the one-argument change to `UdpReceiver::open` in `src/ingest/`.
5. Retire the Python viewer and its test outright, or keep them one more
   session as a fallback?
