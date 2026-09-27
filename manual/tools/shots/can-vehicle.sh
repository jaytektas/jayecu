#!/usr/bin/env bash
# The screenshots of chapters 13 (CAN bus) and 14 (Vehicle integration), reproducibly. Re-run after a
# UI change:
#   manual/tools/shots/can-vehicle.sh
#
# Array indices used below (from the definition the studio loads, shared/tuneit-meta.json):
#   sensors.sensor[1]   Wideband O2 1         sensors.sensor[51]  Drive Shaft Speed
#   sensors.sensor[128] Air Conditioner Request
#   outputs.output[20..23] = LS9..LS12 (rows run IGN1-12, LS1-22, HS1-8)
#   sensor interface index: 3 Frequency, 4 Digital, 5 CAN Bus; digital source index: DIG1 = 0
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"

# ---- Chapter 13: a Haltech WB1 wideband on CAN1 at 1 Mbit, read by Wideband O2 1 ------------------
# The frame is what Load Template ▸ "Haltech WB1 (single channel)" writes: id 0x2B1 (689), one 16-bit
# big-endian field at start bit 7, multiplier 1024, Valid For 300 ms, No Reading code 32767.
WB=(--set 'can.bus[0].bitrate=3'
    --set 'can.gc_frame[0].flags=1' --set 'can.gc_frame[0].bus=0' --set 'can.gc_frame[0].id=689'
    --set 'can.gc_frame[0].dlc=8' --set 'can.gc_frame[0].first_field=0' --set 'can.gc_frame[0].field_count=1'
    --set 'can.gc_field[0].bit_off=7' --set 'can.gc_field[0].width=16' --set 'can.gc_field[0].flags=4'
    --set 'can.gc_field[0].scale=1024' --set 'can.gc_field[0].ttl_ms=300' --set 'can.gc_field[0].sentinel=32767')

"$SHOOT" --crop 1040x452+310+236 "${WB[@]}" "Configuration/CAN Bus/CAN1"          can-vehicle-can1
"$SHOOT" --crop 1182x250+310+236 "${WB[@]}" "Configuration/CAN Bus/CAN1/Receive"  can-vehicle-receive
"$SHOOT" --crop 1000x594+310+236 "${WB[@]}" --set 'sensors.sensor[1].enabled=1' --set 'sensors.sensor[1].interface=5' \
         --set 'sensors.sensor[1].can_frame=689' --set 'sensors.sensor[1].can_bit=7' \
         "Configuration/Sensors/O2 & Lambda/Wideband O2 1"                          can-vehicle-wideband

# ---- Chapter 14: the example car's outputs and inputs ----------------------------------------------
# LS9 fuel pump relay, LS10 fan relay, LS11 A/C clutch relay (all Generic, Digital); LS12 tachometer
# (Generic, PWM, fixed 50 % duty, frequency from an expression, 2 pulses per rev, squelch 60 rpm).
OUTS=(--set 'outputs.output[20].function=3' --set 'outputs.output[20].kind=1' --set 'outputs.output[20].value_source=2'
      --set 'outputs.output[21].function=3' --set 'outputs.output[21].kind=1' --set 'outputs.output[21].value_source=2'
      --set 'outputs.output[22].function=3' --set 'outputs.output[22].kind=1' --set 'outputs.output[22].value_source=2'
      --set 'outputs.output[23].function=3' --set 'outputs.output[23].kind=0' --set 'outputs.output[23].value_source=2'
      --set 'outputs.output[23].fixed_x10=50' --set 'outputs.output[23].freq_source=2'
      --set 'outputs.output[23].param_a=2' --set 'outputs.output[23].param_b=60')

"$SHOOT" --crop 1070x640+310+236 "${OUTS[@]}" "Configuration/Electrical/Outputs"            can-vehicle-outputs
"$SHOOT" --crop 900x656+310+236  "${OUTS[@]}" "Configuration/Electrical/Outputs/LS12"       can-vehicle-tacho
"$SHOOT" --crop 1034x490+310+236 "${OUTS[@]}" "Configuration/Electrical/Outputs/LS12/Frequency" can-vehicle-tacho-freq

# Road speed from a drive-shaft pickup on DIG3: 4 pulses per shaft turn, 3.9:1 diff, 1.95 m tyre
# circumference -> 1000 / 1.95 x 3.9 x 4 = 8000 pulses/km.
VSS=(--set 'sensors.sensor[51].enabled=1' --set 'sensors.sensor[51].interface=3' --set 'sensors.sensor[51].source=2'
     --set 'vehicle_speed.enabled=1' --set 'vehicle_speed.main_source=0' --set 'vehicle_speed.shaft_ppr=4'
     --set 'vehicle_speed.source[0].pulses_per_km=8000')
"$SHOOT" --crop 1140x536+310+236 "${VSS[@]}" "Configuration/Vehicle Functions/Vehicle Speed" can-vehicle-vss

# A/C request switch on DIG4, read as a digital level.
"$SHOOT" --crop 1000x594+310+236 --set 'sensors.sensor[128].enabled=1' --set 'sensors.sensor[128].interface=4' \
         --set 'sensors.sensor[128].source=3' \
         "Configuration/Sensors/Air Conditioning/Air Conditioner Request"            can-vehicle-ac-request

# OBD-II answering on CAN1 (the bus that reaches the harness on jaytek_v1).
"$SHOOT" --crop 1024x296+310+236 --set 'can.obd_enabled=1' --set 'can.obd_bus=0' "Configuration/CAN Bus/OBD-II" can-vehicle-obd

# ---- Annotated figures: numbered callouts over two of the captures above (STANDARD.md §6) ----------
# The PNG is embedded in the SVG, so the figure is one self-contained file an <img> can show.
python3 - "$(dirname "$0")/../../docs/img/studio" <<'PY'
import base64, struct, sys, os
d = sys.argv[1]
def size(png):
    with open(png, 'rb') as f: f.read(16); return struct.unpack('>II', f.read(8))
def annotate(name, marks):
    png = os.path.join(d, name + '.png')
    w, h = size(png)
    b64 = base64.b64encode(open(png, 'rb').read()).decode()
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 {w} {h}" '
           f'font-family="system-ui, -apple-system, \'Segoe UI\', Ubuntu, sans-serif">',
           f'  <title>{name} with numbered callouts</title>',
           f'  <image width="{w}" height="{h}" xlink:href="data:image/png;base64,{b64}"/>']
    for n, (x, y) in enumerate(marks, 1):
        out.append(f'  <circle cx="{x}" cy="{y}" r="13" fill="#ff8c28" stroke="#1b1d22" stroke-width="2"/>')
        out.append(f'  <text x="{x}" y="{y + 5}" text-anchor="middle" font-size="15" font-weight="700" fill="#1b1d22">{n}</text>')
    out.append('</svg>')
    open(os.path.join(d, name + '-annotated.svg'), 'w').write('\n'.join(out) + '\n')
    print('annotated: manual/docs/img/studio/' + name + '-annotated.svg')
# CAN1: 1 Enabled, 2 Bitrate, 3 Listen Only, 4 Bus Load / Sent / Received, 5 State and Last Error
annotate('can-vehicle-can1',  [(16, 122), (136, 158), (16, 193), (520, 125), (520, 300)])
# LS12: 1 Function, 2 Set up…, 3 Kind, 4 Value From, 5 Fixed Value, 6 Turn on / off when, 7 Template Numbers
annotate('can-vehicle-tacho', [(196, 124), (16, 193), (216, 435), (676, 105), (736, 136), (560, 520), (505, 643)])
PY
