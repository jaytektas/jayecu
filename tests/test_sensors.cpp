// Host test for the Tier-3 Sensors runtime pipeline (firmware/Sensors/Sensors.cpp).
// Exercises: analog 2-point linear calibration, on-board battery read, switch
// debounce, raw + operating diagnostics (valid flag), and the EMA filter.
//
// Provides its own platform-HAL stubs; catalog indices are looked up by id so the
// test survives catalog reordering.

#include "Sensors/Sensors.h"
#include "Integration/HardwareInput.h"   // publishes SIG_HW_* (raw ADC) the analog Acquire reads off the bus
#include "Signal/Expr.h"                 // op-window arming runs a precondition EXPRESSION (expr::)
#include "Engine/EngineFrame.h"
#include "Signal/EnginePosition.h"
#include "Can/GenericCan.h"          // a CAN sensor resolves its field through the tune's frames

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

// Config-generation counter (defined in CommsManager in firmware) — Sensors re-derives
// its enabled-channel mask + pin arbitration when it changes. Stub it for the host test;
// bump it to force a recompute mid-test.
volatile uint32_t g_config_generation = 0;
// The written byte range, which on target the comms layer records (see CommsManager). A host test
// writes g_config directly, so it records none — and Sensors then falls back to the full sweep, which
// is exactly the behaviour an unknown range is supposed to get.
volatile uint32_t g_config_dirty_lo = 0xFFFFFFFFu;
volatile uint32_t g_config_dirty_hi = 0;

// System-active (key-on) gate, normally owned by EngineTask. Default true so the existing tests
// exercise every sensor; the bootstrap-gate test below flips it.
bool g_system_active = true;
// The 'key' bench override, defined in main.cpp on target. Sensors now owns the key decision, so the
// override it honours has to exist here too. 0 = follow the battery.
volatile int g_key_override = 0;
volatile int g_pg_override[2] = {0, 0};   // the 'pg' bench override, likewise defined in main.cpp

// --- platform HAL stubs ---
static uint16_t g_ain[64]  = {};
static bool     g_din[32]  = {};
static uint32_t g_freq[32] = {};
static uint32_t g_pulse[32]= {};
static float    g_batt     = 12.0f;
static uint32_t g_tick     = 0;
static uint16_t g_cap_disabled = 0;   // pins platform_capture_disable() was called for (teardown test)
extern "C" uint16_t platform_read_ain_raw(uint8_t pin) { return pin < 64 ? g_ain[pin] : 0; }
extern "C" bool     platform_read_din(uint8_t pin)    { return pin < 32 ? g_din[pin]  : false; }
static bool g_pg[2] = {true, true};   // the 5 V references' power-good lines (true = good)
extern "C" bool     platform_read_power_good(uint8_t i) { return i < 2 ? g_pg[i] : false; }
extern "C" uint32_t platform_read_freq(uint8_t pin)   { return pin < 32 ? g_freq[pin] : 0; }
extern "C" bool     platform_freq_enable(uint8_t pin) { return pin < 8; }   // pretend DIG1-8 wire up
extern "C" bool     platform_sent_enable(uint8_t pin) { return pin < 8; }
extern "C" uint32_t platform_read_sent(uint8_t, bool)  { return 0; }
extern "C" bool     platform_pulse_enable(uint8_t pin) { return pin < 8; }
extern "C" bool     platform_capture_disable(uint8_t pin) { if (pin < 8) g_cap_disabled |= (1u << pin); return pin < 8; }
extern "C" uint32_t platform_read_pulse_us(uint8_t pin) { return pin < 32 ? g_pulse[pin] : 0; }
extern "C" float    platform_read_battery_v()         { return g_batt; }
extern "C" float    platform_read_baro_kpa()          { return 101.3f; }
// Ambient from the same part + cache freshness. Deliberately a DIFFERENT value from the pressure:
// routing an on-board temperature to the barometric acquire is the bug these exist to catch, and
// identical stub values would hide it.
extern "C" float    platform_read_baro_temp_c()        { return 23.5f; }
extern "C" bool     platform_baro_valid()              { return true; }
extern "C" uint32_t platform_get_tick_ms()            { return g_tick; }
extern "C" uint32_t platform_cyccnt()                 { return 0; }

// A sensor's cadence is its TYPE's (SENSOR_TYPE_CATALOG[].update_hz), raised by a consumer claim and
// never lowered. DERIVE the slot and ttl from that rather than writing the millisecond figures in by
// hand: hardcoding them is exactly how this file rotted when the per-sensor `update_hz` was removed --
// the numbers stayed plausible and stopped matching what Sensors actually does.
static uint32_t slot_ms(int idx) {                 // one pipeline run per slot
    const uint16_t hz = SENSOR_TYPE_CATALOG[SENSOR_CATALOG[idx].type].update_hz;
    return hz ? (1000u / hz) : 1u;
}
static uint32_t ttl_of(int idx) {                  // Sensors stamps ttl_ms = 3000/hz
    const uint16_t hz = SENSOR_TYPE_CATALOG[SENSOR_CATALOG[idx].type].update_hz;
    return hz ? (3000u / hz) : 0u;
}

static int idx_of(const char* id) {
    for (int i = 0; i < SENSOR_COUNT; i++)
        if (std::strcmp(SENSOR_CATALOG[i].id, id) == 0) return i;
    fprintf(stderr, "catalog id not found: %s\n", id);
    assert(false);
    return -1;
}

static bool approx(float a, float b) { return std::fabs(a - b) < 0.01f; }

// Seed a 2-point linear calibration curve (0..raw_max mV -> 0..val_max) over the fixed
// SENSOR_CAL_POINTS curve — the tail points collapse onto the last point.
static void lin(SensorConfig& c, uint32_t raw_max, int32_t val_max) {
    c.cal_n = SENSOR_CAL_POINTS;                 // all points live (the curve is now resizable)
    c.cal_raw[0] = 0; c.cal_raw[1] = raw_max;
    c.cal_val[0] = 0; c.cal_val[1] = val_max;
    for (int i = 2; i < SENSOR_CAL_POINTS; i++) { c.cal_raw[i] = raw_max; c.cal_val[i] = val_max; }
}

int main() {
    const int CLT = idx_of("clt");
    const int BAT = idx_of("battery");
    const int SWP = idx_of("oil_pressure_sw");

    EnginePosition pos{};
    EngineFrame    frame{};

    // Real INPUT-phase order: HardwareInput publishes the raw ADC (from the g_ain stub) onto the bus,
    // THEN Sensors' analog Acquire reads SIG_HW_* off it. `pump` mirrors that so g_ain stays the input
    // knob. Harmless for freq/pulse/switch/engine-sync sections (their Acquire doesn't read SIG_HW_*).
    HardwareInput hw;
    auto pump = [&](Sensors& sen, SignalBus& b) { hw.update(pos, b, frame); sen.update(pos, b, frame); };

    // Sensors OWNS the key now (Sensors::key_on): it derives it from the battery mid-pass, so setting
    // g_system_active from a test no longer forces anything. Most sections here configure no battery at
    // all, which would read 0 V and gate every sensor off, so force the key exactly as the `key on`
    // bench command does. Section 17b clears this to test the real battery-driven decision.
    g_key_override = 1;

    // ---- 1. analog linear calibration: CLT on AV0, 0..5000mV -> 0..100 degC ----
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
        lin(c, 5000, 1000);
           // 0..100.00                                  // filter off — test cal directly

        Sensors s; s.init(cfg);
        SignalBus bus;
        g_ain[0] = 2500; g_tick = 10;
        pump(s, bus);
        assert(bus.valid(SIG_CLT));
        assert(approx(bus.get(SIG_CLT), 50.0f));                  // midpoint
        printf("ok  cal: CLT 2500mV -> %.2f degC\n", bus.get(SIG_CLT));
    }

    // ---- 2. battery: analog AV12 -> cal curve (the 0.1091 divider line, 0..5000mV -> 0..30.25V) ----
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[BAT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 11;
        lin(c, 5000, 3025);            // divider: raw 5000mV -> 30.25V (stored x100)
        Sensors s; s.init(cfg);
        SignalBus bus;
        g_ain[11] = 2232; g_tick = 20;  // 2232mV -> 2232/5000 * 30.25 = 13.5 V
        pump(s, bus);
        assert(bus.valid(SIG_BATTERY));
        assert(approx(bus.get(SIG_BATTERY), 13.5f));
        printf("ok  battery (analog AV12): %.2f V\n", bus.get(SIG_BATTERY));
    }

    // ---- 3. raw diagnostic trips the valid flag ----
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
        lin(c, 5000, 1000);
        c.diag_enable = 0x01;          // raw_min
        c.diag_raw_min = 500;
        Sensors s; s.init(cfg);
        SignalBus bus;
        g_ain[0] = 200; g_tick = 30;   // below the raw-min window -> faulted
        pump(s, bus);
        assert(!bus.valid(SIG_CLT));   // published but marked invalid
        assert(!s.healthy(CLT));
        printf("ok  raw-diag: 200mV < 500mV -> invalid + unhealthy\n");
    }

    // ---- 4. switch debounce (needs DEBOUNCE_COUNTS frames to latch) ----
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[SWP];
        c.enabled = 1; c.interface = IFSEL_DIGITAL; c.source = 0;
        Sensors s; s.init(cfg);
        SignalBus bus;
        const SignalId ch = (SignalId)SENSOR_CATALOG[SWP].primary_channel;
        g_din[0] = true;
        // ONE pipeline run per slot — step the clock by a full slot per frame or the debounce counts
        // runs that never happened. Derived from the type catalog, not written in: this used to force
        // every-frame updates via the config field `update_hz`, which no longer exists, and a
        // hand-written millisecond figure would rot the same way when a type's cadence changes.
        const uint32_t sw_slot = slot_ms(SWP);
        for (int f = 0; f < 2; f++) { g_tick += sw_slot; pump(s, bus); }
        assert(bus.get(ch) < 0.5f);    // not latched yet
        for (int f = 0; f < 2; f++) { g_tick += sw_slot; pump(s, bus); }
        assert(bus.get(ch) > 0.5f);    // latched on
        printf("ok  switch: debounced latch after %d frames\n", 4);
    }

    // ---- 4b. A SWITCH READS BOTH WAYS: a digital level, and an analog voltage against thresholds ----
    //
    // These are the two things "switch" means and they are different pipelines. The digital one reads
    // the pin's level; the analog one reads a voltage and decides with the first two calibration
    // breakpoints — which are the on/off trip points the wiring dialog edits. Nothing exercised the
    // analog path's actual DECISION before: the pipeline-builder test proves the stages get built and
    // stops there, so a threshold comparison could have been inverted, or ignored, and both suites
    // would still have passed.
    //
    // The interface enum was called "Switch" for the digital case until today, which is what made this
    // worth pinning: a switch is not one interface, it is a sensor that either interface can feed.
    {
        const uint32_t sw_slot = slot_ms(SWP);
        const SignalId ch = (SignalId)SENSOR_CATALOG[SWP].primary_channel;

        // ---- digital: the pin's level IS the answer, both directions ----
        {
            SensorsConfig cfg{};
            SensorConfig& c = cfg.sensor[SWP];
            c.enabled = 1; c.interface = IFSEL_DIGITAL; c.source = 0;
            Sensors s; s.init(cfg);
            SignalBus bus;
            g_din[0] = true;
            for (int f = 0; f < 6; f++) { g_tick += sw_slot; pump(s, bus); }
            assert(bus.get(ch) > 0.5f);
            g_din[0] = false;
            for (int f = 0; f < 6; f++) { g_tick += sw_slot; pump(s, bus); }
            assert(bus.get(ch) < 0.5f);          // and it comes back DOWN, not just up
            printf("ok  switch/digital: pin level drives the channel both ways\n");
        }

        // ---- analog: the same sensor, decided by voltage against the trip points ----
        {
            SensorsConfig cfg{};
            SensorConfig& c = cfg.sensor[SWP];
            c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
            // The first two breakpoints are the trip points, in ADC counts: off below 1000, on above
            // 2000. The gap between them is the hysteresis band a real switch needs.
            c.cal_n = 2; c.cal_raw[0] = 1000; c.cal_raw[1] = 2000;
            Sensors s; s.init(cfg);
            SignalBus bus;

            g_ain[0] = 500;                                     // well below the off point
            for (int f = 0; f < 6; f++) { g_tick += sw_slot; pump(s, bus); }
            assert(bus.get(ch) < 0.5f);
            printf("ok  switch/analog: 500 counts (below the off trip) reads OFF\n");

            g_ain[0] = 3000;                                    // well above the on point
            for (int f = 0; f < 6; f++) { g_tick += sw_slot; pump(s, bus); }
            assert(bus.get(ch) > 0.5f);
            printf("ok  switch/analog: 3000 counts (above the on trip) reads ON\n");

            // BETWEEN THE TRIP POINTS THE LEVEL STANDS. That is the hysteresis, and it is also what
            // makes this test sensitive: a pipeline that ignored the breakpoints and mapped the raw
            // value straight through would answer the two cases above correctly by accident and get
            // this one wrong, because 1500 is neither.
            g_ain[0] = 1500;
            for (int f = 0; f < 6; f++) { g_tick += sw_slot; pump(s, bus); }
            assert(bus.get(ch) > 0.5f);          // still ON — inside the band, nothing changed
            printf("ok  switch/analog: 1500 counts (inside the band) HOLDS ON\n");

            g_ain[0] = 500;
            for (int f = 0; f < 6; f++) { g_tick += sw_slot; pump(s, bus); }
            assert(bus.get(ch) < 0.5f);          // AND BACK — a trip point that only latches one way
            printf("ok  switch/analog: back below the off trip reads OFF again\n");

            g_ain[0] = 1500;
            for (int f = 0; f < 6; f++) { g_tick += sw_slot; pump(s, bus); }
            assert(bus.get(ch) < 0.5f);          // ...and holds OFF in the same band it held ON in
            printf("ok  switch/analog: 1500 counts HOLDS OFF — the band works both ways\n");
        }
    }

    // ---- 5. a step arrives WHOLE — there is no sensor-side filter ----
    // This used to assert that an EMA moved a fraction of the way toward a step (tau 90 ms, dt 10 ms,
    // so ~10 % of it). That filter is gone: a sensor publishes what it measured, and any smoothing is
    // the consumer's own business. Kept as a test because the property is worth pinning down — a
    // reading that lags its input is now a bug, not a setting.
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
        lin(c, 5000, 1000);
        Sensors s; s.init(cfg);
        SignalBus bus;
        // `temperature` samples at 5 Hz -> a 200 ms slot; step the clock a full slot per update.
        g_ain[0] = 0; g_tick = 200; pump(s, bus);     // seed at 0
        assert(approx(bus.get(SIG_CLT), 0.0f));
        g_ain[0] = 5000; g_tick = 400; pump(s, bus);  // step to 100
        const float v = bus.get(SIG_CLT);
        assert(v > 99.0f && v < 101.0f);              // the whole step, no lag
        printf("ok  no sensor filter: step lands whole -> %.2f (expect 100)\n", v);
    }

    // ---- 6. frequency interface: Hz -> linear units ----
    {
        const int TS1 = idx_of("turbo_speed_1");
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[TS1];
        c.enabled = 1; c.interface = IFSEL_DIGITAL_FREQ; c.source = 0;
        lin(c, 10000, 100);      // 0..10000 Hz
          // -> 0..100.00
        Sensors s; s.init(cfg);
        SignalBus bus;
        const SignalId ch = (SignalId)SENSOR_CATALOG[TS1].primary_channel;
        g_freq[0] = 5000; g_tick = 200;
        pump(s, bus);
        assert(approx(bus.get(ch), 50.0f));
        printf("ok  frequency: 5000 Hz -> %.2f\n", bus.get(ch));
    }

    // ---- disable-on-unassign: a freq sensor's DIG pin capture is torn down when the sensor is removed ----
    {
        const int TS1 = idx_of("turbo_speed_1");
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[TS1];
        c.enabled = 1; c.interface = IFSEL_DIGITAL_FREQ; c.source = 3;   // DIG4
        lin(c, 10000, 100);
        Sensors s; s.init(cfg);
        SignalBus bus;

        // While assigned, its pin must NOT be in the teardown set (the other, unused pins are).
        g_cap_disabled = 0;
        pump(s, bus);                                   // first pump forces the initial recompute
        assert(!(g_cap_disabled & (1u << 3)));
        printf("ok  teardown: assigned freq pin NOT disabled (disabled mask %#x)\n", g_cap_disabled);

        // Remove the sensor -> next recompute frees the pin -> its capture is disabled.
        g_cap_disabled = 0;
        c.enabled = 0;
        g_config_generation++;                          // force a recompute on the next pump
        pump(s, bus);
        assert(g_cap_disabled & (1u << 3));
        printf("ok  teardown: freed freq pin disabled on unassign\n");
    }

    // ---- 7. operating-max diagnostic sets severity -> worst_severity() ----
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
        lin(c, 5000, 1000);
        c.diag_enable   = 0x08;          // op_max
        c.diag_op_max = 500;       // 50.0 degC ceiling
        c.diag_severity = 0xC0;          // slot 3 (op_max) = 2-bit severity 3 (level 3)
        Sensors s; s.init(cfg);
        SignalBus bus;
        g_ain[0] = 4000; g_tick = 210;   // -> 80 degC > 50 -> op_max trips
        pump(s, bus);
        assert(bus.valid(SIG_CLT));               // op-range does NOT invalidate — value is real
        assert(approx(bus.get(SIG_CLT), 80.0f));  // ...still publishes for gauges/telemetry
        assert(s.worst_severity() == 3);          // ...while the DTC/severity still raises
        printf("ok  severity: op_max trip -> value=%.1f valid, worst_severity=%u\n",
               bus.get(SIG_CLT), s.worst_severity());
    }

    // ---- 8. channel_enabled gates which channels protection watches ----
    {
        SensorsConfig cfg{};
        cfg.sensor[CLT].enabled = 1; cfg.sensor[CLT].interface = IFSEL_ANALOG_VOLTAGE;
        Sensors s; s.init(cfg);
        assert(s.channel_enabled(SIG_CLT));     // configured
        assert(!s.channel_enabled(SIG_IAT));    // not configured
        printf("ok  channel_enabled: CLT yes, IAT no\n");
    }

    // ---- 9. tripped check emits its catalog DTC (P-code) ----
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
        lin(c, 5000, 1000);
        c.diag_enable = 0x08;            // op_max
        c.diag_op_max = 500;       // 50.0 ceiling
        c.diag_severity = 0xC0;          // op_max severity 3
        Sensors s; s.init(cfg);
        SignalBus bus;
        g_ain[0] = 4000; g_tick = 220;   // 80 > 50 -> op_max trips
        pump(s, bus);
        uint16_t dtcs[4]; const uint8_t n = s.active_dtcs(dtcs, 4);
        assert(n == 1);
        assert(dtcs[0] == SENSOR_CATALOG[CLT].dtc_op_max);  // clt op_max = P0116
        printf("ok  dtc: clt op_max -> 0x%04X (n=%u)\n", dtcs[0], n);
    }

    // ---- 9b. SWITCHING A CHECK OFF HEALS ITS CODE ----
    // Reported from the bench: arm ECU Temperature's reading-high check, drive it over the ceiling so the
    // DTC goes current, then untick the check — and the DTC stayed current for ever.
    //
    // The op-window codes are the only ones the policy loop can STRAND. It skips them whenever the
    // operating-window Condition did not evaluate ("not under load → leave op codes"), and a check that
    // has been switched off does not evaluate either — with both directions off the stage is not even
    // built. So the one path that could heal the code was the one path that no longer ran.
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
        lin(c, 5000, 1000);
        c.diag_enable = 0x08;            // op_max, and nothing else
        c.diag_op_max = 500;             // 50.0 degC ceiling
        c.diag_severity = 0xC0;
        DtcManager dtc; dtc.init(1);
        Sensors s; s.init(cfg); s.set_dtc(&dtc);
        SignalBus bus;
        g_ain[0] = 4000; g_tick = 230;   // 80 > 50 -> trips
        pump(s, bus);
        assert(dtc.active_count() == 1);
        // …and now the user unticks it. The reading does not change; the CHECK is gone.
        c.diag_enable = 0x00; g_config_generation++;
        g_tick = 400; pump(s, bus);   // past the input's decimation period, so the pipeline really runs
        assert(dtc.active_count() == 0);
        printf("ok  dtc: unticking a tripped op check heals its code\n");
    }

    // ---- 10. rate decimation + ttl expiry ----
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
        lin(c, 5000, 1000);              // period 100 ms, ttl 300 ms
        Sensors s; s.init(cfg);
        SignalBus bus;

        g_ain[0] = 2500; g_tick = 1000; pump(s, bus);   // due -> 50
        assert(approx(bus.get(SIG_CLT), 50.0f));
        g_ain[0] = 4000; g_tick = 1050; pump(s, bus);   // 50ms < 100ms period -> skipped
        assert(approx(bus.get(SIG_CLT), 50.0f));                     // value held, not 80
        g_tick = 1100; pump(s, bus);                    // now due -> 80
        assert(approx(bus.get(SIG_CLT), 80.0f));
        printf("ok  decimation: 10 Hz holds value between updates\n");

        // PHASE. Two sensors at the same rate must publish in the SAME pass, whenever each was started:
        // the ETB reads tps_a and tps_b off the bus and faults on their difference, so a pair sampled
        // half a period apart disagrees by however far the plate moved in between — a scheduling
        // artefact read as a sensor fault. Scheduling on a global grid is what makes that structural
        // rather than a side effect of init() happening to zero everyone at once.
        {
            SensorsConfig two{};
            SensorConfig& a = two.sensor[CLT];
            a.enabled = 1; a.interface = IFSEL_ANALOG_VOLTAGE; a.source = 0;
            lin(a, 5000, 1000);   // 100 ms
            Sensors sp; sp.init(two);
            SignalBus b2;

            g_ain[0] = 2500; g_tick = 1000; pump(sp, b2);   // A starts at 1000
            // B joins mid-period, at 1050 — the phase A would have handed it under `now + period`.
            const int IAT_ = idx_of("iat");
            SensorConfig& bcfg = two.sensor[IAT_];
            bcfg.enabled = 1; bcfg.interface = IFSEL_ANALOG_VOLTAGE; bcfg.source = 1;
            lin(bcfg, 5000, 1000);
            g_config_generation++;   // enabling an input after init() needs a recompute, as a 'w' write does
            g_ain[1] = 2500; g_tick = 1050; pump(sp, b2);   // B's first run — off-grid

            // By DEFAULT they are staggered — the phase is the sensor's own index, so a big pool spreads
            // across the period instead of landing on one engine frame. Aligned, they coincide.
            sp.align_phase(SIG_CLT, SIG_IAT);

            g_ain[0] = 4000; g_ain[1] = 4000;
            bool sawTogether = false;
            // `temperature` samples at 5 Hz -> a 200 ms slot (this used to be forced to 100 ms via the
            // removed `update_hz`), so walk far enough to cross several slot boundaries.
            for (uint32_t t = 1100; t <= 2400; t += 10) {     // walk a few periods
                g_tick = t; pump(sp, b2);
                const bool aM = approx(b2.get(SIG_CLT), 80.0f), bM = approx(b2.get(SIG_IAT), 80.0f);
                assert(aM == bM);                             // never one without the other
                if (aM) sawTogether = true;
            }
            assert(sawTogether);
            printf("ok  phase: an aligned pair publishes together, started 50ms apart\n");
        }

        // …and WITHOUT alignment they spread, which is the point: 120 inputs at one rate must not all
        // decode on the same engine frame.
        {
            SensorsConfig many{};
            const int IAT2 = idx_of("iat");
            for (int idx : { CLT, IAT2 }) {
                SensorConfig& sc = many.sensor[idx];
                sc.enabled = 1; sc.interface = IFSEL_ANALOG_VOLTAGE; sc.source = (idx == CLT) ? 0 : 1;
                lin(sc, 5000, 1000);   // 100 ms period
            }
            Sensors sp2; sp2.init(many);
            SignalBus b3;
            g_ain[0] = 2500; g_ain[1] = 2500; g_tick = 2000; pump(sp2, b3);   // both due on frame one
            g_ain[0] = 4000; g_ain[1] = 4000;
            int aFrame = -1, bFrame = -1;
            for (uint32_t t = 2001; t <= 2200; t += 1) {
                g_tick = t; pump(sp2, b3);
                if (aFrame < 0 && approx(b3.get(SIG_CLT), 80.0f)) aFrame = int(t);
                if (bFrame < 0 && approx(b3.get(SIG_IAT), 80.0f)) bFrame = int(t);
            }
            assert(aFrame > 0 && bFrame > 0 && aFrame != bFrame);   // different indices -> different frames
            printf("ok  phase: unaligned sensors of one rate land on different frames (%d vs %d)\n",
                   aFrame, bFrame);
        }

        // …AND IT FOLLOWS THE SELECTOR. Which sensors are cross-checked is whatever a module has been
        // pointed at — throttle A/B can be tps + an aux today and two auxes tomorrow — so the alignment
        // is re-derived from those selectors on every config change rather than fixed at boot. Done the
        // way the ECU does it: point the ETB at the pair, bump the generation, let Sensors re-derive.
        {
            const int AUX1 = idx_of("aux_1");
            const int TPS_ = idx_of("tps");
            SensorsConfig cfg2{};
            // A PERCENT sensor paired with a generic set to percent — the real ETB pairing, and the
            // only way the two share a rate. A sensor's cadence now comes from its TYPE (percent 200 Hz,
            // temperature 5 Hz), so pairing tps with clt as this once did aligns nothing: they cannot
            // refresh on the same frames when one runs forty times slower. That used to be masked by
            // the removed per-sensor `update_hz`, which let a tune force both to one rate.
            for (int idx : { TPS_, AUX1 }) {
                SensorConfig& sc = cfg2.sensor[idx];
                sc.enabled = 1; sc.interface = IFSEL_ANALOG_VOLTAGE; sc.source = (idx == TPS_) ? 0 : 1;
                lin(sc, 5000, 1000);
            }
            cfg2.sensor[AUX1].type = 3;   // percent — a generic input needs a type to build a pipeline
            extern EcuConfig g_config;
            extern volatile uint32_t g_config_generation;
            g_config.electronic_throttle.etb[0].tps_a_src = SIG_TPS;    // "use these two as A/B"
            g_config.electronic_throttle.etb[0].tps_b_src = SIG_AUX_1;
            g_config_generation++;

            Sensors sa; sa.init(cfg2);
            SignalBus b5;
            g_tick = 4000; pump(sa, b5);          // first update re-derives the alignment from g_config

            // Assert the INVARIANT — they refresh on the same frames — rather than predicting which
            // frames those are. A catalogued sensor and a generic one publish different NUMBERS from
            // the same cal (their types scale differently); what has to match is the timing.
            float lastA = b5.get(SIG_TPS), lastB = b5.get(SIG_AUX_1);
            int changes = 0;
            for (uint32_t t = 4001; t <= 4600; t += 1) {
                g_ain[0] = g_ain[1] = static_cast<uint16_t>(2000 + (t % 7) * 300);   // keep both moving
                g_tick = t; pump(sa, b5);
                const float a = b5.get(SIG_TPS), b = b5.get(SIG_AUX_1);
                const bool movedA = (a != lastA), movedB = (b != lastB);
                assert(movedA == movedB);         // never one without the other
                if (movedA) ++changes;
                lastA = a; lastB = b;
            }
            assert(changes > 3);
            printf("ok  phase: pointing the ETB at tps+aux aligns them (%d co-updates)\n", changes);
        }

        // A LATE TICK still runs. The engine task is vTaskDelay(1), not a fixed rate, so milliseconds
        // get skipped under load; a boundary test would drop the whole period instead of catching up.
        {
            SensorsConfig late{};
            SensorConfig& c2 = late.sensor[CLT];
            c2.enabled = 1; c2.interface = IFSEL_ANALOG_VOLTAGE; c2.source = 0;
            lin(c2, 5000, 1000);   // 100 ms
            Sensors sl; sl.init(late);
            SignalBus b4;
            g_ain[0] = 2500; g_tick = 3000; pump(sl, b4);
            g_ain[0] = 4000; g_tick = 3157; pump(sl, b4);   // jumped clean over the 3100 boundary
            assert(approx(b4.get(SIG_CLT), 80.0f));
            printf("ok  phase: a skipped millisecond still runs on the next frame\n");
        }

        // ttl follows the sensor's ACTUAL rate -- derived, not written in: Sensors stamps 3000/hz and
        // hz is the type's cadence. Last publish @1100.
        const uint32_t ttl = ttl_of(CLT);
        bus.expire_stale(1100 + ttl - 100); assert(bus.valid(SIG_CLT));
        bus.expire_stale(1100 + ttl + 100); assert(!bus.valid(SIG_CLT));
        printf("ok  ttl: SIG_CLT expires after its ttl when not refreshed\n");
    }

    // ---- 11. multi-point calibration curve (piecewise-linear interpolation) ----
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
        c.cal_n = SENSOR_CAL_POINTS;   // all points live (resizable curve)
        // 3-point curve: (0mV,0) (1000mV,10) (5000mV,200) — non-linear. Tail collapses onto the last.
        c.cal_raw[0] = 0;    c.cal_val[0] = 0;
        c.cal_raw[1] = 1000; c.cal_val[1] = 100;      // 10.0 degC
        c.cal_raw[2] = 5000; c.cal_val[2] = 2000;     // 200.0 degC
        for (int i = 3; i < SENSOR_CAL_POINTS; i++) { c.cal_raw[i] = 5000; c.cal_val[i] = 2000; }
        Sensors s; s.init(cfg);
        SignalBus bus;

        // One read per SLOT -- two reads inside one slot silently skip the second.
        const uint32_t clt_slot = slot_ms(CLT);
        g_ain[0] = 500;  g_tick = 2 * clt_slot; pump(s, bus);   // seg 0: 0..10 over 0..1000
        assert(approx(bus.get(SIG_CLT), 5.0f));
        g_ain[0] = 3000; g_tick = 3 * clt_slot; pump(s, bus);   // seg 1: 10 + 0.5*(200-10)
        assert(approx(bus.get(SIG_CLT), 105.0f));
        g_ain[0] = 6000; g_tick = 4 * clt_slot; pump(s, bus);   // beyond last -> clamp
        assert(approx(bus.get(SIG_CLT), 200.0f));
        printf("ok  curve: 3-point cal -> 500mV=5, 3000mV=105, 6000mV=200 (clamped)\n");
    }

    // ---- 12. switch stuck detection (debounced state held past the timeout) ----
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[SWP];
        c.enabled = 1; c.interface = IFSEL_DIGITAL; c.source = 0;                       // run every frame
        c.diag_enable   = 0x10;                   // stuck
        c.diag_stuck_ms = 100;
        c.diag_severity = (uint16_t)(2u << 8);    // slot 4 (stuck) severity 2
        Sensors s; s.init(cfg);
        SignalBus bus;
        g_din[0] = true;
        g_tick = 1000;
        // `switch` samples at 50 Hz, so ONE pipeline run per 20 ms slot — step a full slot per frame
        // or the debounce counts runs that never happened.
        const uint32_t sw_slot2 = slot_ms(SWP);          // one run per slot
        for (int f = 0; f < 4; f++) { g_tick += sw_slot2; pump(s, bus); }  // debounce -> latch
        assert(s.healthy(SWP));                   // just latched, not stuck yet
        g_tick += 150; pump(s, bus); // held same state > 100ms → stuck
        assert(!s.healthy(SWP));
        assert(s.worst_severity() == 2);
        printf("ok  stuck: switch held > %ums -> faulted (sev=%u)\n", c.diag_stuck_ms, s.worst_severity());

        // a transition resets the timer → no longer stuck
        g_din[0] = false;
        for (int f = 0; f < 4; f++) { g_tick += sw_slot2; pump(s, bus); }  // toggles off
        assert(s.healthy(SWP));
        printf("ok  stuck: transition clears the stuck fault\n");
    }

    // ---- 13. operating-window DTC arming via the precondition EXPRESSION ----
    // The gate is a compiled bytecode program on the sensor itself (firmware/Signal/Expr.h);
    // Sensors runs it each frame to arm/disarm the operating-range checks. The four flat
    // {signal,op,combine,value} slots are gone — they could not group terms.
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
        lin(c, 5000, 1000);   // every frame, no decimation
        c.diag_enable    = 0x08;      // op_max
        c.diag_op_max    = 500;       // 50.0 degC ceiling
        c.diag_severity  = 0xC0;      // op_max severity 3
        g_ain[0] = 4000;              // -> 80 degC, over the 50 ceiling

        // (a) no preconditions (all combine Off — the zero-init default) -> always armed -> op_max trips.
        Sensors s; s.init(cfg);
        SignalBus bus; g_tick = 2000; pump(s, bus);
        assert(!s.healthy(CLT));      // armed -> out-of-range trips the DTC (value still publishes valid)
        printf("ok  precond: all Off = always armed -> op_max trips\n");

        // (b) gated on "map >= 50": disarmed at low load, armed at high load.
        // Bytecode as the studio compiler emits it: PUSH_SIG map, PUSH_F 5000, GE, END.
        {
            uint8_t* q = c.precond_expr;
            *q++ = expr::OP_PUSH_SIG;
            const uint16_t sel = (uint16_t)(SIG_MAP + 1);      // options_from:signals = SignalId+1
            memcpy(q, &sel, 2); q += 2;
            *q++ = expr::OP_PUSH_F;
            const int32_t v = 5000;                            // 50.00, stored x100
            memcpy(q, &v, 4); q += 4;
            *q++ = expr::OP_GE;
            *q++ = expr::OP_END;
            assert(expr::validate(c.precond_expr, sizeof(c.precond_expr),
                                  sizeof(EcuConfig), 0) == expr::Invalid::None);
        }
        Sensors s2; s2.init(cfg);
        SignalBus bus2;
        bus2.set(SIG_MAP, 20.0f, true, 2200, 1000);
        g_tick = 2200; pump(s2, bus2);    // MAP 20 < 50 -> disarmed -> no op DTC
        assert(s2.healthy(CLT));
        bus2.set(SIG_MAP, 80.0f, true, 2400, 1000);
        g_tick = 2400; pump(s2, bus2);    // MAP 80 >= 50 -> armed -> op_max trips
        assert(!s2.healthy(CLT));
        printf("ok  precond: op_max gated by expression `map >= 50`\n");

        // (c) PRECEDENCE — the thing the four flat slots could not express:
        //     map >= 50 AND (tps > 80 OR rpm > 4000)
        // With MAP high but neither TPS nor RPM over their thresholds the gate is SHUT, which a
        // sum-of-products encoding of the same three tests could not represent.
        {
            uint8_t* q = c.precond_expr;
            memset(c.precond_expr, 0, sizeof(c.precond_expr));
            auto sig = [&](SignalId id) {
                *q++ = expr::OP_PUSH_SIG; const uint16_t v = (uint16_t)(id + 1);
                memcpy(q, &v, 2); q += 2;
            };
            auto konst = [&](int32_t x100) {
                *q++ = expr::OP_PUSH_F; memcpy(q, &x100, 4); q += 4;
            };
            sig(SIG_MAP); konst(5000);  *q++ = expr::OP_GE;
            sig(SIG_TPS); konst(8000);  *q++ = expr::OP_GT;
            sig(SIG_RPM); konst(400000); *q++ = expr::OP_GT;
            *q++ = expr::OP_OR; *q++ = expr::OP_AND; *q++ = expr::OP_END;
            assert(expr::validate(c.precond_expr, sizeof(c.precond_expr),
                                  sizeof(EcuConfig), 0) == expr::Invalid::None);
        }
        Sensors s3; s3.init(cfg);
        SignalBus bus3;
        bus3.set(SIG_MAP, 80.0f, true, 2600, 1000);
        bus3.set(SIG_TPS, 10.0f, true, 2600, 1000);
        bus3.set(SIG_RPM, 1000.0f, true, 2600, 1000);
        g_tick = 2600; pump(s3, bus3);    // MAP high, but the OR term is false -> disarmed
        assert(s3.healthy(CLT));
        bus3.set(SIG_RPM, 5000.0f, true, 2800, 1000);
        g_tick = 2800; pump(s3, bus3);    // RPM carries the OR -> armed -> op_max trips
        assert(!s3.healthy(CLT));
        printf("ok  precond: grouping honoured — map >= 50 AND (tps > 80 OR rpm > 4000)\n");

        // (d) an INVALID program fails ARMED and raises its own per-sensor DTC. Detection that
        //     silently switches itself off because a gate is broken is the worst outcome.
        {
            memset(c.precond_expr, 0, sizeof(c.precond_expr));
            c.precond_expr[0] = expr::OP_AND;      // binary op with an empty stack
            c.precond_expr[1] = expr::OP_END;
            assert(expr::validate(c.precond_expr, sizeof(c.precond_expr),
                                  sizeof(EcuConfig), 0) != expr::Invalid::None);
        }
        DtcManager dtcs; dtcs.init(1);
        Sensors s4; s4.init(cfg); s4.set_dtc(&dtcs);
        SignalBus bus4;
        g_tick = 3000; pump(s4, bus4);
        assert(!s4.healthy(CLT));              // armed despite the broken gate
        assert(dtcs.code_severity(SENSOR_CATALOG[CLT].dtc_precond) > 0);  // and it says WHICH sensor
        printf("ok  precond: a broken expression fails ARMED and raises P%04X\n",
               SENSOR_CATALOG[CLT].dtc_precond);
    }

    // ---- 14. frequency max-derivative (rate-of-change) plausibility ----
    {
        const int TRPM = idx_of("trans_input_rpm");
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[TRPM];
        c.enabled = 1; c.interface = IFSEL_DIGITAL_FREQ; c.source = 0;                       // every frame
        lin(c, 1000, 1000);                       // 0..1000 Hz -> 0..1000 units (1:1)
        c.diag_enable    = 0x20;                  // max_deriv
        c.diag_max_deriv = 100;                   // 100 units/sec limit
        c.diag_severity  = (uint16_t)(2u << 10);  // slot 5 severity 2
        Sensors s; s.init(cfg);
        SignalBus bus;

        g_freq[0] = 50; g_tick = 3000; pump(s, bus);   // seed prev_val
        g_freq[0] = 55; g_tick = 3100; pump(s, bus);   // (55-50)/0.1 = 50/s < 100 -> ok
        assert(s.healthy(TRPM));
        g_freq[0] = 90; g_tick = 3200; pump(s, bus);   // (90-55)/0.1 = 350/s > 100 -> trip
        assert(!s.healthy(TRPM));
        assert(s.worst_severity() == 2);
        printf("ok  max-deriv: 350 units/s > 100 limit -> faulted (sev=%u)\n", s.worst_severity());
    }

    // ---- 15. composition (flex): ONE input -> ethanol% (frequency) + fuel temp (pulse width) ----
    {
        const int FLEX = idx_of("flex_fuel");
        auto base = [&](SensorConfig& c) {
            c.enabled = 1; c.interface = IFSEL_DIGITAL_FREQ; c.source = 0;
            lin(c, 100, 1000);              // 0..100 Hz -> 0..100% ethanol (val_scale 0.1)
        };
        g_freq[0] = 50;                     // 50 Hz -> 50% ethanol throughout

        // (a) unfiltered: both outputs publish off one input. 3.000 ms -> 42.5 C (1ms=-40, 5ms=125).
        {
            SensorsConfig cfg{}; base(cfg.sensor[FLEX]);
            g_pulse[0] = 3000;
            Sensors s; s.init(cfg); SignalBus bus;
            g_tick = 5000; pump(s, bus);
            assert(approx(bus.get(SIG_ETHANOL), 50.0f));          // primary: frequency -> ethanol%
            assert(approx(bus.get(SIG_FUEL_TEMP_FLEX), 42.5f));   // side output: pulse width -> temp
        }
        // (b) the side output tracks its input with NO lag — the per-sensor EMA is gone, so a step
        // lands where it lands. This used to assert half a step of filter lag.
        {
            SensorsConfig cfg{}; base(cfg.sensor[FLEX]);
            Sensors s; s.init(cfg); SignalBus bus;
            g_pulse[0] = 3000; g_tick = 6000; pump(s, bus);
            assert(approx(bus.get(SIG_FUEL_TEMP_FLEX), 42.5f));
            g_pulse[0] = 5000; g_tick = 6400; pump(s, bus);   // step to 125 C
            assert(approx(bus.get(SIG_FUEL_TEMP_FLEX), 125.0f));           // arrives immediately
        }
        // (c) out-of-range diagnostic: 6.000 ms -> 166 C, past the valid band. The temp pipeline's
        // cond_op_window KEEPS the publish valid (the reading is real, just unhealthy) AND raises the
        // output's OWN high P-code (P0183) via the standard DTC path; ethanol stays valid; heals on recovery.
        {
            SensorsConfig cfg{}; base(cfg.sensor[FLEX]);
            DtcManager dtc; dtc.init(1);
            Sensors s; s.init(cfg); s.set_dtc(&dtc); SignalBus bus;
            const uint16_t aux_hi = SENSOR_AUX_OUTPUTS[SENSOR_CATALOG[FLEX].aux_off].dtc_op_max;
            g_pulse[0] = 6000; g_tick = 7000; pump(s, bus);
            assert(bus.valid(SIG_FUEL_TEMP_FLEX));               // op-range keeps it valid (DTC carries the fault)
            assert(bus.valid(SIG_ETHANOL));                       // primary unaffected
            assert(dtc.active_count() == 1);
            uint16_t codes[4]; dtc.list_active(codes, 4);
            assert(codes[0] == aux_hi && aux_hi == 0x0183);       // fuel-temp circuit high (P0183)
            g_pulse[0] = 3000; g_tick = 7400; pump(s, bus);  // back in range
            assert(dtc.active_count() == 0);                      // healed
        }
        printf("ok  composition: flex ethanol+temp; temp pipeline EMA + op-window + P0183 DTC\n");
    }

    // ---- 16. input pin-conflict detection (two sensors claiming one physical pin) ----
    {
        const int IAT = idx_of("iat");
        const int VSP = idx_of("driveshaft_speed");   // a frequency input; renamed with the VSS pickups
        const int TRPM = idx_of("trans_input_rpm");

        // two analog sensors on the same AV pin -> conflict
        SensorsConfig a{};
        a.sensor[CLT].enabled = 1; a.sensor[CLT].interface = IFSEL_ANALOG_VOLTAGE; a.sensor[CLT].source = 0;
        a.sensor[IAT].enabled = 1; a.sensor[IAT].interface = IFSEL_ANALOG_VOLTAGE; a.sensor[IAT].source = 0;
        { Sensors s; s.init(a); assert(s.input_conflict()); }
        a.sensor[IAT].source = 1;                              // distinct pin -> clear
        { Sensors s; s.init(a); assert(!s.input_conflict()); }

        // analog AV0 and digital DIG0 share an index but are different pools -> no conflict
        SensorsConfig b{};
        b.sensor[CLT].enabled = 1; b.sensor[CLT].interface = IFSEL_ANALOG_VOLTAGE; b.sensor[CLT].source = 0;
        b.sensor[VSP].enabled = 1; b.sensor[VSP].interface = IFSEL_DIGITAL_FREQ;    b.sensor[VSP].source = 0;
        { Sensors s; s.init(b); assert(!s.input_conflict()); }

        // two digital sensors in the SAME mode on the same DIG pin -> conflict (same SIG_HW channel)
        b.sensor[TRPM].enabled = 1; b.sensor[TRPM].interface = IFSEL_DIGITAL_FREQ; b.sensor[TRPM].source = 0;
        { Sensors s; s.init(b); assert(s.input_conflict()); }

        // ...but DIFFERENT modes on one pin are different (pin,mode) channels -> they COEXIST (the
        // consumption-keyed arbitration; the old pin-pool model wrongly rejected this).
        SensorsConfig e{};
        e.sensor[VSP].enabled = 1; e.sensor[VSP].interface = IFSEL_DIGITAL_FREQ; e.sensor[VSP].source = 0;
        e.sensor[SWP].enabled = 1; e.sensor[SWP].interface = IFSEL_DIGITAL;       e.sensor[SWP].source = 0;
        { Sensors s; s.init(e); assert(!s.input_conflict()); }
        printf("ok  conflict: same-channel rejected, freq+switch on one pin coexist\n");
    }

    // ---- 16b. config error: enabled sensor, pin interface, NO pin assigned -> dtc_config ----
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = -1;    // -1 = SOURCE_NONE (unassigned)
        DtcManager dtc; dtc.init(1);
        Sensors s; s.init(cfg); s.set_dtc(&dtc);
        SignalBus bus;
        g_tick = 500; pump(s, bus);
        assert(dtc.active_count() == 1);                            // only the config DTC (pipeline is empty)
        uint16_t codes[4]; dtc.list_active(codes, 4);
        assert(codes[0] == SENSOR_CATALOG[CLT].dtc_config);         // CLT's auto-allocated P18xx code
        // Disabling the sensor clears the config error (it's no longer "enabled but unassigned").
        c.enabled = 0; g_config_generation++;
        g_tick = 600; pump(s, bus);
        assert(dtc.active_count() == 0);                            // healed
        printf("ok  config: clt enabled+unassigned -> dtc_config 0x%04X, heals\n",
               SENSOR_CATALOG[CLT].dtc_config);
    }

    // ---- 16c. config error: an interface this sensor is NOT BUILT to be read through ----
    // Only TWO interfaces are actually impossible, and the mask says which. CAN binds a device signal
    // to the sensor's channel without asking what kind of sensor it is, and analog / engine-sync /
    // frequency / pulse / SENT all decode a raw scalar through the cal curve — so which of those a car
    // uses is a fact about its wiring, not something the ECU can refuse. `digital` is different: the
    // pin level IS the value, with no decode, so a TEMPERATURE read that way publishes 0 or 1 degrees.
    {
        assert(!(SENSOR_CATALOG[CLT].interface_mask & IFACE_DIGITAL));       // no decode -> 0/1 degrees
        assert(!(SENSOR_CATALOG[CLT].interface_mask & IFACE_ON_BOARD));      // no on-board coolant probe
        assert(SENSOR_CATALOG[CLT].interface_mask & IFACE_CAN_DEVICE);       // ...but a CAN clt is fine
        assert(SENSOR_CATALOG[CLT].interface_mask & IFACE_SENT);             // ...and so is a SENT one
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_DIGITAL; c.source = 0;            // a PIN is assigned: not 16b
        DtcManager dtc; dtc.init(1);
        Sensors s; s.init(cfg); s.set_dtc(&dtc);
        SignalBus bus;
        g_tick = 500; pump(s, bus);
        uint16_t codes[4]; dtc.list_active(codes, 4);
        assert(dtc.active_count() == 1 && codes[0] == SENSOR_CATALOG[CLT].dtc_config);
        // Move it to an interface the firmware CAN build and the code heals — the same edge as
        // assigning a missing pin, because it is the same class of fault.
        c.interface = IFSEL_SENT; g_config_generation++;
        g_tick = 600; pump(s, bus);
        assert(dtc.active_count() == 0);
        printf("ok  config: clt as a digital level -> dtc_config 0x%04X, heals on SENT\n",
               SENSOR_CATALOG[CLT].dtc_config);
    }

    // ---- 16d. a SWITCH ON CAN raises nothing. The catalogue calls a coolant-flow switch
    // [digital, analog_voltage] because that is what it usually IS, not what it may be — and read as
    // a permission list that refused a CAN coolant switch, which is a thing you can buy.
    {
        const int CFS = idx_of("coolant_flow_sw");
        assert(SENSOR_CATALOG[CFS].interface_mask & IFACE_CAN_DEVICE);
        assert(SENSOR_CATALOG[CFS].interface_mask & IFACE_SENT);
        assert(SENSOR_CATALOG[CFS].interface_mask & IFACE_DIGITAL);          // still its natural read
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CFS];
        c.enabled = 1; c.interface = IFSEL_CAN_DEVICE; c.source = 0;
        // NAMES NO FIELD, which is what the shipped default writes. A value-initialised SensorsConfig
        // leaves can_bit at 0 — a real start bit — and that is a sensor pointed at a frame the tune
        // does not have, which is its own fault and is checked in 16e.
        c.can_bit = -1;
        DtcManager dtc; dtc.init(1);
        Sensors s; s.init(cfg); s.set_dtc(&dtc);
        SignalBus bus;
        g_tick = 500; pump(s, bus);
        assert(dtc.active_count() == 0);
        printf("ok  config: coolant_flow_sw on CAN raises nothing — every sensor can come off the bus\n");
    }

    // ---- 16e. A CAN SENSOR NAMES ITS FIELD BY FRAME AND START BIT, and a reference that no longer
    // resolves is a CODE, not silence. The pool index this replaced was not durable — the studio
    // repacks the whole field pool on any structural edit — so a sensor could end up decoding another
    // frame's bits into a plausible reading with nothing to say it had moved.
    {
        const int CFS = idx_of("coolant_flow_sw");
        CanConfig can{};
        for (auto& b : can.bus) b.enabled = 1;
        can.gc_frame[0].flags       = canmsg::FRAME_USED;          // receive
        can.gc_frame[0].bus         = 0;
        can.gc_frame[0].id          = 0x4A0;
        can.gc_frame[0].dlc         = 8;
        can.gc_frame[0].first_field = 0;
        can.gc_frame[0].field_count = 1;
        can.gc_field[0].sig     = SIG_NONE;                        // channel-less: a sensor reads it
        can.gc_field[0].bit_off = 23;
        can.gc_field[0].width   = 8;
        can.gc_field[0].scale   = 1.0f;
        can.gc_field[0].ttl_ms  = 500;
        GenericCan gcan; gcan.configure(can, 0);

        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CFS];
        c.enabled = 1; c.interface = IFSEL_CAN_DEVICE;
        c.can_frame = GenericCan::frame_key(0, false, 0x4A0);
        c.can_bit   = 23;
        DtcManager dtc; dtc.init(1);
        Sensors s; s.set_generic_can(&gcan); s.init(cfg); s.set_dtc(&dtc);
        SignalBus bus;
        g_tick = 500; pump(s, bus);
        assert(dtc.active_count() == 0);                           // it resolves: nothing to report

        // THE FIELD MOVES TO OTHER BITS — the edit that used to be silent. The frame is still there
        // and still has exactly one field, so a stored pool index would have gone on reading it.
        can.gc_field[0].bit_off = 31;
        gcan.configure(can, 0);
        g_config_generation++;
        g_tick = 600; pump(s, bus);
        uint16_t codes[4]; dtc.list_active(codes, 4);
        assert(dtc.active_count() == 1 && codes[0] == SENSOR_CATALOG[CFS].dtc_config);

        // Point it at where the field went and the code heals.
        c.can_bit = 31; g_config_generation++;
        g_tick = 700; pump(s, bus);
        assert(dtc.active_count() == 0);
        printf("ok  config: a CAN sensor whose frame/start bit no longer resolves -> dtc_config 0x%04X\n",
               SENSOR_CATALOG[CFS].dtc_config);
    }

    // ---- 17. battery is a regular analog sensor (AV12), enable-gated — NOT board-managed ----
    {
        const int BAT2 = idx_of("battery");
        assert(SENSOR_CATALOG[BAT2].locked_interface == IFACE_ANALOG_VOLTAGE);  // moved off on-board
        SensorsConfig cfg{};                          // fresh config: every sensor enabled=0
        // enabled stays 0 -> unlike the old board-managed battery, it does NOT publish
        Sensors s; s.init(cfg);
        SignalBus bus;
        g_ain[11] = 2232; g_tick = 5000;
        pump(s, bus);
        assert(!bus.valid(SIG_BATTERY));              // enable-gated now (default tune ships enabled=1)
        printf("ok  battery: analog AV12, enable-gated (no longer board-managed)\n");
    }

    // ---- 17b. system-active (key-on) gate: USB/bench runs ONLY the bootstrap battery ----
    {
        SensorsConfig cfg{};
        SensorConfig& b = cfg.sensor[BAT]; b.enabled = 1; b.interface = IFSEL_ANALOG_VOLTAGE;
        b.source = 11; lin(b, 5000, 3025);
        SensorConfig& c = cfg.sensor[CLT]; c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE;
        c.source = 0; lin(c, 5000, 1000);
        Sensors s; s.init(cfg); SignalBus bus;
        g_ain[11] = 2232; g_ain[0] = 2500; g_tick = 8000;

        // Drive the KEY ITSELF -- the battery -- rather than poking the gate, because the battery is
        // what decides it now. 5000 mV maps to 30.25 V here, so 800 mV = 4.8 V (key off, below the
        // 7 V drop-out) and 2232 mV = 13.5 V (key on, above the 8 V pick-up).
        g_key_override = 0;                       // follow the battery, not the bench override
        g_ain[11] = 800;                          // 4.8 V: key off
        pump(s, bus);
        assert(bus.valid(SIG_BATTERY));           // bootstrap battery still reads (sources key_on)
        assert(!bus.valid(SIG_CLT));              // every other sensor stays quiet
        // 0 IN AS WELL AS 0 OUT. HardwareInput publishes every raw pin a phase earlier, so gating the
        // sensor alone left the RAW channel still carrying counts — and the studio's sensor page shows
        // that raw channel beside the calibrated one. With the key off those counts are measured against
        // a reference the USB-fed followers shift (a diode drop), so they are plausible and wrong, which
        // is the whole reason the gate exists. Sensors retracts them in the pass it finds the key off.
        assert(!bus.valid(SIG_HW_AV1));           // the gated pin reads nothing, not a wrong number
        assert(!bus.valid(SIG_HW_DIG1_LEVEL));    // digital reads through the same unpowered front end
        assert(!bus.valid(SIG_HW_DIG1_FREQ));
        assert(bus.valid(SIG_HW_AV12));           // ...and the battery's own pin still does

        // THE REGRESSION: the pass in which the key turns is the pass in which the rest publish. The
        // key used to be decided a phase LATER, in EngineTask, so this very pass ran with the gate
        // still false and skipped CLT -- and any module reading it that frame found nothing there and
        // raised "signal missing" for exactly one frame, on every key-on and every boot.
        // Advance past the battery's own 20 Hz period, or it is decimated out of this pass and the key
        // would be decided from the stale 4.8 V still on the bus. The key can only be recognised on a
        // frame the battery actually runs -- up to its period late, which is what a 20 Hz key costs.
        g_tick += 100;
        g_ain[11] = 2232;                         // 13.5 V: key on
        pump(s, bus);                             // ONE pump: the key turns AND the rest publish
        assert(bus.valid(SIG_CLT));               // alive in the SAME pass the key turned
        assert(bus.valid(SIG_HW_AV1));            // ...and its raw pin is back with it, same pass
        g_key_override = 1;                       // restore for anything after this section
        printf("ok  system-active gate: bench=battery-only, key-on=all sensors\n");
    }

    // ---- 18. sensor diagnostics raise/heal into the unified DTC table ----
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
        lin(c, 5000, 1000);  // every frame, no decimation
        c.diag_enable = 0x01;                 // raw_min (short-to-GND)
        c.diag_raw_min = 500;
        c.diag_severity = 0x02;               // raw_min severity = 2 (slot 0)
        DtcManager dtc; dtc.init(1);
        Sensors s; s.init(cfg); s.set_dtc(&dtc);
        SignalBus bus;
        const uint16_t code = SENSOR_CATALOG[CLT].dtc_raw_min;   // clt raw_min P-code (P0117 -> 0x0117)

        const uint32_t dtc_slot = slot_ms(CLT);      // one read per slot
        g_ain[0] = 200; g_tick = dtc_slot; pump(s, bus);  // 200mV < 500 -> fault
        assert(dtc.active_count() == 1);                          // fault raised a code
        assert(dtc.source_severity(CLT) == 2);                    // carries sensor index + severity
        uint16_t codes[4]; dtc.list_active(codes, 4);
        assert(codes[0] == code && code != 0);                   // the check's catalog P-code

        g_ain[0] = 2500; g_tick = 2 * dtc_slot; pump(s, bus); // back in range -> heal
        assert(dtc.active_count() == 0 && dtc.stored_count() == 1);  // healed, history kept
        printf("ok  dtc: sensor raises P-code 0x%04X, heals on recovery\n", code);
    }

    // ---- pin arbitration: two analog sensors on the same source -> one wins, one rejected ----
    {
        const int IAT = idx_of("iat");
        const int lo = CLT < IAT ? CLT : IAT, hi = CLT < IAT ? IAT : CLT;   // lower index wins
        SensorsConfig cfg{};
        for (int k : {CLT, IAT}) {
            SensorConfig& c = cfg.sensor[k];
            c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 5;
            lin(c, 5000, 1000);
        }
        g_ain[5] = 2500;

        Sensors s; s.init(cfg);
        SignalBus bus; g_tick = 10;
        assert(s.input_conflict());                      // clash detected (would set P1650)
        pump(s, bus);
        const SignalId lo_ch = static_cast<SignalId>(SENSOR_CATALOG[lo].primary_channel);
        const SignalId hi_ch = static_cast<SignalId>(SENSOR_CATALOG[hi].primary_channel);
        assert(bus.valid(lo_ch));                        // winner reads + publishes
        assert(!bus.valid(hi_ch));                       // loser rejected -> no double-read
        printf("ok  pin arbitration: AV5 shared -> sensor %d wins, %d rejected, conflict flagged\n", lo, hi);

        // Resolve the clash live (move IAT to AV6) + bump the generation -> re-derive, no re-init.
        cfg.sensor[IAT].source = 6; g_ain[6] = 2500;
        g_config_generation++;
        g_tick = 20; pump(s, bus);
        assert(!s.input_conflict());                     // recomputed live: clash gone
        assert(bus.valid(SIG_CLT) && bus.valid(SIG_IAT));// both now read their own pins
        printf("ok  pin arbitration: live re-derive on config change clears the conflict\n");
    }

    // ---- 19. LIVE reconfigure: swap a sensor's interface analog -> frequency, no re-init ----
    // The doc's headline case. A config-generation bump re-derives the per-input pipeline, so
    // the Acquire stage itself changes (different interface + pin) under the running manager —
    // no reflash, no Sensors::init(). (recompute_live runs in EngineTask, same thread as the
    // pipeline run, so the rebuild never races the executor — no lock-free flip needed here.)
    {
        const int M = idx_of("maf");                  // catalog allows analog + frequency
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[M];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 2;
        lin(c, 5000, 5000);                           // linear raw -> value (1:1 before scale)
        g_ain[2]  = 1000;                             // analog pin reads 1000
        g_freq[4] = 3000;                             // a different freq pin reads 3000

        Sensors s; s.init(cfg);
        SignalBus bus; g_tick = 10;
        pump(s, bus);
        const SignalId ch = static_cast<SignalId>(SENSOR_CATALOG[M].primary_channel);
        assert(bus.valid(ch));
        const float analog_val = bus.get(ch);         // reflects the analog source (raw 1000)
        assert(analog_val > 0.0f);

        c.interface = IFSEL_DIGITAL_FREQ; c.source = 4;  // swap interface + pin, LIVE
        g_config_generation++;
        g_tick = 20; pump(s, bus);
        const float freq_val = bus.get(ch);           // now reflects the freq source (raw 3000)
        assert(bus.valid(ch));
        // same linear curve, raw 3000 vs 1000 -> 3x (scale-independent). Proves the Acquire swapped
        // (had it stayed analog it would read g_ain[4]=0).
        assert(approx(freq_val, analog_val * 3.0f));
        printf("ok  live reconfigure: analog->frequency interface swap rebuilds the pipeline\n");
    }

    // ---- 20. 5 V sensor references: EITHER power-good low -> EVERY pin sensor invalid, P0641 / P0651 ----
    // Not only the sensors on the failed rail: the ECU is not told which reference feeds which sensor.
    // The battery (its own divider, and the key comes from it) keeps publishing. The sensors' own
    // diagnostics stand down — CLT is sitting at 0 V below its raw-min here and must NOT raise P0117;
    // the supply's code is the one fault. That code waits 100 ms; the invalidation does not.
    {
        SensorsConfig cfg{};
        SensorConfig& c = cfg.sensor[CLT];
        c.enabled = 1; c.interface = IFSEL_ANALOG_VOLTAGE; c.source = 0;
        lin(c, 5000, 1000);
        c.diag_enable = 0x01; c.diag_raw_min = 500;        // a rail at 0 V would trip this
        SensorConfig& b = cfg.sensor[BAT];
        b.enabled = 1; b.interface = IFSEL_ANALOG_VOLTAGE; b.source = 11;
        lin(b, 5000, 3025);
        DtcManager dtc; dtc.init(1);
        Sensors s; s.init(cfg); s.set_dtc(&dtc);
        SignalBus bus;
        g_ain[0] = 2500; g_ain[11] = 2232; g_pg[0] = g_pg[1] = true;

        g_tick = 200; pump(s, bus);
        assert(bus.valid(SIG_CLT) && bus.valid(SIG_BATTERY) && !s.supply_lost());
        assert(bus.valid(SIG_SENSOR_SUPPLY_1) && bus.get(SIG_SENSOR_SUPPLY_1) == 1.0f);
        assert(bus.valid(SIG_SENSOR_SUPPLY_2) && bus.get(SIG_SENSOR_SUPPLY_2) == 1.0f);

        g_pg[1] = false; g_ain[0] = 0;                     // reference 2 folds back; the rail reads 0 V
        g_tick = 400; pump(s, bus);
        assert(s.supply_lost());
        assert(!bus.valid(SIG_CLT));                       // invalid on the FIRST low sample
        assert(bus.valid(SIG_BATTERY));                    // …the battery is not on the followers
        assert(bus.get(SIG_SENSOR_SUPPLY_2) == 0.0f && bus.get(SIG_SENSOR_SUPPLY_1) == 1.0f);
        g_tick = 450; pump(s, bus);
        assert(dtc.active_count() == 0);                   // 50 ms: not a stored fault yet

        g_tick = 600; pump(s, bus);                        // 200 ms low
        assert(dtc.active_count() == 1);                   // P0651 — and NOT CLT's raw-min
        uint16_t codes[4]; dtc.list_active(codes, 4);
        assert(codes[0] == 0x0651);
        assert(dtc.source_severity(DtcSource::SUPPLY) == DTC_SEV_LEVEL2);
        assert(!bus.valid(SIG_CLT));

        g_pg[1] = true; g_ain[0] = 2500;                   // reference back
        g_tick = 800; pump(s, bus);
        assert(!s.supply_lost() && bus.valid(SIG_CLT));
        assert(dtc.active_count() == 0);                   // healed

        g_pg[0] = false;                                   // …and reference 1 is P0641
        g_tick = 1000; pump(s, bus);
        g_tick = 1200; pump(s, bus);
        dtc.list_active(codes, 4);
        assert(dtc.active_count() == 1 && codes[0] == 0x0641);
        g_pg[0] = true;

        // The 'pg' bench override stands in for the pin both ways: FAULT over a good line, OK over a bad one.
        g_pg_override[1] = -1;
        g_tick = 1400; pump(s, bus);
        assert(s.supply_lost() && !bus.valid(SIG_CLT) && bus.get(SIG_SENSOR_SUPPLY_2) == 0.0f);
        g_pg_override[1] = 0; g_pg[1] = false; g_pg_override[1] = 1;
        g_tick = 1600; pump(s, bus);
        assert(!s.supply_lost() && bus.get(SIG_SENSOR_SUPPLY_2) == 1.0f);
        g_pg_override[1] = 0; g_pg[1] = true;
        printf("ok  5 V references: either PG low -> pin sensors invalid (battery kept), P0651/P0641 after 100 ms\n");
    }

    // ---- 21. A write ELSEWHERE in the tune leaves every input's state alone; a write to an input
    // rebuilds THAT input and no other. Rebuilding a pipeline zeroes its stage state, and every write
    // used to rebuild all of them: a held switch read "off" until it re-debounced and a multi-position
    // switch went invalid until it re-settled, because somebody edited a VE cell. ----
    {
        const int AUX1 = idx_of("aux_1");
        SensorsConfig cfg{};
        SensorConfig& sw = cfg.sensor[SWP];                  // a held digital switch
        sw.enabled = 1; sw.interface = IFSEL_DIGITAL; sw.source = 0;
        SensorConfig& ms = cfg.sensor[AUX1];                 // a multi-position switch on AV3
        ms.type = SENSOR_TYPE_MULTI_SWITCH; ms.interface = IFSEL_ANALOG_VOLTAGE; ms.source = 2;
        ms.cal_n = 4;
        ms.cal_raw[0] = 1000; ms.cal_raw[1] = 1400; ms.cal_val[0] = ms.cal_val[1] = 3;
        ms.cal_raw[2] = 3000; ms.cal_raw[3] = 3400; ms.cal_val[2] = ms.cal_val[3] = 0;   // rest
        ms.enabled = 1;
        Sensors s; s.init(cfg);
        SignalBus bus;
        const SignalId SW = (SignalId)SENSOR_CATALOG[SWP].primary_channel;
        const uint32_t step = 1000u / SENSOR_TYPE_CATALOG[SENSOR_TYPE_MULTI_SWITCH].update_hz;   // 20 ms
        g_din[0] = true; g_ain[2] = 3200;
        for (int f = 0; f < 6; f++) { g_tick += step; pump(s, bus); }
        assert(bus.get(SW) > 0.5f);                          // debounced on
        assert(bus.valid(SIG_AUX_1) && bus.get(SIG_AUX_1) == 0.0f);   // settled at rest

        g_config_generation++;                               // a write to some OTHER part of the tune
        g_tick += step; pump(s, bus);
        assert(bus.get(SW) > 0.5f);                          // still on — its debounce was not reset
        assert(bus.valid(SIG_AUX_1));                        // still settled — not re-settling

        ms.diag_stuck_ms = 1234;                             // a write to the multi-switch's OWN settings
        g_config_generation++;
        g_tick += step; pump(s, bus);
        assert(!bus.valid(SIG_AUX_1));                       // rebuilt: settling afresh…
        assert(bus.get(SW) > 0.5f);                          // …and the switch beside it untouched
        for (int f = 0; f < 3; f++) { g_tick += step; pump(s, bus); }
        assert(bus.valid(SIG_AUX_1) && bus.get(SIG_AUX_1) == 0.0f);

        sw.enabled = 0; g_config_generation++;               // switching an input OFF still tears it down
        g_tick += step; pump(s, bus);
        g_tick += 200; pump(s, bus);
        bus.expire_stale(g_tick);                            // the engine task's stale sweep: past its ttl
        assert(!bus.valid(SW));
        assert(bus.valid(SIG_AUX_1));                        // and still leaves the others alone
        g_din[0] = false; g_ain[2] = 0;
        printf("ok  a write elsewhere leaves inputs' state alone; a write to one input rebuilds only it\n");
    }

    printf("\nAll Sensors runtime tests passed.\n");
    return 0;
}
