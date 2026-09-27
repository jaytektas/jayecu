#!/usr/bin/env bash
# The screenshots of chapter 22 (Electronic throttle), reproducibly. Re-run after a UI change:
#   manual/tools/shots/etb.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700               # the throttle pages are drawn wider than a 1600 window leaves room for
ETB="Configuration/Engine Functions/Electronic throttle"

# The chapter's example: one throttle body on Half Bridge A, a two-track pedal, and both pairs of
# sensors switched on (sensors 16/17 are Throttle Position 1/2, 18/19 Accelerator Pedal 1/2).
EX=(--set app.enabled=1 --set "electronic_throttle.etb[0].enabled=1" --set "h_bridge.half[0].enabled=1"
    --set "sensors.sensor[16].enabled=1" --set "sensors.sensor[17].enabled=1"
    --set "sensors.sensor[18].enabled=1" --set "sensors.sensor[19].enabled=1")

"$SHOOT" --crop 1340x675+310+237 "${EX[@]}" "Configuration/Engine Functions/Accelerator Pedal"      etb-pedal
"$SHOOT" --crop 1000x455+310+237 "${EX[@]}" "Configuration/Engine Functions/Accelerator Pedal/Pedal to Throttle" etb-pedal-map
"$SHOOT" --crop 1340x675+310+237 "${EX[@]}" "Configuration/Electrical/Half Bridges/Half Bridge A"   etb-half-bridge
"$SHOOT" --crop 1340x675+310+237 "${EX[@]}" "$ETB/Throttle body A"                                   etb-body
"$SHOOT" --crop 1340x475+310+237 "${EX[@]}" "$ETB/Throttle body A/Calibration Settings"              etb-cal-settings
"$SHOOT" --crop 1340x245+310+237 "${EX[@]}" "$ETB"                                                   etb-branch
