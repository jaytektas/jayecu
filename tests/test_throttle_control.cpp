#include "test_helpers.h"
#include "../firmware/Engine/Modules/ElectronicThrottle.h"
#include <algorithm>
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../generated/signal_ids.h"
#include "../generated/ecu_config.h"      // g_config — autocal reads sensor.source + writes cal
#include "../generated/sensors_catalog.h" // SENSOR_CATALOG — resolve a feedback sensor by its SignalId
#include "../firmware/Diagnostics/DtcManager.h"   // assert the L2 supervisor raises throttle P-codes

static uint32_t g_ms = 0;

// Resolve a sensor's config index from the SignalId it provides — the same mapping the ETB uses
// (ElectronicThrottle::find_sensor). Tracking the catalog beats hardcoding indices that silently shift
// when sensors are added/reordered (which is exactly how this test broke).
static int sensor_idx(SignalId sig) {
    for (int i = 0; i < static_cast<int>(SENSOR_COUNT); i++)
        if (SENSOR_CATALOG[i].primary_channel == sig) return i;
    return -1;
}
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }
extern "C" uint16_t platform_read_ain_raw(uint8_t) { return 0; }
// No DWT on the host. Returning 0 tells the module to take its timestep from the tick instead, which is
// what makes the test's frame interval a real interval to the integral and derivative terms.
extern "C" uint32_t platform_cyccnt() { return 0; }
extern "C" uint32_t platform_cpu_hz() { return 216000000u; }   // counts seam default; overridden in tests
// Key-on (12 V) gate, owned by EngineTask on target. The ETB parks entirely while it is false, so
// every existing test needs it TRUE to exercise anything; the key-cycle test drives it directly.
bool g_system_active = true;
uint32_t g_config_generation = 0;          // autocal bumps this so Sensors would re-read
volatile uint16_t g_command_state = 0;     // ETB reports bench-routine progress here (defined in CommsManager on target)

// Simulated BIPOLAR plate: position integrates the SIGNED duty (+ opens, - closes), clamped to the
// mechanical stops [0,100]. raw mV: AV1 (pin 0) descending 3366->235, AV2 (pin 1) opposite-slope
// ascending 604->3729. plant_step() is called each frame with the published signed duty.
static float g_plate = 30.0f;     // % open
static uint16_t sim_raw(uint8_t pin) {
    const float frac = g_plate / 100.0f;
    if (pin == 0) return static_cast<uint16_t>(3366 - frac * (3366 - 235));
    if (pin == 1) return static_cast<uint16_t>(604  + frac * (3729 - 604));
    return 0;
}
static void plant_step(float duty_signed) {            // duty 100 => +4%/frame; -100 => -4%/frame
    g_plate += duty_signed * 0.04f;
    g_plate = std::clamp(g_plate, 0.0f, 100.0f);
}

// One ETB on TPS_1/TPS_2, no control-layer filter (deterministic), 10% match window, 200ms debounce.
static ElectronicThrottleConfig make_cfg() {
    ElectronicThrottleConfig c{};
    EtbConfig& e = c.etb[0];
    e.enabled           = 1;   // per-instance enable is the only switch; module runs if any instance is on
    e.tps_a_src         = SIG_TPS;
    e.tps_b_src         = SIG_AUX_1;
    e.tps_match_err_pct = 100;
    e.tps_match_ms      = 200;
    e.max_tps_pct       = 1000;
    c.traction_cap_src  = -1;   // no caps -> the demand arbitration passes the pedal straight through
    c.torque_cap_src    = -1;
    return c;
}

static void run(ElectronicThrottle& tc, SignalBus& bus, uint32_t dt_ms) {
    g_ms += dt_ms;
    EnginePosition pos{};
    EngineFrame frame{};
    tc.update(pos, bus, frame);
}

int main() {
    SECTION("Stage 0: read-only — motor never driven");
    {
        ElectronicThrottleConfig cfg = make_cfg();
        ElectronicThrottle tc;
        tc.init(cfg);
        SignalBus bus;
        bus.set(SIG_TPS, 30.0f, true, g_ms);
        bus.set(SIG_AUX_1, 32.0f, true, g_ms);
        run(tc, bus, 10);
        // position IS tps_a (the blade); tps_b only cross-checks
        CHECK_NEAR(bus.get(SIG_ETB_POSITION_1), 30.0f, 0.01f);
        // EN deasserted (disabled) and duty 0 — the bridge can never move the plate in Stage 0
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);
        CHECK_NEAR(bus.get(SIG_ETB_DUTY_1), 0.0f, 0.001f);
        // healthy A/B agreement, no cal/motor -> UNCAL (monitored, disabled)
        CHECK(bus.get(SIG_ETB_STATE_1) == ElectronicThrottle::ST_UNCAL);
    }

    SECTION("A/B agreement holds across the match window");
    {
        ElectronicThrottleConfig cfg = make_cfg();
        ElectronicThrottle tc;
        tc.init(cfg);
        SignalBus bus;
        // 8% apart — within the 10% window: never faults
        bus.set(SIG_TPS, 50.0f, true, g_ms);
        bus.set(SIG_AUX_1, 58.0f, true, g_ms);
        for (int i = 0; i < 50; i++) { bus.set(SIG_TPS, 50.0f, true, g_ms); bus.set(SIG_AUX_1, 58.0f, true, g_ms); run(tc, bus, 10); }
        CHECK(bus.get(SIG_ETB_STATE_1) == ElectronicThrottle::ST_UNCAL);
        CHECK_NEAR(bus.get(SIG_ETB_POSITION_1), 50.0f, 0.01f);   // position = tps_a (50), not the A/B mean
    }

    SECTION("A/B disagreement latches FAULT only after the debounce");
    {
        ElectronicThrottleConfig cfg = make_cfg();
        ElectronicThrottle tc;
        tc.init(cfg);
        SignalBus bus;
        // 40% apart — well beyond the 10% window
        auto feed = [&]{ bus.set(SIG_TPS, 20.0f, true, g_ms); bus.set(SIG_AUX_1, 60.0f, true, g_ms); };
        feed(); run(tc, bus, 10);   // first frame: dt anchors, no accrual yet
        CHECK(bus.get(SIG_ETB_STATE_1) == ElectronicThrottle::ST_UNCAL);
        // accrue ~150ms of disagreement (< 200ms): still not latched
        for (int i = 0; i < 15; i++) { feed(); run(tc, bus, 10); }
        CHECK(bus.get(SIG_ETB_STATE_1) == ElectronicThrottle::ST_UNCAL);
        // push past 200ms total -> FAULT
        for (int i = 0; i < 10; i++) { feed(); run(tc, bus, 10); }
        CHECK(bus.get(SIG_ETB_STATE_1) == ElectronicThrottle::ST_FAULT);
        // recovery: agreement clears the debounce and the fault
        for (int i = 0; i < 5; i++) { bus.set(SIG_TPS, 40.0f, true, g_ms); bus.set(SIG_AUX_1, 41.0f, true, g_ms); run(tc, bus, 10); }
        CHECK(bus.get(SIG_ETB_STATE_1) == ElectronicThrottle::ST_UNCAL);
    }

    SECTION("single-sensor dropout -> usable position but FAULT (no cross-check)");
    {
        ElectronicThrottleConfig cfg = make_cfg();
        ElectronicThrottle tc;
        tc.init(cfg);
        SignalBus bus;
        // only A valid; B never written (stale -> invalid)
        for (int i = 0; i < 30; i++) { bus.set(SIG_TPS, 25.0f, true, g_ms); run(tc, bus, 10); }
        CHECK_NEAR(bus.get(SIG_ETB_POSITION_1), 25.0f, 0.01f);   // falls back to the one valid sensor
        CHECK(bus.get(SIG_ETB_STATE_1) == ElectronicThrottle::ST_FAULT);  // can't cross-check -> not trusted
    }

    SECTION("disabled ETB -> publishes nothing (consumer fail-safes on invalid demand)");
    {
        ElectronicThrottleConfig cfg = make_cfg();
        cfg.etb[0].enabled = 0;
        ElectronicThrottle tc;
        tc.init(cfg);
        SignalBus bus;
        bus.set(SIG_TPS, 30.0f, true, g_ms);
        bus.set(SIG_AUX_1, 30.0f, true, g_ms);
        run(tc, bus, 10);
        // A disabled module MUST NOT publish signals — EN/state/position all stay absent.
        // The H-bridge actuator fail-safes the plate because the demand signal is invalid.
        CHECK(bus.valid(SIG_ETB_EN_1) == false);
        CHECK(bus.valid(SIG_ETB_STATE_1) == false);
        CHECK(bus.valid(SIG_ETB_POSITION_1) == false);   // nothing published when disabled
    }

    SECTION("Stage 1: manual nudge arms -> slews -> releases DIS -> auto-expires");
    {
        ElectronicThrottleConfig cfg = make_cfg();
        cfg.etb[0].open_rate_pct_s  = 300;   // 3%/frame @10ms — must slew, not jump
        cfg.etb[0].close_rate_pct_s = 600;
        ElectronicThrottle tc; tc.init(cfg);
        SignalBus bus;
        auto feed = [&]{ bus.set(SIG_TPS,30.0f,true,g_ms); bus.set(SIG_AUX_1,31.0f,true,g_ms);
                         bus.set(SIG_ENGINE_STATE,0.0f,true,g_ms); };  // engine STOPPED
        feed(); run(tc,bus,10);
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);            // not armed -> EN deasserted
        CHECK_NEAR(bus.get(SIG_ETB_DUTY_1), 0.0f, 0.01f);

        tc.set_manual(0, 50.0f, 1000);                          // arm 50% for 1s
        for (int i=0;i<5;i++){ feed(); run(tc,bus,10); }
        CHECK(bus.get_bool(SIG_ETB_EN_1) == true);            // armed -> enabled
        const float mid = bus.get(SIG_ETB_DUTY_1);
        CHECK(mid > 5.0f && mid < 50.0f);                       // slewing up, didn't jump
        for (int i=0;i<25;i++){ feed(); run(tc,bus,10); }
        CHECK_NEAR(bus.get(SIG_ETB_DUTY_1), 50.0f, 2.0f);       // reached demand
        CHECK(bus.get_bool(SIG_ETB_EN_1) == true);
        // expire (advance well past 1s) without re-arming: eases to 0, then DIS re-asserts
        for (int i=0;i<160;i++){ feed(); run(tc,bus,10); }
        CHECK_NEAR(bus.get(SIG_ETB_DUTY_1), 0.0f, 0.5f);
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);
    }

    SECTION("Stage 1: engine running inhibits the nudge (hard cut)");
    {
        ElectronicThrottleConfig cfg = make_cfg();
        cfg.etb[0].open_rate_pct_s = 0;        // unlimited slew: target would jump if allowed
        ElectronicThrottle tc; tc.init(cfg);
        SignalBus bus;
        tc.set_manual(0, 80.0f, 5000);
        for (int i=0;i<3;i++){ bus.set(SIG_TPS,30.0f,true,g_ms); bus.set(SIG_AUX_1,31.0f,true,g_ms);
                               bus.set(SIG_ENGINE_STATE,2.0f,true,g_ms); run(tc,bus,10); }  // RUNNING
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);            // not stopped -> EN deasserted
        CHECK_NEAR(bus.get(SIG_ETB_DUTY_1), 0.0f, 0.01f);      // no drive
    }

    SECTION("Stage 2: autocal sweeps to the stop and writes both sensors' cal (opposite slopes)");
    {
        ElectronicThrottle::set_raw_reader(sim_raw);
        const int i_tps = sensor_idx(SIG_TPS), i_aux = sensor_idx(SIG_AUX_1);   // tps_a_src / tps_b_src feedbacks
        g_config.sensors.sensor[i_tps].source = 0;   // tps_1 -> AV1
        g_config.sensors.sensor[i_aux].source = 1;   // tps_2 -> AV2
        ElectronicThrottleConfig cfg = make_cfg();
        cfg.etb[0].ac_duty_cap_pct = 550;
        cfg.etb[0].ac_timeout_ms   = 3000;
        cfg.etb[0].ac_move_min  = 200;
        cfg.etb[0].open_rate_pct_s = 300; cfg.etb[0].close_rate_pct_s = 300;
        ElectronicThrottle tc; tc.init(cfg);
        SignalBus bus;
        g_plate = 40.0f;                                   // plate starts mid; autocal drives both stops
        tc.start_autocal(0);
        bool saw_autocal = false, reached_ready = false;
        for (int i = 0; i < 600; i++) {
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms);   // STOPPED
            run(tc, bus, 10);
            plant_step(bus.get(SIG_ETB_DUTY_1));           // plate integrates the signed duty
            const int st = static_cast<int>(bus.get(SIG_ETB_STATE_1));
            if (st == ElectronicThrottle::ST_AUTOCAL) saw_autocal = true;
            if (st == ElectronicThrottle::ST_READY)  { reached_ready = true; break; }
            if (st == ElectronicThrottle::ST_FAULT)  break;
        }
        CHECK(saw_autocal);
        CHECK(reached_ready);
        const SensorConfig& s1 = g_config.sensors.sensor[i_tps];   // tps_1 / AV1 (descending raw)
        const SensorConfig& s2 = g_config.sensors.sensor[i_aux];   // tps_2 / AV2 (ascending raw)
        CHECK(s1.cal_n == 2 && s2.cal_n == 2);
        // cal_raw must be ascending; closed(rest)->0%, open->100% regardless of slope.
        CHECK(s1.cal_raw[0] < s1.cal_raw[1]);
        CHECK_NEAR(s1.cal_raw[0], 235, 40);  CHECK_NEAR(s1.cal_raw[1], 3366, 40);
        CHECK(s1.cal_val[0] == 1000 && s1.cal_val[1] == 0);    // AV1 open=low mV -> descending value
        CHECK_NEAR(s2.cal_raw[0], 604, 40);  CHECK_NEAR(s2.cal_raw[1], 3729, 40);
        CHECK(s2.cal_val[0] == 0 && s2.cal_val[1] == 1000);    // AV2 open=high mV -> ascending value
        // raw DTC window straddles the observed range.
        CHECK(s1.diag_raw_min < 235 && s1.diag_raw_max > 3366);
    }

    SECTION("Stage 2: autocal aborts to FAULT when the plate doesn't move (stuck/dead)");
    {
        ElectronicThrottle::set_raw_reader([](uint8_t){ return uint16_t(2000); });  // never changes
        ElectronicThrottleConfig cfg = make_cfg();
        cfg.etb[0].ac_duty_cap_pct = 550; cfg.etb[0].ac_timeout_ms = 1500;
        cfg.etb[0].ac_move_min = 200; cfg.etb[0].open_rate_pct_s = 300; cfg.etb[0].close_rate_pct_s = 300;
        ElectronicThrottle tc; tc.init(cfg);
        SignalBus bus;
        tc.start_autocal(0);
        int last = -1;
        for (int i = 0; i < 600; i++) {
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms);
            run(tc, bus, 10);
            last = static_cast<int>(bus.get(SIG_ETB_STATE_1));
            if (last == ElectronicThrottle::ST_FAULT) break;
        }
        CHECK(last == ElectronicThrottle::ST_FAULT);
        ElectronicThrottle::set_raw_reader(sim_raw);   // restore for any later use
    }

    // Calibrate a fresh controller via autocal (sim plate), leaving it READY for closed-loop tests.
    auto calibrate = [](ElectronicThrottle& tc, SignalBus& bus, ElectronicThrottleConfig& cfg) {
        ElectronicThrottle::set_raw_reader(sim_raw);
        g_config.sensors.sensor[sensor_idx(SIG_TPS)].source = 0;
        g_config.sensors.sensor[sensor_idx(SIG_AUX_1)].source = 1;
        cfg.etb[0].ac_duty_cap_pct = 550; cfg.etb[0].ac_timeout_ms = 3000;
        cfg.etb[0].ac_move_min = 200; cfg.etb[0].open_rate_pct_s = 300; cfg.etb[0].close_rate_pct_s = 300;
        tc.init(cfg);
        g_plate = 40.0f;
        tc.start_autocal(0);
        for (int i = 0; i < 600; i++) {
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms); run(tc, bus, 10);
            plant_step(bus.get(SIG_ETB_DUTY_1));
            if (static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_READY) break;
        }
    };

    SECTION("Stage 3: bipolar closed-loop servos to target BOTH ways (open and close)");
    {
        ElectronicThrottleConfig cfg = make_cfg();
        cfg.etb[0].kp = 1.0f; cfg.etb[0].ki = 2.0f; cfg.etb[0].kd = 0.0f;   // FLOAT gains, used as stored
        cfg.etb[0].max_tps_pct = 1000; cfg.etb[0].open_rate_pct_s = 150; cfg.etb[0].close_rate_pct_s = 150;
        ElectronicThrottle tc; SignalBus bus;
        calibrate(tc, bus, cfg);
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_READY);
        // Bipolar plant: position integrates the signed duty (no spring); tps_1/2 = the calibrated plate.
        auto servo = [&](float tgt, int frames){
            for (int i = 0; i < frames; i++) {
                bus.set(SIG_TPS, g_plate, true, g_ms); bus.set(SIG_AUX_1, g_plate, true, g_ms);
                bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms);
                tc.set_manual(0, tgt, 5000);
                run(tc, bus, 10);
                plant_step(bus.get(SIG_ETB_DUTY_1));      // + opens, - closes
            }
        };
        g_plate = 0.0f;
        servo(60.0f, 300);                                 // open to 60%
        CHECK_NEAR(bus.get(SIG_ETB_POSITION_1), 60.0f, 6.0f);
        CHECK(bus.get_bool(SIG_ETB_EN_1) == true);
        servo(25.0f, 300);                                 // CLOSE (drive back down) to 25% — bidirectional
        CHECK_NEAR(bus.get(SIG_ETB_POSITION_1), 25.0f, 6.0f);
        CHECK(std::fabs(bus.get(SIG_ETB_DUTY_1)) < 95.0f); // not railed
    }

    SECTION("ETB actuator: servos throttle_demand from the bus (disarmed) + holds closed, no seat-release");
    {
        ElectronicThrottleConfig cfg = make_cfg();
        cfg.etb[0].kp = 1.0f; cfg.etb[0].ki = 2.0f; cfg.etb[0].kd = 0.0f;
        cfg.etb[0].max_tps_pct = 1000; cfg.etb[0].open_rate_pct_s = 150; cfg.etb[0].close_rate_pct_s = 150;
        ElectronicThrottle tc; SignalBus bus;
        calibrate(tc, bus, cfg);
        // Production path: drive via the PEDAL — the module's arbitration turns it into throttle_demand
        // (no idle floor, no caps), then the actuator servos to it. NO set_manual nudge.
        auto servo = [&](float demand, int frames){
            for (int i = 0; i < frames; i++) {
                bus.set(SIG_TPS, g_plate, true, g_ms); bus.set(SIG_AUX_1, g_plate, true, g_ms);
                bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms);
                bus.set(SIG_PEDAL_DEMAND, demand, true, g_ms);
                run(tc, bus, 10);
                plant_step(bus.get(SIG_ETB_DUTY_1));
            }
        };
        g_plate = 0.0f;
        servo(35.0f, 300);
        CHECK_NEAR(bus.get(SIG_ETB_POSITION_1), 35.0f, 6.0f);   // followed throttle_demand, disarmed
        CHECK(bus.get_bool(SIG_ETB_EN_1) == true);
        // demand 0: HOLD closed with torque. The OLD seat-release would assert DIS at <2% — it must not.
        servo(0.0f, 300);
        CHECK_NEAR(bus.get(SIG_ETB_POSITION_1), 0.0f, 4.0f);
        CHECK(bus.get_bool(SIG_ETB_EN_1) == true);            // still actively held (no seat-release)
    }

    SECTION("Stage 3: post-cal A/B disagreement latches DIS, clears only via autocal");
    {
        ElectronicThrottleConfig cfg = make_cfg();
        cfg.etb[0].kp = 1.5f; cfg.etb[0].ki = 2.5f;
        ElectronicThrottle tc; SignalBus bus;
        calibrate(tc, bus, cfg);
        auto step = [&](float t1, float t2){ bus.set(SIG_TPS,t1,true,g_ms); bus.set(SIG_AUX_1,t2,true,g_ms);
                                             bus.set(SIG_ENGINE_STATE,0.0f,true,g_ms); tc.set_manual(0,40.0f,5000); run(tc,bus,10); };
        for (int i=0;i<60;i++) step(30,30);                   // agree (past the autocal-settle) -> closed-loop
        CHECK(bus.get_bool(SIG_ETB_EN_1) == true);
        for (int i=0;i<40;i++) step(30,70);                   // 40% apart > 10% for > debounce -> latch
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_FAULT);
        for (int i=0;i<30;i++) step(30,30);                   // agreement returns, but the latch HOLDS
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);
        // A STALL DOES NOT FORGIVE IT. on_engine_stop() used to clear the latch with the rest of the
        // session state, putting a throttle that had just failed back into closed loop at the next start.
        tc.on_engine_stop();
        for (int i=0;i<30;i++) step(30,30);
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_FAULT);
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);
        for (int i=0;i<30;i++) { tc.start_autocal(0); step(30,30); }  // autocal clears the latch
        CHECK(bus.get_bool(SIG_ETB_EN_1) == true || static_cast<int>(bus.get(SIG_ETB_STATE_1)) != ElectronicThrottle::ST_FAULT);
        ElectronicThrottle::set_raw_reader(sim_raw);
    }

    SECTION("L2: lost feedback in closed-loop latches DIS + raises P2135 (correlation/feedback)");
    {
        ElectronicThrottleConfig cfg = make_cfg();
        cfg.etb[0].kp = 1.0f; cfg.etb[0].ki = 2.0f; cfg.etb[0].max_tps_pct = 1000;
        ElectronicThrottle tc; SignalBus bus; DtcManager dtc; tc.set_dtc(&dtc);
        calibrate(tc, bus, cfg);
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_READY);
        for (int i = 0; i < 100; i++) {                    // both TPS go INVALID while armed (past AC_DONE)
            bus.set(SIG_TPS, 0.0f, false, g_ms); bus.set(SIG_AUX_1, 0.0f, false, g_ms);
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms); tc.set_manual(0, 40.0f, 5000);
            run(tc, bus, 10);
        }
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);                       // can't control blind -> EN low
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_FAULT);
        CHECK(dtc.code_severity(0x2135) == 3);                           // P2135 raised, level 3
    }

    SECTION("L2: commanded-but-stuck plate latches DIS + raises P2112 (stuck closed)");
    {
        ElectronicThrottleConfig cfg = make_cfg();
        cfg.etb[0].kp = 1.0f; cfg.etb[0].ki = 2.0f; cfg.etb[0].max_tps_pct = 1000;
        cfg.etb[0].open_rate_pct_s = 300; cfg.etb[0].close_rate_pct_s = 300;
        ElectronicThrottle tc; SignalBus bus; DtcManager dtc; tc.set_dtc(&dtc);
        calibrate(tc, bus, cfg);
        for (int i = 0; i < 120; i++) {                    // target 60, but plate FROZEN at 5 (stuck)
            bus.set(SIG_TPS, 5.0f, true, g_ms); bus.set(SIG_AUX_1, 5.0f, true, g_ms);
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms); tc.set_manual(0, 60.0f, 5000);
            run(tc, bus, 10);
        }
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);                       // stuck -> CUT (don't keep driving)
        CHECK(dtc.code_severity(0x2112) == 3);                           // commanded open + stuck -> P2112
        // autocal recovers: clears the latch AND heals the code
        for (int i = 0; i < 5; i++) { tc.start_autocal(0); bus.set(SIG_ENGINE_STATE,0.0f,true,g_ms); run(tc,bus,10); }
        CHECK(dtc.code_severity(0x2112) == 0);                           // healed
        ElectronicThrottle::set_raw_reader(sim_raw);
    }

    SECTION("key-on gates the throttle: key-off parks it and must NOT latch on absent feedback");
    {
        // With the key off, Sensors::update runs only the bootstrap battery -- every other analog
        // front-end is unpowered -- so the TPS pair publishes NOTHING. To this loop that is
        // indistinguishable from feedback that has died, and it used to latch P2135 within
        // tps_match_ms and stay latched until a findlimits. An ECU sat powered with the key off would
        // come to the key with a throttle already dead.
        ElectronicThrottleConfig cfg = make_cfg();
        cfg.etb[0].kp = 1.0f; cfg.etb[0].ki = 2.0f;
        ElectronicThrottle tc; SignalBus bus;
        DtcManager dtc; tc.set_dtc(&dtc);
        calibrate(tc, bus, cfg);
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_READY);
        // autocal writes its learned outputs into g_config.electronic_throttle. On the target that IS
        // the object cfg_ points at, so the write lands in the module's own config; this harness passes
        // a LOCAL config, so mirror the one field the key-on re-adopt reads back out of it.
        cfg.etb[0].relax_pct = g_config.electronic_throttle.etb[0].relax_pct;
        CHECK(cfg.etb[0].relax_pct != 0);            // findlimits really did learn a spring rest

        g_system_active = false;                      // key off: sensors gated, nothing to read
        for (int i = 0; i < 200; i++) {               // 2 s -- ten times the 200 ms match debounce
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms);
            run(tc, bus, 10);                         // deliberately publish NO tps
        }
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);              // parked: bridge off
        CHECK(dtc.code_severity(0x2135) == 0);                   // and NOT latched on absent feedback

        g_system_active = true;                       // key on: the edge re-arms the stored-cal verify
        bool saw_sweep = false, cut_while_sweeping = true;
        for (int i = 0; i < 600; i++) {
            bus.set(SIG_TPS, g_plate, true, g_ms); bus.set(SIG_AUX_1, g_plate, true, g_ms);
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms);
            run(tc, bus, 10);
            plant_step(bus.get(SIG_ETB_DUTY_1));
            if (static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_AUTOCAL) {
                saw_sweep = true;
                // THE ENGINE MUST BE HELD DISABLED WHILE THE PLATE IS BEING DRIVEN. 12 V switching
                // includes a reset, so a driver who turns the key and goes straight to the starter
                // would otherwise be cranking into a throttle the sweep has just driven wide open.
                if (!bus.valid(SIG_IGN_CUT) || !bus.valid(SIG_FUEL_CUT)) cut_while_sweeping = false;
            }
            if (static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_READY) break;
        }
        CHECK(saw_sweep);                                        // the key-on sweep actually ran
        CHECK(cut_while_sweeping);                               // ... with fuel+spark inhibited throughout
        // READY on the key alone: no findlimits was run, so this also proves nothing latched while the
        // key was off -- a latch would have demanded one.
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_READY);
        // and the sweep COMPARED rather than recalibrated: the stored cal is untouched.
        CHECK(g_config.electronic_throttle.etb[0].relax_pct == cfg.etb[0].relax_pct);
    }

    SECTION("a default 2-point sensor cal is NOT a findlimits — the loop stays open");
    {
        // Every sensor in the catalog ships cal_n = 2, so "cal_n >= 2" was true on a factory ECU that
        // had never swept anything, and the position loop closed against a generic 0-5 V cal instead of
        // the real mechanical stops. relax_pct is the marker findlimits actually writes.
        ElectronicThrottleConfig cfg = make_cfg();          // fresh: relax_pct = 0
        ElectronicThrottle::set_raw_reader(sim_raw);
        ElectronicThrottle tc; SignalBus bus;
        tc.init(cfg);
        CHECK(cfg.etb[0].relax_pct == 0);                   // never calibrated
        for (int i = 0; i < 60; i++) {
            bus.set(SIG_TPS, 30.0f, true, g_ms); bus.set(SIG_AUX_1, 30.0f, true, g_ms);
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms);
            bus.set(SIG_PEDAL_DEMAND, 50.0f, true, g_ms);
            run(tc, bus, 10);
        }
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_UNCAL);
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);         // pedal demand ignored: nothing is driven
    }

    SECTION("a key-on verify must never forgive a latched fault");
    {
        // findlimits doubles as the authorized fail-safe CLEAR -- it drops the latch so a deliberate
        // recal recovers a faulted ETB. The key-on check reuses that same routine, so it MUST NOT
        // inherit that: if it did, every key cycle would quietly clear a fault that is supposed to
        // require intervention, and the latch would mean nothing at all.
        ElectronicThrottleConfig cfg = make_cfg();
        ElectronicThrottle tc; SignalBus bus;
        DtcManager dtc; tc.set_dtc(&dtc);
        calibrate(tc, bus, cfg);
        cfg.etb[0].relax_pct = g_config.electronic_throttle.etb[0].relax_pct;

        // Latch a genuine A/B disagreement: tracks far apart, held past the debounce.
        for (int i = 0; i < 60; i++) {
            bus.set(SIG_TPS, 20.0f, true, g_ms); bus.set(SIG_AUX_1, 80.0f, true, g_ms);
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms);
            run(tc, bus, 10);
        }
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_FAULT);

        tc.start_autocal(0, /*verify=*/true);       // the automatic key-on check
        for (int i = 0; i < 600; i++) {
            bus.set(SIG_TPS, g_plate, true, g_ms); bus.set(SIG_AUX_1, g_plate, true, g_ms);
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms);
            run(tc, bus, 10);
            plant_step(bus.get(SIG_ETB_DUTY_1));
        }
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_FAULT);   // still latched
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);                                          // still dead

        // A DELIBERATE findlimits still clears it -- that is the authorized path.
        tc.start_autocal(0, /*verify=*/false);
        for (int i = 0; i < 600; i++) {
            bus.set(SIG_TPS, g_plate, true, g_ms); bus.set(SIG_AUX_1, g_plate, true, g_ms);
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms);
            run(tc, bus, 10);
            plant_step(bus.get(SIG_ETB_DUTY_1));
            if (static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_READY) break;
        }
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_READY);
    }

    SECTION("a key-on sweep that cannot move the plate latches a code, not a silent UNCAL");
    {
        // A seized plate or a dead motor/bridge: the key-on sweep never sees the plate travel, times out
        // on the open stop, and used to leave the body at UNCAL with nothing in the fault table.
        ElectronicThrottleConfig cfg = make_cfg();
        ElectronicThrottle tc; SignalBus bus;
        DtcManager dtc; tc.set_dtc(&dtc);
        calibrate(tc, bus, cfg);
        cfg.etb[0].relax_pct = g_config.electronic_throttle.etb[0].relax_pct;
        for (int i = 0; i < 60; i++) {              // let findlimits finish: one routine at a time
            bus.set(SIG_TPS, g_plate, true, g_ms); bus.set(SIG_AUX_1, g_plate, true, g_ms);
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms);
            run(tc, bus, 10);
        }

        tc.start_autocal(0, /*verify=*/true);       // the automatic key-on check
        for (int i = 0; i < 600; i++) {             // 6 s, past the 3 s step timeout; NO plant_step: seized
            bus.set(SIG_TPS, g_plate, true, g_ms); bus.set(SIG_AUX_1, g_plate, true, g_ms);
            bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms);
            run(tc, bus, 10);
        }
        CHECK(dtc.code_severity(0x2112) == 3);                                               // stuck closed
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_FAULT);
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);
    }

    SECTION("ETB2 exists: routines are single-owner, and the key parks both bodies");
    {
        // The sweep/autotune accumulators are shared and tagged with ff_etb_/at_etb_. A second body
        // starting a routine on top of the first used to steal them mid-sweep, so a table could be
        // written from the other throttle's crossings -- and the single g_command_state meant the
        // first routine's reply never came.
        ElectronicThrottleConfig cfg = make_cfg();
        cfg.etb[1].enabled           = 1;          // the second body is configured and live
        cfg.etb[1].tps_a_src         = SIG_TPS;
        cfg.etb[1].tps_b_src         = SIG_AUX_1;
        cfg.etb[1].tps_match_err_pct = 100;
        cfg.etb[1].tps_match_ms      = 200;
        ElectronicThrottle::set_raw_reader(sim_raw);
        ElectronicThrottle tc; SignalBus bus;
        tc.init(cfg);

        tc.start_autocal(0);
        bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms); run(tc, bus, 10);
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_AUTOCAL);

        tc.start_autocal(1);                       // must be REFUSED while body 1 is sweeping
        bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms); run(tc, bus, 10);
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_2)) != ElectronicThrottle::ST_AUTOCAL);
        CHECK(static_cast<int>(bus.get(SIG_ETB_STATE_1)) == ElectronicThrottle::ST_AUTOCAL);  // undisturbed

        // Key off parks BOTH bodies, not just the first.
        g_system_active = false;
        for (int i = 0; i < 50; i++) { bus.set(SIG_ENGINE_STATE, 0.0f, true, g_ms); run(tc, bus, 10); }
        CHECK(bus.get_bool(SIG_ETB_EN_1) == false);
        CHECK(bus.get_bool(SIG_ETB_EN_2) == false);
        g_system_active = true;
    }

    return test_summary();
}
