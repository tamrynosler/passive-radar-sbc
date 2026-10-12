# S0_aoa

S0_long with a second surveillance antenna half a wavelength from the
first, for the angle-of-arrival chain (`docs/third-channel-aoa-design.md`).
Same transmitter, reference, target path, waveform (600 s) and noise as
S0_long; only the platform `ArmasuisseSur2RxPlatform` with the receiver
`ArmasuisseSur2RxClean` is added.

## The array

- Element 1: the surveillance receiver, (287946.01, 6297267.09, 241).
- Element 2: 1.6842 m (λ/2 at 89 MHz) away along compass bearing 35°,
  (287946.976, 6297268.470, 241). The baseline is perpendicular to the
  125° boresight, so the array looks where the antennas point; the chain
  takes the look direction as baseline + 90°.
- Same antenna, rotation, `timing="RxClock"` and noise temperature as
  element 1. Receivers naming the same timing share one clock realisation
  in FERS, so the random frequency offset cancels between the elements.
  FERS models carrier phase per path (`phase = -2π f_c τ`), which is what
  the interferometer reads.

`config/fers.conf` names the third receiver and carries the array keys
(`SURVEILLANCE2_RECEIVER_NAME`, `CARRIER_FREQ_Hz`, `ARRAY_SPACING_m`,
`ARRAY_BASELINE_AZ_deg`, `PHASE_CAL_deg`). Replaying S0_clean or S0_long,
which have no third stream, needs `SURVEILLANCE2_RECEIVER_NAME = none`.

## Truth

`matlab/fers_truth.m` writes, besides range and Doppler, the target's
compass bearing, the phase difference FERS should produce and the bearing
the chain should report (`apparentBearingDeg`), and prints the direct
path's phase: the transmitter sits at 203° from the receiver, 78° off the
look direction, so the raw surveillance channels should differ by about
-176°. The chain's `directPhaseDeg` is that check.

    matlab -batch "addpath('matlab'); fers_truth('scenarios/S0_aoa/scenario.fersxml', ...
        'config/fers.conf', 'data/S0_aoa/truth.csv')"

The target's bearing runs from 168° at t = 0 (43° off the look direction)
through 124° at t = 420 s to about 86° at the end.

## Run it

From the Mac, against the chain (`config/fers.conf`; `--vita49-fullscale`
must equal its `VITA49_FULLSCALE`):

```bash
cd ~/FERS && ./build/release/packages/fers-cli/fers-cli \
    ~/passive-radar-sbc/scenarios/S0_aoa/scenario.fersxml \
    --out-dir=/tmp/fers-out --vita49 <jetson-ip>:4991 \
    --vita49-fullscale 0.005 -n=6
```

Three streams arrive; `pbr_run` names them as it resolves them and prints
`direct` and `aoa` on every CPI line. The capture `data/S0_aoa/capture.pbrcap`
(the first 88 s, 222 MB, recorded 2026-10-11) was recorded with `--capture`
from such a run on the Mac. The CPU chain cannot keep up live, so it was
recorded with `N_RANGE_BINS = 4` and `N_CLUTTER_TAPS = 4` (processing under
0.1 s per CPI) and no datagram was lost; the capture holds the datagrams,
not the processing, so any config replays it.

Measured on that capture, replayed on the Mac CPU (ECA-CD, Batches), 30
CPIs: `directPhaseDeg` -176.1 on every CPI against -176.1 from the
geometry; the strongest plot's bearing within 0.5 deg of `apparentBearingDeg`
(RMS 0.26 deg, mean -0.2 deg), coherence 1.00. Range and Doppler sidelobe plots carry the
target's bearing; the noise plots have coherence below 0.95.
