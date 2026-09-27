#!/usr/bin/env bash
# The screenshots of chapter 18 (Outputs and the pin system), reproducibly. Re-run after a UI change:
#   manual/tools/shots/outputs.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
PAGE="Configuration/Electrical/Outputs"

# THE ENGINE: the default 4-cylinder, switched to coil-on-plug, so the studio lays the coils out.
# The injector stage's mode is changed and changed back, which makes it lay out the injectors too.
# (Row numbers are 0-based: IGN1-12 are rows 0-11, LS1-22 rows 12-33, HS1-8 rows 34-41.)
ENGINE=(--set engine.ign_mode=2 --set "engine.inj_stage[0].mode=1" --set "engine.inj_stage[0].mode=0")

# EXAMPLE 1 — a boost solenoid on LS5 (row 16): Generic, PWM at 30 Hz, one candidate, Wastegate Duty
# (bus signal 12), as Primary. Failsafe and clamps at their defaults.
BOOST=(--set "outputs.output[16].function=3" --set "outputs.output[16].kind=0"
       --set "outputs.output[16].pwm_freq_hz=30" --set "outputs.output[16].value_source=0"
       --set "outputs.output[16].n_cand=1" --set "outputs.output[16].cand[0].sig=12"
       --set "outputs.output[16].cand[0].role=0")

# The fan (LS6, row 17) and the fuel pump (HS1, row 34) of the other examples, so the overview shows
# a whole car's worth of pins.
OTHERS=(--set "outputs.output[17].function=3" --set "outputs.output[17].kind=1"
        --set "outputs.output[34].function=3" --set "outputs.output[34].kind=1")

"$SHOOT" --crop 1062x642+312+240 "${ENGINE[@]}" "${BOOST[@]}" "${OTHERS[@]}" "$PAGE"              outputs-overview
"$SHOOT" --crop 432x486+318+292  "${ENGINE[@]}" "${BOOST[@]}" "$PAGE/LS5"                         outputs-pin-this
"$SHOOT" --crop 456x522+750+292  "${ENGINE[@]}" "${BOOST[@]}" "$PAGE/LS5"                         outputs-pin-value
"$SHOOT" --crop 382x572+1206+292 "${ENGINE[@]}" "${BOOST[@]}" "$PAGE/LS5"                         outputs-pin-shaping
"$SHOOT" --crop 1034x490+308+238 "${ENGINE[@]}" "${BOOST[@]}" "$PAGE/LS5/Frequency"               outputs-frequency
"$SHOOT" --crop 432x486+318+292  "${ENGINE[@]}" "$PAGE/IGN1"                                      outputs-pin-coil
