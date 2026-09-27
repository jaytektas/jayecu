#!/usr/bin/env bash
# The screenshots of chapters 1 and 2 (How to use this manual, The system at a glance), reproducibly.
# Re-run after a UI change:
#   manual/tools/shots/overview.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SHOOT="$HERE/../shoot.sh"
IMG="$HERE/../../docs/img/studio"
PAGE="Configuration/Engine Configuration/Cylinders & Firing"

# A four-cylinder, 2.0 litre, 86 mm bore: the page the naming example in chapter 1 points at.
EXAMPLE=(--set engine.cylinder_count=4 --set engine.displacement=2000 --set engine.bore_mm=86)

# Chapter 1: one setting as the studio shows it (label, value, units).
"$SHOOT" --crop 420x250+318+285 "${EXAMPLE[@]}" "$PAGE" overview-setting

# Chapter 2: the whole window, then the toolbar on its own.
"$SHOOT" "${EXAMPLE[@]}" "$PAGE" overview-window
"$SHOOT" --crop 560x64+0+56 "${EXAMPLE[@]}" "$PAGE" overview-toolbar

# Chapter 2, Figure 2.2: the window with numbered callouts. An SVG with the capture embedded (an <img>
# SVG cannot load a second file), scaled to 1200 px wide; the markers are keyed to the list in the text.
python3 - "$IMG" <<'EOF'
import base64, subprocess, sys, pathlib
img = pathlib.Path(sys.argv[1])
small = img / "overview-window-small.png"
subprocess.run(["convert", str(img / "overview-window.png"), "-resize", "1200x750", str(small)], check=True)
b64 = base64.b64encode(small.read_bytes()).decode()
small.unlink()
# (number, x, y) in the 1200x750 frame: centre of each marker.
marks = [
    (1, 275, 34),    # menu bar
    (2, 585, 50),    # connection banner
    (3, 46, 66),     # Locked / Editing
    (4, 98, 66),     # Connect
    (5, 180, 66),    # Burn
    (6, 426, 80),    # Verify <-> ECU
    (7, 170, 250),   # navigation tree
    (8, 560, 110),   # surface tabs and gauge strip
    (9, 700, 420),   # the page
    (10, 620, 691),  # bottom docks
]
dots = "\n".join(
    f'  <circle cx="{x}" cy="{y}" r="11" class="m"/><text x="{x}" y="{y+5}" text-anchor="middle" class="t">{n}</text>'
    for n, x, y in marks)
svg = f'''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 1200 750" font-family="system-ui, -apple-system, 'Segoe UI', Ubuntu, sans-serif" font-size="13">
  <title>The jayecu Studio window with numbered areas</title>
  <style>
    .m {{ fill: #ff8c28; stroke: #1d1f23; stroke-width: 2; }}
    .t {{ fill: #1d1f23; font-weight: 700; }}
  </style>
  <image x="0" y="0" width="1200" height="750" href="data:image/png;base64,{b64}"/>
{dots}
</svg>
'''
(img / "overview-window.svg").write_text(svg)
print("annotated: manual/docs/img/studio/overview-window.svg")
EOF
