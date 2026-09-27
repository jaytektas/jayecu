#!/usr/bin/env bash
# The screenshots of chapters 4 (A tour of the studio) and 5 (First connection), reproducibly.
# Re-run after a UI change:
#   manual/tools/shots/studio-tour.sh
#
# Most captures go through shoot.sh. Three states shoot.sh cannot reach, because it always opens a demo
# tune and only takes --set: the landing page (no project open), Editing mode (--mode-flips) and a
# connect attempt with no ECU present (--connect). Those use run_studio below, which sets up the same
# throwaway HOME, window size and shipped layout as shoot.sh and then passes its arguments straight to
# the studio. --connect is only safe on a machine with no ECU plugged in: it is run only when no
# /dev/ttyACM* or /dev/ttyUSB* device exists.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SHOOT="$HERE/../shoot.sh"
ROOT="$(cd "$HERE/../../.." && pwd)"
STUDIO="${STUDIO_BIN:-$ROOT/apps/studio-jf/build/studio}"
META="${SHOOT_META:-$ROOT/shared/tuneit-meta.json}"
OUT="$ROOT/manual/docs/img/studio"

# run_studio <name> [--crop GEOM] -- <studio args...>
run_studio() {
    local name="$1"; shift
    local crop=""
    if [ "${1:-}" = "--crop" ]; then crop="$2"; shift 2; fi
    [ "${1:-}" = "--" ] && shift
    local tmp; tmp="$(mktemp -d)"
    local data="$tmp/.local/share/jayecu/jayecu Studio"
    mkdir -p "$data/dashboards" "$tmp/.config/jayecu-studio"
    cp "$ROOT/definition/boards/jaytek_v1.dashboard.gui" "$data/dashboards/jaytek_v1.gui"
    echo '{ "saveOnExit": 2, "window.w": 1600, "window.h": 1000, "window.x": 0, "window.y": 0, "dock.dtc": 0, "dock.properties": 0 }' \
        > "$tmp/.config/jayecu-studio/settings.json"
    HOME="$tmp" timeout 90 xvfb-run -a -s "-screen 0 1600x1000x24" \
        "$STUDIO" "$@" --shot "$tmp/shot" --quit-after 9000 > "$tmp/studio.log" 2>&1 || true
    if [ ! -s "$tmp/shot_0.ppm" ]; then tail -20 "$tmp/studio.log" >&2; rm -rf "$tmp"; echo "no capture for $name" >&2; return 1; fi
    if [ -n "$crop" ]; then convert "$tmp/shot_0.ppm" -crop "$crop" +repage "$OUT/$name.png"
    else                    convert "$tmp/shot_0.ppm" "$OUT/$name.png"; fi
    rm -rf "$tmp"
    echo "shot: manual/docs/img/studio/$name.png"
}

PAGE="Configuration/Engine Configuration/Cylinders & Firing"

# ---- Chapter 4 ------------------------------------------------------------------------------------
# The whole window, offline, on a page every engine has. The annotated figure (4.1) is built from it.
"$SHOOT" "$PAGE" studio-tour-window
# The landing: what the studio shows with no project open.
run_studio studio-tour-landing --
# The toolbar and the notice strip above it.
"$SHOOT" --crop 560x76+0+56 "$PAGE" studio-tour-toolbar
# The navigation tree.
"$SHOOT" --crop 300x790+0+130 "$PAGE" studio-tour-navtree
# A module switched on: its node joins the tree, and its page opens.
"$SHOOT" --crop 300x600+0+130 --set boost.enabled=1 "Configuration/Engine Functions/Boost Control" studio-tour-tree-module
# Editing mode: one flip of the Locked/Editing toggle, the way a click makes it.
run_studio studio-tour-editing -- --open-meta "$META" --node "$PAGE" --mode-flips 1

# ---- Chapter 5 ------------------------------------------------------------------------------------
# A connect attempt with nothing on the end of the wire: the chip goes red and the status bar says why.
if ls /dev/ttyACM* /dev/ttyUSB* >/dev/null 2>&1; then
    echo "skipping studio-tour-connect-failed: a serial device is present, and --connect would open it" >&2
else
    run_studio studio-tour-connect-failed --crop 560x76+0+56 -- --open-meta "$META" --node "$PAGE" --connect
fi

# ---- The annotated window (Figure 4.1) ---------------------------------------------------------------
# The capture, embedded in an SVG with numbered markers keyed to the text. Regenerated from the PNG
# above, so it follows the UI whenever this script is re-run.
python3 - "$OUT/studio-tour-window.png" "$OUT/studio-tour-anatomy.svg" <<'PY'
import base64, sys
png, out = sys.argv[1], sys.argv[2]
data = base64.b64encode(open(png, "rb").read()).decode()
# (number, marker centre x, y, outline x, y, w, h) in the 1600x1000 capture's own pixels.
marks = [
    (1,  372,  45,    2,  36,  326,  20),   # menu bar
    (2,  500,  68,    2,  57, 1596,  22),   # notice strip
    (3,  560, 105,    2,  90,  538,  30),   # toolbar
    (4,  235, 560,    2, 134,  266,  840),  # navigation dock
    (5,  284, 420,  270, 134,   28,  780),  # dock side tabs
    (6,  720, 147,  303, 134,  380,  26),   # surface tabs
    (7, 1095, 180,  303, 168, 1284,  64),   # readout strip on the Main surface
    (8,  500, 247,  308, 238, 1286,  668),  # the page window
    (9,  800, 921,  303, 910, 1296,  56),   # bottom docks
    (10,  80, 988,    2, 978, 1596,  20),   # status bar
]
o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1600 1000" font-family="system-ui, -apple-system, \'Segoe UI\', Ubuntu, sans-serif">',
     '  <title>The studio window with its parts numbered</title>',
     '  <style>.ring{fill:none;stroke:#d9822b;stroke-width:2;stroke-dasharray:6 4}.dot{fill:#d9822b;stroke:#ffffff;stroke-width:2}.num{fill:#ffffff;font-size:15px;font-weight:700}</style>',
     f'  <image href="data:image/png;base64,{data}" x="0" y="0" width="1600" height="1000"/>']
for n, cx, cy, rx, ry, rw, rh in marks:
    o.append(f'  <rect class="ring" x="{rx}" y="{ry}" width="{rw}" height="{rh}" rx="4"/>')
for n, cx, cy, *_ in marks:
    o.append(f'  <circle class="dot" cx="{cx}" cy="{cy}" r="13"/>')
    o.append(f'  <text class="num" x="{cx}" y="{cy + 5}" text-anchor="middle">{n}</text>')
o.append('</svg>')
open(out, "w").write("\n".join(o) + "\n")
print("annotated: " + out)
PY
