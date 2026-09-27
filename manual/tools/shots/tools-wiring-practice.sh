#!/usr/bin/env bash
# The screenshots of chapters 8 (Tools and materials) and 9 (Wiring practice), reproducibly. Re-run after a
# UI change:
#   manual/tools/shots/tools-wiring-practice.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"

# Chapter 9, labelling: a sensor's wiring row names the terminal and the suggested wire colour.
# The coolant sensor switched on and assigned to AT1 (option 16 of the analog source list), which the
# board file puts on CN3 pin 14, colour Gy/B.
"$SHOOT" --crop 320x215+322+320 --set "sensors.sensor[clt].enabled=1" --set "sensors.sensor[clt].source=16" \
    "Configuration/Sensors/Engine/Coolant Temperature" tools-wiring-practice-wiring-row
