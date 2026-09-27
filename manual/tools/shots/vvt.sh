#!/usr/bin/env bash
# The screenshots of chapter 25 (Cam and valve control), reproducibly. Re-run after a UI change:
#   manual/tools/shots/vvt.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700               # the Cam Control pages are drawn wider than a 1600 window leaves room for
P="Configuration/Engine Functions"

# The chapter's example: intake and exhaust phasers on one bank, long-term trim on; and a VTEC-style
# lift changeover.
EX=(--set vvt_control.enabled=1 --set vvt_control.mode=2 --set vvt_control.num_banks=1
    --set vvt_control.enable_ltt=1 --set vvl.enabled=1)

"$SHOOT" --crop 1300x540+310+237 "${EX[@]}" "$P/Cam Control"                                 vvt-page
"$SHOOT" --crop 1060x140+310+237 "${EX[@]}" "$P/Cam Control/Intake Target (cam advance)"     vvt-intake-target
"$SHOOT" --crop 1060x140+310+237 "${EX[@]}" "$P/Cam Control/Exhaust Target (cam advance)"    vvt-exhaust-target
"$SHOOT" --crop 1270x230+310+237 "${EX[@]}" "$P/Cam Control/Cam Target Scalar"               vvt-scalar
"$SHOOT" --crop 870x165+310+237 "${EX[@]}" "$P/Variable Valve Lift"                         vvl-page
