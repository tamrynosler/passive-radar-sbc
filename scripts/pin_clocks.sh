#!/usr/bin/env bash
#
# pin_clocks.sh - sourced by the Jetson run scripts, never run on its own.
#
#   source scripts/pin_clocks.sh
#   pinClocks "$sOutDir"
#
# Max power mode, clocks pinned, fan at 100 of 255 (enough to hold the
# temperature; 255 only costs power), and a record of both in sOutDir:
# clocks.txt from jetson_clocks --show, tegrastats.log at 1 Hz. On exit the
# tegrastats logger stops and the fan goes back to the thermal governor.
# Needs sudo, so the calling script must run under `ssh -t`.

pinClocks() {
  local sOutDir="$1"
  echo "Pinning clocks (sudo)"
  sudo nvpmodel -m 0 || echo "  nvpmodel failed; continuing"
  sudo jetson_clocks || echo "  jetson_clocks FAILED; results NOT citable"
  echo 100 | sudo tee /sys/devices/pwm-fan/target_pwm >/dev/null || true
  sudo jetson_clocks --show > "${sOutDir}/clocks.txt" 2>&1 || true
  sudo tegrastats --interval 1000 --logfile "${sOutDir}/tegrastats.log" &
  iTegrastatsPid=$!
  trap releaseClocks EXIT
}

releaseClocks() {
  sudo kill "${iTegrastatsPid}" >/dev/null 2>&1 || true
  echo 0 | sudo tee /sys/devices/pwm-fan/target_pwm >/dev/null || true
}
