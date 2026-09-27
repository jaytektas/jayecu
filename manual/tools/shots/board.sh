#!/usr/bin/env bash
# The screenshots of chapter 7 (The jaytek_v1 board), reproducibly. Re-run after a UI change:
#   manual/tools/shots/board.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"

# ---- The three connector diagrams (Figures 7.2-7.4), GENERATED from the board file, so a pin or wire
#      colour changed there is redrawn here rather than copied by hand. Needs python3 + PyYAML.
python3 - "$ROOT" "$ROOT/manual/docs/img/diagrams" <<'PY'
import sys, yaml, html
ROOT = sys.argv[1]
OUT = sys.argv[2]
b = yaml.safe_load(open(f"{ROOT}/definition/boards/jaytek_v1.board.yaml"))

WIRE = {"w": "#ebebeb", "b": "#282828", "r": "#c83c37", "g": "#2da04b", "bu": "#3c6ec8", "br": "#87552d",
        "o": "#e1912d", "y": "#e1c837", "v": "#9655be", "gy": "#969696", "pk": "#e67daf", "lg": "#87cd7d",
        "lb": "#7dc3e6"}
POWER_NAME = {"CON_12V_RAW": "12V RAW", "CON_12V_MR": "12V MR", "CON_12V_PROT": "12V PROT",
              "CON_5V_SENSOR1": "5V SENS 1", "CON_5V_SENSOR2": "5V SENS 2"}

def kind(t):
    r = t.get("role")
    if r == "ground": return "gnd"
    if r in ("power", "sensor_supply"): return "pwr"
    s = t["signal"]
    if s.startswith(("IGN", "LS", "HS", "HBRIDGE")): return "out"
    if s.startswith("CAN"): return "com"
    return "in"

def label(t):
    r = t.get("role")
    if r == "ground": return "GND"
    if r: return POWER_NAME.get(t["net"], t["net"])
    s = t["signal"]
    if "line" in t:
        ln = t["line"].replace("-", "−")
        return f"{s} {ln}" if s.startswith("CAN") else f"{s}{ln}"
    return s

STYLE = """  <style>
    .fg   { fill: #1d1f23; }  .muted { fill: #5f6670; }
    .box  { fill: #ffffff; stroke: #5f6670; stroke-width: 2; }
    .shell{ fill: #f4f5f7; stroke: #5f6670; stroke-width: 2; }
    .in   { fill: #e8f1fb; stroke: #3b82c4; stroke-width: 2; }
    .out  { fill: #fdf0e3; stroke: #d9822b; stroke-width: 2; }
    .com  { fill: #eef7ee; stroke: #2e9d4f; stroke-width: 2; }
    .pwr  { fill: #fbeaea; stroke: #c8412d; stroke-width: 2; }
    .gnd  { fill: #e9eaed; stroke: #5f6670; stroke-width: 2; }
    .wire { stroke: #5f6670; stroke-width: 1; }
    .mono { font-family: ui-monospace, 'Ubuntu Mono', Consolas, monospace; font-size: 12px; }
    .head { font-weight: 600; }
    @media (prefers-color-scheme: dark) {
      .fg { fill: #e4e6ea; } .muted { fill: #9aa1ab; }
      .box { fill: #20232a; stroke: #9aa1ab; } .shell { fill: #181a1f; stroke: #9aa1ab; }
      .in { fill: #16283b; } .out { fill: #33240f; } .com { fill: #13291a; } .pwr { fill: #361512; }
      .gnd { fill: #2a2d33; stroke: #9aa1ab; } .wire { stroke: #9aa1ab; }
    }
  </style>"""

CW, CH, GAP = 92, 78, 8

def swatch(x, y, code):
    parts = code.split("/")
    base = WIRE.get(parts[0].lower(), "#969696")
    s = f'<rect x="{x}" y="{y}" width="36" height="10" rx="3" fill="{base}" class="wire"/>'
    if len(parts) > 1:
        st = WIRE.get(parts[1].lower(), "#969696")
        s += f'<rect x="{x+13}" y="{y}" width="10" height="10" fill="{st}" class="wire"/>'
    return s

for c in b["connectors"]:
    terms = sorted(c["terminals"], key=lambda t: t["pin"])
    n = len(terms)
    rows = [(1, 12), (13, 23), (24, 35)] if n == 35 else [(1, 8), (9, 15), (16, 23)]
    ncol = rows[0][1] - rows[0][0] + 1
    W = 40 + ncol * (CW + GAP) + 20
    H = 108 + 3 * (CH + GAP) + 28 + 62
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" font-family="system-ui, -apple-system, \'Segoe UI\', Ubuntu, sans-serif" font-size="14">',
         f'  <title>Connector {c["id"]} pin map</title>', STYLE]
    o.append(f'  <text x="30" y="34" class="fg head" font-size="18">{c["id"]} · {c["color"]} shell · {n}-way · {html.escape(c["desc"])}</text>')
    o.append(f'  <text x="30" y="58" class="muted">Header TE {c["part"]} \u00b7 harness housing TE {c["mate"]}</text>')
    o.append(f'  <text x="30" y="78" class="muted">Drawn in pin-number order. Each cell: pin, signal, wire colour.</text>')
    o.append(f'  <rect x="20" y="94" width="{W-40}" height="{3*(CH+GAP)+28}" rx="14" class="shell"/>')
    tmap = {t["pin"]: t for t in terms}
    for ri, (a, z) in enumerate(rows):
        cnt = z - a + 1
        x0 = 40 + (ncol - cnt) * (CW + GAP) / 2
        y = 108 + ri * (CH + GAP)
        for i, p in enumerate(range(a, z + 1)):
            t = tmap[p]
            x = x0 + i * (CW + GAP)
            k = kind(t)
            o.append(f'  <rect x="{x:.0f}" y="{y}" width="{CW}" height="{CH}" rx="6" class="{k}"/>')
            o.append(f'  <text x="{x+CW/2:.0f}" y="{y+20}" text-anchor="middle" class="fg head">{p}</text>')
            o.append(f'  <text x="{x+CW/2:.0f}" y="{y+40}" text-anchor="middle" class="fg mono">{html.escape(label(t))}</text>')
            col = t.get("color", "")
            o.append("  " + swatch(x + 10, y + 53, col))
            o.append(f'  <text x="{x+52:.0f}" y="{y+63}" class="muted mono">{col}</text>')
    ly = 108 + 3 * (CH + GAP) + 44
    lx = 30
    for k, name in [("in", "input"), ("out", "output"), ("com", "CAN bus"), ("pwr", "power / 5 V supply"), ("gnd", "ground")]:
        o.append(f'  <rect x="{lx}" y="{ly-13}" width="18" height="18" rx="4" class="{k}"/>')
        o.append(f'  <text x="{lx+26}" y="{ly+1}" class="fg">{name}</text>')
        lx += 26 + len(name) * 8 + 34
    o.append("</svg>")
    open(f"{OUT}/board-{c['id'].lower()}.svg", "w").write("\n".join(o) + "\n")
    print("wrote", f"{OUT}/board-{c['id'].lower()}.svg")
PY

# ---- Studio screenshots.

# The example the chapter uses: a four-cylinder with a coil per cylinder on IGN1-4, an injector per
# cylinder on LS1-4, a generic output on LS13 (one of the four low-sides with a freewheel diode) and a
# generic output on HS1. Output rows run IGN1-12 (0-11), LS1-22 (12-33), HS1-8 (34-41).
FOUR_CYL=(--set engine.cylinder_count=4
          --set "outputs.output[0].function=1"  --set "outputs.output[0].cylinder=1"
          --set "outputs.output[1].function=1"  --set "outputs.output[1].cylinder=2"
          --set "outputs.output[2].function=1"  --set "outputs.output[2].cylinder=3"
          --set "outputs.output[3].function=1"  --set "outputs.output[3].cylinder=4"
          --set "outputs.output[12].function=2" --set "outputs.output[12].cylinder=1"
          --set "outputs.output[13].function=2" --set "outputs.output[13].cylinder=2"
          --set "outputs.output[14].function=2" --set "outputs.output[14].cylinder=3"
          --set "outputs.output[15].function=2" --set "outputs.output[15].cylinder=4"
          --set "outputs.output[24].function=3"
          --set "outputs.output[34].function=3")

# The output pin table: every IGN, LS and HS pin the board has, by name.
"$SHOOT" --crop 1060x630+310+240 "${FOUR_CYL[@]}" "Configuration/Electrical/Outputs" board-outputs

# One output's own page: the "This Pin" row names the connector, shell colour, terminal and wire colour.
"$SHOOT" --crop 430x150+318+292 "${FOUR_CYL[@]}" "Configuration/Electrical/Outputs/LS13" board-output-pin

# A sensor's wiring row: coolant temperature on AT1, which the board brings out at CN3 pin 14.
"$SHOOT" --crop 330x260+318+290 --set "sensors.sensor[clt].enabled=1" --set "sensors.sensor[clt].source=16" \
         "Configuration/Sensors/Engine/Coolant Temperature" board-sensor-pin

# The CAN1 controller page: bus on, bitrate, listen-only.
"$SHOOT" --crop 1040x420+310+240 "Configuration/CAN Bus/CAN1" board-can1

# The two H-bridges (the studio's Half Bridges page), bridge A switched on.
"$SHOOT" --crop 1040x430+310+240 --set "h_bridge.half[0].enabled=1" "Configuration/Electrical/Half Bridges" board-half-bridges

# ---- The pinout tables (chapter 7, "Pinout"), GENERATED from the board file like the diagrams above,
#      into snippet files the chapter includes. Never edit the snippets by hand.
python3 - "$ROOT" <<'PY'
import sys, yaml
root = sys.argv[1]
b = yaml.safe_load(open(f"{root}/definition/boards/jaytek_v1.board.yaml"))
caps = {p["signal"]: p.get("caps", []) for p in b["pins"]}
CAP = {"ANALOG_VOLTAGE": "analogue 0–5 V", "ANALOG_TEMP": "temperature (2.7 kΩ pull-up)",
       "TRIGGER_INPUT": "trigger", "DIGITAL_INPUT": "switch", "FREQUENCY_INPUT": "frequency",
       "SENT_INPUT": "SENT", "KNOCK_INPUT": "knock", "IGNITION_OUT": "ignition",
       "LOWSIDE_OUT": "low-side", "HIGHSIDE_OUT": "high-side", "PWM_OUT": "PWM", "TACH_OUTPUT": "tacho"}
POWER = {"CON_12V_RAW": "12 V RAW — the ECU's own supply", "CON_12V_MR": "12 V MR — main-relay feed to the power stage",
         "CON_12V_PROT": "12 V PROT — protected 12 V out", "CON_5V_SENSOR1": "5 V SENS 1 — 5 V supply for sensors (output)",
         "CON_5V_SENSOR2": "5 V SENS 2 — 5 V supply for sensors (output)"}
for c in b["connectors"]:
    rows = ["| Pin | Signal | Can be used as | Wire |", "|---|---|---|---|"]
    for t in sorted(c["terminals"], key=lambda t: t["pin"]):
        role = t.get("role")
        if role == "ground":
            sig, use = "GND", "ground"
        elif role:
            sig, use = POWER.get(t["net"], t["net"]).split(" — ")[0], POWER.get(t["net"], t["net"])
            use = use.split(" — ")[1] if " — " in use else use
        else:
            s = t["signal"]
            sig = f'{s} {t["line"]}' if "line" in t else s
            if s.startswith("HBRIDGE"): use = "H-bridge output"
            elif s.startswith("CAN"): use = "CAN bus"
            elif s.startswith("VR"): use = "VR sensor input: trigger, frequency"
            else: use = ", ".join(CAP.get(k, k) for k in caps.get(s, [])) or "—"
        rows.append(f'| {t["pin"]} | {sig} | {use} | {t.get("color", "")} |')
    path = f"{root}/manual/docs/part2/_pins-{c['id'].lower()}.md"
    open(path, "w", encoding="utf-8").write("<!-- GENERATED by manual/tools/shots/board.sh from definition/boards/jaytek_v1.board.yaml. Do not edit. -->\n\n" + "\n".join(rows) + "\n")
    print("pins:", path)
PY
