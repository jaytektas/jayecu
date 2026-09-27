#!/usr/bin/env bash
# The screenshot of chapter 31 (Misfire detection), reproducibly. Re-run after a UI change:
#   manual/tools/shots/misfire.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700
"$SHOOT" --crop 1330x660+310+237 --set misfire.enabled=1 "Configuration/Protection/Misfire Detection" misfire-page
