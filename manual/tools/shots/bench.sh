#!/usr/bin/env bash
# The screenshots of chapter 43 (Bench testing), reproducibly. Re-run after a UI change:
#   manual/tools/shots/bench.sh
# Offline, so both tools show their controls and an empty view: live data needs an ECU and a stimulator.
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700
P="Configuration/Engine Configuration/Trigger System"
"$SHOOT" --crop 1400x745+300+163 --tool "Trigger Log"  "$P" bench-trigger-log
"$SHOOT" --crop 1400x745+300+163 --tool "Engine Cycle" "$P" bench-engine-cycle
