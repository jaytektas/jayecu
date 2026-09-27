#!/usr/bin/env bash
# The screenshots of chapter 15 (Engine and vehicle basics), reproducibly. Re-run after a UI change:
#   manual/tools/shots/engine-vehicle.sh
#
# The TDC angles are --set by hand here. On an even-fire engine the ECU computes them from the firing
# order and the studio reads them back when it is connected; a demo project has no ECU, so the shots
# set the values the ECU would write (position x cycle / cylinders).
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
ENG="Configuration/Engine Configuration"

# Example 1: a 2.0 L inline four, firing order 1-3-4-2, wasted spark, sequential injection.
I4=(--set engine.cylinder_count=4 --set engine.displacement=1998 --set engine.bore_mm=86
    --set engine.cycle_type=1 --set engine.ign_mode=1
    --set 'engine.firing_order[0].cyl=1' --set 'engine.firing_order[1].cyl=3'
    --set 'engine.firing_order[2].cyl=4' --set 'engine.firing_order[3].cyl=2'
    --set 'engine.cyl[0].tdc_angle=0'   --set 'engine.cyl[2].tdc_angle=180'
    --set 'engine.cyl[3].tdc_angle=360' --set 'engine.cyl[1].tdc_angle=540')

# Example 2: a 5.7 L GM LS V8, firing order 1-8-7-2-6-5-4-3, coil-on-plug, odd cylinders on bank 1.
V8=(--set engine.cylinder_count=8 --set engine.displacement=5665 --set engine.bore_mm=99
    --set engine.cycle_type=1 --set engine.ign_mode=2)
ORDER=(1 8 7 2 6 5 4 3)
for i in "${!ORDER[@]}"; do
    c=${ORDER[$i]}
    V8+=(--set "engine.firing_order[$i].cyl=$c" --set "engine.cyl[$((c - 1))].tdc_angle=$((i * 90))"
         --set "engine.cyl[$((c - 1))].bank=$(( c % 2 == 1 ? 1 : 2 ))")
done

# Example 3: a 90-degree V-twin, odd-fire: cylinder 2 reaches TDC 270 degrees after cylinder 1.
VTWIN=(--set engine.cylinder_count=2 --set engine.displacement=1100 --set engine.bore_mm=0
       --set engine.cycle_type=1 --set engine.odd_fire=1 --set engine.ign_mode=2
       --set 'engine.firing_order[0].cyl=1' --set 'engine.firing_order[1].cyl=2'
       --set 'engine.cyl[0].tdc_angle=0' --set 'engine.cyl[1].tdc_angle=270')

# Example 4: a two-rotor rotary: six faces, 1080-degree cycle, faces fire 1-4-2-5-3-6.
ROTARY=(--set engine.cycle_type=2 --set engine.cylinder_count=6 --set engine.displacement=1308
        --set engine.bore_mm=0)
FACES=(1 4 2 5 3 6)
for i in "${!FACES[@]}"; do
    f=${FACES[$i]}
    ROTARY+=(--set "engine.firing_order[$i].cyl=$f" --set "engine.cyl[$((f - 1))].tdc_angle=$((i * 180))")
done

"$SHOOT" --crop 880x240+310+291  "${I4[@]}"     "$ENG"                        engine-vehicle-branch
"$SHOOT" --crop 965x462+315+238  "${I4[@]}"     "$ENG/Cylinders & Firing"     engine-vehicle-cylinders
"$SHOOT" --crop 965x462+315+238  "${V8[@]}"     "$ENG/Cylinders & Firing"     engine-vehicle-v8
"$SHOOT" --crop 965x462+315+238  "${VTWIN[@]}"  "$ENG/Cylinders & Firing"     engine-vehicle-vtwin
"$SHOOT" --crop 965x462+315+238  "${ROTARY[@]}" "$ENG/Cylinders & Firing"     engine-vehicle-rotary
"$SHOOT" --crop 875x305+310+238                 "$ENG/Vehicle Identity"       engine-vehicle-identity
"$SHOOT" --crop 475x274+315+291  "${I4[@]}"     "$ENG/Ignition System"        engine-vehicle-ignition
"$SHOOT" --crop 475x274+315+291  "${I4[@]}" --set engine.ign_enable=0 "$ENG/Ignition System" engine-vehicle-ign-off
"$SHOOT" --crop 1055x367+315+238 "${I4[@]}"     "$ENG/Fuel System"            engine-vehicle-fuel
