#!/usr/bin/env bash
# The screenshots of chapter 33 (CAN configuration), reproducibly. Re-run after a UI change:
#   manual/tools/shots/can-config.sh
#
# Transmit: frame 0x600 on CAN1, 8 bytes every 50 ms —
#   rpm   start bit  7, 16 bits, x1
#   clt   start bit 23, 16 bits, signed, x10
#   map   start bit 39, 16 bits, x10
#   tps   start bit 55,  8 bits, x2
# Receive: frame 0x650 on CAN1 — gear at start bit 7, 8 bits, Valid For 500 ms, 0xFF = no reading.
# Signal ids from generated/signal_ids.h: clt 61, gear 131, map 273, rpm 319, tps 337.
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700 SHOOT_H=1200

TX=(--set 'can.gc_frame[0].flags=3' --set 'can.gc_frame[0].bus=0' --set 'can.gc_frame[0].id=1536'
    --set 'can.gc_frame[0].dlc=8' --set 'can.gc_frame[0].period_ms=50'
    --set 'can.gc_frame[0].first_field=0' --set 'can.gc_frame[0].field_count=4'
    --set 'can.gc_field[0].sig=319' --set 'can.gc_field[0].bit_off=7'  --set 'can.gc_field[0].width=16'
    --set 'can.gc_field[1].sig=61'  --set 'can.gc_field[1].bit_off=23' --set 'can.gc_field[1].width=16'
    --set 'can.gc_field[1].flags=1' --set 'can.gc_field[1].scale=10'
    --set 'can.gc_field[2].sig=273' --set 'can.gc_field[2].bit_off=39' --set 'can.gc_field[2].width=16'
    --set 'can.gc_field[2].scale=10'
    --set 'can.gc_field[3].sig=337' --set 'can.gc_field[3].bit_off=55' --set 'can.gc_field[3].width=8'
    --set 'can.gc_field[3].scale=2')
RX=(--set 'can.gc_frame[1].flags=1' --set 'can.gc_frame[1].bus=0' --set 'can.gc_frame[1].id=1616'
    --set 'can.gc_frame[1].dlc=8'
    --set 'can.gc_frame[1].first_field=4' --set 'can.gc_frame[1].field_count=1'
    --set 'can.gc_field[4].sig=131' --set 'can.gc_field[4].bit_off=7' --set 'can.gc_field[4].width=8'
    --set 'can.gc_field[4].ttl_ms=500' --set 'can.gc_field[4].flags=4' --set 'can.gc_field[4].sentinel=255')

"$SHOOT" --crop 1340x800+305+237 --act "$(dirname "$0")/can-config.act" "${TX[@]}" "${RX[@]}" "Configuration/CAN Bus/CAN1/Transmit" can-transmit
"$SHOOT" --crop 1340x600+305+237 --act "$(dirname "$0")/can-config.act" "${TX[@]}" "${RX[@]}" "Configuration/CAN Bus/CAN1/Receive"  can-receive
