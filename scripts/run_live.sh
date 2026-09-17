#!/usr/bin/env bash
#
# run_live.sh - the live chain on the Jetson, with clocks pinned and
# tegrastats logged. Run this ON THE JETSON.
#
# FERS streams from another machine on the same subnet. Start this FIRST (it
# binds the UDP port), then start fers-cli pointed at this board:
#
#   cd ~/FERS
#   ./build/release/packages/fers-cli/fers-cli \
#       ~/fers-runs/clean/SingleTargetCleanNativeFileCw.fersxml \
#       --out-dir=/tmp/fers-out \
#       --vita49 <THIS-BOARD-IP>:4991 \
#       --vita49-fullscale 0.005 -n=6
#
# FERS takes about 3 s to emit its first packet, and the first CPI cannot
# complete until a full second of signal has arrived, so expect roughly 8 s of
# startup offset before CPI 0. That offset is reported separately from lag
# growth, which is the number that actually says whether the chain kept up.
#
#   bash scripts/run_live.sh --seconds 120

set -euo pipefail

SECONDS_TO_RUN=30
FULLSCALE=0.005
BACKEND=GPU
OUTROOT="${OUTROOT:-/data/results}"
ARD_EVERY=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --seconds)   SECONDS_TO_RUN="$2"; shift 2 ;;
    --fullscale) FULLSCALE="$2";      shift 2 ;;
    --backend)   BACKEND="$2";        shift 2 ;;
    --ard-every) ARD_EVERY="$2";      shift 2 ;;
    -h|--help)
      echo "usage: $0 [--seconds N] [--fullscale F] [--ard-every N]"
      exit 0 ;;
    *) echo "unrecognised argument: $1" >&2; exit 1 ;;
  esac
done

STAMP="$(date +%Y%m%d_%H%M%S)"
OUTDIR="${OUTROOT}/live_${STAMP}"
mkdir -p "${OUTDIR}/ard"

echo "Output dir : ${OUTDIR}"
echo "Board IP   : $(hostname -I | awk '{print $1}')   <- point fers-cli here"

{
  echo "date        $(date -Is)"
  echo "seconds     ${SECONDS_TO_RUN}"
  echo "fullscale   ${FULLSCALE}"
  echo "backend     ${BACKEND}"
  echo "l4t         $(head -1 /etc/nv_tegra_release 2>/dev/null || echo unknown)"
  echo "git         $(git -C . rev-parse --short HEAD 2>/dev/null || echo unknown)"
} > "${OUTDIR}/run.txt"

echo "Setting nvpmodel mode 0 (max)"
sudo nvpmodel -m 0 || echo "  (nvpmodel failed; continuing)"
echo "Pinning clocks"
sudo jetson_clocks || echo "  (jetson_clocks failed; continuing UNPINNED -- not citable)"
echo 255 | sudo tee /sys/devices/pwm-fan/target_pwm >/dev/null || true
sudo jetson_clocks --show > "${OUTDIR}/clocks.txt" 2>&1 || true

sudo tegrastats --interval 1000 --logfile "${OUTDIR}/tegrastats.log" &
TEGRA_PID=$!
cleanup() {
  sudo kill "${TEGRA_PID}" >/dev/null 2>&1 || true
  echo 0 | sudo tee /sys/devices/pwm-fan/target_pwm >/dev/null || true
}
trap cleanup EXIT

echo
echo "Listening now -- start fers-cli on the other machine."
echo

./build/pbr_run --live \
  --seconds "${SECONDS_TO_RUN}" \
  --backend "${BACKEND}" \
  --fullscale "${FULLSCALE}" \
  --ard-dir "${OUTDIR}/ard" \
  --ard-every "${ARD_EVERY}" \
  --telemetry "${OUTDIR}/telemetry.csv" \
  --detections "${OUTDIR}/detections.csv" \
  | tee "${OUTDIR}/run.log"

echo
echo "Results  : ${OUTDIR}"
echo "Play it  : matlab -batch \"addpath('matlab'); play_ard('${OUTDIR}/ard')\""
