#!/usr/bin/env bash
# The screenshots of chapter 32 (Vehicle functions), reproducibly. Re-run after a UI change:
#   manual/tools/shots/vehicle.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700
TQ="Configuration/Engine Functions/Torque Model"

"$SHOOT" --crop 1330x535+310+237 --set vehicle_speed.enabled=1 "Configuration/Vehicle Functions/Vehicle Speed" vss-page
"$SHOOT" --crop 875x620+310+237  --set gear_detect.enabled=1   "Configuration/Vehicle Functions/Gear Detection" gear-page
"$SHOOT" --crop 1330x200+310+237 --set alternator.enabled=1    "Configuration/Electrical/Alternator Control"   alternator-page
"$SHOOT" --crop 1330x245+310+237 --set torque_model.enabled=1  "$TQ"                                          torque-page
"$SHOOT" --crop 1330x380+310+237 --set torque_model.enabled=1  "$TQ/Engine Torque (rpm x load)"              torque-table
