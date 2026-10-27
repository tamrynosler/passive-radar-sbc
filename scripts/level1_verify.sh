#!/usr/bin/env bash
#
# level1_verify.sh - the level-1 correctness checks of docs/results-rerun-plan.md
# Task 5 (steps 5 and 6), on the board, from the repo root after make build.
#
#   scripts/level1_verify.sh SCENARIO [OUT_ROOT]
#
# Reads data/SCENARIO/: cpi_0000.bin, cleaned_0000.bin (dsi_verify
# --dump-output of ECA-B on the Mac), golden/*.bin (the MATLAB goldens) and
# configs/<ALG>.conf (scripts/results_run.sh config, one per canceller).
# Writes OUT_ROOT/SCENARIO/ (default /data/results/L1_correctness):
#
#   <alg>_cpu_fp64.txt      dsi_verify, CPU FP64 against the MATLAB golden:
#                           ECA-m, ECA-B, ECA-LD, FBLMS (the four with one)
#   ard_caf_cpu_fp64.txt    ard_verify --stage caf, CPU, against golden_ard.bin
#   ard_cfar_cpu_fp64.txt   ard_verify --stage cfar, CPU
#   <alg>_gpu.txt           dsi_verify, GPU FP32: against the golden for the
#                           four above, against the CPU FP64 run for ECA-S,
#                           ECA-CD and CGLS; tolerance 1e-2, the FP32 bound
#   ard_chained_<alg>_gpu.txt   ard_verify --chained --backend GPU: the
#                           canceller and the map device-coupled, as pbr_run
#                           runs them, against golden_ard.bin (made from
#                           ECA-B's output, so only ECA-B is expected to match
#                           the map; the detection list and the device
#                           coupling check are the point for the others)
#   level1_summary.txt      one line per check: name, PASS/FAIL, exit code
#
# Every check's full output is kept; the exit code goes in the summary.

kAlgs="ECA-m ECA-B ECA-S ECA-CD ECA-LD CGLS FBLMS"
kDefaultRoot="/data/results/L1_correctness"

sScenario="$1"
sRoot="${2:-$kDefaultRoot}"
if [ -z "$sScenario" ]; then
  sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi

sData="data/${sScenario}"
sOut="${sRoot}/${sScenario}"
sCpi="${sData}/cpi_0000.bin"
sCleaned="${sData}/cleaned_0000.bin"
for f in "$sCpi" "$sCleaned" "${sData}/golden/golden_ard.bin" ./build/dsi_verify ./build/ard_verify; do
  if [ ! -e "$f" ]; then
    echo "level1_verify: missing $f" >&2
    exit 1
  fi
done
mkdir -p "$sOut" || exit 1
sSummary="${sOut}/level1_summary.txt"
: > "$sSummary"

# goldenFor ALG: the golden file name, or empty for a canceller without one.
goldenFor() {
  case "$1" in
    ECA-m)  echo golden_ecam.bin ;;
    ECA-B)  echo golden_ecab.bin ;;
    ECA-LD) echo golden_ecald.bin ;;
    FBLMS)  echo golden_fblms.bin ;;
    *)      echo "" ;;
  esac
}

# lowerName ALG: the file-name form, eca-m -> ecam.
lowerName() {
  echo "$1" | tr 'A-Z' 'a-z' | tr -d '-'
}

# check NAME OUTFILE CMD...: run, keep the output, note the result.
check() {
  local sName="$1" sFile="$2"
  shift 2
  echo "== ${sName}"
  "$@" > "$sFile" 2>&1
  local iExit=$?
  local sResult="FAIL"
  [ "$iExit" = "0" ] && sResult="PASS"
  echo "${sName} ${sResult} exit=${iExit}" | tee -a "$sSummary"
  grep -E "relative L2|suppression|differ|detections|identical|EXACT" "$sFile" | head -6 | sed 's/^/   /'
}

# ---- CPU FP64 against the MATLAB goldens
for sAlg in $kAlgs; do
  sGolden=$(goldenFor "$sAlg")
  [ -n "$sGolden" ] || continue
  sConf="${sData}/configs/${sAlg}.conf"
  check "${sAlg} cpu_fp64" "${sOut}/$(lowerName "$sAlg")_cpu_fp64.txt" \
    ./build/dsi_verify --cpi "$sCpi" --golden "${sData}/golden/${sGolden}" \
      --config "$sConf" --backend CPU --precision FP64
done
check "ard caf cpu_fp64" "${sOut}/ard_caf_cpu_fp64.txt" \
  ./build/ard_verify --cleaned "$sCleaned" --golden "${sData}/golden/golden_ard.bin" \
    --stage caf --backend CPU --config "${sData}/configs/ECA-B.conf"
check "ard cfar cpu_fp64" "${sOut}/ard_cfar_cpu_fp64.txt" \
  ./build/ard_verify --cleaned "$sCleaned" --golden "${sData}/golden/golden_ard.bin" \
    --stage cfar --backend CPU --config "${sData}/configs/ECA-B.conf"

# ---- GPU FP32
for sAlg in $kAlgs; do
  sGolden=$(goldenFor "$sAlg")
  sConf="${sData}/configs/${sAlg}.conf"
  if [ -n "$sGolden" ]; then
    check "${sAlg} gpu" "${sOut}/$(lowerName "$sAlg")_gpu.txt" \
      ./build/dsi_verify --cpi "$sCpi" --golden "${sData}/golden/${sGolden}" \
        --config "$sConf" --backend GPU --tolerance 1e-2
  else
    check "${sAlg} gpu" "${sOut}/$(lowerName "$sAlg")_gpu.txt" \
      ./build/dsi_verify --cpi "$sCpi" --against CPU --precision FP64 \
        --config "$sConf" --backend GPU --tolerance 1e-2
  fi
  check "ard chained ${sAlg} gpu" "${sOut}/ard_chained_$(lowerName "$sAlg")_gpu.txt" \
    ./build/ard_verify --chained --cpi "$sCpi" --cleaned "$sCleaned" \
      --golden "${sData}/golden/golden_ard.bin" --backend GPU --config "$sConf"
done

echo "LEVEL1_DONE ${sScenario}" | tee -a "$sSummary"
