# S0_clean_wgn

S0_clean with white Gaussian noise as the illuminator in place of the FM
recording. Same transmitter, receivers, target path, RCS, noise temperature
and 180 s; only both `cw_from_file` waveforms change, so the truth is the
same as S0_clean's.

## What it tests

Verification (results chapter, Section 7.2). The ambiguity function of an
FM signal depends on the programme content and its sidelobes are detected
as if they were targets. Noise has a flat spectrum: one peak, sidelobes set
by the bandwidth alone, the same on every CPI. With noise the map holds the
direct signal, the target and a noise floor, and every detection away from
the target is a false alarm whose expected count is Pfa times the number of
cells. A difference between the MATLAB reference, the C++ implementation
and the geometry therefore shows directly.

## Waveform

`data/waveforms/WGN_180s_204800Hz.h5`, written by `matlab/write_wgn_h5.m`:
complex white Gaussian noise, 180 s at 204800 Hz, unit variance, float32,
`rng(1, 'twister')`, so it regenerates bit for bit. The older
`~/FERS/Waveforms/WGN.h5` (2026-08-20) is the right length and rate but
float64; it is not used.

    matlab -batch "addpath('matlab'); write_wgn_h5(180, 204800, 1, 'data/waveforms/WGN_180s_204800Hz.h5')"

## Regenerate

```bash
# capture and frozen CPI (Mac; the receiver first, FERS after it)
./bin/pbr_run --live --config <2-channel CPU config> --seconds 185 \
    --capture data/S0_clean_wgn/capture.pbrcap --dump-cpi data/S0_clean_wgn/cpi_0000.bin
cd ~/FERS && ./build/release/packages/fers-cli/fers-cli \
    ~/passive-radar-sbc/scenarios/S0_clean_wgn/scenario.fersxml \
    --out-dir=/tmp/fers-out --vita49 127.0.0.1:4991 \
    --vita49-fullscale 0.005 -n=6

# truth
matlab -batch "addpath('matlab'); fers_truth('scenarios/S0_clean_wgn/scenario.fersxml', \
    'config/fers.conf', 'data/S0_clean_wgn/truth.csv')"
```

The capture config is `config/fers.conf` with `SURVEILLANCE2_RECEIVER_NAME
= none` (no third stream in this scenario), `DSI_BACKEND = CPU`, and
`N_RANGE_BINS = 4`, `N_CLUTTER_TAPS = 4` so the Mac's CPU chain keeps up
while recording; the capture holds the datagrams, not the processing, so
any config replays it. `--vita49-fullscale` must equal `VITA49_FULLSCALE`
in `config/fers.conf`.

Goldens (`data/S0_clean_wgn/golden/`) come from `eca_golden.m`,
`eca_ld_golden.m`, `fblms_golden.m` and `ard_golden.m`, all reading
`config/fers.conf`; see `docs/results-rerun-plan.md` Task 5.
