# S0_long

One target flying past the receiver for the full 600 s waveform, for long
live sessions with the viewer. Same transmitter, receivers, antennas,
noise and target RCS as S0_clean; no clutter.

## What changes from S0_clean

- **Length:** 600 s (`<endtime>600`).
- **Waveform:** `data/waveforms/Malmesbury_360s_600s.h5`, 600 s of the
  Malmesbury recording from 360 s in, unit variance. The path in the
  `.fersxml` is relative to this directory, so `fers-cli` finds it from any
  working directory. Cut with `matlab/rcf_to_fers_h5.m`
  (`scenarios/README.md`).
- **Target path:** a straight fly-by at 150 m/s and 8000 m, from
  (305136, 6213168) to (327485, 6300349). The line is tangent at t = 300 s
  to the 40 km bistatic-range ellipse, so the bistatic range has its minimum
  there and the Doppler passes smoothly from positive, through zero, to
  negative. The target crosses the zero-Doppler cancellation notch slowly:
  |Doppler| < 5 Hz from 264 s to 334 s.

  It is laid close to the surveillance antenna's boresight (azimuth 125°,
  elevation 10°, FERS sinc pattern), so the signal level stays within 4 dB
  of S0_clean's at t = 0 throughout: no fade at the ends. Faster or longer
  passes either leave the 120-bin map or go 40° to 70° off boresight and
  lose 20 dB or more.

  | t (s) | bistatic range | Doppler | antenna gain (off boresight) | level vs S0 at t = 0 |
  |---|---|---|---|---|
  | 0 | 61.2 km (bin 41.8) | +43.1 Hz | 0.7 dB (43°) | -3.9 dB |
  | 60 | 53.4 km (bin 36.5) | +33.9 Hz | 1.6 dB (40°) | -2.1 dB |
  | 120 | 47.4 km (bin 32.4) | +24.8 Hz | 2.6 dB (37°) | -0.5 dB |
  | 180 | 43.3 km (bin 29.6) | +16.2 Hz | 3.7 dB (32°) | +1.0 dB |
  | 240 | 40.8 km (bin 27.9) | +8.1 Hz | 4.9 dB (27°) | +2.5 dB |
  | 300 | 40.0 km (bin 27.3) | 0.0 Hz | 5.9 dB (20°) | +3.8 dB |
  | 360 | 40.9 km (bin 27.9) | -8.8 Hz | 6.8 dB (11°) | +4.9 dB |
  | 420 | 43.6 km (bin 29.8) | -18.8 Hz | 7.2 dB (1°) | +5.2 dB |
  | 480 | 48.6 km (bin 33.2) | -30.2 Hz | 6.7 dB (13°) | +4.4 dB |
  | 540 | 55.9 km (bin 38.2) | -42.0 Hz | 5.0 dB (26°) | +2.1 dB |
  | 600 | 65.5 km (bin 44.7) | -52.8 Hz | 2.1 dB (39°) | -2.0 dB |

  Computed by hand from the geometry, 89 MHz carrier: bistatic range is
  |target - Tx| + |target - Rx| - baseline; "level" is the antenna gain
  minus 20 log10(R_Tx R_Rx), relative to S0's target at t = 0. Range and
  Doppler agree with `matlab/fers_truth.m`, which writes them for every CPI
  to `data/S0_long/truth.csv`:

      matlab -batch "addpath('matlab'); fers_truth('scenarios/S0_long/scenario.fersxml', ...
          'config/fers.conf', 'data/S0_long/truth.csv')"
- **Removed:** the unused `JammerWaveform`, so FERS loads one waveform file.

## Run it live

From the Mac, against the chain on the Jetson (`config/fers.conf`;
`--vita49-fullscale` must equal its `VITA49_FULLSCALE`):

```bash
cd ~/FERS && ./build/release/packages/fers-cli/fers-cli \
    ~/passive-radar-sbc/scenarios/S0_long/scenario.fersxml \
    --out-dir=/tmp/fers-out --vita49 <jetson-ip>:4991 \
    --vita49-fullscale 0.005 -n=6
```

No capture or goldens are recorded for this scenario yet.
