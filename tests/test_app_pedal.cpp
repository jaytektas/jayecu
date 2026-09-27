// Host test for App — drive-by-wire accelerator pedal. Covers the safety behaviours the audit flagged
// as untested: A/B correlation fault, pedal-loss fail-safe, and calibrate holding demand at 0.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/App.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"
#include "../generated/ecu_config.h"
#include "../generated/sensors_catalog.h"   // SENSOR_CATALOG / SENSOR_COUNT (find a sensor by its channel)
#include "../generated/module_dtc.h"         // ModuleDtc::APP_CORRELATION / APP_A_MISSING
#include "../firmware/Diagnostics/DtcManager.h"

static uint32_t g_ms = 0;
bool g_system_active = true;   // key-on (the pedal fault latch holds until it goes false)
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }
// Programmable raw seam: pedalcal reads each track through it, so a test can sweep synthetic pedals.
static uint16_t g_raw[64] = {0};
extern "C" uint16_t platform_read_ain_raw(uint8_t ch) { return ch < 64 ? g_raw[ch] : 0; }

// Same lookup the module uses: a sensor is found by the channel it publishes.
static int find_app(SignalId sig) {
    for (int i = 0; i < static_cast<int>(SENSOR_COUNT); i++)
        if (SENSOR_CATALOG[i].primary_channel == sig) return i;
    return -1;
}
uint32_t g_config_generation = 0;         // pedalcal bumps this so Sensors would re-read
volatile uint16_t g_command_state = 0;    // pedalcal progress (defined in CommsManager on target)

// enabled, 10% match window / 200 ms debounce; a CONSTANT pedal->throttle table
// (42.0%) so a matched pedal maps to a deterministic non-zero demand regardless of the axis.
static AppConfig make_cfg() {
    AppConfig c{};
    c.enabled       = 1;
    // No app_a_src/app_b_src: the module is FIXED to the APP sensors (app_1 = value, app_2 = check),
    // so there is no selector to point at the wrong channel — and nothing for a test to set.
    c.match_err_pct = 100;    // 10.0% (scale 0.1)
    c.match_ms      = 200;
    c.cal_min_span  = 100;
    c.pedal_to_throttle_table_x_src = SIG_APP_1;
    for (unsigned i = 0; i < APP_PEDAL_TO_THROTTLE_TABLE_ALLOC; i++)
        c.pedal_to_throttle_table[i] = 420;   // CELL_U16 * 0.1 = 42.0%
    return c;
}

static void run(App& a, SignalBus& bus, uint32_t dt) {
    g_ms += dt; EnginePosition p{}; EngineFrame f{}; a.update(p, bus, f);
}

int main() {
    fprintf(stdout, "=== App (DBW pedal) ===\n");

    SECTION("matched pedal -> demand from the pedal->throttle table");
    {
        auto cfg = make_cfg(); App a; a.init(cfg);
        SignalBus bus{};
        bus.set(SIG_APP_1, 50.0f, true); bus.set(SIG_APP_2, 50.0f, true);
        bus.set(wk::engine_state, 2.0f);   // RUNNING
        run(a, bus, 10);
        CHECK_NEAR(bus.get(SIG_PEDAL_DEMAND, -1.0f), 42.0f, 0.5f);
    }

    SECTION("A/B correlation fault (disagree past debounce) -> pedal_demand 0 (fail-safe)");
    {
        auto cfg = make_cfg(); App a; a.init(cfg);
        SignalBus bus{};
        bus.set(SIG_APP_1, 50.0f, true); bus.set(SIG_APP_2, 90.0f, true);  // 40% apart >> 10% window
        bus.set(wk::engine_state, 2.0f);
        run(a, bus, 10);                                // establish last_ms_
        for (int i = 0; i < 25; i++) run(a, bus, 10);   // 250 ms of disagreement > match_ms (200)
        CHECK_NEAR(bus.get(SIG_PEDAL_DEMAND, -1.0f), 0.0f, 0.01f);
    }

    SECTION("a pedal fault HOLDS until key-off: an intermittent track cannot toggle the throttle");
    {
        auto cfg = make_cfg(); App a; a.init(cfg);
        SignalBus bus{};
        bus.set(wk::engine_state, 2.0f);
        bus.set(SIG_APP_1, 50.0f, true); bus.set(SIG_APP_2, 90.0f, true);
        run(a, bus, 10);
        for (int i = 0; i < 25; i++) run(a, bus, 10);            // fault latches
        bus.set(SIG_APP_2, 50.0f, true);                          // the track comes back into agreement
        for (int i = 0; i < 25; i++) run(a, bus, 10);
        CHECK_NEAR(bus.get(SIG_PEDAL_DEMAND, -1.0f), 0.0f, 0.01f);   // still limp: not trusted again yet
        g_system_active = false; run(a, bus, 10);                 // key off...
        g_system_active = true;  run(a, bus, 10);                 // ...and on
        CHECK_NEAR(bus.get(SIG_PEDAL_DEMAND, -1.0f), 42.0f, 0.5f);   // a new key cycle trusts it again
    }

    SECTION("pedal A invalid -> pedal_demand 0 (fail-safe idle)");
    {
        auto cfg = make_cfg(); App a; a.init(cfg);
        SignalBus bus{};
        bus.set(SIG_APP_2, 50.0f, true);   // app_a NOT published -> invalid
        bus.set(wk::engine_state, 2.0f);
        run(a, bus, 10);
        CHECK_NEAR(bus.get(SIG_PEDAL_DEMAND, -1.0f), 0.0f, 0.01f);
    }

    SECTION("calibrate mode holds pedal_demand at 0 (no throttle from a mid-cal value)");
    {
        auto cfg = make_cfg(); App a; a.init(cfg);
        SignalBus bus{};
        bus.set(SIG_APP_1, 80.0f, true); bus.set(SIG_APP_2, 80.0f, true);  // would map to 42%
        bus.set(wk::engine_state, 0.0f);   // STOPPED (calibrate requires it)
        a.start_calibrate();
        run(a, bus, 10);
        CHECK_NEAR(bus.get(SIG_PEDAL_DEMAND, -1.0f), 0.0f, 0.01f);
    }

    // A CUT MUST NAME ITS CAUSE. Zero demand is the same number as a lifted foot, so for a long time a
    // pedal cut was invisible: no code, no channel, nothing to tell a correlation failure from a missing
    // track — or from the driver. Both causes now report, and they report DIFFERENTLY.
    SECTION("a correlation cut raises P2138 and says so on app_state");
    {
        auto cfg = make_cfg(); App a; a.init(cfg);
        DtcManager dtc; dtc.init(1); dtc.set_active(true); a.set_dtc(&dtc);
        SignalBus bus{};
        bus.set(SIG_APP_1, 50.0f, true); bus.set(SIG_APP_2, 90.0f, true);
        bus.set(wk::engine_state, 2.0f);
        run(a, bus, 10);
        CHECK(dtc.code_severity(ModuleDtc::APP_CORRELATION) == 0);          // inside the debounce: not yet a fault
        CHECK_NEAR(bus.get(SIG_APP_STATE, -1.0f), 0.0f, 0.01f);     // ST_OK

        for (int i = 0; i < 25; i++) run(a, bus, 10);               // 250 ms > match_ms (200)
        CHECK(dtc.code_severity(ModuleDtc::APP_CORRELATION) != 0);
        CHECK(dtc.code_severity(ModuleDtc::APP_A_MISSING) == 0);            // not the missing-track code
        CHECK_NEAR(bus.get(SIG_APP_STATE, -1.0f), 2.0f, 0.01f);     // ST_FAULT_CORRELATION
        CHECK_NEAR(bus.get(SIG_PEDAL_DEMAND, -1.0f), 0.0f, 0.01f);

        bus.set(SIG_APP_2, 50.0f, true);                            // pedal agrees again
        run(a, bus, 10);
        CHECK(dtc.code_severity(ModuleDtc::APP_CORRELATION) != 0);          // HELD for this key cycle
        CHECK_NEAR(bus.get(SIG_APP_STATE, -1.0f), 2.0f, 0.01f);
        g_system_active = false; run(a, bus, 10);                   // key off...
        g_system_active = true;  run(a, bus, 10);                   // ...on: trusted again, healed
        CHECK(dtc.code_severity(ModuleDtc::APP_CORRELATION) == 0);
        CHECK_NEAR(bus.get(SIG_APP_STATE, -1.0f), 0.0f, 0.01f);
    }

    SECTION("a missing primary track raises the OTHER code, not the correlation one");
    {
        auto cfg = make_cfg(); App a; a.init(cfg);
        DtcManager dtc; dtc.init(1); dtc.set_active(true); a.set_dtc(&dtc);
        SignalBus bus{};
        bus.set(SIG_APP_2, 50.0f, true);                            // app_1 never published
        bus.set(wk::engine_state, 2.0f);
        run(a, bus, 10);
        CHECK(dtc.code_severity(ModuleDtc::APP_A_MISSING) != 0);
        CHECK(dtc.code_severity(ModuleDtc::APP_CORRELATION) == 0);          // nothing to correlate WITH
        CHECK_NEAR(bus.get(SIG_APP_STATE, -1.0f), 3.0f, 0.01f);     // ST_FAULT_NO_SIGNAL
    }

    // The pedal cut and the throttle cut must never be mistaken for one another: P2138 is the pedal pair,
    // P2135 (raised by ElectronicThrottle) is the TPS pair, and they are different numbers.
    SECTION("the pedal code is not the ETB's TPS code");
    {
        CHECK(ModuleDtc::APP_CORRELATION == 0x2138);
        CHECK(ModuleDtc::APP_CORRELATION != 0x2135);
        CHECK(ModuleDtc::APP_A_MISSING   != ModuleDtc::APP_CORRELATION);
    }

    SECTION("disabled -> module does not publish pedal_demand");
    {
        auto cfg = make_cfg(); cfg.enabled = 0; App a; a.init(cfg);
        SignalBus bus{};
        bus.set(SIG_APP_1, 50.0f, true); bus.set(SIG_APP_2, 50.0f, true);
        run(a, bus, 10);
        CHECK(bus.get(SIG_PEDAL_DEMAND, -999.0f) == -999.0f);   // unpublished
    }

    SECTION("calibrate: a FALLING second track calibrates the right way round (declared sense)");
    {
        // Point the two APP sensors at distinct analog channels so the raw seam feeds them separately.
        g_config.sensors.sensor[find_app(SIG_APP_1)].source = 2;
        g_config.sensors.sensor[find_app(SIG_APP_2)].source = 3;
        // The pedal that started this: track A rises with travel, track B falls. Sweeping gives the same
        // four raw values whichever way the foot moved, so direction comes from configuration.
        auto cfg = make_cfg();
        cfg.app1_sense = 0;   // rising
        cfg.app2_sense = 1;   // falling
        App a; a.init(cfg);
        SignalBus bus{};
        bus.set(wk::engine_state, 0.0f);   // STOPPED

        const uint8_t ch_a = g_config.sensors.sensor[find_app(SIG_APP_1)].source;
        const uint8_t ch_b = g_config.sensors.sensor[find_app(SIG_APP_2)].source;
        a.start_calibrate();
        // released -> pressed -> released. A rises 500..3500, B falls 3500..500.
        const uint16_t seq_a[] = { 500, 1200, 2400, 3500, 2400, 1200, 500 };
        const uint16_t seq_b[] = { 3500, 2800, 1600, 500, 1600, 2800, 3500 };
        for (unsigned i = 0; i < 7; ++i) {
            g_raw[ch_a] = seq_a[i]; g_raw[ch_b] = seq_b[i];
            run(a, bus, 100);
        }
        run(a, bus, 5000);                 // close the window

        const SensorConfig& sa = g_config.sensors.sensor[find_app(SIG_APP_1)];
        const SensorConfig& sb = g_config.sensors.sensor[find_app(SIG_APP_2)];
        // A rising track: low raw -> 0 %, high raw -> 100 %.
        CHECK(sa.cal_n == 2 && sa.cal_raw[0] == 500 && sa.cal_val[0] == 0
                            && sa.cal_raw[1] == 3500 && sa.cal_val[1] == 1000);
        // A FALLING track must come out inverted — high raw is the RELEASED end, so it reads 0 %.
        // Written ascending by raw, so the pair is (500 -> 100 %, 3500 -> 0 %).
        CHECK(sb.cal_n == 2 && sb.cal_raw[0] == 500 && sb.cal_val[0] == 1000
                            && sb.cal_raw[1] == 3500 && sb.cal_val[1] == 0);
    }

    SECTION("calibrate: sweeping the other way round gives the SAME cal (order cannot matter)");
    {
        auto cfg = make_cfg(); cfg.app1_sense = 0; cfg.app2_sense = 1;
        App a; a.init(cfg);
        SignalBus bus{};
        bus.set(wk::engine_state, 0.0f);
        const uint8_t ch_a = g_config.sensors.sensor[find_app(SIG_APP_1)].source;
        const uint8_t ch_b = g_config.sensors.sensor[find_app(SIG_APP_2)].source;
        a.start_calibrate();
        // Start PRESSED and finish pressed — the ambiguous case. Same extremes, so the same calibration:
        // the operator's timing no longer decides which end is released.
        const uint16_t seq_a[] = { 3500, 2000, 500, 2000, 3500 };
        const uint16_t seq_b[] = { 500, 2000, 3500, 2000, 500 };
        for (unsigned i = 0; i < 5; ++i) { g_raw[ch_a] = seq_a[i]; g_raw[ch_b] = seq_b[i]; run(a, bus, 100); }
        run(a, bus, 5000);
        const SensorConfig& sa = g_config.sensors.sensor[find_app(SIG_APP_1)];
        const SensorConfig& sb = g_config.sensors.sensor[find_app(SIG_APP_2)];
        CHECK(sa.cal_raw[0] == 500 && sa.cal_val[0] == 0 && sa.cal_raw[1] == 3500 && sa.cal_val[1] == 1000);
        CHECK(sb.cal_raw[0] == 500 && sb.cal_val[0] == 1000 && sb.cal_raw[1] == 3500 && sb.cal_val[1] == 0);
    }

    return test_summary();
}
