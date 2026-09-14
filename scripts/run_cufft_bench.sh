#!/usr/bin/env bash
#
# run_cufft_bench.sh - run the cuFFT harness under controlled, documented conditions.
#
# Pins the GPU/CPU clocks and the fan, captures tegrastats alongside the run, and
# writes both the CSV results and the thermal/power log to a timestamped folder on
# /data. Flat clocks + bounded temperature + a recorded tegrastats trace is what
# makes a latency number defensible in the methodology chapter: it rules out
# thermal throttling and DVFS as hidden variables.
#
# Usage:
#   scripts/run_cufft_bench.sh -- --sweep-size --min-size 1024 --max-size 33554432 --csv
#   scripts/run_cufft_bench.sh -- --size 204800 --batch 1 --iters 200 --csv
#
# Everything after "--" is passed straight through to ./cufft_bench.

set -euo pipefail

BENCH="${BENCH_BIN:-./cufft_bench}"
OUTROOT="${OUTROOT:-/data/bench}"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUTDIR="${OUTROOT}/${STAMP}"
mkdir -p "${OUTDIR}"

# Split args on the "--" separator.
BENCH_ARGS=()
seen_sep=0
for a in "$@"; do
  if [[ "$a" == "--" && $seen_sep -eq 0 ]]; then seen_sep=1; continue; fi
  if [[ $seen_sep -eq 1 ]]; then BENCH_ARGS+=("$a"); fi
done

echo "Output dir : ${OUTDIR}"
echo "Benchmark  : ${BENCH} ${BENCH_ARGS[*]:-}"

# --- pin clocks and fan for determinism (needs sudo) -------------------------
if command -v jetson_clocks >/dev/null 2>&1; then
  echo "Pinning clocks to max (jetson_clocks)"
  sudo jetson_clocks || echo "  (jetson_clocks failed; continuing unpinned)"
fi
echo "Pinning fan to max"
echo 255 | sudo tee /sys/devices/pwm-fan/target_pwm >/dev/null || true

# --- start tegrastats in the background --------------------------------------
TEGRA_LOG="${OUTDIR}/tegrastats.log"
echo "Logging tegrastats -> ${TEGRA_LOG}"
sudo tegrastats --interval 1000 --logfile "${TEGRA_LOG}" &
TEGRA_PID=$!
# Ensure tegrastats is always stopped, even on error/Ctrl-C.
cleanup() {
  echo "Stopping tegrastats"
  sudo kill "${TEGRA_PID}" >/dev/null 2>&1 || true
  # Hand the fan back to the thermal governor.
  echo 0 | sudo tee /sys/devices/pwm-fan/target_pwm >/dev/null || true
}
trap cleanup EXIT

sleep 2  # a couple of idle samples for a thermal baseline

# --- run the benchmark, CSV to file ------------------------------------------
CSV_OUT="${OUTDIR}/results.csv"
echo "Running..."
"${BENCH}" "${BENCH_ARGS[@]:-}" >"${CSV_OUT}"

echo "Done."
echo "  results : ${CSV_OUT}"
echo "  thermal : ${TEGRA_LOG}"
