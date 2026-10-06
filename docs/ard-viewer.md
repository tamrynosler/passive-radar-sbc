# ARD web viewer — design note (v1, 2026-09-22)

> **Superseded 2026-09-25.** The Python sidecar and its file tailing are
> gone. `pbr_run` now streams each frame over UDP multicast and the C++
> `ard_view` (`src/app/ard_view.cpp`) serves the same page from memory; see
> `docs/ard-stream-design.md`. The page's rendering rules below (format,
> axes, colour scale, polling) still apply; the page lives in
> `src/display/ard_viewer.html`.

A sidecar that lets a browser watch a `pbr_run` run as it happens. It reads
the files `pbr_run --out DIR` already writes and serves them over HTTP; the
browser does every pixel of the rendering. Nothing in the C++ changes.

**Status:** v1 verified on the board (Python 3.6.9) with `make replay` and
with a live FERS run, 2026-09-22. The viewer followed the new run on its
own and kept pace at 1 CPI/s. Replay looks laggy because `pbr_run --file`
outruns the browser's 1.5 MB-per-frame fetch; that is the transport, not
the chain.

## Why a sidecar, why the browser renders

- **Zero risk to the timed path.** `pbr_run` never knows the viewer exists.
  It cannot add latency to a CPI and it cannot crash the chain.
- **Zero new dependencies.** Python 3 standard library on the board, a
  browser on whatever machine is handy. No pip, no third_party headers, no
  GUI toolkit on the Jetson, no CMake changes.
- **Zero compute on the Jetson.** The board's four A57 cores are shared with
  `pbr_run`. So the sidecar does not touch the map: it validates a file and
  sends its bytes. dB conversion, colour mapping and axis drawing happen in
  JavaScript, where the cycles are free. A frame is 1.5 MB at 1 CPI/s —
  nothing on Ethernet, fine on Wi-Fi.
- **Same path for live and replay.** `make run` and `make replay` write the
  same directory layout, so the viewer is identical for both.

## Files

    scripts/ard_viewer.py     HTTP server. Stdlib only.
    scripts/ard_viewer.html   the page: one file, inline CSS and JS, no
                              external resources (the board's LAN may have
                              no internet, and the page is served from it).
    tests/test_ard_viewer.py  plain main()+assert test, Mac only, run by
                              `make host-test`.
    Makefile                  `make view`, `make host-view`, host-test hook.

(This plan put the viewer in `scripts/`; `docs/ard-stream-design.md` later
moved it to `src/display/` as the C++ `ard_view`.)

## Board constraints the code must respect

The Jetson runs L4T on Ubuntu 18.04: **Python 3.6**. numpy is not assumed.
Anything newer than 3.6 is a runtime error on the board and invisible on the
Mac (3.9). Forbidden: `http.server.ThreadingHTTPServer` (3.7 — use
`socketserver.ThreadingMixIn` + `HTTPServer`), `dataclasses`, the walrus
operator, positional-only parameters, `dict | dict`, `str.removeprefix`,
`list[int]`-style builtin generics, `math.dist`, `importlib.resources`.
f-strings are fine (3.6).

## Input: what `pbr_run --out DIR` produces

    DIR/ard/ard_NNNNN.bin    one map per CPI, NNNNN = CPI index (%05d)
    DIR/telemetry.csv        one row per CPI, header row first
    DIR/detections.csv       not used by the viewer (detections are in .bin)
    DIR/prb.conf             copy of the config, written by run_live.sh and
                             the `make replay` target (may be absent)

Runs live under `/data/results/live_<stamp>/` and
`/data/results/replay_<algo>_<stamp>/`. On the Mac, `data/` has the same
layout (`data/ard/`, `data/telemetry.csv`) and is the local test fixture.

### `.bin` format (PBRARD01, from `include/pbr/ArdFile.hpp`)

Little-endian throughout (aarch64 and x86 agree).

    offset  size  contents
         0     8  magic "PBRARD01"
         8     8  uint64 uRangeBins            R
        16     8  uint64 uDopplerBins          D
        24     8  uint64 uSampleRateHz         fs
        32     8  uint64 uDopplerDecimation    dec
        40     8  uint64 uCpiStartIndex
        48     8  uint64 uDetections           N
        56     8  reserved
        64  8*R*D map: interleaved float32 I,Q; map[doppler*R + range],
              range fastest (display layout, no transpose needed)
      then  48*N detections, each: uint64 rangeBin, uint64 dopplerBin,
              float64 rangeKm, float64 dopplerHz, float64 powerDb,
              float64 snrDb

    expected file size = 64 + 8*R*D + 48*N

Current parameters: R = 120, D = 1600, fs = 204800, dec = 128 → 1,536,064 B
plus detections.

Axes (from `matlab/play_ard.m`, which is the display convention):

    range_km(r)   = r * c / fs / 1000,           c = 299792458
    binHz         = fs / (dec * D)
    doppler_Hz(d) = (d - D/2) * binHz            row 0 is the MOST NEGATIVE
                                                 Doppler (fftshift applied)

Display: range on x, Doppler on y, positive Doppler UP (`axis xy`). Canvas y
grows downward, so row d is drawn at pixel row (D_crop - 1 - d_local).

### The write is not atomic

`writeArdFile` is `fopen`/`fwrite`/`fclose`. A file can be seen half-written.
The server therefore treats a frame as complete only when its size equals the
expected size computed from its own header, and serves the newest COMPLETE
frame. A short file is simply the one still being written; skip it and use the
previous one.

## Server: `scripts/ard_viewer.py`

    python3 scripts/ard_viewer.py [--root DIR] [--dir DIR] [--port N] [--host H]

- `--root DIR` (default `/data/results`): watch this directory and always
  show the newest run — the subdirectory with the latest mtime that contains
  an `ard/` directory. Re-evaluated on every status request, so starting a
  new `make run` switches the browser to it with no action from the user.
- `--dir DIR`: pin one run directory (a `pbr_run --out` dir). Overrides
  `--root`. Used for local testing: `--dir data`.
- `--port` default 8080. `--host` default `0.0.0.0`.
- `ThreadingMixIn` so a slow client cannot block the status poll.
- Logs one line per request to stderr is too noisy at 2 Hz; log startup,
  run-dir switches and errors only.

### Endpoints

`GET /` — serves `ard_viewer.html` from the script's own directory.

`GET /api/status` — JSON:

    {
      "runDir":     "/data/results/live_20260922_101500",   // or null
      "runName":    "live_20260922_101500",
      "frameCount": 37,
      "latestCpi":  36,                      // newest COMPLETE frame, or null
      "telemetry":  { "cpi": 36, "lagS": 0.31, "dsiMs": 231.0, ... },  // last
                                             // row of telemetry.csv keyed by
                                             // its header, values as numbers;
                                             // null if the file is absent
      "config":     { "DSI_ALGORITHM": "ECA-S", "DSI_BACKEND": "GPU",
                      "ARD_ALGORITHM": "Direct", "CFAR_PFA": "1e-5",
                      "CPI_LENGTH_s": "1.0" },   // from DIR/prb.conf, strings,
                                                 // only these keys; null if absent
      "serverTime": 1758530000.0
    }

telemetry.csv is read from the tail (seek to end - 4096, take the last full
line), never whole. prb.conf is parsed with the rules in its own header:
`KEY = VALUE`, `#` comments, whitespace stripped.

`GET /api/frame/latest` — the newest complete `.bin`, verbatim,
`Content-Type: application/octet-stream`, `X-Pbr-Cpi: <n>`. 404 if no
complete frame exists yet.

`GET /api/frame/<n>` — that CPI's `.bin`, same rules. 404 if absent or
incomplete. `<n>` must parse as a non-negative int; anything else is 404.
No other paths are served — no directory listing, no arbitrary files.

## Page: `scripts/ard_viewer.html`

Layout, top to bottom:

1. **Header strip**: run name; CPI index; signal time; lag (s); DSI ms;
   ARD ms; total ms; suppression dB; peak dB; detections (border count in
   brackets); `DSI_ALGORITHM / DSI_BACKEND / ARD_ALGORITHM / PFA`. Values
   come straight from `/api/status`. Show `—` for anything missing. Show
   "waiting for frames…" when `latestCpi` is null, and "stale — no new CPI
   for N s" once the latest CPI has not changed for more than 3 s.
2. **The map**: a `<canvas>`, range on x, Doppler on y, positive up. Render
   one pixel per bin into an offscreen canvas of size R × D_crop, then
   `drawImage` it scaled onto the display canvas with
   `imageSmoothingEnabled = false` (blocky like `imagesc`, not blurred).
   Axis ticks and labels: bistatic range (km), bistatic Doppler (Hz).
   Detections drawn as small white rings at their bin indices (use the
   uint64 bins from the record, not km/Hz — exact).
3. **Controls**: Doppler window ±fdMax Hz (number input, default 400, the
   `play_ard` default); colour floor dB (default −50, matching `caxis([-50
   0])`); the 0 dB reference with a **Reset** button.

Colour scale: dB = 20·log10(|cell|) − ref. `ref` is the running maximum
frame peak seen since the page loaded (or since Reset). This mirrors what
`play_ard` does with its run-wide peak: a quiet frame must look quiet, not be
stretched to look as busy as one with an aircraft in it. Display the current
`ref` value beside the button. Colormap: a turbo-like 256-entry LUT built
once (the Google turbo polynomial approximation is fine; a 5–7 anchor linear
interpolation is also fine).

Parsing in JS: `DataView` over the response `ArrayBuffer`, little-endian.
uint64 fields: high * 2^32 + low as a Number (exact below 2^53;
`uCpiStartIndex` is ~1e14, so the low word alone is NOT enough). Map:
`new Float32Array(buf, 64, 2*R*D)`. Detection records at
`64 + 8*R*D`, stride 48.

Polling: `setInterval` on `/api/status` at 500 ms. Fetch
`/api/frame/latest` only when `latestCpi` changes. Never two frame fetches in
flight. If the run directory name changes, reset the running peak.

Everything inline. No CDN, no fonts, no frameworks. Plain functions, no
classes, comments where the format matters.

## Makefile

    view:      ssh -L 8080:localhost:8080 $(JETSON) \
                   'cd $(REMOTE) && python3 scripts/ard_viewer.py --root /data/results'
               then open http://localhost:8080 on the Mac.
    host-view: python3 scripts/ard_viewer.py --dir data --port 8080
               (local development against the fixture in data/)

The port forward is deliberate: the board's firewall blocks ICMP and may
block inbound 8080 too; forwarding over the existing ssh path needs no board
configuration. When the firewall allows it, `http://<board-ip>:8080` works
directly from any device on the subnet — that is the lab-demo path.

### How to use it

Every `make` target runs **on the Mac**. Never run `make` on the Jetson: the
Makefile is the Mac-side driver and does the ssh for you.

    Terminal 1 (Mac):  make sync          # after any edit to scripts/
                       make view          # stops any old viewer on the board,
                                          # starts a fresh one, forwards 8080
    Browser   (Mac):   http://localhost:8080
    Terminal 2 (Mac):  make replay        # or make run, then start fers-cli

The page shows the newest run under `/data/results`. With nothing running it
shows the last run's final frame and the stale banner — that is the idle
state, not a fault. Frames appear as soon as terminal 2 starts writing them.

`make host-view` serves the three recorded frames in `data/` on the Mac
itself, for working on the page without the board. It is static: the CPI
counter stops at 2.

## Test: `tests/test_ard_viewer.py`

Plain `main()` with asserts, importing the server module from `scripts/`.
Runs on the Mac against `data/`; prints a skip and exits 0 if `data/ard` is
absent (the fixture is gitignored and never on a fresh clone).

1. Header parse of `data/ard/ard_00000.bin`: R = 120, D = 1600,
   fs = 204800, dec = 128, expected size == actual size.
2. Completeness: copy a frame to a temp run dir and truncate it by 1000
   bytes; `newest complete frame` must skip it and return the previous one.
   With only the truncated file present, the result is None.
3. Run selection under a temp root with two subdirs: the one containing
   `ard/` and the newer mtime wins; a subdir without `ard/` is ignored.
4. Telemetry tail parse of `data/telemetry.csv`: keys match the header,
   values are floats, `cpi` is the last row's.
5. prb.conf parse: comments, whitespace, only the listed keys.
6. Server in a thread on port 0 against a temp run dir copied (or
   symlinked) from `data/`: `GET /` is HTML; `/api/status` decodes and
   `latestCpi` is the max index; `/api/frame/latest` bytes equal the file
   and `X-Pbr-Cpi` matches; `/api/frame/1` equal to `ard_00001.bin`;
   `/api/frame/999`, `/api/frame/abc`, `/etc/passwd`, `/api/nope` are 404.

There is no JS test runner on the Mac (no node), so the page is verified by
desk-check against the format table above, then by eye in a browser.

## Out of scope for v1 (ordered next steps)

1. Server-side Doppler crop (`?fd=400`) — rows are contiguous, so it is a
   byte-range slice; halves the bandwidth. Only if Wi-Fi proves marginal.
2. Detection history panel (accumulated range–Doppler points, coloured by
   time) — `matlab/plot_detection_history.m` defines the look.
3. Latency sparkline from telemetry (`totalMs`, `lagS` over the run) — the
   thesis-relevant view of "is it keeping up".
4. A run launcher (start/stop `pbr_run`, pick algorithm). The config is read
   at run time, so this is a wrapper around the binary, not a change to it.
5. Replace the file tail with a socket feed from `pbr_run` when latency below
   one CPI matters. The page does not change.
