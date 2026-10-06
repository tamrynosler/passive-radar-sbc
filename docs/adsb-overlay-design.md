# ADS-B overlay on the range-Doppler map (design, v1)

Written 2026-09-30, before any code. **Status: proposal, scheduled in
`PLAN.md` ("Then: geometry in the config, and ADS-B truth") for after the
antennas and the first live runs; §10 questions 2 and 3 are settled, the
rest still open.** Revised 2026-10-01 with the geometry trace (§4.6).
Companion to `ard-stream-design.md` (the frame stream and the page) and
`live-parameters.md` (the viewer's proxy to the chain). Peripheral code throughout: display, an HTTP client, a small table.
No ring buffer, nothing on the chain's timed path.

## 1. What the author asked for

Aircraft heard by an RTL-SDR dongle through `dump1090` drawn as green dots
on the range-Doppler map, at the bistatic range and Doppler the radar
*should* see them at. Hovering a dot shows everything known about that
aircraft. Later, the same data validates detections offline.

## 2. Design in one paragraph

`dump1090` keeps decoding on whichever machine holds the dongle and serves
its aircraft table as JSON on its HTTP port. `ard_view` gains one thread
that polls that JSON once a second, keeps its own table keyed by ICAO
address, and, for each aircraft with a valid position, computes the
bistatic range and bistatic Doppler from the transmitter, receiver and
aircraft positions and the aircraft's velocity. It serves the result at
`GET /api/adsb`. The page fetches it alongside `/api/status`, draws one
green dot per aircraft on the map panel with the existing `binToX` /
`binToY`, and on mouse-over shows a tooltip. Geometry lives in one C++
file with its own asserts; the page only draws. Nothing in `pbr_run`
changes.

Why in `ard_view` and not the page: the fork of `dump1090` in use sends no
`Access-Control-Allow-Origin` header, so a page served from one host cannot
fetch JSON from another; `ard_view` already has an HTTP client
(`httpRequest`) and already proxies to the chain, so a second upstream is
the same pattern; and the geometry is then testable with plain asserts on
the Mac, which JavaScript in a browser is not.

## 3. Data source: `dump1090 --net`

`GET /data.json` on its HTTP port (8081 here; 8080 collides with `make
view`) returns one object per aircraft heard in the last 60 s:

```json
{"hex":"0083af", "squawk":"1234", "flight":"SAA321  ", "lat":-33.981200,
 "lon":18.601100, "validposition":1, "altitude":37000, "vert_rate":-64,
 "track":93, "validtrack":1, "speed":455, "messages":412, "seen":1}
```

Units as ADS-B gives them: altitude in feet (barometric), speed in knots
(ground speed), track in degrees true, vertical rate in feet per minute,
`seen` seconds since the last message of any kind. Parsed by hand exactly
as `controlPortFrom` parses the status JSON: find the key, read the number.
No JSON library. An aircraft without `validposition` is kept in the table
(the tooltip list still shows it) but gets no dot.

Polled at 1 Hz, the frame rate. A poll that fails (dump1090 not running,
host unreachable) is counted and retried next second; the page shows
"ADS-B: no source" in the header strip, never an error dialog. The 5 s
timeout already in `httpRequest` bounds a stalled poll. Rule 6 of
`live-parameters.md` holds: nothing ends the program.

## 4. Geometry: from position to a map cell

Inputs, from the config (§5): transmitter T and receiver R as latitude,
longitude, height; the carrier frequency. Per aircraft A: latitude,
longitude, altitude, ground speed, track, vertical rate.

1. Convert T, R and A to Earth-centred Cartesian (WGS-84 ellipsoid, the
   standard closed form, ten lines). Altitude is barometric, so A is off by
   the local pressure offset, a few hundred metres at most: at the ranges
   in question that is a fraction of a bin.
2. Bistatic range, the quantity on the map's x axis:
   `Rb = |A−T| + |A−R| − |T−R|`, metres. Range bin `= Rb / (c / fs)`, with
   `c / fs` = 1499 m at 200 kHz. The map holds bins 0..119, so 0..180 km.
3. Velocity vector: east `v·sin(track)`, north `v·cos(track)`, up the
   vertical rate, rotated to Earth-centred at A's latitude and longitude.
4. Bistatic Doppler, the y axis: the rate of change of `Rb` divided by the
   wavelength, `fd = −(v · û_TA + v · û_RA) / λ`, where `û_TA` is the unit
   vector from T to A and `û_RA` from R to A, `λ = c / carrier`. Sign as
   written makes an aircraft closing on both sites positive. **Checked on
   2026-10-01 against the FERS scenario** (§4.6): the chain's positive
   Doppler is a closing target, so this formula and the map agree with no
   sign flip. The USRP spectrum-orientation test (`PLAN.md`, antennas item
   2) can still mirror the live Doppler axis; range is unaffected either
   way.
5. Doppler bin `= fd / binHz + dopplerBins / 2` (row 0 is the most negative
   Doppler, as the page's `dopplerHz` says). Both bin values are floats; the
   page draws them fractionally through `binToX` / `binToY` so a dot sits at
   the true position, not snapped to a cell.

6. **Why the baseline is subtracted, and why it is never a config key.**
   The chain correlates the surveillance channel against the reference,
   and the reference already carries the direct signal's delay `|T−R| / c`.
   So delay bin 0 means "arrived together with the direct signal", which
   is bistatic range equal to the baseline, the shortest range physically
   possible. The map's axis is therefore `Rb` as written in step 2, with
   `|T−R|` subtracted, and nothing in map formation needs the geometry.
   Hand calculation from the `.fersxml` against what the chain reported for
   the first FERS CPI (`design_decisions.md` 1e.6):

   | | from the `.fersxml` | chain |
   |---|---|---|
   | baseline `\|T−R\|` | 74.48 km | not used |
   | `\|A−T\| + \|A−R\|` at t = 0 | 142.27 km | not shown |
   | `Rb` | 67.79 km, bin 46.3 at 204800 Hz | 67.34 km, bin 46 |
   | `fd` at t = 0, closing positive | +99.1 Hz | +99 Hz |

   Both axes land on the simulated target with the baseline subtracted.
   The baseline is derived from the two positions at load time, never
   stored as its own key: two sources of one number is how a golden file
   silently stops matching the run it is compared against.

Scale of things at 89 MHz (`λ` = 3.37 m): an airliner at 250 m/s closing
on both sites gives at most 148 Hz, inside the ±200 Hz the chain computes.
A 2000 ft/min climb moves the Doppler by up to 6 Hz, six bins, so the
vertical rate is used, not ignored. One second of position age at 250 m/s
is 250 m, a sixth of a range bin, so the 1 Hz poll and `seen` age are
fine without extrapolation; the dot is dead-reckoned forward by `seen`
seconds anyway because a position is only refreshed every 0.5 to 5 s and
the arithmetic is one multiply.

Aircraft outside the map (range beyond bin 119, or Doppler outside the
computed span) are not drawn; they remain in the `/api/adsb` list with an
`onMap` flag so the page can count them in the header ("ADS-B: 7 aircraft,
3 on map"). A `validposition` aircraft is on the map whenever its bin lands
in range, whatever its line-of-sight to the radar: the point of the overlay
is to show where the radar *should* see it, including when it does not.

## 5. Config keys

Read by `ard_view` through a new `--config` argument (the Makefile's
`view` target passes the same file `run` does), and the two positions and
the carrier also by `pbr_run` and the driver, as below. `pbr_run` ignores
the keys it does not know, per the config policy, so nothing else changes. All are
required when `ADSB_SOURCE` is set; `ADSB_SOURCE` empty disables the
feature and the other keys may be absent.

| Key | Meaning | Example |
|---|---|---|
| `ADSB_SOURCE` | host:port of `dump1090`'s HTTP server | `<mac-ip>:8081` |
| `TX_LAT_deg`, `TX_LON_deg`, `TX_HEIGHT_m` | FM transmitter site | station mast |
| `RX_LAT_deg`, `RX_LON_deg`, `RX_HEIGHT_m` | the surveillance antenna | the roof |
| `CARRIER_FREQ_Hz` | station carrier, sets `λ` | `89000000` |

`CARRIER_FREQ_Hz` duplicates the driver's `--fc`. To stop them drifting
apart the driver reads this key too and takes `--fc` as an override
(settled 2026-10-01, `PLAN.md` A.5; one line in `uhd_to_vita49`, not done).

`pbr_run` also reads the two positions, for one startup line: the baseline
in km and the absolute bistatic range the map covers. Nothing on the timed
path changes. The viewer may offer an absolute-range axis label (add the
baseline) as a display option.

**The `.fersxml` gives projected metres (UTM-like x, y, altitude), not
latitude and longitude.** For `fers.conf` either convert the three FERS
points to lat/lon once by hand, or let the geometry code accept local
Cartesian metres as well. Open, `PLAN.md` A.2; the sign fixture in §8.1
needs one or the other.

## 6. `ard_view` changes

- `include/pbr/Adsb.hpp`, `src/display/Adsb.cpp`: `struct AdsbAircraft`
  (the JSON fields plus `dRangeKm`, `dDopplerHz`, `dRangeBin`,
  `dDopplerBin`, `bOnMap`, `dWallTime`); `parseDump1090Json(text, out)`;
  `geodeticToEcef`; `bistaticRangeDoppler(geometry, aircraft, out)`;
  `adsbToJson(table, out)`. Plain functions, no CUDA, in the host build.
- `ard_view.cpp`: a third thread, `std::thread` like the receiver, loop:
  `httpRequest(GET /data.json)` → parse → project with the frame's
  `sampleRateHz`, `dopplerBins`, `dopplerDecimation` (taken from the
  newest published frame, so a live CPI change re-projects correctly) →
  swap the table into the shared state under the existing mutex → sleep
  1 s. Counters: polls, failures, aircraft.
- `GET /api/adsb` → `{"source":"…","ok":true,"aircraft":[…],"polls":N,
  "failures":N}`. Aircraft sorted by ICAO so the list is stable.
- `--adsb-log FILE`: one CSV row per aircraft per poll (wall time, latest
  CPI seen, ICAO, callsign, lat, lon, alt, speed, track, vert rate, range
  km, Doppler Hz, on-map flag). Kilobytes a minute. This is the file the
  offline validation reads later; without it the overlay is only a picture.

## 7. The page

- `pollStatus` also fetches `/api/adsb` (same `POLL_MS`, failures ignored)
  and keeps `state.adsb`.
- `drawMapPanel` and `drawDetectionPanel` end with `plotAdsb(ctx, frame,
  win, box)`: for each `onMap` aircraft, a green filled circle of radius 4
  at `binToX(frame, dRangeBin − 0.5, box)`, `binToY(win, dDopplerBin −
  0.5, box)` (the −0.5 undoes the bin-centre offset the two helpers add,
  since these are already fractional positions). Age dims it: `seen` over
  10 s draws the outline only. The callsign, or the ICAO when there is
  none, is written beside the dot in small text.
- Hover: a `mousemove` listener on each map canvas finds the nearest dot
  within 10 px and shows an absolutely positioned `<div id="tip">` with:
  ICAO, callsign, squawk; latitude, longitude, altitude ft; ground speed
  kt, track °, vertical rate ft/min; computed bistatic range km and
  Doppler Hz; messages, seen s. `mouseleave` hides it. Canvas has no
  per-point `title`, hence the div.
- Header strip: "ADS-B: N aircraft, M on map" or "ADS-B: no source".
- A checkbox "ADS-B aircraft" in Display, on by default when the source is
  set.

Nothing existing is redrawn differently. The red CFAR dots stay red;
a red dot inside a green one is the picture this is for.

## 8. Tests and acceptance

1. `tests/test_adsb.cpp` (Mac and Jetson, plain asserts):
   - `geodeticToEcef` against a textbook point (equator/prime meridian gives
     (a, 0, 0); the pole gives (0, 0, b)).
   - Aircraft on the line beyond R, moving away from both: `Rb` equals
     `2·|A−R|`, `fd = −2v/λ` within 1e-6.
   - Aircraft stationary: `fd = 0`. Aircraft moving perpendicular to both
     lines of sight: `fd = 0`.
   - Aircraft at the midpoint of the baseline: `Rb = 0`.
   - `parseDump1090Json` on the sample of §3 and on `[]`, and on a
     truncated string (returns false, table unchanged).
   - Sign check against the FERS scenario: the target's `.fersxml` position
     and velocity through `bistaticRangeDoppler` must land on the peak the
     replay produces, in both bins: bin 46 and +99 Hz at t = 0 (§4.6). The
     hand calculation already agrees; this test pins the code to it. It
     needs the FERS scenario's Tx/Rx positions as the geometry, not the
     real station.
2. Mac: `bin/ard_view --file data/S0_clean/ard/ard_00000.bin --config config/fers.conf`
   with `dump1090` running: dots appear on the fixture frame, hover works.
   Also `--adsb-source` pointed at a small Python that serves a fixed
   `data.json`, so the page can be exercised with no dongle.
3. Jetson: `make view` with the real keys in `prb.conf` during a live run;
   green dots move across the map at ~1 Hz; the log file fills.
4. Accept when 1 to 3 pass and, on the first real aircraft, a red CFAR
   detection sits within a couple of bins of a green dot. That is also the
   moment the overlay becomes the validation tool.

## 9. Not built now

- Automatic association of CFAR detections to aircraft (nearest within a
  gate) and a per-aircraft hit rate: the offline validation, from the log.
- Drawing aircraft on a map of the ground (lat/lon): FlightRadar24 does it.
- Predicted Doppler trails on the after-processing spectrogram.
- A second station: the geometry takes one transmitter.

## 10. Questions for the author

1. **Where does the dongle live?** On the Mac, with `ard_view` on the Nano
   polling the Mac at `<mac-ip>:8081` across the switch; or on the
   Mac with `ard_view` also on the Mac (`make host`, it joins the multicast
   group); or plugged into the Nano, with `librtlsdr` and `dump1090` built
   there. The design is the same; only `ADSB_SOURCE` and the install differ.
   Note the V3 running hot is normal: its aluminium case is the heatsink
   and it is rated to run like that indefinitely.
2. **Transmitter and receiver coordinates.** Settled 2026-10-01: latitude,
   longitude and height for both go in `prb.conf` (placeholders and a
   `TODO` until the station and roof are fixed) and in `fers.conf` for the
   scenario; the baseline is derived, never a key. Still open: which FM
   station, and the `.fersxml` metres-to-lat/lon question of §5.
3. **Carrier frequency key.** Settled 2026-10-01: add `CARRIER_FREQ_Hz`,
   the driver reads it, `--fc` overrides.
4. **Off-map aircraft.** Count them in the header only, as proposed, or
   list them in a small table under the panel with their computed range
   and Doppler so you can see what is coming into view?
5. **Which panels.** Map and Detections, as proposed, or the map only?
6. **Log file.** `--adsb-log` in `ard_view` as proposed, or should the log
   live with the run directory that `pbr_run --out` writes? The two
   programs may be on different machines, which is why I put it in the
   viewer.
7. **Tooltip content.** The list in §7, or fewer fields?
