#!/usr/bin/env bash
#
# run_cgls.sh - one CGLS test on the LAPTOP CPU: the whole chain on the
# scenario's capture with the parameters set below, so every test returns
# suppression, time per CPI, detections and one range-Doppler map per CPI.
#
# EDIT THE PARAMETERS BELOW, then from any directory:
#
#   ./run_cgls.sh NAME
#
# NAME is your label for the test (e.g. it60_fp64). Output:
#
#   results/CGLS_tuning/NAME_<stamp>/   (gitignored)
#     config.conf      the exact config the chain ran with
#     run.log          pbr_run's output
#     telemetry.csv    per CPI: dsiMs, ardMs, totalMs, suppressionDb, detections
#     detections.csv   every CFAR detection: cpi, bins, km, Hz, power, SNR
#     ard/ard_0000N.bin   the range-Doppler map of each CPI
#   results/CGLS_tuning/index.csv   one row per test, appended: the parameters
#                                   and the headline numbers, for comparison
#
# SWEEP: set CGLS_BATCH_SWEEP to a list of batch lengths and every value is
# run in turn with the same other parameters; each run is named
# NAME_b<batch> and gets its own directory and index row. Leave it empty
# for a single run at CGLS_BATCH_s.
#
# Then plot:  plot_cgls('NAME_<stamp>') in MATLAB (see plot_cgls.m)
#
# The capture holds 3 CPIs. CPI 0 is the cold result (the solver starts from
# zero weights). With CGLS_PRIME = 1, CPIs 1 and 2 start from the weights CPI
# 0 and 1 reached, so the three rows together show what priming buys. With
# CGLS_PRIME = 0 all three are cold and should agree closely.
#
# Laptop timings say only how the cost scales with the parameters; the
# citable numbers come from the Nano (make prelim ARGS=CGLS).

set -euo pipefail

# ============================ PARAMETERS ======================================

CGLS_ITERATIONS=60        # fixed iteration count: the regularisation AND the cost
CGLS_BATCH_s=0.0025       # window the weights are estimated from and applied to
                          # (used when CGLS_BATCH_SWEEP is empty)
CGLS_PRIME=0              # 1: start each window from the previous CPI's weights
N_CLUTTER_TAPS=120        # K delay taps; at least N_RANGE_BINS (120)
DSI_MATH_PRECISION=FP64   # FP64 (the reference) or FP32 (emulates the GPU)

# Batch lengths to sweep, in seconds, space separated; empty for no sweep.
# Each must divide the 1 s CPI into a whole number of windows of MORE than
# K samples (the chain refuses otherwise). At 204800 Hz these all do:
#   1 0.5 0.25 0.125 0.0625 0.05 0.025 0.0125 0.01 0.005 0.0025 0.00125
CGLS_BATCH_SWEEP="1 0.25 0.0625 0.025 0.01 0.0025 0.00125"

# ============================ FIXED ===========================================

# Every path below is relative to the repository root, so go there first;
# the script can then be run from any directory, including this one.
cd "$(dirname "$0")/../.."

SCENARIO="${SCENARIO:-S0_clean}"
CAPTURE="data/${SCENARIO}/capture.pbrcap"
BASE_CONFIG="config/fers.conf"
OUTROOT="${OUTROOT:-results/CGLS_tuning}"
PBR_RUN="bin/pbr_run"
INDEX="${OUTROOT}/index.csv"

NAME="${1:?usage: $0 NAME   (a label for this test, e.g. it60_fp64)}"
[[ -x "${PBR_RUN}" ]] || { echo "error: ${PBR_RUN} missing: run 'make host' first" >&2; exit 1; }
[[ -f "${CAPTURE}" ]] || { echo "error: ${CAPTURE} missing" >&2; exit 1; }

# one test: run_one LABEL BATCH_s
run_one() {
  local name="$1" batch="$2"
  local stamp out config tel
  stamp="$(date +%Y%m%d_%H%M%S)"
  out="${OUTROOT}/${name}_${stamp}"
  mkdir -p "${out}"
  config="${out}/config.conf"

  # the config: the base with the algorithm, backend, outputs and the
  # parameters above substituted (macOS sed, so no -i)
  sed -e "s/^DSI_ALGORITHM *=.*/DSI_ALGORITHM = CGLS/" \
      -e "s/^DSI_BACKEND *=.*/DSI_BACKEND = CPU/" \
      -e "s/^DSI_MATH_PRECISION *=.*/DSI_MATH_PRECISION = ${DSI_MATH_PRECISION}/" \
      -e "s/^ARD_WRITE_FILES *=.*/ARD_WRITE_FILES = 1/" \
      -e "s/^ARD_STREAM *=.*/ARD_STREAM = 0/" \
      -e "s/^CGLS_ITERATIONS *=.*/CGLS_ITERATIONS = ${CGLS_ITERATIONS}/" \
      -e "s/^CGLS_BATCH_s *=.*/CGLS_BATCH_s = ${batch}/" \
      -e "s/^CGLS_PRIME *=.*/CGLS_PRIME = ${CGLS_PRIME}/" \
      -e "s/^N_CLUTTER_TAPS *=.*/N_CLUTTER_TAPS = ${N_CLUTTER_TAPS}/" \
      "${BASE_CONFIG}" > "${config}"

  echo
  echo "test     ${name}"
  echo "output   ${out}"
  echo "params   iterations ${CGLS_ITERATIONS}  batch ${batch} s  prime ${CGLS_PRIME}  K ${N_CLUTTER_TAPS}  ${DSI_MATH_PRECISION}"
  echo

  "${PBR_RUN}" --file "${CAPTURE}" --config "${config}" --out "${out}" 2>&1 | tee "${out}/run.log"

  tel="${out}/telemetry.csv"
  [[ -f "${tel}" ]] || { echo "error: no telemetry.csv; see ${out}/run.log" >&2; return 1; }

  echo
  echo "================ ${name} ================"
  awk -F, 'NR>1 {printf "  cpi %s  suppression %8.4f dB  dsi %8.1f ms  ard %6.1f ms  total %8.1f ms  detections %s\n", $1, $9, $6, $7, $8, $11}' "${tel}"
  echo "========================================="

  # one row per test in the index, header once
  [[ -f "${INDEX}" ]] || echo "name,stamp,iterations,batchS,prime,K,precision,supp0Db,supp1Db,supp2Db,dsi0Ms,det0,det1,det2" > "${INDEX}"
  awk -F, -v n="${name}" -v s="${stamp}" -v it="${CGLS_ITERATIONS}" -v b="${batch}" \
      -v p="${CGLS_PRIME}" -v k="${N_CLUTTER_TAPS}" -v pr="${DSI_MATH_PRECISION}" \
      'NR>1 {supp[$1]=$9; det[$1]=$11; if ($1==0) dsi0=$6}
       END {printf "%s,%s,%s,%s,%s,%s,%s,%.4f,%.4f,%.4f,%.1f,%s,%s,%s\n", n, s, it, b, p, k, pr, supp[0], supp[1], supp[2], dsi0, det[0], det[1], det[2]}' \
      "${tel}" >> "${INDEX}"
  echo "plot     plot_cgls('$(basename "${out}")')"
}

if [[ -z "${CGLS_BATCH_SWEEP}" ]]; then
  run_one "${NAME}" "${CGLS_BATCH_s}"
else
  for b in ${CGLS_BATCH_SWEEP}; do
    run_one "${NAME}_b${b}" "${b}"
  done
  echo
  echo "================ sweep ${NAME}: ${CGLS_ITERATIONS} iterations ================"
  printf "  %-10s %10s %10s %10s %8s %6s\n" batch_s supp0_dB supp1_dB supp2_dB dsi0_ms det0
  awk -F, -v n="${NAME}_b" 'index($1, n) == 1 {printf "  %-10s %10.3f %10.3f %10.3f %8.1f %6s\n", $4, $8, $9, $10, $11, $12}' "${INDEX}"
  echo "=============================================================="
fi
echo "index    ${INDEX}"
