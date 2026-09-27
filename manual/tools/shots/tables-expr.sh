#!/usr/bin/env bash
# The screenshots of chapter 34 (Generic tables and expressions), reproducibly. Re-run after a UI change:
#   manual/tools/shots/tables-expr.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700

# Generic Table 1 set up as a fan curve against coolant (signal id 61): X axis 80..105 C. (--set reaches
# single settings only, so the cells stay at their default 0.)
FAN=(--set 'generic_tables.table_1_x_src=61'
     --set 'generic_tables.t1_x_axis[0]=80'  --set 'generic_tables.t1_x_axis[1]=85'
     --set 'generic_tables.t1_x_axis[2]=90'  --set 'generic_tables.t1_x_axis[3]=95'
     --set 'generic_tables.t1_x_axis[4]=98'  --set 'generic_tables.t1_x_axis[5]=100'
     --set 'generic_tables.t1_x_axis[6]=103' --set 'generic_tables.t1_x_axis[7]=105')

"$SHOOT" --crop 1330x330+310+237 "Configuration/Generic Tables" generic-tables
"$SHOOT" --crop 1330x520+310+237 "${FAN[@]}" "Configuration/Generic Tables/Generic Table 1" generic-table-1
"$SHOOT" --crop 544x464+578+268 --act "$(dirname "$0")/tables-expr.act" --set launch.enabled=1 \
         "Configuration/Vehicle Functions/Launch Control" expression-editor
