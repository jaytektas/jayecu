#!/usr/bin/env bash
# The screenshots of chapter 6 (Quick start — box to first start), reproducibly. Re-run after a UI change:
#   manual/tools/shots/quick-start.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
CFG="Configuration/Engine Configuration"

# EXAMPLE 1, the one the chapter walks through: a 2.0 L four-cylinder, firing 1-3-4-2, a 36-1 crank
# wheel on VR1 and no cam sensor, wasted-spark coils and semi-sequential injection.
EX1=(--set engine.cylinder_count=4 --set engine.displacement=1998 --set engine.bore_mm=86
     --set "engine.firing_order[0].cyl=1" --set "engine.firing_order[1].cyl=3"
     --set "engine.firing_order[2].cyl=4" --set "engine.firing_order[3].cyl=2"
     --set engine.ign_mode=1 --set "engine.inj_stage[0].mode=1"
     --set "trigger.streams[0].enabled=1" --set "trigger.streams[0].capture_index=0"
     --set "trigger.streams[0].primitive=0" --set "trigger.streams[0].slots=36"
     --set "trigger.streams[0].gap_ratio=2" --set "trigger.streams[0].cell_len=1")

# EXAMPLE 2: a 2JZ with the library's "2JZ 36-2 +Rear Cam" wheel — the offset (155) is the library's,
# the pickup angle (+105) the one the schema's own help quotes for this engine.
EX2=(--set engine.cylinder_count=6 --set trigger.trigger_offset_btdc=155 --set trigger.sensor_angle=105
     --set "trigger.streams[0].enabled=1" --set "trigger.streams[0].primitive=0"
     --set "trigger.streams[0].slots=36" --set "trigger.streams[0].gap_ratio=3" --set "trigger.streams[0].cell_len=1"
     --set "trigger.streams[2].enabled=1" --set "trigger.streams[2].primitive=2"
     --set "trigger.streams[2].width_max=720" --set "trigger.streams[2].width_target=365")

"$SHOOT" --crop 960x400+312+290   "${EX1[@]}" "$CFG/Cylinders & Firing"                         quick-start-cylinders
"$SHOOT" --crop 758x545+312+305   "${EX1[@]}" "$CFG/Trigger System/Trigger Streams/Crank Primary" quick-start-crank-stream
"$SHOOT" --crop 1278x480+312+240  "${EX2[@]}" "$CFG/Trigger System"                            quick-start-trigger-reference
"$SHOOT" --crop 1000x650+312+240  --set "sensors.sensor[126].enabled=1" "Configuration/Sensors/Engine/Coolant Temperature" quick-start-sensor
"$SHOOT" --crop 1058x370+312+240  "${EX1[@]}" "$CFG/Fuel System"                               quick-start-fuel-system
"$SHOOT" --crop 1138x500+312+240  "${EX1[@]}" "Configuration/Fuel Tuning/Stage 1/Setup"         quick-start-injector-data
"$SHOOT" --crop 1278x500+312+240  "${EX1[@]}" --set ignition.fixed_timing_enable=1 --set ignition.fixed_timing_deg=10 \
                                  "$CFG/Ignition System"                                       quick-start-ignition-system
"$SHOOT" --crop 435x490+312+292  "${EX1[@]}" --set "outputs.output[0].function=1" --set "outputs.output[0].cylinder=1" \
                                  "Configuration/Electrical/Outputs/IGN1"                      quick-start-output-test
"$SHOOT" --crop 670x215+318+290   "${EX1[@]}" "$CFG/Trigger System/Diagnostics"                quick-start-trigger-diagnostics
"$SHOOT" --crop 1008x270+312+240  "${EX1[@]}" "Configuration/Fuel Tuning/Cranking"             quick-start-cranking
