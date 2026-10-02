#!/usr/bin/env bash
#
# run_prelim.sh - ONE algorithm on ONE frozen CPI, four measurements, with
# clocks pinned. Run this ON THE JETSON (from the Mac: make prelim ARGS=ECA-m).
#
#   1. accuracy     dsi_verify against the MATLAB golden file, twice: the CPU
#                   backend in FP64 (tolerance 1e-4, the strict gate) and the
#                   GPU (tolerance 1e-2, FP32 is all Maxwell has)
#   2. time per CPI dsi_bench: cancel() alone, GPU then Nano CPU, one warm-up
#                   discarded, min/median/mean/std/max over the iterations.
#                   The GPU figure includes the host<->device copies, because
#                   cancel() takes host pointers; that is the stage latency the
#                   chain pays, not kernel time.
#   3. chain        pbr_run on the scenario's capture with this algorithm:
#                   telemetry.csv (dsiMs, ardMs, totalMs, lagS, suppressionDb
#                   per CPI), detections.csv, and one ard_NNNNN.bin map per CPI
#   4. suppression  reported by 1, 2 and 3, all from the same cancel()
#
# Everything lands in ONE directory, with the exact config that was used and
# a manifest of what produced it:
#
#   /data/results/prelim/SCENARIO/ALG/STAMP/
#     config.conf          config/fers.conf with DSI_ALGORITHM set (and, for
#                          ECA-B/ECA-S, the update window the name requires;
#                          for CGLS, priming off so repeated runs are identical)
#     run.txt              manifest: git hash, SHA-256 of CPI/capture/golden/
#                          config, iterations, nvpmodel, L4T, each step's
#                          start and end epoch second, each step's exit status
#     clocks.txt           jetson_clocks --show after pinning
#     tegrastats.log       1 Hz, from 3 s before the first step to the end
#     verify_cpu_fp64.txt  verify_gpu.txt      dsi_verify output
#     bench.csv            two rows (GPU, CPU); bench_gpu.txt bench_cpu.txt
#     chain/               telemetry.csv detections.csv run.log ard/
#
# Usage:
#   bash scripts/run_prelim.sh ECA-m
#   SCENARIO=S0_clean ITERS_GPU=20 ITERS_CPU=3 bash scripts/run_prelim.sh ECA-B
#
# Needs, under data/SCENARIO/ on this board: cpi_0000.bin, capture.pbrcap and
# golden/golden_<tag>.bin from matlab/eca_golden.m (make push-data copies them).

set -uo pipefail

ALG="${1:-ECA-m}"
SCENARIO="${SCENARIO:-S0_clean}"
ITERS_GPU="${ITERS_GPU:-20}"
ITERS_CPU="${ITERS_CPU:-3}"
OUTROOT="${OUTROOT:-/data/results}"
BASE_CONFIG="${BASE_CONFIG:-config/fers.conf}"

case "${ALG}" in
  ECA-m)  TAG=ecam ;;
  ECA-B)  TAG=ecab ;;
  ECA-S)  TAG=ecas ;;
  ECA-CD) TAG=ecacd ;;
  CGLS)   TAG=cgls ;;
  *) echo "unknown algorithm '${ALG}' (ECA-m ECA-B ECA-S ECA-CD CGLS)" >&2; exit 1 ;;
esac

DATA="data/${SCENARIO}"
CPI="${DATA}/cpi_0000.bin"
CAPTURE="${DATA}/capture.pbrcap"
GOLDEN="${DATA}/golden/golden_${TAG}.bin"

for f in build/dsi_verify build/dsi_bench build/pbr_run; do
  [[ -x "$f" ]] || { echo "error: $f missing. 'make build' from the Mac first." >&2; exit 1; }
done
[[ -f "${CPI}" ]]     || { echo "error: ${CPI} missing (ingest_replay --dump-cpi, then make push-data)" >&2; exit 1; }
[[ -f "${CAPTURE}" ]] || { echo "error: ${CAPTURE} missing (make push-data)" >&2; exit 1; }

# Accuracy needs a MATLAB golden. eca_golden.m writes them for ECA-m and
# ECA-B only. Without one (ECA-S, ECA-CD, CGLS as of 2026-10-02) the fallback
# is backend-against-backend: the GPU diffed against the CPU running the same
# algorithm, once in FP32 (precision removed, what is left is structure) and
# once in FP64 (the cost of FP32). That proves the two implementations agree
# with each other, NOT that either matches the reference maths.
if [[ -f "${GOLDEN}" ]]; then
  ACCURACY=golden
else
  ACCURACY=cross
  echo "note: no MATLAB golden at ${GOLDEN}; accuracy is GPU-against-CPU only"
fi

# The board's clock has been wrong before (NTP not synchronised), so the Mac
# passes STAMP in through make; the board's own date is the fallback.
STAMP="${STAMP:-$(date +%Y%m%d_%H%M%S)}"
OUT="${OUTROOT}/prelim/${SCENARIO}/${ALG}/${STAMP}"
mkdir -p "${OUT}/chain"
META="${OUT}/run.txt"
echo "Output: ${OUT}"

# --- the config this run uses, derived from the base and kept beside it -----
CONFIG="${OUT}/config.conf"
cp "${BASE_CONFIG}" "${CONFIG}"
sed -i -e "s/^DSI_ALGORITHM *=.*/DSI_ALGORITHM = ${ALG}/" \
       -e "s/^DSI_BACKEND *=.*/DSI_BACKEND = GPU/" \
       -e "s/^ARD_WRITE_FILES *=.*/ARD_WRITE_FILES = 1/" \
       -e "s/^ARD_STREAM *=.*/ARD_STREAM = 0/" "${CONFIG}"
case "${ALG}" in
  ECA-B) sed -i "s/^ECA_UPDATE_INTERVAL_s *=.*/ECA_UPDATE_INTERVAL_s = 0.025/" "${CONFIG}" ;;
  ECA-S) sed -i "s/^ECA_UPDATE_INTERVAL_s *=.*/ECA_UPDATE_INTERVAL_s = 0.0025/" "${CONFIG}" ;;
  CGLS)  sed -i "s/^CGLS_PRIME *=.*/CGLS_PRIME = 0/" "${CONFIG}" ;;
esac

# --- manifest ---------------------------------------------------------------
{
  echo "date (board)  $(date -Is)"
  echo "stamp         ${STAMP}"
  echo "host          $(hostname)"
  echo "algorithm     ${ALG}"
  echo "scenario      ${SCENARIO}"
  echo "git           $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "base config   ${BASE_CONFIG}"
  echo "cpi           ${CPI}  sha256 $(sha256sum "${CPI}" | cut -c1-16)"
  echo "capture       ${CAPTURE}  sha256 $(sha256sum "${CAPTURE}" | cut -c1-16)"
  if [[ "${ACCURACY}" == golden ]]; then
    echo "golden        ${GOLDEN}  sha256 $(sha256sum "${GOLDEN}" | cut -c1-16)"
  else
    echo "golden        NONE (no MATLAB reference for ${ALG}); accuracy = GPU against CPU"
  fi
  echo "config        sha256 $(sha256sum "${CONFIG}" | cut -c1-16)"
  echo "iters gpu     ${ITERS_GPU}"
  echo "iters cpu     ${ITERS_CPU}"
  echo "l4t           $(head -1 /etc/nv_tegra_release 2>/dev/null || echo unknown)"
} > "${META}"

# --- pin clocks, fan; log tegrastats -----------------------------------------
echo "Pinning clocks (sudo)"
sudo nvpmodel -m 0 || echo "  nvpmodel failed; continuing"
sudo jetson_clocks  || echo "  jetson_clocks FAILED; results NOT citable"
echo 255 | sudo tee /sys/devices/pwm-fan/target_pwm >/dev/null || true
sudo jetson_clocks --show > "${OUT}/clocks.txt" 2>&1 || true
echo "nvpmodel      $(sudo nvpmodel -q 2>/dev/null | tr '\n' ' ')" >> "${META}"

sudo tegrastats --interval 1000 --logfile "${OUT}/tegrastats.log" &
TEGRA_PID=$!
cleanup() {
  sudo kill "${TEGRA_PID}" >/dev/null 2>&1 || true
  echo 0 | sudo tee /sys/devices/pwm-fan/target_pwm >/dev/null || true
}
trap cleanup EXIT
echo "tegrastats t0 $(date +%s)" >> "${META}"
sleep 3

# run a step, record its window and exit status, never abort the sequence
step() {   # step NAME LOGFILE CMD...
  local name="$1" log="$2"; shift 2
  echo; echo "=== ${name}"
  local t0; t0=$(date +%s)
  "$@" 2>&1 | tee "${log}"
  local status=${PIPESTATUS[0]}
  echo "${name}  start ${t0}  end $(date +%s)  exit ${status}" >> "${META}"
}

# --- 1. accuracy --------------------------------------------------------------
if [[ "${ACCURACY}" == golden ]]; then
  step verify_cpu_fp64 "${OUT}/verify_cpu_fp64.txt" \
    ./build/dsi_verify --cpi "${CPI}" --golden "${GOLDEN}" --config "${CONFIG}" \
        --algorithm "${ALG}" --backend CPU --precision FP64 --tolerance 1e-4
  step verify_gpu "${OUT}/verify_gpu.txt" \
    ./build/dsi_verify --cpi "${CPI}" --golden "${GOLDEN}" --config "${CONFIG}" \
        --algorithm "${ALG}" --backend GPU --tolerance 1e-2
else
  step verify_gpu_vs_cpu_fp32 "${OUT}/verify_gpu_vs_cpu_fp32.txt" \
    ./build/dsi_verify --cpi "${CPI}" --against CPU --config "${CONFIG}" \
        --algorithm "${ALG}" --backend GPU --precision FP32 --tolerance 1e-2
  step verify_gpu_vs_cpu_fp64 "${OUT}/verify_gpu_vs_cpu_fp64.txt" \
    ./build/dsi_verify --cpi "${CPI}" --against CPU --config "${CONFIG}" \
        --algorithm "${ALG}" --backend GPU --precision FP64 --tolerance 1e-2
fi

# --- 2. time per CPI ------------------------------------------------------------
step bench_gpu "${OUT}/bench_gpu.txt" \
  ./build/dsi_bench --cpi "${CPI}" --config "${CONFIG}" --algorithm "${ALG}" \
      --backend GPU --iters "${ITERS_GPU}" --csv "${OUT}/bench.csv"
step bench_cpu "${OUT}/bench_cpu.txt" \
  ./build/dsi_bench --cpi "${CPI}" --config "${CONFIG}" --algorithm "${ALG}" \
      --backend CPU --iters "${ITERS_CPU}" --csv "${OUT}/bench.csv"

# --- 3. the chain on the capture ------------------------------------------------
step chain "${OUT}/chain/run.log" \
  ./build/pbr_run --file "${CAPTURE}" --config "${CONFIG}" --out "${OUT}/chain"

# --- summary --------------------------------------------------------------------
echo
echo "================ ${ALG} on ${SCENARIO} ================"
for f in "${OUT}"/verify_*.txt; do printf "  %-26s %s\n" "$(basename "$f" .txt)" "$(grep -h 'PASS\|FAIL' "$f")"; done
[[ -f "${OUT}/bench.csv" ]] && awk -F, 'NR>1 {printf "  %-6s %-4s suppression %7.3f dB  min %9.2f ms  median %9.2f ms  %.3fx real time\n", $1, $2, $11, $12, $13, $17}' "${OUT}/bench.csv"
[[ -f "${OUT}/chain/telemetry.csv" ]] && awk -F, 'NR>1 {printf "  chain cpi %s  dsi %.1f ms  ard %.1f ms  total %.1f ms  suppression %.3f dB  detections %s\n", $1, $6, $7, $8, $9, $11}' "${OUT}/chain/telemetry.csv"
grep -E "^(verify|bench|chain)" "${META}" | sed 's/^/  /'
echo "=========================================================="
echo "Results: ${OUT}   (make pull on the Mac)"
