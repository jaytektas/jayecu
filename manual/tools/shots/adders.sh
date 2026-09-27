#!/usr/bin/env bash
# The screenshots of chapter 28 (Power adders), reproducibly. Re-run after a UI change:
#   manual/tools/shots/adders.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700
P="Configuration/Engine Functions"
EX=(--set nitrous.enabled=1 --set wmi.enabled=1 --set anti_lag.enabled=1)

"$SHOOT" --crop 1300x220+310+237 "${EX[@]}" "$P/Nitrous"           nitrous-page
"$SHOOT" --crop 870x160+310+237  "${EX[@]}" "$P/Water & Methanol"  wmi-page
"$SHOOT" --crop 870x190+310+237  "${EX[@]}" "$P/Anti-Lag"          antilag-page
