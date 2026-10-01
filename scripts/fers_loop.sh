#!/usr/bin/env bash
#
# fers_loop.sh - run the FERS scenario on repeat, streaming VITA 49 to the
# chain, until Ctrl-C. Run this ON THE MAC (FERS lives here).
#
# The scenario is 180 s long and fers-cli exits at its end, so "repeat" is a
# loop of runs. Each run restarts the timeline at t = 0; the ingest sees the
# backwards jump, re-anchors and clears its buffers (ChannelIngest), so the
# chain carries on. Expect a gap of a few seconds at every restart (FERS
# takes ~3 s to emit its first packet): blank CPIs, black rows in the
# viewer's waterfall. That is the restart, not the chain falling behind.
#
#   bash scripts/fers_loop.sh 192.168.10.3            # the board's address
#   bash scripts/fers_loop.sh 192.168.10.3 4991 0.005 # port, full scale
#
# The full scale must match VITA49_FULLSCALE in config/fers.conf, and the
# chain must run with --config config/fers.conf (204800 Hz).

set -u

HOST="${1:?usage: $0 HOST [PORT] [FULLSCALE]}"
PORT="${2:-4991}"
FULLSCALE="${3:-0.005}"
FERS="${FERS:-$HOME/FERS/build/release/packages/fers-cli/fers-cli}"
SCENARIO="${SCENARIO:-$HOME/fers-runs/clean/SingleTargetCleanNativeFileCw.fersxml}"
OUTDIR="${OUTDIR:-/tmp/fers-out}"

[[ -x "$FERS" ]] || { echo "fers-cli not found at $FERS" >&2; exit 1; }
[[ -f "$SCENARIO" ]] || { echo "scenario not found at $SCENARIO" >&2; exit 1; }
mkdir -p "$OUTDIR"

trap 'echo; echo "fers_loop: stopped after $RUN runs"; exit 0' INT TERM

RUN=0
while true; do
  RUN=$((RUN + 1))
  echo "fers_loop: run $RUN -> $HOST:$PORT  ($(date +%H:%M:%S))"
  "$FERS" "$SCENARIO" --out-dir="$OUTDIR" \
      --vita49 "$HOST:$PORT" --vita49-fullscale "$FULLSCALE" -n=6 \
      > "$OUTDIR/fers_run_$RUN.log" 2>&1
  STATUS=$?
  if [[ $STATUS -ne 0 ]]; then
    echo "fers_loop: fers-cli exited with $STATUS; see $OUTDIR/fers_run_$RUN.log" >&2
    sleep 5
  fi
done
