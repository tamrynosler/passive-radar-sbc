# S1_clutter_wgn

S1_clutter with white Gaussian noise as the illuminator in place of the FM
recording: the same 28 stationary scatterers, 4 slow movers, aircraft,
geometry, antennas and noise; only both `cw_from_file` waveforms change.
The truth files are identical to S1_clutter's. See
`scenarios/S1_clutter/README.md` for the clutter design and tables.

## What it tests

The clutter result without the FM waveform's programme-dependent
sidelobes: what the cancellers leave of the mountains and the slow movers
when the ambiguity function has one peak and a flat floor. Against
S1_clutter it separates the cost of the clutter from the cost of the
waveform (results chapter, Section 7.3.3).

## Waveform

`data/waveforms/WGN_600s_204800Hz.h5`, written by `matlab/write_wgn_h5.m`:
complex white Gaussian noise, 600 s at 204800 Hz, unit variance, float32,
`rng(2, 'twister')`.

    matlab -batch "addpath('matlab'); write_wgn_h5(600, 204800, 2, 'data/waveforms/WGN_600s_204800Hz.h5')"

## Regenerate

Truth, capture and runs as in `scenarios/S1_clutter/README.md`, with this
directory's `scenario.fersxml`. FERS runs at about a fifth of real time
with 33 scatterers, so let `fers-cli` finish (about 50 min) before
stopping the receiver, or the capture is cut short.
