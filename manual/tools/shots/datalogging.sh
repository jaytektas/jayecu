#!/usr/bin/env bash
# The screenshot of chapter 42 (Datalogging and analysis), reproducibly. Re-run after a UI change:
#   manual/tools/shots/datalogging.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700
"$SHOOT" --crop 1330x165+310+237 --set datalog.enabled=1 --set datalog.rate_hz=50 "Configuration/Datalogging" datalog-page
