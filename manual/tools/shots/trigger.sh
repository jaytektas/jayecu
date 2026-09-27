#!/usr/bin/env bash
# The screenshots of chapter 16 (The trigger system), reproducibly. Re-run after a UI change:
#   manual/tools/shots/trigger.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
# The stream pages are drawn wider than the space a 1600 window leaves beside the navigation.
export SHOOT_W=1700
TRIG="Configuration/Engine Configuration/Trigger System"

# The example the chapter walks through (Example 1): a 60-2 crank wheel on VR1 and a one-pulse cam on
# DIG1, the reference tooth 90° before TDC #1 — the library's "60-2 + cam", written out field by field
# because the demo tune starts with every stream off.
S0=trigger.streams[0]; S2=trigger.streams[2]
EXAMPLE=(--set trigger.trigger_offset_btdc=90
         --set $S0.enabled=1 --set $S0.capture_index=0 --set $S0.edge=0 --set $S0.primitive=0
         --set $S0.slots=60 --set $S0.gap_ratio=3 --set $S0.cell_len=1 --set "$S0.cell[0].v=0"
         --set $S2.enabled=1 --set $S2.capture_index=2 --set $S2.edge=2 --set $S2.primitive=2
         --set $S2.width_min=0 --set $S2.width_max=720 --set $S2.width_target=0 --set $S2.nominal_angle=0)

"$SHOOT" --crop 1350x640+298+232 "${EXAMPLE[@]}" "$TRIG"                              trigger-system
"$SHOOT" --crop 950x278+298+288 "${EXAMPLE[@]}" "$TRIG/Trigger Streams"              trigger-streams
"$SHOOT" --crop 1350x640+298+232 "${EXAMPLE[@]}" "$TRIG/Trigger Streams/Crank Primary" trigger-crank-primary
SHOOT_H=1200 "$SHOOT" --crop 1350x760+298+232 "${EXAMPLE[@]}" "$TRIG/Trigger Streams/Cam Intake B1" trigger-cam
"$SHOOT" --crop 1350x440+298+232 "${EXAMPLE[@]}" "$TRIG/Diagnostics"                  trigger-diagnostics

# The Trigger Designer and the library beside it, with the 2JZ wheel loaded. No navigation path reaches
# a tool tab, so these are clicked to (trigger-designer.act).
ACT="$(dirname "$0")/trigger-designer.act"
SHOOT_H=1100 "$SHOOT" --act "$ACT" --crop 1400x880+300+132 "$TRIG" trigger-designer
SHOOT_H=1100 "$SHOOT" --act "$ACT" --crop 268x830+0+132    "$TRIG" trigger-library
