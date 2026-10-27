#!/usr/bin/env bash
#
# backend_bench.sh - CPU against GPU timing for Figure 4.5 and the backends
# table of the results chapter: every canceller and the map + CFAR on the
# frozen CPI 0 of S0_clean, on the CPU backend and the GPU backend, clocks
# pinned. The CPU cancellers run in the configured DSI_MATH_PRECISION (FP64
# in config/fers.conf); the CPU map + CFAR is FP32 by design, with a direct
# DFT for Doppler (src/ard/cpu/ArdCpu.cpp), so its time is a reference, not
# a tuned CPU figure.
#
#   scripts/backend_bench.sh
#
# Board, from the repo root, after make build, under `ssh -t` (pinning the
# clocks needs sudo) and inside tmux, since the CPU runs take about 15 min:
#   ssh -t jetson 'cd ~/passive-radar-sbc && tmux new -s bench \
#       "SOURCE_GIT=<hash> STAMP=<stamp> scripts/backend_bench.sh; bash"'
#
# Output: ROOT/<stamp>_backends/ (ROOT defaults to /data/results/L3_bench):
#   dsi_bench.csv        every canceller, CPU and GPU, speed-work settings
#   dsi_bench_cherk.csv  ECA-m, ECA-B and ECA-S on the GPU with the Gram
#                        recurrence off (ECA_GRAM_RECURRENCE = 0): the
#                        "before" of the Gram item, as the cherk rows of
#                        results/performance_improvements/after/
#   dsi_bench_chain.csv  ECA-LD on CPU and GPU with the chain's settings
#                        (one batch per CPI)
#   ard_bench.csv        map + CFAR (Batches), CPU and GPU, speed-work settings
#   ard_bench_chain.csv  the same with the chain's settings (CFAR training 6)
#   <name>.txt           each run's printout
#   conf_*.conf          the configs used, verbatim
#   run.txt              manifest: git, inputs, clocks, OC alarms, one line per run
#   clocks.txt, tegrastats.log   from scripts/pin_clocks.sh
#
# The speed-work settings are those of results/performance_improvements/baseline/,
# so the GPU rows compare with its "before" medians: config/fers.conf with
# the second surveillance receiver off (the CPI has two channels) and, for
# ECA-B only, ECA_UPDATE_INTERVAL_s = 0.025 (one update per batch). The
# chain settings come from `scripts/results_run.sh config`, as in every
# level-2 run.
#
# Environment: SOURCE_GIT and STAMP from the Mac (the board's checkout has
# no .git, and its clock has no NTP on the direct link); ROOT, BIN_DIR
# (default ./build), ITERS_GPU (default 20), ITERS_CPU (default 3); NO_PIN=1
# skips the clock pinning, for a dry run on the Mac (run.txt then says so).
#
# Plain bash 3.2 compatible (macOS) and bash 4.4 (Ubuntu 18.04).

kCpi="data/S0_clean/cpi_0000.bin"
kCleaned="data/S0_clean/cleaned_0000.bin"
kAlgsGpu="ECA-m ECA-B ECA-S ECA-CD ECA-LD CGLS FBLMS"
kAlgsCpu="ECA-CD ECA-LD FBLMS ECA-m ECA-B CGLS ECA-S"   # slowest last
kGpuFreqDir="/sys/devices/gpu.0/devfreq/57000000.gpu"
kCpuFreqDir="/sys/devices/system/cpu/cpu0/cpufreq"

g_sBin="${BIN_DIR:-./build}"
g_iItersGpu="${ITERS_GPU:-20}"
g_iItersCpu="${ITERS_CPU:-3}"
g_sOut="${ROOT:-/data/results/L3_bench}/${STAMP:-$(date +%Y%m%d_%H%M%S)}_backends"
g_iFailures=0

# setKey FILE KEY VALUE: replace the KEY = ... line, keep every comment.
# Fails if the key is not in the file exactly once at line start.
setKey() {
  local sFile="$1" sKey="$2" sValue="$3"
  local iCount
  iCount=$(grep -c "^${sKey} *=" "$sFile")
  if [ "$iCount" != "1" ]; then
    echo "backend_bench: key ${sKey} found ${iCount} times in ${sFile}, need 1" >&2
    return 1
  fi
  sed "s/^${sKey} *=.*/${sKey} = ${sValue}/" "$sFile" > "${sFile}.tmp" &&
    mv "${sFile}.tmp" "$sFile"
}

# deriveConf OUT BASE KEY VALUE: a copy of BASE with one key changed.
deriveConf() {
  cp "$2" "$1" && setKey "$1" "$3" "$4"
}

# sha256Of FILE: the hash alone (sha256sum on the board, shasum on the Mac).
sha256Of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{ print $1 }'
  else
    shasum -a 256 "$1" | awk '{ print $1 }'
  fi
}

# clocksLine: pinned or not, from sysfs (no sudo needed).
clocksLine() {
  local sGpuCur sGpuMax sCpuCur sCpuMax sState
  sGpuCur=$(cat "${kGpuFreqDir}/cur_freq" 2>/dev/null || echo 0)
  sGpuMax=$(cat "${kGpuFreqDir}/max_freq" 2>/dev/null || echo 1)
  sCpuCur=$(cat "${kCpuFreqDir}/scaling_cur_freq" 2>/dev/null || echo 0)
  sCpuMax=$(cat "${kCpuFreqDir}/cpuinfo_max_freq" 2>/dev/null || echo 1)
  sState="NOT pinned"
  if [ "$sGpuCur" = "$sGpuMax" ] && [ "$sCpuCur" = "$sCpuMax" ]; then
    sState="pinned"
  fi
  echo "${sState} (gpu ${sGpuCur} of ${sGpuMax} Hz, cpu0 ${sCpuCur} of ${sCpuMax} kHz)"
}

# ocAlarms: soctherm over-current alarms since boot (they throttle the clocks).
ocAlarms() {
  dmesg 2>/dev/null | grep -c "OC ALARM" || true
}

# runBench NAME CSV TOOL ARGS...: one bench run, its printout to NAME.txt,
# one row appended to CSV, and a line in run.txt with its duration and exit.
runBench() {
  local sName="$1" sCsv="$2" sTool="$3"
  local iStart iEnd iExit
  shift 3
  echo "$(date +%H:%M:%S)  ${sName}"
  iStart=$(date +%s)
  "${g_sBin}/${sTool}" "$@" --csv "${g_sOut}/${sCsv}" > "${g_sOut}/${sName}.txt" 2>&1
  iExit=$?
  iEnd=$(date +%s)
  echo "run          ${sName}  $((iEnd - iStart)) s  exit ${iExit}  ${sTool} $*" >> "${g_sOut}/run.txt"
  if [ "$iExit" != "0" ]; then
    echo "  FAILED (exit ${iExit}); see ${g_sOut}/${sName}.txt"
    g_iFailures=$((g_iFailures + 1))
  fi
}

# confFor ALG CHERK: the speed-work config of ALG, with the Gram recurrence
# off when CHERK is 1.
confFor() {
  local sConf="conf_bench"
  if [ "$1" = "ECA-B" ]; then sConf="${sConf}_ecab"; fi
  if [ "$2" = "1" ]; then sConf="${sConf}_cherk"; fi
  echo "${g_sOut}/${sConf}.conf"
}

# ---- checks ----------------------------------------------------------------
for sFile in "$kCpi" "$kCleaned" config/fers.conf scripts/results_run.sh \
             "${g_sBin}/dsi_bench" "${g_sBin}/ard_bench"; do
  if [ ! -e "$sFile" ]; then
    echo "backend_bench: no ${sFile}; run from the repo root after make build" >&2
    exit 1
  fi
done
if [ -e "$g_sOut" ]; then
  echo "backend_bench: ${g_sOut} exists; give a new STAMP" >&2
  exit 1
fi
mkdir -p "$g_sOut" || exit 1

# ---- configs ---------------------------------------------------------------
deriveConf "${g_sOut}/conf_bench.conf" config/fers.conf SURVEILLANCE2_RECEIVER_NAME none &&
  deriveConf "${g_sOut}/conf_bench_ecab.conf" "${g_sOut}/conf_bench.conf" ECA_UPDATE_INTERVAL_s 0.025 &&
  deriveConf "${g_sOut}/conf_bench_cherk.conf" "${g_sOut}/conf_bench.conf" ECA_GRAM_RECURRENCE 0 &&
  deriveConf "${g_sOut}/conf_bench_ecab_cherk.conf" "${g_sOut}/conf_bench_ecab.conf" ECA_GRAM_RECURRENCE 0 &&
  scripts/results_run.sh config "${g_sOut}/conf_chain.conf" ECA-LD 2 0 > /dev/null || exit 1

# ---- manifest --------------------------------------------------------------
{
  echo "what         CPU against GPU, frozen CPI 0 of S0_clean; CPU map + CFAR is FP32 by design"
  echo "git          ${SOURCE_GIT:-unknown}"
  echo "host         $(hostname)"
  echo "stamp        $(basename "$g_sOut")"
  echo "boardDate    $(date '+%Y-%m-%dT%H:%M:%S%z')"
  echo "cpi          ${kCpi}  sha256 $(sha256Of "$kCpi")"
  echo "cleaned      ${kCleaned}  sha256 $(sha256Of "$kCleaned")"
  for sConf in "${g_sOut}"/conf_*.conf; do
    echo "config       $(basename "$sConf")  sha256 $(sha256Of "$sConf")"
  done
  echo "precision    $(grep '^DSI_MATH_PRECISION' "${g_sOut}/conf_bench.conf")"
  echo "iters        GPU ${g_iItersGpu}, CPU ${g_iItersCpu}, each after one warm-up"
  echo "l4t          $(head -1 /etc/nv_tegra_release 2>/dev/null || echo unknown)"
  echo "ocAlarms     $(ocAlarms) before"
} > "${g_sOut}/run.txt"

if [ "${NO_PIN:-0}" = "1" ]; then
  echo "pinning      skipped (NO_PIN=1): NOT citable" >> "${g_sOut}/run.txt"
else
  source scripts/pin_clocks.sh
  pinClocks "$g_sOut"
  # Keep the sudo ticket alive, so releaseClocks at exit does not wait at a
  # password prompt after the long CPU runs.
  ( while kill -0 $$ 2>/dev/null; do sudo -n true 2>/dev/null; sleep 60; done ) &
  sleep 2   # idle tegrastats samples before the first run
fi
echo "clocks       $(clocksLine) at the start" >> "${g_sOut}/run.txt"
echo "Output: ${g_sOut}"

# ---- GPU -------------------------------------------------------------------
for sAlg in $kAlgsGpu; do
  runBench "dsi_${sAlg}_GPU" dsi_bench.csv dsi_bench --cpi "$kCpi" \
    --config "$(confFor "$sAlg" 0)" --algorithm "$sAlg" --backend GPU --iters "$g_iItersGpu"
done
for sAlg in ECA-m ECA-B ECA-S; do
  runBench "dsi_${sAlg}_GPU_cherk" dsi_bench_cherk.csv dsi_bench --cpi "$kCpi" \
    --config "$(confFor "$sAlg" 1)" --algorithm "$sAlg" --backend GPU --iters "$g_iItersGpu"
done
runBench "dsi_ECA-LD_GPU_chain" dsi_bench_chain.csv dsi_bench --cpi "$kCpi" \
  --config "${g_sOut}/conf_chain.conf" --algorithm ECA-LD --backend GPU --iters "$g_iItersGpu"
runBench "ard_GPU" ard_bench.csv ard_bench --cleaned "$kCleaned" \
  --config "${g_sOut}/conf_bench.conf" --backend GPU --algorithm Batches --iters "$g_iItersGpu"
runBench "ard_GPU_chain" ard_bench_chain.csv ard_bench --cleaned "$kCleaned" \
  --config "${g_sOut}/conf_chain.conf" --backend GPU --algorithm Batches --iters "$g_iItersGpu"

# ---- CPU -------------------------------------------------------------------
runBench "ard_CPU" ard_bench.csv ard_bench --cleaned "$kCleaned" \
  --config "${g_sOut}/conf_bench.conf" --backend CPU --algorithm Batches --iters "$g_iItersCpu"
runBench "ard_CPU_chain" ard_bench_chain.csv ard_bench --cleaned "$kCleaned" \
  --config "${g_sOut}/conf_chain.conf" --backend CPU --algorithm Batches --iters "$g_iItersCpu"
runBench "dsi_ECA-LD_CPU_chain" dsi_bench_chain.csv dsi_bench --cpi "$kCpi" \
  --config "${g_sOut}/conf_chain.conf" --algorithm ECA-LD --backend CPU --iters "$g_iItersCpu"
for sAlg in $kAlgsCpu; do
  runBench "dsi_${sAlg}_CPU" dsi_bench.csv dsi_bench --cpi "$kCpi" \
    --config "$(confFor "$sAlg" 0)" --algorithm "$sAlg" --backend CPU --iters "$g_iItersCpu"
done

# ---- end -------------------------------------------------------------------
{
  echo "clocks       $(clocksLine) at the end"
  echo "ocAlarms     $(ocAlarms) after"
  echo "failures     ${g_iFailures}"
} >> "${g_sOut}/run.txt"
echo "Done, ${g_iFailures} failed. Output: ${g_sOut}"
