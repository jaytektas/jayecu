#!/usr/bin/env bash
# The screenshots of chapter 10 (Power, grounds and protection), reproducibly. Re-run after a UI change:
#   manual/tools/shots/power-grounds.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
BATT="Configuration/Sensors/Other/Battery Voltage"

# The battery input as shipped: fixed to AV12, a straight line from the pin's 0-5 V to 0-30.25 V.
"$SHOOT" --crop 1000x650+312+240 "$BATT" power-grounds-battery

# The chapter's example checks: warn below 10 V and above 16 V (P0562 / P0563).
BATT_CHECKS=(--set "sensors.sensor[battery].diag_enable=12"
             --set "sensors.sensor[battery].diag_op_min=10"
             --set "sensors.sensor[battery].diag_op_max=16")
"$SHOOT" --crop 1030x352+312+240 "${BATT_CHECKS[@]}" "$BATT/Diagnostics" power-grounds-battery-checks

# Example 2: LS17 (outputs.output[28]: IGN1-12 are 0-11, LS1 is 12) set up as the Main Relay template writes it (Generic, Digital, Fixed 100 %, On if
# unanswerable). The template's Turn On When condition ("1") is an expression and cannot be set from
# here, so the crop stops above the When group.
MAIN_RELAY=(--set "outputs.output[28].function=3" --set "outputs.output[28].kind=1"
            --set "outputs.output[28].value_source=2" --set "outputs.output[28].on_invalid=1"
            --set "outputs.output[28].fixed_x10=100")
"$SHOOT" --crop 890x452+312+240 "${MAIN_RELAY[@]}" "Configuration/Electrical/Outputs/LS17" power-grounds-main-relay
