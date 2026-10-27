#!/usr/bin/env bash
#
# cpi_sweep.sh - the CPI length sweep of docs/cpi-sweep-plan.md.
#
#   scripts/cpi_sweep.sh configs OUTDIR [BASE_CONF]
#       Mac or board. Writes OUTDIR/T<len>/<alg>/config.conf for every
#       (length, canceller), derived from BASE_CONF (default config/fers.conf).
#
#   scripts/cpi_sweep.sh truth CONFDIR
#       Mac. One MATLAB fers_truth run per length, using CONFDIR/T<len>/ECA-CD/
#       config.conf, writing data/S0_aoa/truth_T<len>.csv. Run from the repo root.
#
#   scripts/cpi_sweep.sh run CAPTURE [RESULTS_ROOT]
#       Board, from the repo root, inside tmux. RESULTS_ROOT defaults to
#       /data/results/cpi_sweeps. Replays CAPTURE through pbr_run for every
#       length then canceller, with a memory sampler beside each run. Appends
#       to RESULTS_ROOT/runs.log; ends it with ALL_DONE. A run whose run.log
#       already holds a "pbr_run summary" is skipped, so a rerun resumes.
#
# Optional, in the environment (configs and run):
#   SWEEP_ALGS="ECA-LD ..."   only these cancellers (default all seven)
#   ECA_LD_BATCH=whole        ECA-LD solves the whole CPI in one batch, as
#                             the chain does, instead of 0.1 s batches
#   FRAMES_ALGS="ECA-CD ..."  cancellers whose maps are written (default ECA-CD)
#
# Plain bash 3.2 compatible (macOS) and bash 4.4 (Ubuntu 18.04).

kLengths="0.5 1.0 1.5 2.0 2.5 3.0 3.5 4.0"
kAlgs="${SWEEP_ALGS:-ECA-CD ECA-B ECA-S ECA-LD FBLMS ECA-m CGLS}"
kFramesAlgs="${FRAMES_ALGS:-ECA-CD}"
kDefaultRoot="/data/results/cpi_sweeps"
kProcDir="${PROC_DIR:-/proc}"    # overridable only so the sampler can be tested

usage() {
  sed -n '3,19p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
}

# ---------------------------------------------------------------------------
# configs
# ---------------------------------------------------------------------------

# setKey FILE KEY VALUE: replace the KEY = ... line, keep every comment.
# Fails if the key is not in the file exactly once at line start.
setKey() {
  local sFile="$1" sKey="$2" sValue="$3"
  local iCount
  iCount=$(grep -c "^${sKey} *=" "$sFile")
  if [ "$iCount" != "1" ]; then
    echo "cpi_sweep: key ${sKey} found ${iCount} times in ${sFile}, need 1" >&2
    return 1
  fi
  sed "s/^${sKey} *=.*/${sKey} = ${sValue}/" "$sFile" > "${sFile}.tmp" &&
    mv "${sFile}.tmp" "$sFile"
}

# makeConfigs OUTDIR BASE: the 56 config files.
makeConfigs() {
  local sOut="$1" sBase="$2"
  local sLen sAlg sDir sConf sGuard sTrain sWrite
  if [ ! -f "$sBase" ]; then
    echo "cpi_sweep: no base config $sBase" >&2
    return 1
  fi
  for sLen in $kLengths; do
    # constant in hertz: 4 Hz guard, 8 Hz training, so cells = round(4 T), round(8 T)
    sGuard=$(awk -v t="$sLen" 'BEGIN { printf "%d", 4 * t + 0.5 }')
    sTrain=$(awk -v t="$sLen" 'BEGIN { printf "%d", 8 * t + 0.5 }')
    for sAlg in $kAlgs; do
      sDir="${sOut}/T${sLen}/${sAlg}"
      sConf="${sDir}/config.conf"
      mkdir -p "$sDir" || return 1
      cp "$sBase" "$sConf" || return 1

      setKey "$sConf" CPI_LENGTH_s "$sLen" || return 1
      setKey "$sConf" CPI_HOP_s "$sLen" || return 1
      setKey "$sConf" BUFFER_LENGTH_s 8 || return 1
      setKey "$sConf" CFAR_GUARD_DOPPLER "$sGuard" || return 1
      setKey "$sConf" CFAR_TRAIN_DOPPLER "$sTrain" || return 1
      setKey "$sConf" DSI_ALGORITHM "$sAlg" || return 1
      if [ "$sAlg" = "ECA-B" ] || [ "$sAlg" = "ECA-m" ]; then
        setKey "$sConf" ECA_UPDATE_INTERVAL_s 0.025 || return 1
      fi
      if [ "${ECA_LD_BATCH:-}" = "whole" ]; then
        setKey "$sConf" ECA_LD_BATCH_s "$sLen" || return 1
      else
        setKey "$sConf" ECA_LD_BATCH_s 0.1 || return 1
      fi
      setKey "$sConf" ARD_STREAM 0 || return 1
      sWrite=0
      case " $kFramesAlgs " in *" $sAlg "*) sWrite=1 ;; esac
      setKey "$sConf" ARD_WRITE_FILES "$sWrite" || return 1
      setKey "$sConf" ARD_SNAPSHOT_EVERY 1 || return 1
    done
  done
  echo "cpi_sweep: wrote configs under ${sOut}"
}

# ---------------------------------------------------------------------------
# truth
# ---------------------------------------------------------------------------

makeTruth() {
  local sConfDir="$1"
  local sLen sConf
  for sLen in $kLengths; do
    sConf="${sConfDir}/T${sLen}/ECA-CD/config.conf"
    if [ ! -f "$sConf" ]; then
      echo "cpi_sweep: missing $sConf (run the configs subcommand first)" >&2
      return 1
    fi
    echo "truth T${sLen}"
    /Applications/MATLAB_R2025a.app/bin/matlab -batch \
      "addpath('matlab'); fers_truth('scenarios/S0_aoa/scenario.fersxml', '${sConf}', 'data/S0_aoa/truth_T${sLen}.csv')" ||
      echo "cpi_sweep: truth T${sLen} FAILED" >&2
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

runSweep() {
  local sCapture="$1" sRoot="$2"
  local sConfRoot="${sRoot}/configs"   # inputs kept apart from the run directories:
                                       # --save-config copies the config onto
                                       # RUNDIR/config.conf and truncates its
                                       # own input if that is the same file
  local sLen sAlg sRun iPid iSampler iExit sStart sEnd

  if [ ! -f "$sCapture" ]; then
    echo "cpi_sweep: no capture $sCapture" >&2
    return 1
  fi
  if [ ! -x ./build/pbr_run ]; then
    echo "cpi_sweep: ./build/pbr_run not found; run from the repo root after make build" >&2
    return 1
  fi
  mkdir -p "$sRoot" || return 1
  makeConfigs "$sConfRoot" config/fers.conf || return 1

  for sLen in $kLengths; do
    for sAlg in $kAlgs; do
      sRun="${sRoot}/T${sLen}/${sAlg}"
      mkdir -p "$sRun" || return 1

      if grep -q "pbr_run summary" "${sRun}/run.log" 2>/dev/null; then
        echo "T${sLen} ${sAlg}: already done, skipping"
        continue
      fi

      sStart=$(date +%H:%M:%S)
      echo "T${sLen} ${sAlg}: start ${sStart}"
      rm -f "${sRun}/mem.log"
      ./build/pbr_run --file "$sCapture" --config "${sConfRoot}/T${sLen}/${sAlg}/config.conf" \
          --out "$sRun" --save-config > "${sRun}/run.log" 2>&1 &
      iPid=$!
      sampler "$iPid" "${sRun}/mem.log" &
      iSampler=$!

      wait "$iPid"
      iExit=$?
      kill "$iSampler" 2>/dev/null
      wait "$iSampler" 2>/dev/null

      sEnd=$(date +%H:%M:%S)
      echo "T${sLen} ${sAlg} start=${sStart} end=${sEnd} exit=${iExit}" >> "${sRoot}/runs.log"
      echo "T${sLen} ${sAlg}: end ${sEnd} exit=${iExit}"
    done
  done
  echo "ALL_DONE" >> "${sRoot}/runs.log"
  echo "ALL_DONE"
}

# ---------------------------------------------------------------------------

sCommand="$1"
case "$sCommand" in
  configs)
    [ -n "$2" ] || usage
    makeConfigs "$2" "${3:-config/fers.conf}"
    ;;
  truth)
    [ -n "$2" ] || usage
    makeTruth "$2"
    ;;
  run)
    [ -n "$2" ] || usage
    runSweep "$2" "${3:-$kDefaultRoot}"
    ;;
  *)
    usage
    ;;
esac
