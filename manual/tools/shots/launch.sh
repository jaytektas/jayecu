#!/usr/bin/env bash
# The screenshots of chapter 26 (Launch, shift and traction), reproducibly. Re-run after a UI change:
#   manual/tools/shots/launch.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700               # these pages are drawn wider than a 1600 window leaves room for
P="Configuration/Vehicle Functions"

EX=(--set launch.enabled=1 --set flat_shift.enabled=1 --set traction_control.enabled=1
    --set vehicle_speed.enabled=1)

"$SHOOT" --crop 1300x200+310+237 "${EX[@]}" "$P/Launch Control"                          launch-page
"$SHOOT" --crop 1300x400+310+237 "${EX[@]}" "$P/Launch Control/Launch Ignition Advance"  launch-ign
"$SHOOT" --crop 860x180+310+237  "${EX[@]}" "$P/Flat Shift"                              flatshift-page
"$SHOOT" --crop 1300x230+310+237 "${EX[@]}" "$P/Traction Control"                        traction-page
"$SHOOT" --crop 1060x215+310+237 "${EX[@]}" "$P/Traction Control/Target Slip"            traction-target
