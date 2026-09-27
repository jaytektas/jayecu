#!/usr/bin/env bash
# The screenshot of chapter 35 (Lua scripting), reproducibly. Re-run after a UI change:
#   manual/tools/shots/lua.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700
"$SHOOT" --crop 1330x560+310+237 "Configuration/Lua Scripting" lua-page
