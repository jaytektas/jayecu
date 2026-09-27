#!/usr/bin/env bash
# The screenshots of chapter 30 (Knock), reproducibly. Re-run after a UI change:
#   manual/tools/shots/knock.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700
P="Configuration/Ignition Tuning/Knock Control"
EX=(--set knock.enabled=1)

"$SHOOT" --crop 1330x640+310+237 "${EX[@]}" "$P"              knock-page
"$SHOOT" --crop 1330x440+310+237 "${EX[@]}" "$P/Threshold"    knock-threshold
"$SHOOT" --crop 1110x320+310+237 "${EX[@]}" "$P/Noise Floor"  knock-noise-floor
