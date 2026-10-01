# S0_clean

The original scenario, unchanged: `SingleTargetCleanNativeFileCw.fersxml`
as it was in `matlab/` and `~/fers-runs/clean/`. One aircraft, no clutter,
180 s. Every result before 2026-10-01 was produced from it.

## What it tests

The baseline. The surveillance channel holds the direct signal and the one
target, so the cancellers only have bin 0 to remove and all score alike
(about 51 dB). It proves the chain, not the algorithms.

## Geometry (from scenario.fersxml, projected metres)

| | x | y | alt |
|---|---|---|---|
| Transmitter, Constantiaberg | 258804.41 | 6228720.84 | 397 |
| Reference receiver | 287942.01 | 6297267.09 | 241 |
| Surveillance receiver | 287946.01 | 6297267.09 | 241 |
| Target at t = 0 | 331995.77 | 6291261.10 | 10000 |
| Target at t = 180 | 305242.56 | 6267172.40 | 5000 |

Carrier 89 MHz, rate 204800 Hz, target RCS 200 m^2 isotropic, receiver noise
temperature 438.4 K, transmit power 16400 (FERS units).

Truth at t = 0, by hand (PROGRESS.md 2026-10-01): baseline 74.48 km, target
bistatic range 67.79 km = bin 46.3, Doppler +99.1 Hz (closing, positive).
The chain's first CPI reported bin 46 / +99 Hz. `data/S0_clean/truth.csv`
from `matlab/fers_truth.m` gives every CPI.

## Waveform

`../FERS/Waveforms/BroadbandJammerNormalised.h5`, relative to `~/FERS` where
`fers-cli` runs. 180 s of the Malmesbury FM recording, unit variance.

## Regenerate

```bash
# capture (Mac, two terminals; the receiver first)
./bin/ingest_probe --seconds 185 --capture data/S0_clean/capture.pbrcap
cd ~/FERS && ./build/release/packages/fers-cli/fers-cli \
    ~/passive-radar-sbc/scenarios/S0_clean/scenario.fersxml \
    --out-dir=/tmp/fers-out --vita49 127.0.0.1:4991 \
    --vita49-fullscale 0.005 -n=6

# frozen CPI, truth
./bin/ingest_replay --file data/S0_clean/capture.pbrcap --fullscale 0.005 \
                    --dump-cpi data/S0_clean/cpi_0000.bin
matlab -batch "addpath('matlab'); fers_truth('scenarios/S0_clean/scenario.fersxml', \
    'config/fers.conf', 'data/S0_clean/truth.csv')"
```

The capture in `data/S0_clean/` on 2026-10-01 is the 2026-09-16 recording
(`--vita49-fullscale 0.005`, a few seconds long), moved from
`data/fers_capture.pbrcap`. The full-length capture is still to be recorded.
