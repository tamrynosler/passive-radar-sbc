#!/usr/bin/env bash
#
# results_run.sh - the level-2 runs of docs/results-rerun-plan.md, on the
# board, from the repo root, inside tmux or in the foreground of an ssh call.
#
#   scripts/results_run.sh config OUT_CONF ALG CHANNELS FRAMES [BASE_CONF]
#       Mac or board. Derives OUT_CONF from BASE_CONF (default
#       config/fers.conf) with the settings every rerun shares (below),
#       the canceller ALG, CHANNELS 2 or 3 (3 keeps the second surveillance
#       receiver, 2 sets it to none) and FRAMES 0 or 1 (ARD_WRITE_FILES).
#
#   scripts/results_run.sh run SCENARIO CAPTURE ALG CHANNELS FRAMES [ROOT]
#       Board. One replay of CAPTURE through pbr_run, output to
#       ROOT/SCENARIO/ALG/<stamp>[_2ch]/ (ROOT defaults to
#       /data/results/L2_performance), with the memory sampler beside it,
#       config.conf, run.log, mem.log and the run.txt manifest of
#       results/README.md. Appends one line to ROOT/runs.log.
#
#   scripts/results_run.sh sweep SCENARIO CAPTURE CHANNELS FRAMES [ROOT]
#       Board. `run` for every canceller in turn. A canceller that already
#       has a run with exit 0 in ROOT/runs.log for this scenario is skipped,
#       so a stopped sweep resumes. Ends runs.log with SWEEP_DONE SCENARIO.
#
#   scripts/results_run.sh live SCENARIO ALG CHANNELS FRAMES SECONDS [ROOT]
#       Board. As `run`, but pbr_run --live for SECONDS, fed by FERS on the
#       Mac (start this first, then fers-cli at this board's IPv4:4991).
#       The stamp ends in _live; run.txt says "live" for the capture.
#
# Settings shared by every rerun (decided by the author 2026-10-19):
#   CFAR 1-D along Doppler: guard 4, training 6, range 0 / 0, Pfa 1e-5
#   ECA-LD solves the whole CPI in one batch (Lu et al. 2026 use no batches)
#   ECA-B and ECA-m update once per batch (ECA_UPDATE_INTERVAL_s = batch)
#   GPU backend, no stream to the viewer, every frame kept when FRAMES = 1
#
# The board's checkout is an rsync copy, so its git hash is stale: the
# Mac passes its own as SOURCE_GIT=<hash> in the environment.
#
# run, sweep and live derive each run's config from config/fers.conf, or
# from the file named by BASE_CONF=<path> in the environment (run.txt
# records which). EXTRA_KEYS="KEY=value ..." in the environment sets keys
# after the shared settings below, e.g. a batch length for a sweep; run.txt
# records them. STAMP=<yyyymmdd_hhmmss> in the environment names the run
# folder of `run` instead of the board's clock, which has no NTP on the
# direct cable link (pass the Mac's `date +%Y%m%d_%H%M%S`).
#
# Plain bash 3.2 compatible (macOS) and bash 4.4 (Ubuntu 18.04).

kAlgs="ECA-CD ECA-B ECA-S ECA-LD FBLMS ECA-m CGLS"
kDefaultRoot="/data/results/L2_performance"
kProcDir="${PROC_DIR:-/proc}"
kGpuFreqDir="/sys/devices/gpu.0/devfreq/57000000.gpu"
kCpuFreqDir="/sys/devices/system/cpu/cpu0/cpufreq"

usage() {
  sed -n '3,42p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
}

# ---------------------------------------------------------------------------
# config
# ---------------------------------------------------------------------------

# setKey FILE KEY VALUE: replace the KEY = ... line, keep every comment.
# Fails if the key is not in the file exactly once at line start.
setKey() {
  local sFile="$1" sKey="$2" sValue="$3"
  local iCount
  iCount=$(grep -c "^${sKey} *=" "$sFile")
  if [ "$iCount" != "1" ]; then
    echo "results_run: key ${sKey} found ${iCount} times in ${sFile}, need 1" >&2
    return 1
  fi
  sed "s/^${sKey} *=.*/${sKey} = ${sValue}/" "$sFile" > "${sFile}.tmp" &&
    mv "${sFile}.tmp" "$sFile"
}

# makeConfig OUT ALG CHANNELS FRAMES BASE
makeConfig() {
  local sOut="$1" sAlg="$2" iChannels="$3" iFrames="$4" sBase="$5"
  if [ ! -f "$sBase" ]; then
    echo "results_run: no base config $sBase" >&2
    return 1
  fi
  case " $kAlgs " in
    *" $sAlg "*) ;;
    *) echo "results_run: unknown canceller $sAlg" >&2; return 1 ;;
  esac
  if [ "$iChannels" != "2" ] && [ "$iChannels" != "3" ]; then
    echo "results_run: CHANNELS must be 2 or 3" >&2
    return 1
  fi
  if [ "$iFrames" != "0" ] && [ "$iFrames" != "1" ]; then
    echo "results_run: FRAMES must be 0 or 1" >&2
    return 1
  fi
  cp "$sBase" "$sOut" || return 1

  setKey "$sOut" DSI_ALGORITHM "$sAlg" || return 1
  setKey "$sOut" DSI_BACKEND GPU || return 1
  if [ "$sAlg" = "ECA-B" ] || [ "$sAlg" = "ECA-m" ]; then
    setKey "$sOut" ECA_UPDATE_INTERVAL_s 0.025 || return 1
  fi
  setKey "$sOut" ECA_LD_BATCH_s 1.0 || return 1
  setKey "$sOut" CFAR_PFA 1e-5 || return 1
  setKey "$sOut" CFAR_GUARD_RANGE 0 || return 1
  setKey "$sOut" CFAR_GUARD_DOPPLER 4 || return 1
  setKey "$sOut" CFAR_TRAIN_RANGE 0 || return 1
  setKey "$sOut" CFAR_TRAIN_DOPPLER 6 || return 1
  if [ "$iChannels" = "2" ]; then
    setKey "$sOut" SURVEILLANCE2_RECEIVER_NAME none || return 1
  fi
  setKey "$sOut" ARD_STREAM 0 || return 1
  setKey "$sOut" ARD_WRITE_FILES "$iFrames" || return 1
  setKey "$sOut" ARD_SNAPSHOT_EVERY 1 || return 1

  # EXTRA_KEYS="KEY=value KEY=value": last word, after the shared settings
  local sKv
  for sKv in ${EXTRA_KEYS:-}; do
    setKey "$sOut" "${sKv%%=*}" "${sKv#*=}" || return 1
  done
}

# ---------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------

# memLine PID: one sampler line, "HH:MM:SS used=.. free=.. avail=.. [vmhwm=..]".
# free -m line 2 is "Mem: total used free shared buff/cache available".
memLine() {
  local iPid="$1"
  local sTime sFree sHwm
  sTime=$(date +%H:%M:%S)
  sFree=$(free -m | awk 'NR == 2 { printf "used=%s free=%s avail=%s", $3, $4, $7 }')
  sHwm=$(awk '/^VmHWM:/ { printf " vmhwm=%s", $2 }' "${kProcDir}/${iPid}/status" 2>/dev/null)
  echo "${sTime} ${sFree}${sHwm}"
}

# sampler PID LOGFILE: until killed, append a line every 0.5 s.
sampler() {
  local iPid="$1" sLog="$2"
  while true; do
    memLine "$iPid" >> "$sLog"
    sleep 0.5
  done
}

# clocksLine: "pinned" when the GPU and CPU sit at their maximum, else
# "NOT pinned", with the frequencies read.
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

# writeManifest RUNDIR SCENARIO CAPTURE ALG CHANNELS FRAMES CMDLINE
writeManifest() {
  local sRun="$1" sScenario="$2" sCapture="$3" sAlg="$4" iChannels="$5" iFrames="$6" sCmd="$7"
  {
    echo "scenario     ${sScenario}"
    echo "canceller    ${sAlg}"
    echo "channels     ${iChannels}"
    echo "frames       ${iFrames}"
    echo "git          ${SOURCE_GIT:-$(git rev-parse --short HEAD 2>/dev/null || echo unknown)}"
    echo "host         $(hostname)"
    echo "date         $(date -Is)"
    echo "command      ${sCmd}"
    echo "capture      ${sCapture}"
    if [ "$sCapture" = "live" ]; then
      echo "captureSha   none (live from FERS on the Mac)"
    else
      echo "captureSha   $(sha256sum "$sCapture" | awk '{ print $1 }')"
    fi
    echo "configSha    $(sha256sum "${sRun}/config.conf" | awk '{ print $1 }')"
    echo "baseConf     ${BASE_CONF:-config/fers.conf}"
    echo "extraKeys    ${EXTRA_KEYS:-none}"
    echo "clocks       $(clocksLine)"
    echo "l4t          $(head -1 /etc/nv_tegra_release 2>/dev/null || echo unknown)"
    echo "ecaLdBatch   ECA_LD_BATCH_s = 1.0: whole-CPI solve, Lu et al. 2026 (Digital Signal Processing 177, 106133) use no batches"
  } > "${sRun}/run.txt"
}

# runOne SCENARIO CAPTURE ALG CHANNELS FRAMES ROOT [SECONDS]
# CAPTURE is a .pbrcap to replay, or "live" with SECONDS to receive from UDP.
runOne() {
  local sScenario="$1" sCapture="$2" sAlg="$3" iChannels="$4" iFrames="$5" sRoot="$6" iSeconds="$7"
  local sStamp sRun sConf sCmd sSource iPid iSampler iExit sStart sEnd

  if [ "$sCapture" != "live" ] && [ ! -f "$sCapture" ]; then
    echo "results_run: no capture $sCapture" >&2
    return 1
  fi
  if [ ! -x ./build/pbr_run ]; then
    echo "results_run: ./build/pbr_run not found; run from the repo root after make build" >&2
    return 1
  fi
  sStamp=${STAMP:-$(date +%Y%m%d_%H%M%S)}
  [ "$iChannels" = "2" ] && sStamp="${sStamp}_2ch"
  [ "$sCapture" = "live" ] && sStamp="${sStamp}_live"
  sRun="${sRoot}/${sScenario}/${sAlg}/${sStamp}"
  mkdir -p "$sRun" || return 1

  # the config pbr_run reads is kept apart from the copy --save-config makes
  sConf="${sRun}/input.conf"
  makeConfig "$sConf" "$sAlg" "$iChannels" "$iFrames" "${BASE_CONF:-config/fers.conf}" || return 1

  if [ "$sCapture" = "live" ]; then
    sSource="--live --seconds ${iSeconds}"
  else
    sSource="--file ${sCapture}"
  fi
  sCmd="./build/pbr_run ${sSource} --config ${sConf} --out ${sRun} --save-config"
  sStart=$(date +%H:%M:%S)
  echo "${sScenario} ${sAlg} ${iChannels}ch: start ${sStart} -> ${sRun}"
  $sCmd > "${sRun}/run.log" 2>&1 &
  iPid=$!
  sampler "$iPid" "${sRun}/mem.log" &
  iSampler=$!

  wait "$iPid"
  iExit=$?
  kill "$iSampler" 2>/dev/null
  wait "$iSampler" 2>/dev/null
  rm -f "$sConf"

  writeManifest "$sRun" "$sScenario" "$sCapture" "$sAlg" "$iChannels" "$iFrames" "$sCmd"
  sEnd=$(date +%H:%M:%S)
  echo "${sScenario} ${sAlg} ${iChannels}ch ${sStamp} start=${sStart} end=${sEnd} exit=${iExit}" >> "${sRoot}/runs.log"
  echo "${sScenario} ${sAlg} ${iChannels}ch: end ${sEnd} exit=${iExit}"
  return "$iExit"
}

# isDone SCENARIO ALG CHANNELS ROOT: true when runs.log has an exit 0 line.
isDone() {
  grep -q "^$1 $2 $3ch .* exit=0$" "$4/runs.log" 2>/dev/null
}

runSweep() {
  local sScenario="$1" sCapture="$2" iChannels="$3" iFrames="$4" sRoot="$5"
  local sAlg
  for sAlg in $kAlgs; do
    if isDone "$sScenario" "$sAlg" "$iChannels" "$sRoot"; then
      echo "${sScenario} ${sAlg} ${iChannels}ch: already done, skipping"
      continue
    fi
    runOne "$sScenario" "$sCapture" "$sAlg" "$iChannels" "$iFrames" "$sRoot" || true
  done
  echo "SWEEP_DONE ${sScenario} ${iChannels}ch" >> "${sRoot}/runs.log"
  echo "SWEEP_DONE ${sScenario}"
}

# ---------------------------------------------------------------------------

sCommand="$1"
case "$sCommand" in
  config)
    [ -n "$5" ] || usage
    makeConfig "$2" "$3" "$4" "$5" "${6:-config/fers.conf}"
    ;;
  run)
    [ -n "$6" ] || usage
    mkdir -p "${7:-$kDefaultRoot}" || exit 1
    runOne "$2" "$3" "$4" "$5" "$6" "${7:-$kDefaultRoot}"
    ;;
  sweep)
    [ -n "$5" ] || usage
    mkdir -p "${6:-$kDefaultRoot}" || exit 1
    runSweep "$2" "$3" "$4" "$5" "${6:-$kDefaultRoot}"
    ;;
  live)
    [ -n "$6" ] || usage
    mkdir -p "${7:-$kDefaultRoot}" || exit 1
    runOne "$2" live "$3" "$4" "$5" "${7:-$kDefaultRoot}" "$6"
    ;;
  *)
    usage
    ;;
esac
