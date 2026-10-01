# scenarios/

The experiment definitions. One directory per scenario, committed. Everything
generated from a scenario (waveform, capture, frozen CPIs, truth, goldens)
lives under `data/<scenario>/`, gitignored, and is regenerated from here.

```
scenarios/<name>/
  scenario.fersxml   the FERS scenario: geometry, targets, clutter, waveform
  README.md          what it tests, the truth at t = 0, how to regenerate
data/<name>/
  capture.pbrcap     the VITA 49 stream recorded once; every run replays it
  cpi_0000.bin       frozen CPI(s) for the level-1 correctness checks
  truth.csv          expected range bin and Doppler per CPI (matlab/fers_truth.m)
  golden/            MATLAB outputs the C++ is compared against
```

The chain configuration is `config/fers.conf` for every scenario: the
scenarios differ in the world (`.fersxml`), not in the chain. A run's
algorithm is set by `scripts/run_experiment.sh`, which derives a per-run
config from `config/fers.conf` and keeps the copy beside the results. A
scenario that needs a different chain key (a longer CPI, say) states it in
its README and the run script applies it.

The waveform is real FM: `data/waveforms/Malmesbury_<start>s_<len>s.h5`, cut
from the 2012 Malmesbury recording (88.2 MHz, 204800 Hz, 1107 s) by
`matlab/rcf_to_fers_h5.m`. The `BroadbandJammerNormalised.h5` S0 uses is the
same recording, 180 s, despite its name (116 kHz occupied bandwidth, measured
2026-10-01).

## The scenarios

Change one thing at a time. Every scenario after S0 uses the 600 s waveform.

| Name | Change from the previous | What it tests |
|---|---|---|
| `S0_clean` | (the original scenario: one target, no clutter, 180 s) | baseline; links to every result before 2026-10-01 |
| `S1_clutter` | + static scatterers over range bins 2..60 | the cancellers differ only on clutter: K vs extent, batch length vs conditioning, ECA-CD's delay-spread error |
| `S2_tangential` | trajectory crossing zero Doppler | the target inside the cancellation notch; ECA-B's 1/T_B ambiguities vs ECA-S |
| `S3_close` | target in the first range bins | detection beside the direct signal and the clutter ridge |
| `S4_weak` | distant or small-RCS target | the detection threshold: Pd against SNR per canceller |
| `S5_two` | a second target | resolution and masking |

S3 to S5 build on S1. If time is short, S0 to S2 and S4 carry the analysis.

## Three levels of test

1. **Correctness** (one frozen CPI, pass/fail): MATLAB FP64 vs C++ CPU FP64,
   relative L2 < 1e-4; CPU FP32 vs GPU FP32; GPU vs MATLAB at the outcome
   level (suppression within 0.1 dB, same CFAR detections, target SNR within
   tolerance). The relaxed GPU criterion is forced by the hardware: Maxwell
   FP64 runs at 1/32 rate, so the GPU is FP32 or nothing.
2. **Radar performance** (the full scenario, scored against `truth.csv`):
   per CPI, target SNR in the map, detected or missed, false alarms,
   noise-floor rise around zero Doppler, peak ambiguity level, suppression.
3. **Embedded cost** (Nano, clocks pinned, `tegrastats` logged): latency per
   stage (min, median, p99), real-time factor, peak memory, power, and the
   parameter sweeps that give suppression against latency per algorithm.

The laptop CPU is a correctness stepping stone only; its timings are not
reported.
