#!/usr/bin/env bash
# The screenshots of chapter 27 (Speed limiting), reproducibly. Re-run after a UI change:
#   manual/tools/shots/limits.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700               # the cruise pages are drawn wider than a 1600 window leaves room for

EX=(--set rev_limiter.enabled=1 --set pit_limiter.enabled=1 --set cruise_control.enabled=1)

"$SHOOT" --crop 870x160+310+237  "${EX[@]}" "Configuration/Protection/Rev Limiter"               revlimit-page
"$SHOOT" --crop 870x190+310+237  "${EX[@]}" "Configuration/Vehicle Functions/Pit Speed Limiter"  pitlimit-page
"$SHOOT" --crop 1330x620+310+237 "${EX[@]}" "Configuration/Vehicle Functions/Cruise Control"     cruise-page
"$SHOOT" --crop 1330x510+310+237 "${EX[@]}" "Configuration/Vehicle Functions/Cruise Control/Limits" cruise-limits
