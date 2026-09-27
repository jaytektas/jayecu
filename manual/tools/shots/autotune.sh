#!/usr/bin/env bash
# The screenshot of chapter 40 (Auto tune), reproducibly. Re-run after a UI change:
#   manual/tools/shots/autotune.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700
"$SHOOT" --crop 1400x745+300+163 --tool "Auto Tune" "Configuration/Fuel Tuning/VE Table" autotune-panel
