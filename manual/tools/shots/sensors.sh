#!/usr/bin/env bash
# The screenshots and plots of chapter 17 (Sensors and calibration), reproducibly. Re-run after a UI
# change or a change to the wideband preset files:
#   manual/tools/shots/sensors.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SHOOT="$HERE/../shoot.sh"
ROOT="$(cd "$HERE/../../.." && pwd)"
P="Configuration/Sensors"

# The car the chapter walks through: MAP on AV1 (engine-synchronous), coolant on AT1, air temperature on
# AT2, throttle on AV2, oil pressure on AV3, clutch switch on DIG3 read as a digital level.
# Analog sources are pool indices (AV1 = 0 ... AV16 = 15, AT1 = 16 ... AT4 = 19); digital ones DIG1 = 0.
# Interface is the list position: 0 Analogue Voltage, 1 Engine Sync, 2 On-board, 3 Frequency,
# 4 Digital, 5 CAN Bus, 6 SENT, 7 Pulse Width.
CAR=(--set "sensors.sensor[map].enabled=1"          --set "sensors.sensor[map].source=0"
     --set "sensors.sensor[clt].enabled=1"          --set "sensors.sensor[clt].source=16"
     --set "sensors.sensor[iat].enabled=1"          --set "sensors.sensor[iat].source=17"
     --set "sensors.sensor[tps].enabled=1"          --set "sensors.sensor[tps].source=1"
     --set "sensors.sensor[oil_pressure].enabled=1" --set "sensors.sensor[oil_pressure].source=2"
     --set "sensors.sensor[clutch_sw].enabled=1"    --set "sensors.sensor[clutch_sw].interface=4"
     --set "sensors.sensor[clutch_sw].source=2")

# Coolant's checks as Example 2 sets them: both electrical checks at Level 1, reading-high at Level 2.
CLT_DIAG=(--set "sensors.sensor[clt].diag_enable.raw_min=1"   --set "sensors.sensor[clt].diag_severity.raw_min=1"
          --set "sensors.sensor[clt].diag_enable.raw_max=1"   --set "sensors.sensor[clt].diag_severity.raw_max=1"
          --set "sensors.sensor[clt].diag_enable.op_max=1"    --set "sensors.sensor[clt].diag_severity.op_max=2"
          --set "sensors.sensor[clt].diag_op_max=115")

"$SHOOT" --crop 1270x436+312+238 "${CAR[@]}" "$P"                                        sensors-switchboard
"$SHOOT" --crop 1050x195+312+240 "${CAR[@]}" "$P/Engine Sync"                            sensors-group-sync
"$SHOOT" --crop 996x655+312+238  "${CAR[@]}" "$P/Engine/Coolant Temperature"             sensors-clt-page
"$SHOOT" --crop 1030x380+312+238 "${CAR[@]}" "${CLT_DIAG[@]}" "$P/Engine/Coolant Temperature/Diagnostics" sensors-clt-diag
"$SHOOT" --crop 996x655+312+238  "${CAR[@]}" "$P/Engine Sync/Manifold Pressure"          sensors-map-page
"$SHOOT" --crop 330x385+312+238  "${CAR[@]}" "$P/Transmission/Clutch Pedal Switch"       sensors-clutch-input
# A switch read through an ANALOG pin: the wiring row grows its two trip points (Switch On / Switch Off).
"$SHOOT" --crop 330x305+312+238  --set "sensors.sensor[start_sw].enabled=1" --set "sensors.sensor[start_sw].source=4" \
         "$P/Chassis/Start Button"                                                         sensors-start-input
"$SHOOT" --crop 996x655+312+238  --set "sensors.sensor[flex_fuel].enabled=1" --set "sensors.sensor[flex_fuel].source=4" \
         "$P/Engine/Flex Fuel Composition & Temperature"                                  sensors-flex-page
"$SHOOT" --crop 1000x245+312+238 --set "sensors.sensor[cruise_sw].enabled=1" --set "sensors.sensor[cruise_sw].source=3" \
         "$P/Chassis/Cruise Control Switch"                                               sensors-cruise-bands
"$SHOOT" --crop 400x165+784+632  "${CAR[@]}" "Configuration/Engine Configuration/Trigger System" sensors-sync-window

# ---------------------------------------------------------------------------------------------------
# ANNOTATED: the coolant page with numbered callouts keyed to the steps of "Setting up one sensor".
# The capture is embedded (base64) so the SVG stands alone when shown through an <img>.
# ---------------------------------------------------------------------------------------------------
python3 - "$ROOT" <<'PY'
import base64, os, sys
root = sys.argv[1]
png = os.path.join(root, "manual/docs/img/studio/sensors-clt-page.png")
b64 = base64.b64encode(open(png, "rb").read()).decode()
W, H = 996, 655
# (number, x, y) — centred on the control each step uses, in the capture's own pixels.
marks = [(1, 128, 38), (2, 305, 191), (3, 16, 236), (4, 300, 263), (5, 250, 113), (6, 480, 122), (7, 700, 420)]
out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
       f'font-family="system-ui, -apple-system, \'Segoe UI\', Ubuntu, sans-serif" font-size="16">',
       '  <title>Setting up one sensor: the steps on the Coolant Temperature page</title>',
       '  <style>.c { fill: #ff8c28; stroke: #1d1f23; stroke-width: 2; } .n { fill: #1d1f23; font-weight: 700; }</style>',
       f'  <image x="0" y="0" width="{W}" height="{H}" href="data:image/png;base64,{b64}"/>']
for n, x, y in marks:
    out.append(f'  <circle cx="{x}" cy="{y}" r="14" class="c"/>')
    out.append(f'  <text x="{x}" y="{y + 6}" text-anchor="middle" class="n">{n}</text>')
out.append('</svg>\n')
open(os.path.join(root, "manual/docs/img/studio/sensors-clt-annotated.svg"), "w").write("\n".join(out))
print("annotated: manual/docs/img/studio/sensors-clt-annotated.svg")
PY

# ---------------------------------------------------------------------------------------------------
# PLOTS (docs/img/plots). Written as SVG in the house style, from data in the repository:
#   sensors-wideband-presets.svg — the four wideband preset files in apps/studio-jf/calibrations/
#   sensors-map-clamp.svg        — the two-point MAP curve of Example 1, with the default raw checks
#                                  (250 mV / 4750 mV, ecu.schema.yaml diag_raw_min/diag_raw_max)
# ---------------------------------------------------------------------------------------------------
python3 - "$ROOT" <<'PY'
import csv, glob, os, sys
root = sys.argv[1]
out = os.path.join(root, "manual/docs/img/plots"); os.makedirs(out, exist_ok=True)

STYLE = """  <style>
    .fg { fill: #1d1f23; } .muted { fill: #5f6670; }
    .axis { stroke: #5f6670; stroke-width: 2; fill: none; }
    .grid { stroke: #d6d9de; stroke-width: 1; }
    .l0 { stroke: #3b82c4; stroke-width: 3; fill: none; }
    .l1 { stroke: #d9822b; stroke-width: 3; fill: none; }
    .l2 { stroke: #2e9d4f; stroke-width: 3; fill: none; }
    .l3 { stroke: #8a5cc2; stroke-width: 3; fill: none; stroke-dasharray: 9 5; }
    .flat { stroke: #3b82c4; stroke-width: 3; fill: none; stroke-dasharray: 3 5; }
    .bad { fill: #fbeaea; } .trip { stroke: #c8412d; stroke-width: 2; stroke-dasharray: 6 4; fill: none; }
    .dot { fill: #3b82c4; }
    .mono { font-family: ui-monospace, 'Ubuntu Mono', Consolas, monospace; font-size: 12px; }
    .head { font-weight: 600; }
    @media (prefers-color-scheme: dark) {
      .fg { fill: #e4e6ea; } .muted { fill: #9aa1ab; } .axis { stroke: #9aa1ab; } .grid { stroke: #2c3038; }
      .bad { fill: #361512; }
    }
  </style>"""
HEAD = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" '
        'font-family="system-ui, -apple-system, \'Segoe UI\', Ubuntu, sans-serif" font-size="14">\n'
        '  <title>{t}</title>\n')
def head(w, h, t): return HEAD.format(w=w, h=h, t=t) + STYLE + '\n'

# ---- 1. wideband presets: volts (0..5) -> lambda ----------------------------------------------------
X0, X1, Y0, Y1 = 90, 690, 400, 60          # plot box
def px(v):  return X0 + (v / 5.0) * (X1 - X0)
def py(l):  return Y0 - (l - 0.4) / (1.6 - 0.4) * (Y0 - Y1)
lines = []
for f in sorted(glob.glob(os.path.join(root, "apps/studio-jf/calibrations/*.csv"))):
    rows = [r for r in csv.reader(open(f, encoding="utf-8")) if r and not r[0].startswith("#")]
    raw = [float(x) for x in rows[0][1:]]; val = [float(x) for x in rows[1][1:]]
    volts = [r * 5.0 / 4095.0 for r in raw]            # counts -> connector volts (4095 = 5.000 V)
    lines.append((volts, val))
# Label by the MAPPING, not by a product: what matters is the two numbers.
lines.sort(key=lambda t: (t[1][0], t[1][-1]))
s = [head(1000, 470, "The same voltage, four different lambdas")]
for v in (0.6, 0.8, 1.0, 1.2, 1.4):
    s.append(f'  <line x1="{X0}" y1="{py(v):.1f}" x2="{X1}" y2="{py(v):.1f}" class="grid"/>')
    s.append(f'  <text x="{X0-10}" y="{py(v)+5:.1f}" text-anchor="end" class="muted">{v:.1f}</text>')
for v in range(6):
    s.append(f'  <line x1="{px(v):.1f}" y1="{Y0}" x2="{px(v):.1f}" y2="{Y1}" class="grid"/>')
    s.append(f'  <text x="{px(v):.1f}" y="{Y0+22}" text-anchor="middle" class="muted">{v} V</text>')
s.append(f'  <path d="M{X0} {Y1} V{Y0} H{X1}" class="axis"/>')
s.append(f'  <text x="{X0-60}" y="{Y1-20}" class="muted">lambda</text>')
s.append(f'  <text x="{(X0+X1)/2:.0f}" y="{Y0+50}" text-anchor="middle" class="muted">controller analogue output, at the ECU pin</text>')
# 2.5 V marker
s.append(f'  <line x1="{px(2.5):.1f}" y1="{Y0}" x2="{px(2.5):.1f}" y2="{Y1}" class="trip"/>')
ly = 90
for k, (vs, ls) in enumerate(lines):
    cls = f"l{k % 4}"
    s.append(f'  <path d="M{px(vs[0]):.1f} {py(ls[0]):.1f} L{px(vs[-1]):.1f} {py(ls[-1]):.1f}" class="{cls}"/>')
    at = ls[0] + (ls[-1] - ls[0]) * 0.5
    s.append(f'  <circle cx="{px(2.5):.1f}" cy="{py(at):.1f}" r="4" class="dot"/>')
    s.append(f'  <path d="M720 {ly} h40" class="{cls}"/>')
    s.append(f'  <text x="770" y="{ly+5}" class="fg">0 V = {ls[0]:.2f} · 5 V = {ls[-1]:.2f}</text>')
    s.append(f'  <text x="770" y="{ly+24}" class="muted">at 2.5 V reads λ {at:.2f}</text>')
    ly += 62
s.append(f'  <text x="720" y="{ly+14}" class="muted">Red dashed line: 2.5 V at the pin.</text>')
s.append('</svg>\n')
open(os.path.join(out, "sensors-wideband-presets.svg"), "w", encoding="utf-8").write("\n".join(s))

# ---- 2. the MAP curve of Example 1, and what the raw checks guard ----------------------------------
PTS = [(0.5, 10.0), (4.5, 300.0)]                    # data-sheet points, volts -> kPa
RAW_LO, RAW_HI = 0.25, 4.75                          # schema defaults: 250 mV, 4750 mV
X0, X1, Y0, Y1 = 90, 690, 400, 60
def px(v):  return X0 + (v / 5.0) * (X1 - X0)
def py(k):  return Y0 - (k / 320.0) * (Y0 - Y1)
s = [head(1000, 470, "A two-point MAP calibration and its raw checks")]
s.append(f'  <rect x="{X0}" y="{Y1}" width="{px(RAW_LO)-X0:.1f}" height="{Y0-Y1}" class="bad"/>')
s.append(f'  <rect x="{px(RAW_HI):.1f}" y="{Y1}" width="{X1-px(RAW_HI):.1f}" height="{Y0-Y1}" class="bad"/>')
for k in (50, 100, 150, 200, 250, 300):
    s.append(f'  <line x1="{X0}" y1="{py(k):.1f}" x2="{X1}" y2="{py(k):.1f}" class="grid"/>')
    s.append(f'  <text x="{X0-10}" y="{py(k)+5:.1f}" text-anchor="end" class="muted">{k}</text>')
for v in range(6):
    s.append(f'  <text x="{px(v):.1f}" y="{Y0+22}" text-anchor="middle" class="muted">{v} V</text>')
s.append(f'  <path d="M{X0} {Y1} V{Y0} H{X1}" class="axis"/>')
s.append(f'  <text x="{X0-60}" y="{Y1-20}" class="muted">kPa</text>')
s.append(f'  <text x="{(X0+X1)/2:.0f}" y="{Y0+50}" text-anchor="middle" class="muted">sensor output at the ECU pin</text>')
(a, ka), (b, kb) = PTS
s.append(f'  <path d="M{X0} {py(ka):.1f} H{px(a):.1f}" class="flat"/>')
s.append(f'  <path d="M{px(b):.1f} {py(kb):.1f} H{X1}" class="flat"/>')
s.append(f'  <path d="M{px(a):.1f} {py(ka):.1f} L{px(b):.1f} {py(kb):.1f}" class="l0"/>')
for v, k in PTS:
    s.append(f'  <circle cx="{px(v):.1f}" cy="{py(k):.1f}" r="6" class="dot"/>')
s.append(f'  <line x1="{px(RAW_LO):.1f}" y1="{Y0}" x2="{px(RAW_LO):.1f}" y2="{Y1}" class="trip"/>')
s.append(f'  <line x1="{px(RAW_HI):.1f}" y1="{Y0}" x2="{px(RAW_HI):.1f}" y2="{Y1}" class="trip"/>')
s.append(f'  <text x="{px(a)+12:.1f}" y="{py(ka)+4:.1f}" class="fg">0.5 V → 10 kPa</text>')
s.append(f'  <text x="{px(b)-12:.1f}" y="{py(kb)-12:.1f}" text-anchor="end" class="fg">4.5 V → 300 kPa</text>')
ly = 90
for cls, t1, t2 in (("l0",   "The calibration: two points,", "straight line between them"),
                    ("flat", "Outside the points the reading", "holds the end value"),
                    ("trip", "Raw Low 0.25 V / Raw High 4.75 V", "(the defaults), shaded beyond")):
    s.append(f'  <path d="M720 {ly} h40" class="{cls}"/>')
    s.append(f'  <text x="770" y="{ly+5}" class="fg">{t1}</text>')
    s.append(f'  <text x="770" y="{ly+24}" class="muted">{t2}</text>')
    ly += 62
s.append(f'  <text x="720" y="{ly+10}" class="muted">A short to ground reads 0 V: the curve</text>')
s.append(f'  <text x="720" y="{ly+30}" class="muted">alone would call that 10 kPa. Raw Low</text>')
s.append(f'  <text x="720" y="{ly+50}" class="muted">calls it a fault: the channel goes invalid.</text>')
s.append('</svg>\n')
open(os.path.join(out, "sensors-map-clamp.svg"), "w", encoding="utf-8").write("\n".join(s))
print("plots: manual/docs/img/plots/sensors-wideband-presets.svg, sensors-map-clamp.svg")
PY
