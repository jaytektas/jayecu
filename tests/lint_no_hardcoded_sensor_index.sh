#!/usr/bin/env bash
# Lint: no hardcoded catalog sensor indices in tests.
#
# The sensor catalog (generated/sensors_catalog.h) is codegen'd and REORDERS whenever sensors are added
# or moved, so a literal `g_config.sensors.sensor[4]` silently points at a different sensor than intended.
# This exact drift broke test_throttle_control (index 4 shifted from the aux/tps_2 sensor to
# exh_manifold_pressure). Resolve the index by the SignalId the sensor provides instead, e.g.
#
#     static int sensor_idx(SignalId sig) {           // matches ElectronicThrottle::find_sensor
#         for (int i = 0; i < (int)SENSOR_COUNT; i++)
#             if (SENSOR_CATALOG[i].primary_channel == sig) return i;
#         return -1;
#     }
#     g_config.sensors.sensor[sensor_idx(SIG_TPS)].source = 0;
#
# Runs as a ctest case so a reintroduced literal index fails the suite, not a live ECU.
set -euo pipefail
dir="$(cd "$(dirname "$0")" && pwd)"
hits="$(grep -rnE '\.sensor\[[0-9]+\]' "$dir" --include='*.cpp' --include='*.h' || true)"
if [ -n "$hits" ]; then
    echo "LINT FAIL: hardcoded catalog sensor index — resolve by SignalId via sensor_idx(SIG_...) instead:"
    echo "$hits"
    exit 1
fi
echo "lint OK: no hardcoded sensor indices in tests"
