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
# Run ON THE JETSON from the repository root. Usage:
#   scripts/run_cufft_bench.sh -- --sweep-size --min-size 1024 --max-size 33554432 --csv
#   scripts/run_cufft_bench.sh -- --size 204800 --batch 1 --iters 200 --csv
#
# Everything after "--" is passed straight through to build/cufft_bench.
# Output: /data/results/cufft_bench/<stamp>/ (results.csv, tegrastats.log,
# clocks.txt), which `make pull` brings back to the Mac.

set -euo pipefail

BENCH="${BENCH_BIN:-./build/cufft_bench}"
OUTROOT="${OUTROOT:-/data/results/cufft_bench}"
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

source scripts/pin_clocks.sh
pinClocks "${OUTDIR}"

sleep 2  # a couple of idle samples for a thermal baseline

# --- run the benchmark, CSV to file ------------------------------------------
CSV_OUT="${OUTDIR}/results.csv"
echo "Running..."
"${BENCH}" "${BENCH_ARGS[@]:-}" >"${CSV_OUT}"

echo "Done."
echo "  results : ${CSV_OUT}"
echo "  thermal : ${OUTDIR}/tegrastats.log"
