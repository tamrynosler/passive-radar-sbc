# results/

Gitignored. Runs are made on the Jetson under `/data/results/` with the same
tree and pulled here by `make pull`. Everything before 2026-10-01 was moved
to `~/passive-radar-archive/old-results/`.

```
results/
  L1_correctness/<scenario>/<alg>_<backend>.txt     pass/fail and error values
  L2_performance/<scenario>/<ALG>/<stamp>/          one full-scenario run
      config.conf   the config the run used, verbatim
      run.txt       manifest (below)
      run.log       pbr_run's output
      telemetry.csv detections.csv                  from pbr_run --out
      score.csv     per-CPI scoring against data/<scenario>/truth.csv
  L3_bench/<stamp>_<sweep>/                          dsi_bench.csv tegrastats.log
      clocks.txt run.txt
```

`run.txt` in every directory: scenario, git hash, host, date, command line,
SHA-256 of the capture and of the config, clocks pinned or not, L4T release.
A number in the thesis cites one of these files by path and column; nothing
is typed from memory.

A rerun gets a new stamp; the report names the stamp it used.
