#!/usr/bin/env bash
# The screenshots of chapter 19 (Fuel), reproducibly. Re-run after a UI change:
#   manual/tools/shots/fuel.sh
# Captures are 1600x1000; each is cropped to the page's own panels.
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
FT="Configuration/Fuel Tuning"

# Example 1 of the chapter: a naturally aspirated 2.0 L four, speed-density, sequential, on a
# return-style rail with a vacuum-referenced 300 kPa regulator (Rising Rate at 1.0:1).
EX1=(--set engine.cylinder_count=4 --set engine.displacement=2000
     --set fuel_calculator.fuel_model=0
     --set fuel_calculator.stage1_fuel_press_mode=2 --set fuel_calculator.stage1_fuel_press_base_kpa=300
     --set fuel_calculator.stage1_fuel_press_ratio=1.0
     --set transient_throttle.enabled=1)

"$SHOOT" --crop 970x440+318+262   "${EX1[@]}" "Configuration/Engine Configuration/Fuel System/Fuel Setup" fuel-setup
"$SHOOT" --crop 1285x600+308+236  "${EX1[@]}" "$FT"                                   fuel-overview
"$SHOOT" --crop 1270x490+318+262  "${EX1[@]}" "$FT/Stage 1/Setup"                     fuel-stage1-setup
"$SHOOT" --crop 950x200+308+236   "${EX1[@]}" "$FT/Stage 1/Short Pulse Width Adder"   fuel-short-pulse
"$SHOOT" --crop 1285x110+308+236  "${EX1[@]}" "$FT/Specific Gravity"                  fuel-specific-gravity
"$SHOOT" --crop 1200x525+315+262  "${EX1[@]}" "$FT/VE Table"                          fuel-ve-table
"$SHOOT" --crop 1235x525+315+262  "${EX1[@]}" "$FT/Target Lambda"                     fuel-target-lambda
"$SHOOT" --crop 1275x390+315+262  "${EX1[@]}" --set fuel_calculator.prime_enable=1 \
         --set fuel_calculator.flood_clear_enabled=1 "$FT/Start & Warmup"             fuel-start-warmup
"$SHOOT" --crop 800x165+308+236   "${EX1[@]}" --set fuel_calculator.prime_enable=1 "$FT/Fuel Prime Pulse" fuel-prime
"$SHOOT" --crop 1015x275+308+236  "${EX1[@]}" "$FT/Cranking"                          fuel-cranking
"$SHOOT" --crop 1285x565+308+262  "${EX1[@]}" "$FT/Corrections"                       fuel-corrections
"$SHOOT" --crop 1285x520+308+262  "${EX1[@]}" "$FT/Transient Throttle"                fuel-transient
"$SHOOT" --crop 555x330+308+236   "${EX1[@]}" "$FT/Transient Throttle/Enrich Rate"    fuel-transient-rate
"$SHOOT" --crop 870x635+312+262   "${EX1[@]}" "$FT/Fuel Breakdown"                    fuel-breakdown

# The MAP-prediction + wall-film pair, switched on (and transient throttle off, as the page advises).
"$SHOOT" --crop 1285x575+308+262  "${EX1[@]}" --set fuel_calculator.map_predict_enabled=1 \
         --set fuel_calculator.wallfilm_enabled=1 --set transient_throttle.enabled=0 "$FT/MAP Prediction" fuel-map-prediction

# Example 3: individual throttle bodies on the Blend model — the Predicted MAP page only appears then.
"$SHOOT" --crop 1285x445+308+236  "${EX1[@]}" --set fuel_calculator.fuel_model=3 \
         --set fuel_calculator.blend_rpm_lo=2500 --set fuel_calculator.blend_rpm_hi=4500 "$FT/Predicted MAP" fuel-predicted-map

# Example 4: two stages — the Stage 2 pages only appear with two or more stages.
"$SHOOT" --crop 1015x100+308+236  "${EX1[@]}" --set engine.num_inj_stages=2 "$FT/Stage 2/Staging Duty" fuel-staging-duty
"$SHOOT" --crop 1285x290+308+236  "${EX1[@]}" --set fuel_calculator.map_predict_enabled=1 \
         "$FT/MAP Prediction/Transient TPS Scaling"                                    fuel-tps-scaling
