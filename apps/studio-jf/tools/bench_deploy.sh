#!/usr/bin/env bash
# Put a packaged studio on the bench machine and prove it starts there: `make bench-studio`.
#
#   apps/studio-jf/tools/bench_deploy.sh <jayecu-studio-X.Y.Z-x86_64.AppImage> <ssh host>
#
# The studio is developed and built on the dev box; the bench only runs it, from the same AppImage a
# user would get — so every deploy also tests the package. It lands under ONE fixed name, because that
# file is what the launcher starts and what the studio's own updater replaces ($APPIMAGE).
#
# Steps, each checked:
#   1. copy it over as a temporary file and rename it into place — a studio already running keeps the
#      file it started from, and a half-copied AppImage is never the one the launcher starts
#   2. compare checksums both ends
#   3. start it on the bench's own desktop with a scripted run (--shot, --quit-after), which skips the
#      update check, and require a clean exit and a screenshot. The shot comes back here to look at.

set -euo pipefail

SRC="${1:?usage: bench_deploy.sh <AppImage> <ssh host>}"
HOST="${2:?usage: bench_deploy.sh <AppImage> <ssh host>}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DEST_DIR='$HOME/Applications'
DEST="$DEST_DIR/jayecu-studio-x86_64.AppImage"
SHOT_LOCAL="$(dirname "$SRC")/bench-smoke.ppm"

echo "  bench: copying $(basename "$SRC") to $HOST"
ssh "$HOST" "mkdir -p $DEST_DIR"
scp -q "$SRC" "$HOST:Applications/.jayecu-studio.part"
ssh "$HOST" "chmod 755 ~/Applications/.jayecu-studio.part && mv -f ~/Applications/.jayecu-studio.part $DEST"

want="$(sha256sum "$SRC" | cut -d' ' -f1)"
have="$(ssh "$HOST" "sha256sum $DEST" | cut -d' ' -f1)"
[ "$want" = "$have" ] || { echo "  bench: checksum mismatch ($have != $want)" >&2; exit 1; }
echo "  bench: checksum ok"

# NO LAUNCHER STEP HERE. The studio offers to add its own menu entry and icon the first time it is run
# from an AppImage by hand (src/app/DesktopIntegration.cpp) — the scripted smoke run below never asks.

# The bench's desktop session is Wayland; the studio talks X11 (xcb), so it goes through that
# session's Xwayland — found by its auth file, which mutter names afresh at every login.
ssh "$HOST" bash -s <<EOF
set -e
export DISPLAY=:0
export XAUTHORITY=\$(ls -t /run/user/\$(id -u)/.mutter-Xwaylandauth.* 2>/dev/null | head -1)
[ -n "\$XAUTHORITY" ] || { echo "  bench: no desktop session to start the studio in" >&2; exit 1; }
rm -f /tmp/bench-smoke*
timeout 60 $DEST --shot /tmp/bench-smoke --quit-after 6000 >/tmp/bench-smoke.log 2>&1 \
    || { echo "  bench: studio exited abnormally — /tmp/bench-smoke.log on $HOST:" >&2; tail -20 /tmp/bench-smoke.log >&2; exit 1; }
# The capture is written as <path>_<surface>.ppm.
[ -s /tmp/bench-smoke_0.ppm ] || { echo "  bench: studio ran but took no screenshot" >&2; exit 1; }
EOF
scp -q "$HOST:/tmp/bench-smoke_0.ppm" "$SHOT_LOCAL"
echo "  bench: studio started, drew its window and closed cleanly — screenshot: $SHOT_LOCAL"
