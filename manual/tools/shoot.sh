#!/usr/bin/env bash
# Photograph a studio page for the manual — made by the studio itself, so it can be re-shot whenever
# the UI changes (STANDARD.md §6).
#
#   manual/tools/shoot.sh [--crop GEOM] [--act SCRIPT] [--tool NAME] [--set path=value]... "<navigation path>" <name>
#
#   "<navigation path>"  the page, as the navigation tree names it: "Configuration/Fuel Tuning/VE Table"
#   <name>               output file, written to manual/docs/img/studio/<name>.png
#   --crop GEOM          keep only this ImageMagick geometry, e.g. 663x460+317+237 (the window is 1280x800)
#   --act SCRIPT         after the page is up, run SCRIPT (xdotool clicks and keys, with sleeps) and
#                        photograph the screen afterwards — for what no navigation path reaches: a
#                        menu, a tool tab, a dialog. SCRIPT runs on the virtual display; DISPLAY is set.
#   --tool NAME          open a Tools menu tab once the page is up ("Auto Tune", "Trigger Log", "Knock Scope",
#                        "Engine Cycle", "Trigger Designer") — the tab is what gets photographed.
#   --set path=value     set a value in the demo tune first, in the units the studio shows
#                        (boost.enabled=1, boost.activation_kpa=120; an option by its index). Repeatable.
#                        A switched-off module is a page of greyed controls and documents nothing.
#
# It opens a DEMO project offline (the studio's --open-meta: a new tune from shared/tuneit-meta.json),
# selects the page (--node) and captures the window (--shot). No ECU, no display, and nothing real is
# touched: HOME points at a throwaway folder for the run, so the studio's data folder and settings are
# that folder's, and it is deleted afterwards.
#
# Needs: a built studio (make studio), xvfb-run, ImageMagick.

set -euo pipefail

USAGE='usage: shoot.sh [--crop GEOM] [--act SCRIPT] [--tool NAME] [--set path=value]... "<navigation path>" <name>'
CROP=""
ACT=""
SETS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --crop) CROP="${2:?$USAGE}"; shift 2 ;;
        --act)  ACT="$(cd "$(dirname "${2:?$USAGE}")" && pwd)/$(basename "$2")"; shift 2 ;;
        --set)  SETS+=(--set "${2:?$USAGE}"); shift 2 ;;
        --tool) SETS+=(--tool "${2:?$USAGE}"); shift 2 ;;
        *)      break ;;
    esac
done
NODE="${1:?$USAGE}"
NAME="${2:?$USAGE}"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STUDIO="${STUDIO_BIN:-$ROOT/apps/studio-jf/build/studio}"
META="${SHOOT_META:-$ROOT/shared/tuneit-meta.json}"
BOARD="${SHOOT_BOARD:-jaytek_v1}"       # whose authored layout the demo shows
OUT_DIR="$ROOT/manual/docs/img/studio"
# The window. 1600 wide suits most pages; a page drawn wider than the space beside the navigation
# (the trigger stream pages) needs more, or its right-hand column is cut off by the window edge.
WIN_W="${SHOOT_W:-1600}"; WIN_H="${SHOOT_H:-1000}"
WAIT_MS="${SHOOT_WAIT_MS:-9000}"     # long enough for the demo to open and the page to settle

fail() { echo "shoot: $*" >&2; exit 1; }
[ -x "$STUDIO" ] || fail "no studio at $STUDIO — run make studio"
[ -f "$META" ]   || fail "no definition at $META — run make codegen"
command -v xvfb-run >/dev/null || fail "no xvfb-run (apt install xvfb)"
command -v convert  >/dev/null || fail "no ImageMagick (apt install imagemagick)"

TMP_HOME="$(mktemp -d)"
trap 'rm -rf "$TMP_HOME"' EXIT
mkdir -p "$OUT_DIR"

# THE PAGES A USER SEES. A new project takes its layout from the shipped dashboard library (what
# `make codegen` installs); without it the demo shows the definition's bare navigation tree instead.
DATA="$TMP_HOME/.local/share/jayecu/jayecu Studio"
mkdir -p "$DATA/dashboards" "$TMP_HOME/.config/jayecu-studio"
cp "$ROOT/definition/boards/$BOARD.dashboard.gui" "$DATA/dashboards/$BOARD.gui"
# Quit without asking: a fresh demo is an unsaved project, and the save prompt would hold the run open.
echo "{ \"saveOnExit\": 2, \"window.w\": $WIN_W, \"window.h\": $WIN_H, \"window.x\": 0, \"window.y\": 0, \"dock.dtc\": 0, \"dock.properties\": 0 }" > "$TMP_HOME/.config/jayecu-studio/settings.json"

if [ -z "$ACT" ]; then
    HOME="$TMP_HOME" timeout 90 xvfb-run -a -s "-screen 0 ${WIN_W}x${WIN_H}x24" \
        "$STUDIO" --open-meta "$META" "${SETS[@]}" --node "$NODE" --shot "$TMP_HOME/shot" --quit-after "$WAIT_MS" \
        > "$TMP_HOME/studio.log" 2>&1 || true
else
    # The studio's own --shot fires once the page settles, which is before any clicking; so for an
    # action run the studio just runs, the script clicks, and the screen is photographed after it.
    cat > "$TMP_HOME/act-run.sh" <<RUN
"$STUDIO" --open-meta "$META" ${SETS[@]+"${SETS[@]}"} --node "$NODE" --quit-after 60000 > "$TMP_HOME/studio.log" 2>&1 &
SP=\$!
sleep 7
bash "$ACT"
import -window root "$TMP_HOME/shot_0.ppm"
kill \$SP 2>/dev/null; wait \$SP 2>/dev/null
RUN
    HOME="$TMP_HOME" timeout 120 xvfb-run -a -s "-screen 0 ${WIN_W}x${WIN_H}x24" bash "$TMP_HOME/act-run.sh" || true
fi

SHOT="$TMP_HOME/shot_0.ppm"      # the capture is written as <path>_<surface>.ppm
[ -s "$SHOT" ] || { tail -20 "$TMP_HOME/studio.log" >&2; fail "the studio took no screenshot"; }

if [ -n "$CROP" ]; then convert "$SHOT" -crop "$CROP" +repage "$OUT_DIR/$NAME.png"
else                    convert "$SHOT" "$OUT_DIR/$NAME.png"; fi
echo "shot: manual/docs/img/studio/$NAME.png"
