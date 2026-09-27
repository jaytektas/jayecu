#!/usr/bin/env bash
# The screenshots of chapter 21 (Idle), reproducibly. Re-run after a UI change:
#   manual/tools/shots/idle.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700               # the idle pages are drawn wider than a 1600 window leaves room for
P="Configuration/Engine Functions/Idle Control"

# The chapter's example: closed-loop idle on a PWM idle valve, with the throttle follower, long-term
# trim and one idle-up slot (the A/C request) switched on.
EX=(--set idle.enabled=1 --set idle.mode=1 --set idle.throttle_follower_enabled=1 --set idle.ltt_enabled=1
    --set "idle.idle_up[0].enabled=1" --set "idle.idle_up[0].rpm_offset=100" --set "idle.idle_up[0].base_offset_x10=5")

"$SHOOT" --crop 965x660+298+232  "${EX[@]}" "$P"                         idle-page
"$SHOOT" --crop 770x285+298+232  "${EX[@]}" "$P/Target RPM"              idle-target
"$SHOOT" --crop 1010x285+298+232 "${EX[@]}" "$P/Base Duty"               idle-base-duty
"$SHOOT" --crop 1030x275+298+232 "${EX[@]}" "$P/Ignition Correction"     idle-ign-corr
"$SHOOT" --crop 990x365+298+232  "${EX[@]}" "$P/Throttle Follower"       idle-follower
"$SHOOT" --crop 880x212+298+232  "${EX[@]}" "$P/Long-Term Trim"          idle-ltt
"$SHOOT" --crop 1340x490+298+232 "${EX[@]}" "$P/Idle Up"                 idle-up
# Example 2: a four-wire stepper idle valve on the two H-bridges.
"$SHOOT" --crop 1350x660+298+232 "${EX[@]}" --set stepper.enabled=1 \
         "Configuration/Engine Functions/Idle Stepper"                        idle-stepper
