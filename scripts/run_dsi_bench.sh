#!/usr/bin/env bash
#
# run_dsi_bench.sh - benchmark DSI cancellation under controlled, documented
# conditions. Run this ON THE JETSON.
#
# Sweeps every algorithm x backend combination in ONE session, with clocks and
# fan pinned and tegrastats logged alongside. Three reasons that matters:
#
#   * Pinned clocks remove DVFS as a hidden variable. Without them the same
#     binary on the same data has measured 2% apart between sessions, which is
#     25x the spread seen WITHIN a session.
#   * One session means the rows are comparable to each other. Rows taken on
#     different days are not, and a benchmark table mixing them is misleading.
#   * The tegrastats trace is what lets the methodology chapter rule out
#     thermal throttling after the fact rather than assuming it away.
#
# Usage, from the repo root on the Jetson:
#   bash scripts/run_dsi_bench.sh
#   bash scripts/run_dsi_bench.sh --iters-gpu 50 --cpi data/cpi_0001.bin
#
# The CPU rows are slow on purpose (about 10 s per iteration), so they default
# to few iterations. The GPU rows are cheap, so they get more.

set -euo pipefail

BENCH="${BENCH_BIN:-./build/dsi_bench}"
CPI="${CPI:-data/cpi_0000.bin}"
OUTROOT="${OUTROOT:-/data/results}"
ITERS_GPU=20
ITERS_CPU=3

while [[ $# -gt 0 ]]; do
  case "$1" in
    --cpi)       CPI="$2";        shift 2 ;;
    --iters-gpu) ITERS_GPU="$2";  shift 2 ;;
    --iters-cpu) ITERS_CPU="$2";  shift 2 ;;
    --out)       OUTROOT="$2";    shift 2 ;;
    -h|--help)
      echo "usage: $0 [--cpi FILE] [--iters-gpu N] [--iters-cpu N] [--out DIR]"
      exit 0 ;;
    *) echo "unrecognised argument: $1" >&2; exit 1 ;;
  esac
done

if [[ ! -x "${BENCH}" ]]; then
  echo "error: ${BENCH} not found. Run 'make build' from the Mac first." >&2
  exit 1
fi
if [[ ! -f "${CPI}" ]]; then
  echo "error: ${CPI} not found." >&2
  echo "       Dump one first:" >&2
  echo "       ./build/ingest_replay --file data/fers_capture.pbrcap \\" >&2
  echo "                             --fullscale 0.005 --dump-cpi ${CPI}" >&2
  exit 1
fi

STAMP="$(date +%Y%m%d_%H%M%S)"
OUTDIR="${OUTROOT}/dsi_${STAMP}"
mkdir -p "${OUTDIR}"

CSV="${OUTDIR}/dsi_bench.csv"
TEGRA_LOG="${OUTDIR}/tegrastats.log"
META="${OUTDIR}/run.txt"

echo "Output dir : ${OUTDIR}"
echo "CPI file   : ${CPI}"

# --- record the conditions the numbers were taken under ----------------------
{
  echo "date          $(date -Is)"
  echo "host          $(hostname)"
  echo "cpi           ${CPI}"
  echo "iters gpu     ${ITERS_GPU}"
  echo "iters cpu     ${ITERS_CPU}"
  echo "l4t           $(head -1 /etc/nv_tegra_release 2>/dev/null || echo unknown)"
  echo "nvpmodel      $(sudo nvpmodel -q 2>/dev/null | tr '\n' ' ' || echo unknown)"
  echo "git           $(git -C . rev-parse --short HEAD 2>/dev/null || echo unknown)"
} > "${META}"

# --- pin clocks and fan ------------------------------------------------------
# nvpmodel -m 0 is the 10 W all-cores mode; jetson_clocks then locks every
# clock to that mode's maximum instead of letting the governor scale them.
echo "Setting nvpmodel mode 0 (max)"
sudo nvpmodel -m 0 || echo "  (nvpmodel failed; continuing)"
echo "Pinning clocks (jetson_clocks)"
sudo jetson_clocks || echo "  (jetson_clocks failed; continuing UNPINNED -- results not citable)"
echo "Pinning fan to max"
echo 255 | sudo tee /sys/devices/pwm-fan/target_pwm >/dev/null || true

sudo jetson_clocks --show > "${OUTDIR}/clocks.txt" 2>&1 || true

# --- tegrastats --------------------------------------------------------------
echo "Logging tegrastats -> ${TEGRA_LOG}"
sudo tegrastats --interval 1000 --logfile "${TEGRA_LOG}" &
TEGRA_PID=$!
cleanup() {
  echo "Stopping tegrastats"
  sudo kill "${TEGRA_PID}" >/dev/null 2>&1 || true
  echo 0 | sudo tee /sys/devices/pwm-fan/target_pwm >/dev/null || true
}
trap cleanup EXIT

sleep 3   # a few idle samples, for a thermal baseline

# --- the sweep ---------------------------------------------------------------
# GPU first while the board is coolest, then the CPU rows. Every row lands in
# one CSV so the table is internally comparable.
for alg in ECA-m ECA-B; do
  echo "--- ${alg} on GPU"
  "${BENCH}" --cpi "${CPI}" --algorithm "${alg}" --backend GPU \
             --iters "${ITERS_GPU}" --csv "${CSV}" | tail -6
done

for alg in ECA-m ECA-B; do
  echo "--- ${alg} on CPU (slow: about 10 s per iteration)"
  "${BENCH}" --cpi "${CPI}" --algorithm "${alg}" --backend CPU \
             --iters "${ITERS_CPU}" --csv "${CSV}" | tail -6
done

# --- summary -----------------------------------------------------------------
echo
echo "================ summary ================"
printf "%-8s %-4s %12s %12s %10s\n" alg backend suppression min_ms real_time
tail -n +2 "${CSV}" | awk -F, \
  '{printf "%-8s %-4s %9.4f dB %9.1f ms %8.3f x\n", $1, $2, $11, $12, $17}'
echo "========================================="
echo
echo "Results  : ${CSV}"
echo "Thermals : ${TEGRA_LOG}"
echo "Pull them back to the Mac with:  make pull-results"
