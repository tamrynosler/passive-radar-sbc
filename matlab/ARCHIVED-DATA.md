# Where the .h5 files went

`ArmasuisseRefRxClean_results.h5`, `ArmasuisseSurRxClean_results.h5` and
`WGN.h5` were moved out of the repository on 2026-09-18:

    ~/passive-radar-archive/matlab-h5/

They are 1.6 GB, they are gitignored, and **nothing in the current chain reads
them**. Validation runs from `.pbrcap` captures and frozen CPIs instead
(`pbr_run --capture` / `--dump-cpi`), so MATLAB and C++ start from
byte-identical samples — see `docs/design_decisions.md` §1d.8.

They were moved rather than deleted because they are *untrustworthy* rather
than useless: dated 17 August, they predate the current FERS scenario, and a
calibration script returned a self-contradictory peak of exactly 1.0 from
them. Worth being able to re-examine, not worth carrying around.

## The one script that still reads them

`fm_passive_radar_process.m`, the original reference chain and still the
definition of ECA-m. To run it, either edit its `refFile`/`surFile` paths, or
link the data back in:

    ln -s ~/passive-radar-archive/matlab-h5/*.h5 matlab/

The scripts that existed only to examine these files —
`calibrate_fullscale.m`, `inspect_h5_scaling.m` and the first multi-CPI
animation, `fm_passive_radar_movie.m` — were archived on 2026-10-06 to
`~/passive-radar-archive/tools/`. `fm_passive_radar_stream.m` does not need
them: it runs the same chain on a live FERS stream.
