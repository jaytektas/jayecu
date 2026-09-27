#!/usr/bin/env bash
# The screenshots of chapter 23 (Closed-loop lambda), reproducibly. Re-run after a UI change:
#   manual/tools/shots/lambda.sh
set -euo pipefail
SHOOT="$(dirname "$0")/../shoot.sh"
export SHOOT_W=1700               # the O2 Control pages are drawn wider than a 1600 window leaves room for
P="Configuration/Fuel Tuning"

# The chapter's example: one wideband (sensor 1, Wideband O2 1) given the Overall job, and the loop
# listening to "Wideband Overall", with long-term trim on.
EX=(--set "sensors.sensor[1].enabled=1" --set "lambda.wb[0].assign=1"
    --set lambda.enabled=1 --set lambda.ltft_enabled=1 --set lambda.o2_src_1=19)

"$SHOOT" --crop 1340x625+310+237 "${EX[@]}" "$P/O2 Control"                 lambda-o2-control
"$SHOOT" --crop 1340x570+310+237 "${EX[@]}" "$P/O2 Control/Wideband Scope"  lambda-scope
"$SHOOT" --crop 1340x375+310+237 "${EX[@]}" "$P/O2 Control/Lambda Delay"    lambda-delay
"$SHOOT" --crop 1340x660+310+237 "${EX[@]}" "$P/Long Term Fuel Trim"        lambda-ltft
"$SHOOT" --crop 1340x290+310+237 "${EX[@]}" "$P/Bank Trim"                  lambda-bank
