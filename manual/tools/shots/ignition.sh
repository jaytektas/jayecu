#!/usr/bin/env bash
# The screenshots of chapter 20 (Ignition), reproducibly. Re-run after a UI change:
#   manual/tools/shots/ignition.sh
# It also rebuilds the annotated timing-light figure (docs/img/diagrams/ignition-timing-light.svg), which
# embeds two of these captures with numbered callouts over them.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SHOOT="$HERE/../shoot.sh"
IMG="$HERE/../../docs/img/studio"
DIA="$HERE/../../docs/img/diagrams"
P="Configuration/Ignition Tuning"

# Example 1 of the chapter: a naturally aspirated four-cylinder on wasted spark, cranking advance on.
# (Ignition has no Enabled switch of its own: it always runs. The example values are what the pages show.)
EXAMPLE=(--set engine.ign_mode=1 --set engine.cylinder_count=4
         --set ignition.max_adv_deg=40 --set ignition.min_adv_deg=-10 --set ignition.overall_adv_trim=0
         --set ignition.cranking_ign_enable=1)

"$SHOOT" --crop 1284x496+310+237 "${EXAMPLE[@]}" "Configuration/Engine Configuration/Ignition System" ignition-system
"$SHOOT" --crop 1284x500+310+237 "${EXAMPLE[@]}" "$P"                        ignition-tuning
"$SHOOT" --crop 1155x528+310+237 "${EXAMPLE[@]}" "$P/Advance Table"          ignition-advance-table
"$SHOOT" --crop 620x120+310+237  "${EXAMPLE[@]}" "$P/Dwell Time"             ignition-dwell
"$SHOOT" --crop 1037x260+310+237 "${EXAMPLE[@]}" "$P/Cranking Advance"       ignition-cranking
"$SHOOT" --crop 977x185+310+237  "${EXAMPLE[@]}" "$P/Advance Limits"         ignition-limits
"$SHOOT" --crop 1030x375+310+312 "${EXAMPLE[@]}" "$P/Corrections"            ignition-corrections
"$SHOOT" --crop 1284x330+310+237 "${EXAMPLE[@]}" "$P/Corrections/Coolant"    ignition-corr-coolant
"$SHOOT" --crop 483x233+310+237  "${EXAMPLE[@]}" "$P/Cylinder Trims/Cylinder 1" ignition-cyl-trim
"$SHOOT" --crop 1284x655+310+237 "${EXAMPLE[@]}" "$P/Timing Breakdown"       ignition-breakdown
"$SHOOT" --crop 900x430+310+237  --set engine.cycle_type=2 "$P/Trailing Split" ignition-trailing-split

# The two panels of the timing-light procedure: Fixed Timing switched on at 10°, and the Trigger Offset
# it is used to set (90° here is only an example value).
"$SHOOT" --crop 720x132+310+292 "${EXAMPLE[@]}" --set ignition.fixed_timing_enable=1 --set ignition.fixed_timing_deg=10 \
         "$P/Advance Limits" ignition-fixed-src
"$SHOOT" --crop 468x210+310+306 --set trigger.trigger_offset_btdc=90 \
         "Configuration/Engine Configuration/Trigger System" ignition-offset-src

# The annotated figure: both captures embedded (an SVG shown through <img> cannot load a second file),
# numbered callouts keyed to the steps of "Setting base timing" in the chapter.
python3 - "$IMG/ignition-fixed-src.png" "$IMG/ignition-offset-src.png" "$DIA/ignition-timing-light.svg" <<'PY'
import base64, struct, sys
fixed, offset, out = sys.argv[1:4]
def load(p):
    b = open(p, 'rb').read()
    w, h = struct.unpack('>II', b[16:24])          # PNG IHDR
    return base64.b64encode(b).decode(), w, h
fb, fw, fh = load(fixed)
ob, ow, oh = load(offset)
gap, top = 40, 44
ox = fw + gap
W, H = ox + ow, top + max(fh, oh) + 96
# callouts: (x, y) in each capture's own pixels, and the text in the circle
calls = [(0, 393, 49, '1'), (0, 575, 95, '2'), (1, 220, 53, '4')]
svg = [f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 {W} {H}" '
       "font-family=\"system-ui, -apple-system, 'Segoe UI', Ubuntu, sans-serif\" font-size=\"15\">",
       '<title>Setting base timing with Fixed Timing and Trigger Offset BTDC</title>',
       '<style>.fg{fill:#1d1f23}.muted{fill:#5f6670}.frame{fill:none;stroke:#5f6670;stroke-width:2}'
       '.call{fill:#d9822b;stroke:#ffffff;stroke-width:2}.num{fill:#ffffff;font-weight:700}.head{font-weight:600}'
       '@media (prefers-color-scheme: dark){.fg{fill:#e4e6ea}.muted{fill:#9aa1ab}.frame{stroke:#9aa1ab}}</style>',
       f'<text x="0" y="18" class="fg head">Ignition Tuning ▸ Advance Limits</text>',
       f'<text x="0" y="36" class="muted">steps 1, 2 and 6</text>',
       f'<text x="{ox}" y="18" class="fg head">Engine Configuration ▸ Trigger System</text>',
       f'<text x="{ox}" y="36" class="muted">steps 4 and 5</text>',
       f'<image x="0" y="{top}" width="{fw}" height="{fh}" xlink:href="data:image/png;base64,{fb}"/>',
       f'<rect x="0" y="{top}" width="{fw}" height="{fh}" class="frame"/>',
       f'<image x="{ox}" y="{top}" width="{ow}" height="{oh}" xlink:href="data:image/png;base64,{ob}"/>',
       f'<rect x="{ox}" y="{top}" width="{ow}" height="{oh}" class="frame"/>']
for img, x, y, t in calls:
    cx = (0 if img == 0 else ox) + x
    cy = top + y
    svg.append(f'<circle cx="{cx - 30}" cy="{cy}" r="14" class="call"/>')
    svg.append(f'<text x="{cx - 30}" y="{cy + 5}" text-anchor="middle" class="num">{t}</text>')
by = top + max(fh, oh) + 30
legend = [(0, by, '1 · tick Fixed timing enabled'), (330, by, '2 · set Fixed Advance (10° here)'),
          (0, by + 24, '4 · engine running, light on cylinder 1: move Trigger Offset BTDC until the light reads it'),
          (0, by + 48, '6 · untick Fixed timing enabled and burn')]
for x, y, t in legend:
    svg.append(f'<text x="{x}" y="{y}" class="fg">{t}</text>')
svg.append('</svg>')
open(out, 'w').write('\n'.join(svg) + '\n')
print('annotated:', out)
PY
rm -f "$IMG/ignition-fixed-src.png" "$IMG/ignition-offset-src.png"
