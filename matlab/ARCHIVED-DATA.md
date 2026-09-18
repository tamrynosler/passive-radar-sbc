# Where the .h5 files went

`ArmasuisseRefRxClean_results.h5`, `ArmasuisseSurRxClean_results.h5` and
`WGN.h5` were moved out of the repository on 2026-09-18:

    ~/passive-radar-archive/matlab-h5/

They are 1.6 GB, they are gitignored, and **nothing in the current chain reads
them**. Validation now runs from `.pbrcap` captures via `CpiFile`, so both
MATLAB and C++ start from byte-identical samples — see
`docs/design_decisions.md` §1d.8.

They were moved rather than deleted because `PLAN.md` records them as
*untrustworthy* rather than useless: they are dated 17 August, predate the
current FERS scenario, and `calibrate_fullscale.m` returned a
self-contradictory peak of exactly 1.0 from them. That is worth being able to
re-examine, not worth carrying around.

## Scripts that still expect them

These are kept for the report as a record of the design process, and will not
run until pointed at the new location:

| Script | What it was for |
|---|---|
| `fm_passive_radar_process.m` | the original reference chain; still the definition of ECA-m |
| `fm_passive_radar_movie.m`   | the first multi-CPI animation, superseded by `play_ard.m` |
| `fm_passive_radar_stream.m`  | the first streaming attempt, superseded by `pbr_run` |
| `calibrate_fullscale.m`      | worked out `--vita49-fullscale` from the unquantised peak |
| `inspect_h5_scaling.m`       | diagnosed whether the export saturates or normalises per channel |

To run one, either edit its `refFile`/`surFile` paths, or symlink the data
back in:

    ln -s ~/passive-radar-archive/matlab-h5/*.h5 matlab/
