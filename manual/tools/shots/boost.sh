#!/usr/bin/env bash
# The screenshots of chapter 24 (Boost), reproducibly. Re-run after a UI change:
#   manual/tools/shots/boost.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
PAGE="Configuration/Engine Functions/Boost Control"

# The example the chapter walks through: closed loop, a modest single-turbo setup.
EXAMPLE=(--set boost.enabled=1 --set boost.mode=1 --set boost.activation_rpm=2500
         --set boost.activation_kpa=110 --set boost.control_point_kpa=30 --set boost.kp=0.8
         --set boost.ki=2 --set boost.overboost_limit_kpa=230 --set boost.overboost_offset_kpa=25)

"$SHOOT" --crop 880x582+308+232 "${EXAMPLE[@]}" "$PAGE"                     boost-page
"$SHOOT" --crop 520x356+314+236 "${EXAMPLE[@]}" "$PAGE/Boost Target"         boost-target-table
"$SHOOT" --crop 1100x534+314+236 "${EXAMPLE[@]}" "$PAGE/Wastegate Base Duty"  boost-base-duty
"$SHOOT" --crop 1100x544+314+236 "${EXAMPLE[@]}" --set boost.ltt_en=1 "$PAGE/Long Term Trim" boost-ltt
"$SHOOT" --crop 1100x256+310+236 "${EXAMPLE[@]}" --set boost.corr1_en=1 "$PAGE/Correction 1" boost-correction
