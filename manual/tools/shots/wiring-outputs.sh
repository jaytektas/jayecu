#!/usr/bin/env bash
# The screenshots of chapter 12 (Wiring outputs), reproducibly. Re-run after a UI change:
#   manual/tools/shots/wiring-outputs.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
OUT="Configuration/Electrical/Outputs"

# Output rows are the board's pins in order: IGN1-12 = rows 0-11, LS1-22 = rows 12-33, HS1-8 = rows 34-41.
# function: 0 None, 1 Ignition, 2 Injector, 3 Generic.  kind: 0 PWM, 1 Digital.  value_source 2 = Fixed.
# The chapter's example: a four-cylinder, coil-on-plug and sequential, with a boost solenoid on LS13,
# a PWM idle valve on LS14, a fan relay on LS5 and a fuel pump relay on HS1.
EXAMPLE=()
for i in 0 1 2 3; do                       # IGN1-4 -> coils for cylinders 1-4
    EXAMPLE+=(--set "outputs.output[$i].function=1" --set "outputs.output[$i].cylinder=$((i + 1))")
done
for i in 12 13 14 15; do                   # LS1-4 -> injectors for cylinders 1-4
    EXAMPLE+=(--set "outputs.output[$i].function=2" --set "outputs.output[$i].cylinder=$((i - 11))")
done
EXAMPLE+=(--set "outputs.output[16].function=3" --set "outputs.output[16].kind=1" --set "outputs.output[16].value_source=2")   # LS5 fan relay
EXAMPLE+=(--set "outputs.output[24].function=3" --set "outputs.output[24].kind=0" --set "outputs.output[24].pwm_freq_hz=30"
          --set "outputs.output[24].n_cand=1" --set "outputs.output[24].cand[0].sig=12")        # LS13 boost solenoid <- wastegate_duty
EXAMPLE+=(--set "outputs.output[25].function=3" --set "outputs.output[25].kind=0"
          --set "outputs.output[25].n_cand=1" --set "outputs.output[25].cand[0].sig=137")       # LS14 idle valve <- idle_duty
EXAMPLE+=(--set "outputs.output[34].function=3" --set "outputs.output[34].kind=1" --set "outputs.output[34].value_source=2")  # HS1 pump relay

"$SHOOT" --crop 1062x628+310+240 "${EXAMPLE[@]}" "$OUT"        wiring-outputs-overview
"$SHOOT" --crop 440x520+318+240  "${EXAMPLE[@]}" "$OUT/IGN1"   wiring-outputs-coil
"$SHOOT" --crop 890x655+312+240  "${EXAMPLE[@]}" "$OUT/LS13"   wiring-outputs-boost-solenoid

# Half Bridge A as shipped for an electronic throttle (etb_duty_1 / etb_en_1 are the defaults), switched on.
"$SHOOT" --crop 1150x640+312+240 --set "h_bridge.half[0].enabled=1" \
    "Configuration/Electrical/Half Bridges/Half Bridge A" wiring-outputs-half-bridge-a

# A stepper idle valve across both bridges: coil A on bridge A, coil B on bridge B.
STEPPER=(--set stepper.enabled=1
         --set "h_bridge.half[0].enabled=1" --set "h_bridge.half[0].demand_sig=217" --set "h_bridge.half[0].enable_sig=219"
         --set "h_bridge.half[1].enabled=1" --set "h_bridge.half[1].demand_sig=218" --set "h_bridge.half[1].enable_sig=220")
"$SHOOT" --crop 840x575+312+305 "${STEPPER[@]}" "Configuration/Engine Functions/Idle Stepper" wiring-outputs-stepper
