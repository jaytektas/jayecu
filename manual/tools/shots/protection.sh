#!/usr/bin/env bash
# The screenshots of chapter 29 (Engine protection), reproducibly. Re-run after a UI change:
#   manual/tools/shots/protection.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700
P="Configuration/Protection"
EX=(--set lambda_protect.enabled=1 --set egt_protect.enabled=1 --set dfco.enabled=1
    --set "engine_protection.protection_levels[2].enabled=1")

"$SHOOT" --crop 1330x540+310+237 "${EX[@]}" "$P/Engine Protection"                     protection-page
"$SHOOT" --crop 1330x440+310+237 "${EX[@]}" "$P/Engine Protection/Protection Levels"   protection-levels
"$SHOOT" --crop 1330x510+310+237 "${EX[@]}" "$P/Engine Protection/Threshold Monitors"  protection-monitors
"$SHOOT" --crop 870x220+310+237  "${EX[@]}" "$P/Lambda Protection"                     lambdaprotect-page
"$SHOOT" --crop 870x160+310+237  "${EX[@]}" "$P/EGT Protection"                        egtprotect-page
"$SHOOT" --crop 870x190+310+237  "${EX[@]}" "Configuration/Engine Functions/Deceleration Fuel Cut" dfco-page
