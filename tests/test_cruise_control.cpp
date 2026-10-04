// Host test for CruiseControl — the state machine, the conditions that drive it, tap versus hold,
// and what a Cancel keeps that a Disable does not.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/CruiseControl.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../firmware/Signal/ExprIsa.h"
#include "../generated/signal_ids.h"
#include "../generated/sensors_catalog.h"
#include "../generated/module_dtc.h"
#include "../generated/ecu_config.h"
#include <cstring>
#include <vector>

static uint32_t g_ms = 1000;
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }
bool g_system_active = true;   // key-on (Sensors owns it on target); key-off is cruise off

// Defined in CommsManager on the target. The module watches it to know the tune changed under it and
// its button programs want re-validating, so a test has to supply it.
volatile uint32_t g_config_generation = 0;
// The written byte range, which on target the comms layer records (see CommsManager). A host test
// writes g_config directly, so it records none — and Sensors then falls back to the full sweep, which
// is exactly the behaviour an unknown range is supposed to get.
volatile uint32_t g_config_dirty_lo = 0xFFFFFFFFu;
volatile uint32_t g_config_dirty_hi = 0;

// The buttons are EXPRESSIONS, so a test hands the module bytecode. `cruise_sw == N` is what the studio
// compiles a stalk position to, and it is what the real car will carry.
static void eq_prog(uint8_t* dst, size_t cap, SignalId s, int8_t n) {
    const uint16_t sel = (uint16_t)(s + 1);        // options_from:signals — 0 is None, else id+1
    const uint8_t prog[] = { (uint8_t)expr::OP_PUSH_SIG, (uint8_t)(sel & 0xFF), (uint8_t)(sel >> 8),
                             (uint8_t)expr::OP_PUSH_I8, (uint8_t)n,
                             (uint8_t)expr::OP_EQ, (uint8_t)expr::OP_END };
    memset(dst, 0, cap);
    memcpy(dst, prog, sizeof prog);
}

// A four-position stalk. Position 2 is deliberately wired to BOTH Set and Speed Down, and position 3 to
// BOTH Resume and Speed Up — that is what a "Set / Coast / -" button IS, and it is the claim the
// expression model rests on, so it is what the test drives.
static CruiseControlConfig make_cfg() {
    CruiseControlConfig c{};
    c.enabled = 1; c.power_on_state = 0;                    // starts Disabled
    eq_prog(c.enable_disable_expr, sizeof c.enable_disable_expr, SIG_CRUISE_SW, 1);
    eq_prog(c.set_expr,            sizeof c.set_expr,            SIG_CRUISE_SW, 2);
    eq_prog(c.bump_down_expr,      sizeof c.bump_down_expr,      SIG_CRUISE_SW, 2);
    eq_prog(c.resume_expr,         sizeof c.resume_expr,         SIG_CRUISE_SW, 3);
    eq_prog(c.bump_up_expr,        sizeof c.bump_up_expr,        SIG_CRUISE_SW, 3);
    eq_prog(c.cancel_expr,         sizeof c.cancel_expr,         SIG_CRUISE_SW, 4);
    c.long_press_ms = 500;
    c.brake_sig = (int16_t)SIG_BRAKE_SW; c.clutch_sig = (int16_t)SIG_CLUTCH_SW; c.handbrake_sig = -1;
    c.min_speed_kph = 300; c.max_speed_kph = 2000;          // 30..200 kph
    c.speed_increment_kph = 10;                             // 1.0 kph a tap
    c.accel_rate_kph_s = 20; c.coast_rate_kph_s = 20;       // 2.0 kph/s held
    c.max_demand_pct = 600;                                 // 60%
    c.max_ramp_pct_s = 200;                                 // 20.0 %/s — 0.4% a frame at 20 ms
    c.cancel_decay_s = 500;                                 // 0.5 s to bleed the floor away
    // Runaway and plausibility are OFF in the shared rig, and each has its own section. Most sections
    // step the road speed instantly and hold a large error for as long as they like — neither is
    // something a car does, and a realistic limit would fault all of them for the wrong reason.
    c.max_error_kph = 0; c.max_error_ms = 3000;
    c.max_wheel_diff_kph = 100;                             // 10.0 kph of disagreement
    c.spd_max_age_ms = 250;                                 // a speed older than this is not believed
    c.max_accel_kph_s = 0;
    c.sw_fault_ms = 300;
    c.cruise_err_axis_n = 1; c.cruise_err_axis[0] = 0.0f;   // one bin: each curve is a constant
    c.cruise_p_gain[0] = 300;                               // 3.0 %/kph
    c.cruise_i_gain[0] = 100;                               // 1.0 %/kph/s
    return c;
}

// Arm (or disarm) a sensor's own fault checks. Cruise refuses to run on an UNMONITORED interlock —
// diag_enable ships as 0, so without this every rig would sit in Fault, which is the rule working.
static void watch_sensor(SignalId sig, bool on) {
    for (unsigned i = 0; i < SENSOR_COUNT; ++i)
        if (SENSOR_CATALOG[i].primary_channel == (uint16_t)sig)
            g_config.sensors.sensor[i].diag_enable = on ? 0x01 : 0x00;
}

static void set_sensor_enabled(SignalId sig, bool on) {
    for (unsigned i = 0; i < SENSOR_COUNT; ++i)
        if (SENSOR_CATALOG[i].primary_channel == (uint16_t)sig)
            g_config.sensors.sensor[i].enabled = on ? 1 : 0;
}

struct St { int state; bool active; float demand; float target; uint32_t inhibit; };

// One rig for every section: the stalk position, the road speed and the two pedal switches.
struct Rig {
    CruiseControl cc; SignalBus b; CruiseControlConfig cfg;
    Rig() : cfg(make_cfg()) { init_common(); }
    explicit Rig(const CruiseControlConfig& c) : cfg(c) { init_common(); }
    void init_common() {
        set_sensor_enabled(SIG_CRUISE_SW, true);    // the rig's car HAS a stalk
        watch_sensor(SIG_BRAKE_SW, true);           // a properly set-up car watches its brake switch
        watch_sensor(SIG_CLUTCH_SW, true);
        cc.set_dtc(&dtc);
        dtc.set_active(true);                       // runtime codes are suppressed on the bench otherwise
        cc.init(cfg);
    }
    DtcManager dtc;
    bool dtc_active(uint16_t code) const { return dtc.code_severity(code) != 0; }
    bool brake_valid = true;          // clear it to leave the brake channel INVALID, not merely false
    bool fresh_speed = true;          // clear it to let vehicle_spd go STALE without going invalid
    bool clutch_fitted = true;        // clear it to model a car with no clutch switch at all
    St run(uint32_t dt, float speed, int pos, bool brake = false, bool clutch = false, float pedal = 0.0f) {
        g_ms += dt; EnginePosition p{}; EngineFrame f{};
        if (fresh_speed) b.set(wk::vehicle_spd, speed, true, g_ms, 0);   // stamped: age stays 0
        b.set(SIG_CRUISE_SW, (float)pos, true, g_ms, 0);
        if (brake_valid) b.set_bool(SIG_BRAKE_SW, brake, g_ms, 0); else b.invalidate(SIG_BRAKE_SW);
        if (clutch_fitted) b.set_bool(SIG_CLUTCH_SW, clutch, g_ms, 0);
        else               b.invalidate(SIG_CLUTCH_SW);
        b.set(SIG_PEDAL_DEMAND, pedal, true, g_ms, 0);
        cc.update(p, b, f);
        return St{ (int)b.get(SIG_CRUISE_STATE, -1.0f), b.get_bool(SIG_CRUISE_ACTIVE),
                   b.get(SIG_CRUISE_DEMAND, -1.0f), b.get(SIG_CRUISE_TARGET, -1.0f),
                   b.get_u32(SIG_CRUISE_INHIBIT) };
    }
    // Press and release a position: a TAP is reported on the release, so both halves are needed.
    St tap(int pos, float speed, uint32_t hold_ms = 100) {
        run(hold_ms, speed, pos);
        return run(20, speed, 0);
    }
    // Hold a position for ms at the module's own 20 ms cadence, then release.
    St hold(int pos, float speed, uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 20) run(20, speed, pos);
        return run(20, speed, 0);
    }
    // Run n frames at the module's own cadence. The floor is rate-limited, so anything asking "where
    // does this end up" has to let it get there.
    St settle(float speed, int frames = 300) {
        St s{}; for (int i = 0; i < frames; ++i) s = run(20, speed, 0); return s;
    }
    void arm(float speed) { tap(1, speed); }                 // Disabled -> Ready
    St engage(float speed, float pedal = 0.0f) {             // ... -> Cruising
        run(100, speed, 2, false, false, pedal);
        return run(20, speed, 0, false, false, pedal);
    }
};

static const int OFF = 0, DISABLED = 1, READY = 2, CRUISING = 3;
static const char* state_name(int s) {
    switch (s) { case 0: return "Off"; case 1: return "Disabled"; case 2: return "Ready";
                 case 3: return "Cruising"; case 4: return "Fault"; default: return "?"; }
}
// A state machine that fails should say WHICH state it was in, not print two integers at you.
#define CHECK_STATE(got, want) do { \
    const int g_ = (got), w_ = (want); \
    if (g_ != w_) { fprintf(stdout, "FAIL [%s:%d]  state is %s, expected %s\n", \
                            __FILE__, __LINE__, state_name(g_), state_name(w_)); g_fail++; } \
    else g_pass++; \
} while (0)

int main() {
    fprintf(stdout, "=== CruiseControl ===\n");

    SECTION("no cruise stalk fitted: buttons in the conditions work, and it is not a switch fault");
    {
        // A car driving cruise from separate buttons (or CAN) never publishes cruise_sw. That used to
        // read as a stalk in no band, and faulted cruise 300 ms after it was switched on.
        Rig r;
        set_sensor_enabled(SIG_CRUISE_SW, false);
        St st{};
        for (int i = 0; i < 50; ++i) {                     // 1 s, well past sw_fault_ms
            r.b.invalidate(SIG_CRUISE_SW);
            g_ms += 20; EnginePosition p{}; EngineFrame f{};
            r.b.set(wk::vehicle_spd, 80.0f, true, g_ms, 0);
            r.b.set_bool(SIG_BRAKE_SW, false, g_ms, 0);
            r.b.set_bool(SIG_CLUTCH_SW, false, g_ms, 0);
            r.b.set(SIG_PEDAL_DEMAND, 0.0f, true, g_ms, 0);
            r.cc.update(p, r.b, f);
            st.state = (int)r.b.get(SIG_CRUISE_STATE, -1.0f);
            st.inhibit = r.b.get_u32(SIG_CRUISE_INHIBIT);
        }
        CHECK((st.inhibit & CruiseControl::INH_SW_INVALID) == 0);
        CHECK(st.state != (int)CruiseState::FAULT);
        CHECK(!r.dtc_active(ModuleDtc::CRUISE_SW));
        set_sensor_enabled(SIG_CRUISE_SW, true);           // the other sections' car has one
    }

    SECTION("starts Disabled, and Set alone will not engage it");
    { Rig r; St s = r.run(20, 100, 0);
      CHECK_STATE(s.state, DISABLED);
      s = r.tap(2, 100);                        // Set, but never armed
      CHECK_STATE(s.state, DISABLED); CHECK(!s.active); }

    SECTION("the main button toggles Disabled and Ready");
    { Rig r; r.arm(100);
      CHECK_STATE((int)r.cc.state(), READY);
      r.tap(1, 100);
      CHECK_STATE((int)r.cc.state(), DISABLED); }

    SECTION("Set engages from Ready and latches the speed it was pressed at");
    { Rig r; r.arm(100); St s = r.engage(100);
      CHECK_STATE(s.state, CRUISING); CHECK(s.active); CHECK_NEAR(s.target, 100.0f, 0.01f); }

    SECTION("ONE position can be Set and Coast both, and a tap does only the one it was pressed in");
    { Rig r; r.arm(100);
      St s = r.engage(100);                     // position 2 is Set here...
      CHECK_NEAR(s.target, 100.0f, 0.01f);      // ... and must NOT also coast on the same frame
      s = r.tap(2, 100);                        // ... and Coast once already cruising
      CHECK_STATE(s.state, CRUISING); CHECK_NEAR(s.target, 99.0f, 0.01f); }

    SECTION("a tap is one increment; the same button HELD ramps instead, and taps nothing");
    { Rig r; r.arm(100); r.engage(100);
      St s = r.tap(3, 100);                     // position 3 tapped -> +1.0
      CHECK_NEAR(s.target, 101.0f, 0.01f);
      // Held 1.5 s: 0.5 s before the hold is recognised, then 1.0 s ramping at 2.0 kph/s -> +2.0.
      s = r.hold(3, 100, 1500);
      CHECK_NEAR(s.target, 103.0f, 0.15f);
      // ... and the release must NOT also count as a tap, which would put another 1.0 on top.
      CHECK(s.target < 103.5f); }

    SECTION("Cancel keeps the set speed, and Resume returns to it");
    { Rig r; r.arm(100); r.engage(100);
      St s = r.tap(4, 100);
      CHECK_STATE(s.state, READY); CHECK(!s.active); CHECK_NEAR(s.target, 100.0f, 0.01f);
      s = r.tap(3, 90);                         // Resume at a DIFFERENT speed -> the kept one wins
      CHECK_STATE(s.state, CRUISING); CHECK_NEAR(s.target, 100.0f, 0.01f); }

    SECTION("Disable throws the set speed away, so Resume has nothing to return to");
    { Rig r; r.arm(100); r.engage(100);
      r.tap(1, 100);                            // main button: Cruising -> Disabled
      CHECK_STATE((int)r.cc.state(), DISABLED);
      r.arm(100);                               // back to Ready
      St s = r.tap(3, 100);                     // Resume
      CHECK_STATE(s.state, READY); CHECK(!s.active); }

    SECTION("brake cancels and keeps the speed; an INVALID brake cancels too");
    { Rig r; r.arm(100); r.engage(100);
      St s = r.run(20, 100, 0, true, false);
      CHECK_STATE(s.state, READY); CHECK(s.inhibit & CruiseControl::INH_BRAKE);
      CHECK_NEAR(s.target, 100.0f, 0.01f);
      // Fail-SAFE, the opposite of an expression: a brake that has stopped reporting is not "not braking".
      Rig r2; r2.arm(100); r2.engage(100);
      r2.brake_valid = false;                   // the switch stops reporting altogether
      s = r2.run(20, 100, 0);
      CHECK_STATE(s.state, READY); CHECK(s.inhibit & CruiseControl::INH_BRAKE); }

    SECTION("clutch cancels");
    { Rig r; r.arm(100); r.engage(100);
      St s = r.run(20, 100, 0, false, true);
      CHECK_STATE(s.state, READY); CHECK(s.inhibit & CruiseControl::INH_CLUTCH); }

    SECTION("an automatic has no clutch switch, and must still be able to engage");
    { // An invalid cancel input reads as PRESSED — that is the fail-safe, and it is right. Which means
      // the SHIPPED DEFAULT decides whether a car without a clutch pedal can use cruise at all: point
      // it at clutch_sw and the channel never reports, so cruise sees a clutch held down for ever.
      CHECK(g_config.cruise_control.clutch_sig < 0);
      auto d = make_cfg(); d.clutch_sig = -1; Rig r(d);
      r.clutch_fitted = false;                  // nothing publishes the channel at all
      r.arm(100); St s = r.engage(100);
      CHECK_STATE(s.state, CRUISING);
      CHECK(!(s.inhibit & CruiseControl::INH_CLUTCH));
      // ... whereas a clutch that IS assigned and has stopped reporting still cancels.
      auto d2 = make_cfg(); Rig r2(d2);
      r2.arm(100); r2.engage(100);
      r2.clutch_fitted = false;
      s = r2.run(20, 100, 0);
      CHECK_STATE(s.state, READY);
      CHECK(s.inhibit & CruiseControl::INH_CLUTCH); }

    SECTION("the speed window refuses engagement and cancels, and says which end");
    { Rig r; r.arm(20); St s = r.engage(20);
      CHECK_STATE(s.state, READY); CHECK(s.inhibit & CruiseControl::INH_SPEED_LOW);
      Rig r2; r2.arm(100); r2.engage(100);
      s = r2.run(20, 250, 0);
      CHECK_STATE(s.state, READY); CHECK(s.inhibit & CruiseControl::INH_SPEED_HIGH); }

    SECTION("a fail-to-false condition cannot engage: the stalk reads INVALID");
    { Rig r; r.arm(100);
      r.run(100, 100, 2);
      r.b.invalidate(SIG_CRUISE_SW);            // the band decoder found no calibrated band
      St s = r.run(20, 100, 2);
      CHECK_STATE(s.state, READY); CHECK(!s.active); }

    SECTION("engage starts the demand at the held pedal, not at zero");
    { Rig r; r.arm(100); St s = r.engage(100, 25.0f);
      CHECK_NEAR(s.demand, 25.0f, 0.01f);
      s = r.run(20, 100, 0, false, false, 0.0f); // driver lifts: on target, the floor stays put
      CHECK_NEAR(s.demand, 25.0f, 0.01f); }

    SECTION("under target -> demand, and it is clamped to max_demand_pct");
    { Rig r; r.arm(100); r.engage(100);
      St s = r.run(20, 95, 0);
      CHECK(s.demand > 0.0f);
      s = r.settle(40);                         // a huge error, given time to get there
      CHECK_NEAR(s.demand, 60.0f, 0.01f); }

    SECTION("the floor is rate limited, but engagement SNAPS to the pedal instead of ramping to it");
    { Rig r; r.arm(100);
      St s = r.engage(100, 30.0f);              // the hand-over is not ramped, or it would dip
      CHECK_NEAR(s.demand, 30.0f, 0.01f);
      // ... and from there it may move no faster than max_ramp_pct_s: 20%/s over a 20 ms frame = 0.4%.
      s = r.run(20, 40, 0, false, false, 0.0f); // ask for full demand in one step
      CHECK_NEAR(s.demand, 30.4f, 0.02f); }

    SECTION("a button cancel bleeds the floor away; a brake cancel drops it the same frame");
    { Rig r; r.arm(100); St s = r.engage(100, 30.0f);
      CHECK_NEAR(s.demand, 30.0f, 0.01f);
      r.run(100, 100, 4);                       // Cancel pressed (position 4)
      s = r.run(20, 100, 0);                    // released -> the tap lands, decay begins
      CHECK_STATE(s.state, READY);
      CHECK(s.demand > 20.0f);                  // still bleeding, not gone
      for (int i = 0; i < 30; ++i) s = r.run(20, 100, 0);   // 0.6 s later it has reached zero
      CHECK_NEAR(s.demand, 0.0f, 0.01f);
      // The brake is the exception: a driver already decelerating wants no residual throttle floor.
      Rig r2; r2.arm(100); r2.engage(100, 30.0f);
      s = r2.run(20, 100, 0, true, false);
      CHECK_STATE(s.state, READY); CHECK_NEAR(s.demand, 0.0f, 0.01f); }

    SECTION("the integrator is frozen while the driver is over the floor, so lifting off does not dip");
    { Rig r; r.arm(100); r.engage(100, 20.0f);
      // Two seconds of hard overtake: a large negative error that would otherwise unwind the integrator.
      St s{}; for (int i = 0; i < 100; ++i) s = r.run(20, 130, 0, false, false, 90.0f);
      const float floor_now = s.demand;
      CHECK_NEAR(floor_now, 20.0f, 0.5f);       // the floor is still where the hand-over put it
      CHECK(s.demand > 15.0f); }

    SECTION("disabled in the tune -> Off, and nothing engages");
    { auto d = make_cfg(); d.enabled = 0; Rig r(d);
      St s = r.run(20, 100, 0);
      CHECK_STATE(s.state, OFF);
      s = r.tap(2, 100);
      CHECK_STATE(s.state, OFF); CHECK(!s.active); }

    // ---- the interlocks, each with the code it raises -------------------------------------------

    SECTION("a dead speed sensor FAULTS and says so — it must not merely look like standing still");
    { Rig r; r.arm(100); r.engage(100);
      r.b.invalidate(wk::vehicle_spd);
      r.fresh_speed = false;
      St s = r.run(20, 0, 0);
      // The old module read vehicle_spd with a 0 fallback, so this case looked EXACTLY like a car
      // below the minimum speed. Asserting the code, not just !active, is what tells them apart.
      CHECK_STATE(s.state, (int)CruiseState::FAULT);
      CHECK(s.inhibit & CruiseControl::INH_VSS_INVALID);
      CHECK(r.dtc_active(ModuleDtc::CRUISE_VSS));
      CHECK_NEAR(s.target, 0.0f, 0.01f); }        // a fault discards the set speed; a cancel would keep it

    SECTION("a road speed that steps faster than a car can is not believed");
    { auto d = make_cfg(); d.max_accel_kph_s = 300; Rig r(d);   // 30 kph/s
      r.arm(100); r.engage(100);
      St s = r.run(20, 100, 0);
      CHECK(!(s.inhibit & CruiseControl::INH_VSS_IMPL));
      s = r.run(20, 130, 0);                      // +30 kph in 20 ms = 1500 kph/s
      CHECK(s.inhibit & CruiseControl::INH_VSS_IMPL);
      CHECK(r.dtc_active(ModuleDtc::CRUISE_VSS_IMPL));
      CHECK_STATE(s.state, (int)CruiseState::FAULT); }

    SECTION("a speed that has stopped updating is stale, not zero");
    { Rig r; r.arm(100); r.engage(100);
      r.fresh_speed = false;                      // valid, but never re-stamped
      St s{}; for (int i = 0; i < 20; ++i) s = r.run(20, 100, 0);   // 400 ms > spd_max_age_ms 250
      CHECK(s.inhibit & CruiseControl::INH_VSS_STALE);
      CHECK(r.dtc_active(ModuleDtc::CRUISE_VSS)); }

    SECTION("runaway needs BOTH halves: it trips after the time, and not before it");
    { auto d = make_cfg(); d.max_error_kph = 50; d.max_error_ms = 3000; Rig r(d);
      r.arm(100); r.engage(100);
      St s{};
      for (int i = 0; i < 100; ++i) s = r.run(20, 90, 0);     // 2.0 s at 10 kph error, limit 5.0/3000 ms
      CHECK(!(s.inhibit & CruiseControl::INH_RUNAWAY));       // ... not yet
      for (int i = 0; i < 60; ++i) s = r.run(20, 90, 0);      // 3.2 s total
      CHECK(s.inhibit & CruiseControl::INH_RUNAWAY);
      CHECK(r.dtc_active(ModuleDtc::CRUISE_RUNAWAY));
      // And the accumulator must RESET when the error comes back inside the band, or every hill crest
      // eventually trips it.
      Rig r2(d); r2.arm(100); r2.engage(100);
      for (int cycle = 0; cycle < 4; ++cycle) {
          for (int i = 0; i < 100; ++i) s = r2.run(20, 90, 0);    // 2.0 s out of band
          for (int i = 0; i < 10;  ++i) s = r2.run(20, 100, 0);   // ... then back in
      }
      CHECK(!(s.inhibit & CruiseControl::INH_RUNAWAY)); }

    SECTION("wheel speeds that disagree cancel — but only when all four are actually reading");
    { Rig r; r.arm(100); r.engage(100);
      r.b.set(SIG_WHEEL_FL, 100.0f); r.b.set(SIG_WHEEL_FR, 100.0f);
      r.b.set(SIG_WHEEL_RL, 100.0f); r.b.set(SIG_WHEEL_RR, 112.0f);   // 12 kph spread, limit 10
      St s = r.run(20, 100, 0);
      CHECK(s.inhibit & CruiseControl::INH_WHEEL_DIFF);
      CHECK(r.dtc_active(ModuleDtc::CRUISE_WHEEL_DIFF));
      // Two sensors fitted and disagreeing is NOT a fault: a car with fewer than four skips the check
      // rather than being held off cruise for ever.
      Rig r2; r2.arm(100); r2.engage(100);
      r2.b.set(SIG_WHEEL_FL, 100.0f); r2.b.set(SIG_WHEEL_FR, 130.0f);
      s = r2.run(20, 100, 0);
      CHECK(!(s.inhibit & CruiseControl::INH_WHEEL_DIFF)); }

    SECTION("a pedal fault holds cruise off");
    { Rig r; r.arm(100); r.engage(100);
      r.b.set(SIG_APP_STATE, 2.0f);
      St s = r.run(20, 100, 0);
      CHECK(s.inhibit & CruiseControl::INH_PEDAL_FAULT);
      CHECK(r.dtc_active(ModuleDtc::CRUISE_PEDAL)); }

    SECTION("an unmonitored brake input holds cruise off, because the fail-safe rests on it");
    { Rig r; watch_sensor(SIG_BRAKE_SW, false);   // no checks armed on the brake sensor
      St s = r.run(20, 100, 0);
      CHECK(s.inhibit & CruiseControl::INH_UNMONITORED);
      CHECK(r.dtc_active(ModuleDtc::CRUISE_UNMONITORED));
      r.arm(100); s = r.engage(100);
      CHECK(!s.active);
      watch_sensor(SIG_BRAKE_SW, true); }

    SECTION("no brake input assigned at all holds cruise off");
    { auto d = make_cfg(); d.brake_sig = -1; Rig r(d);
      St s = r.run(20, 100, 0);
      CHECK(s.inhibit & CruiseControl::INH_NO_BRAKE);
      CHECK(r.dtc_active(ModuleDtc::CRUISE_NO_BRAKE));
      r.arm(100); s = r.engage(100);
      CHECK(!s.active); }

    SECTION("a fault holds until the driver acknowledges it, and never resumes straight into Cruising");
    { Rig r; r.arm(100); r.engage(100);
      r.b.invalidate(wk::vehicle_spd); r.fresh_speed = false;
      St s = r.run(20, 0, 0);
      CHECK_STATE(s.state, (int)CruiseState::FAULT);
      r.fresh_speed = true;
      // The cause is gone, but nothing has been pressed: a fault that cleared itself here would have
      // existed for one frame, which nobody can see and no scan tool would ever catch.
      for (int i = 0; i < 50; ++i) s = r.run(20, 100, 0);
      CHECK_STATE(s.state, (int)CruiseState::FAULT);
      CHECK(r.dtc_active(ModuleDtc::CRUISE_VSS));
      s = r.run(20, 100, 2);                      // Set pressed and held: still not an acknowledgement
      CHECK_STATE(s.state, (int)CruiseState::FAULT);
      s = r.run(20, 100, 0);                      // released -> the tap lands: Ready, NOT Cruising
      CHECK_STATE(s.state, READY);
      CHECK(!r.dtc_active(ModuleDtc::CRUISE_VSS)); }

    SECTION("the codes heal when the module is switched off in the tune");
    { Rig r; r.arm(100); r.engage(100);
      r.b.invalidate(wk::vehicle_spd); r.fresh_speed = false;
      r.run(20, 0, 0);
      CHECK(r.dtc_active(ModuleDtc::CRUISE_VSS));
      r.cfg.enabled = 0;                          // the enabled->disabled edge must heal what we raised
      St s = r.run(20, 0, 0);
      CHECK_STATE(s.state, OFF);
      CHECK(!r.dtc_active(ModuleDtc::CRUISE_VSS)); }

    // With the key off the sensors publish nothing. Judged as faults, a dead speed sensor and stalk
    // latched FAULT through key-off, and key-on inherited it: a VSS code every key cycle and a cruise
    // that would not engage until a button acknowledged a fault that was only the key.
    SECTION("key off -> on: cruise wakes in its power-on state, with no fault and no code");
    { auto d = make_cfg(); d.power_on_state = 1; Rig r(d);     // power on into Ready
      St s = r.run(20, 100, 0);
      CHECK_STATE(s.state, READY);
      g_system_active = false;
      r.b.invalidate(wk::vehicle_spd); r.b.invalidate(SIG_CRUISE_SW); r.fresh_speed = false;
      for (int i = 0; i < 50; ++i) { g_ms += 20; EnginePosition p{}; EngineFrame f{}; r.cc.update(p, r.b, f); }
      CHECK(!r.dtc_active(ModuleDtc::CRUISE_VSS));
      CHECK(!r.dtc_active(ModuleDtc::CRUISE_SW));
      g_system_active = true; r.fresh_speed = true;
      s = r.run(20, 100, 0);
      CHECK_STATE(s.state, READY);                 // not FAULT
      CHECK(!r.dtc_active(ModuleDtc::CRUISE_VSS));
      s = r.engage(100);
      CHECK_STATE(s.state, CRUISING); }            // engages without an acknowledge press

    return test_summary();
}
