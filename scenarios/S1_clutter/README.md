# S1_clutter

S0_long with stationary and slow-moving clutter added: the one aircraft,
transmitter, receivers, antennas, noise and 600 s waveform are unchanged,
so `data/S1_clutter/truth.csv` is identical to S0_long's. The clutter
platforms are FERS targets named `Clutter...`, which `matlab/fers_truth.m`
leaves out of the truth.

## What it tests

The cancellers differ only on clutter: the number of taps against the
clutter extent, batch length against conditioning, ECA-CD's delay-spread
error, and what each one does to clutter a hertz or two off zero Doppler.
S0 only ever had bin 0 to remove.

## Stationary clutter

Real peaks around Cape Town and Paarl, placed from their map coordinates
(WGS84 to UTM 34S, the scenario's frame; about 0.5 km from the scenario's
own transmitter coordinate, a third of a range bin), one to three point
scatterers along each crest so a range fills more than one bin. FERS has
point scatterers only (Brooker 2011), so a mountain is a short line of
points. Bins cover 0 to 60; the 2 to 60 band is the one the test asks for,
Table Mountain and Tygerberg sit on the baseline at bins 0 to 2.

The surveillance Yagi (sinc pattern, boresight 125 deg, elevation 10 deg)
weights each scatterer by its azimuth: about 0 dB near boresight, 7 dB
down at 45 deg off, 14 dB at 60 deg, 30 dB at 80 deg. The Paarl,
Du Toitskloof and Hottentots-Holland group is therefore the strong
clutter; Tygerberg, the coast and Kasteelberg are weak. RCS steps from
1e4 to 1e5 m^2 per point, 17 to 27 dB above the 200 m^2 target before the
pattern. Altitudes are the peak heights.

| Scatterer | Bin | Azimuth from Rx (deg) | RCS (m^2) | Altitude (m) |
|---|---|---|---|---|
| TableMountain | 0.3 | 206 | 30000 | 1085 |
| Tygerberg2 | 1.0 | 193 | 20000 | 330 |
| Tygerberg1 | 1.8 | 192 | 20000 | 460 |
| MelkbosRidge | 3.5 | 225 | 10000 | 120 |
| Paardeberg1 | 5.6 | 159 | 30000 | 500 |
| Paardeberg2 | 7.4 | 148 | 30000 | 615 |
| Bottelary1 | 7.8 | 179 | 20000 | 400 |
| Paardeberg3 | 9.6 | 136 | 30000 | 730 |
| Bottelary2 | 11.9 | 170 | 20000 | 500 |
| Porseleinberg1 | 13.8 | 100 | 20000 | 500 |
| PaarlMountain1 | 16.5 | 150 | 50000 | 600 |
| Simonsberg1 | 17.2 | 161 | 50000 | 1200 |
| PaarlMountain2 | 17.4 | 144 | 50000 | 730 |
| Kasteelberg1 | 17.7 | 59 | 30000 | 800 |
| Simonsberg2 | 19.6 | 156 | 50000 | 1390 |
| Porseleinberg2 | 22.8 | 82 | 20000 | 600 |
| Kasteelberg2 | 25.1 | 57 | 30000 | 966 |
| Helderberg | 27.0 | 167 | 50000 | 1137 |
| Limietberg | 27.6 | 118 | 50000 | 1200 |
| Jonkershoek | 30.1 | 157 | 50000 | 1500 |
| DuToitskloof2 | 33.9 | 135 | 100000 | 1700 |
| DuToitskloof3 | 34.3 | 143 | 100000 | 1600 |
| DuToitskloof1 | 35.0 | 128 | 100000 | 1995 |
| HottentotsHolland2 | 39.3 | 165 | 100000 | 1100 |
| HottentotsHolland1 | 40.8 | 159 | 100000 | 1590 |
| Witzenberg1 | 44.8 | 104 | 100000 | 1500 |
| Witzenberg2 | 52.3 | 96 | 100000 | 1750 |
| Witzenberg3 | 60.3 | 89 | 100000 | 2000 |


## Slow-moving clutter, 1 to 2 Hz

At 89 MHz one hertz of bistatic Doppler is 3.37 m/s of bistatic
range-rate, so these are vehicles and a ship at 5 to 6.5 m/s on straight
paths at constant speed over the 600 s, heading chosen so the Doppler is
the value in the table (closing is positive). Each drifts by one or two
range bins over the run. The ship is 85 deg off boresight and weak; the
two trucks are near boresight. Wind-blown vegetation, the other real
source of 1 to 2 Hz spread, is left out: FERS cannot give a point
scatterer a spectral width.

| Mover | Bin at 300 s | Speed (m/s) | Heading (deg) | Doppler (Hz) | RCS (m^2) |
|---|---|---|---|---|---|
| ShipMelkbos | 4.3 | 5.1 | 145 | +1.0 | 1000 |
| BakkieSwartland | 7.1 | 6.5 | 327 | +1.3 | 20 |
| TruckWellington | 21.3 | 5.5 | 40 | -1.6 | 100 |
| TruckDuToitskloof | 36.1 | 6 | 47 | -2.0 | 100 |

## Regenerate

The scenario file was generated 2026-10-20 from the coordinate table above
(the generator was a one-off script; the fersxml is the record). Truth:

    matlab -batch "addpath('matlab'); fers_truth('scenarios/S1_clutter/scenario.fersxml', ...
        '<fers.conf with SURVEILLANCE2_RECEIVER_NAME = none>', 'data/S1_clutter/truth.csv')"

Capture on the Mac as in `scenarios/S0_clean_wgn/README.md` (the receiver
first, then `fers-cli` with this fersxml, `--vita49-fullscale 0.005`),
with a long `--seconds` and stop the receiver only after `fers-cli` has
exited: with 33 scatterers FERS runs at about a fifth of real time (about
50 min for 600 s), and a receiver that stops at 620 s keeps 252 s
(`capture_partial252s.pbrcap`, 2026-10-20); then `scp` the capture to the
board and run `scripts/results_run.sh sweep S1_clutter
data/S1_clutter/capture.pbrcap 2 1`.
