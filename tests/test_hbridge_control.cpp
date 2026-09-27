// Host test for HBridge — two independent half-bridge slots, each driven by its own demand signal
// from the bus and gated by a fail-safe ENABLE signal. DC unipolar/bipolar mapping, authority clamp,
// dir_invert, enable gate, missing-signal DTCs (unconfigured -> config level 1 shown engine-off;
// configured-invalid -> module per-code severity, key-on gated), engine-stop de-energise.
// Runs against fake IHBridges, no hardware required.
//   build: tests/CMakeLists.txt -> ctest -R hbridge_control
#include "test_helpers.h"
#include "../firmware/Engine/Modules/HBridge.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../firmware/Diagnostics/DtcManager.h"
#include "../firmware/Diagnostics/Dtc.h"
#include "../generated/module_dtc.h"
#include "../generated/signal_ids.h"
#include "../generated/well_known_signals.h"   // wk::engine_state
#include "../firmware/Engine/EngineStateMachine.h"  // EngineRunState

struct FakeBridge final : public IHBridge {
    float    cmd  = 999.0f;
    uint32_t freq = 0;
    bool     en   = false;
    void set_freq(uint32_t hz) noexcept override { freq = hz; }
    void drive(float s)        noexcept override { cmd = s; }
    void enable(bool on)       noexcept override { en = on; }
};

static EnginePosition POS{};
static EngineFrame    FR{};

// Arbitrary bus lines used as the per-half enable signals in these tests (a real tune wires
// a producer's enable output here). 255 = unconfigured.
static constexpr uint8_t EN_A = SIG_ETB_EN_1;
static constexpr uint8_t EN_B = SIG_ETB_EN_2;

static HBridgeConfig base_cfg() {
    HBridgeConfig c{};
    c.pwm_freq_hz = 20000;
    c.half[0].enabled = 1; c.half[0].demand_sig = SIG_IDLE_DUTY; c.half[0].enable_sig = EN_A; c.half[0].dc_max_pct = 1000;
    c.half[1].enabled = 1; c.half[1].demand_sig = SIG_IDLE_DUTY; c.half[1].enable_sig = EN_B; c.half[1].dc_max_pct = 1000;
    return c;
}

// Assert the enable line of every enabled half so the gate lets the bridge drive.
static void assert_enables(SignalBus& s, const HBridgeConfig& c) {
    for (int i = 0; i < 2; ++i)
        if (c.half[i].enabled && c.half[i].enable_sig != 255)
            s.set(static_cast<SignalId>(c.half[i].enable_sig), 1.0f, true);
}

int main() {
    fprintf(stdout, "=== HBridge ===\n");

    SECTION("carrier applied to both bridges on init");
    {
        FakeBridge a, b;
        HBridge ctl; ctl.init(base_cfg(), &a, &b);
        CHECK(a.freq == 20000 && b.freq == 20000);
    }

    SECTION("slot 0 unipolar: 0..100% demand -> 0..max duty on bridge A");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.half[0].dc_map = 0; cfg.half[0].dc_max_pct = 1000;
        cfg.half[1].enabled = 0;
        HBridge ctl; ctl.init(cfg, &a, &b);

        auto drive = [&](float d) {
            SignalBus s{}; s.set(SIG_IDLE_DUTY, d, true); assert_enables(s, cfg);
            ctl.update(POS, s, FR); return a.cmd;
        };
        CHECK_NEAR(drive(0.0f),   0.0f,   0.01f);
        CHECK(a.en);
        CHECK_NEAR(drive(50.0f),  50.0f,  0.01f);
        CHECK_NEAR(drive(100.0f), 100.0f, 0.01f);
        CHECK_NEAR(drive(150.0f), 100.0f, 0.01f);   // demand clamped
    }

    SECTION("slot 0 authority ceiling (dc_max_pct)");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.half[0].dc_map = 0; cfg.half[0].dc_max_pct = 800;
        cfg.half[1].enabled = 0;
        HBridge ctl; ctl.init(cfg, &a, &b);
        SignalBus s{}; s.set(SIG_IDLE_DUTY, 100.0f, true); assert_enables(s, cfg);
        ctl.update(POS, s, FR);
        CHECK_NEAR(a.cmd, 80.0f, 0.01f);
    }

    SECTION("slot 1 bipolar: 50%=stop, 100%=+max, 0%=-max on bridge B");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.half[0].enabled = 0;
        cfg.half[1].dc_map = 1; cfg.half[1].dc_max_pct = 1000;
        HBridge ctl; ctl.init(cfg, &a, &b);

        auto drive = [&](float d) {
            SignalBus s{}; s.set(SIG_IDLE_DUTY, d, true); assert_enables(s, cfg);
            ctl.update(POS, s, FR); return b.cmd;
        };
        CHECK_NEAR(drive(50.0f),    0.0f,   0.01f);
        CHECK_NEAR(drive(100.0f), 100.0f,   0.01f);
        CHECK_NEAR(drive(0.0f),  -100.0f,   0.01f);
    }

    SECTION("dir_invert negates the command");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.half[0].dc_map = 0; cfg.half[0].dir_invert = 1;
        cfg.half[1].enabled = 0;
        HBridge ctl; ctl.init(cfg, &a, &b);
        SignalBus s{}; s.set(SIG_IDLE_DUTY, 80.0f, true); assert_enables(s, cfg);
        ctl.update(POS, s, FR);
        CHECK_NEAR(a.cmd, -80.0f, 0.01f);
    }

    SECTION("enable gate: bridge drives only while the enable signal is asserted (fail-safe)");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.half[1].enabled = 0;
        HBridge ctl; ctl.init(cfg, &a, &b);
        SignalBus s{}; s.set(SIG_IDLE_DUTY, 80.0f, true);

        s.set(static_cast<SignalId>(EN_A), 0.0f, true);
        ctl.update(POS, s, FR); CHECK(!a.en);    // enable low  -> bridge off
        s.set(static_cast<SignalId>(EN_A), 1.0f, true);
        ctl.update(POS, s, FR); CHECK(a.en);     // enable high -> bridge drives
    }

    SECTION("unconfigured enable (255): bridge held off + config-WARN DTC, shown even engine-off");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.half[0].enable_sig = -1;   // not wired
        cfg.half[1].enabled = 0;
        DtcManager dtc; dtc.init(1);
        dtc.set_active(false);          // engine off / bench — tuning time
        HBridge ctl; ctl.init(cfg, &a, &b); ctl.set_dtc(&dtc);

        SignalBus s{}; s.set(SIG_IDLE_DUTY, 80.0f, true);
        ctl.update(POS, s, FR);
        CHECK(!a.en);   // fail-safe: no enable configured -> never drives
        // CONFIG-source code is accepted while the system is inactive (bench), at WARN.
        CHECK(dtc.code_severity(ModuleDtc::HBRIDGE_A_ENABLE) == DTC_SEV_LEVEL1);
    }

    SECTION("configured-but-invalid enable: per-code severity DTC, but key-on gated");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.half[1].enabled = 0;
        DtcManager dtc; dtc.init(1);
        HBridge ctl; ctl.init(cfg, &a, &b); ctl.set_dtc(&dtc);

        SignalBus s{}; s.set(SIG_IDLE_DUTY, 80.0f, true);
        // EN_A never set valid on the bus -> configured but invalid.
        dtc.set_active(false);
        ctl.update(POS, s, FR);
        CHECK(!a.en);
        CHECK(dtc.code_severity(ModuleDtc::HBRIDGE_A_ENABLE) == 0);   // MODULE source suppressed while inactive

        dtc.set_active(true);
        ctl.update(POS, s, FR);
        CHECK(dtc.code_severity(ModuleDtc::HBRIDGE_A_ENABLE) == ModuleDtc::HBRIDGE_A_ENABLE_SEV);
    }

    SECTION("healthy half raises no DTC");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.half[1].enabled = 0;
        DtcManager dtc; dtc.init(1);
        HBridge ctl; ctl.init(cfg, &a, &b); ctl.set_dtc(&dtc);
        SignalBus s{}; s.set(SIG_IDLE_DUTY, 80.0f, true); assert_enables(s, cfg);
        ctl.update(POS, s, FR);
        CHECK(a.en);
        CHECK(dtc.active_count() == 0);
    }

    SECTION("both slots independent: different signals, different mappings");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.half[0].demand_sig = SIG_ETB_DUTY_1; cfg.half[0].dc_map = 1;
        cfg.half[1].demand_sig = SIG_ETB_DUTY_2; cfg.half[1].dc_map = 0;
        HBridge ctl; ctl.init(cfg, &a, &b);

        SignalBus s{};
        s.set(SIG_ETB_DUTY_1, 100.0f, true);   // bipolar 100% -> +100 on A
        s.set(SIG_ETB_DUTY_2, 50.0f,  true);   // unipolar 50% ->  +50 on B
        assert_enables(s, cfg);
        ctl.update(POS, s, FR);
        CHECK_NEAR(a.cmd, 100.0f, 0.01f);
        CHECK_NEAR(b.cmd,  50.0f, 0.01f);
    }

    SECTION("on_engine_stop de-energises both bridges");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        HBridge ctl; ctl.init(cfg, &a, &b);
        SignalBus s{}; s.set(SIG_IDLE_DUTY, 80.0f, true); assert_enables(s, cfg);
        ctl.update(POS, s, FR); CHECK(a.en && b.en);
        ctl.on_engine_stop();
        CHECK(!a.en && !b.en);
    }

    SECTION("carrier re-applied on on_config_change (simulates shadow-bit path)");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.pwm_freq_hz = 20000;
        HBridge ctl; ctl.init(cfg, &a, &b);
        CHECK(a.freq == 20000 && b.freq == 20000);
        cfg.pwm_freq_hz = 5000;
        ctl.on_config_change(cfg);
        CHECK(a.freq == 5000 && b.freq == 5000);
    }

    // ---- bench nudge ("prove it moves") -----------------------------------------------------------
    // The whole point of it is that it needs NEITHER a demand signal nor an asserted enable: it answers
    // "does this actuator move at all" before any producer is trustworthy. What it must still respect is
    // the half's own mapping, the engine state, and its own expiry.

    SECTION("bench nudge drives with no demand signal and no enable asserted");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.half[0].demand_sig = -1;                     // nothing publishes to this half
        cfg.half[0].enable_sig = -1;
        HBridge ctl; ctl.init(cfg, &a, &b);
        SignalBus s{};
        ctl.update(POS, s, FR); CHECK(!a.en);            // unconfigured -> off, as before
        ctl.set_manual(0, 40.0f, 4000u);
        ctl.update(POS, s, FR);
        CHECK(a.en);
        CHECK_NEAR(a.cmd, 40.0f, 0.01f);                 // unipolar, full authority
    }

    SECTION("bench nudge goes through dc_map and dir_invert, like a real demand");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.half[0].dc_map = 1;                          // bipolar: 50 = stop
        HBridge ctl; ctl.init(cfg, &a, &b);
        SignalBus s{};
        ctl.set_manual(0, 40.0f, 4000u);                 // a SIGNED 40 read as bipolar is nearly stop...
        ctl.update(POS, s, FR);
        CHECK_NEAR(a.cmd, -20.0f, 0.01f);                // ... which is how a wrong DC map shows itself
        cfg.half[0].dc_map = 0; cfg.half[0].dir_invert = 1;
        ctl.on_config_change(cfg);
        ctl.set_manual(0, 40.0f, 4000u);
        ctl.update(POS, s, FR);
        CHECK_NEAR(a.cmd, -40.0f, 0.01f);                // inverted direction
    }

    SECTION("bench nudge is refused while the engine runs, and dropped mid-nudge by a start");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        HBridge ctl; ctl.init(cfg, &a, &b);
        SignalBus s{};
        ctl.set_manual(0, 40.0f, 4000u);
        ctl.update(POS, s, FR); CHECK(a.en);             // engine stopped: drives
        s.set(wk::engine_state, static_cast<float>(static_cast<int>(EngineRunState::RUNNING)), true);
        ctl.update(POS, s, FR); CHECK(!a.en);            // running: dropped
        s.set(wk::engine_state, 0.0f, true);
        ctl.update(POS, s, FR); CHECK(!a.en);            // and NOT resumed when it stops again
    }

    SECTION("a zero-length nudge is an immediate release");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        HBridge ctl; ctl.init(cfg, &a, &b);
        SignalBus s{};
        ctl.set_manual(0, 40.0f, 4000u);
        ctl.update(POS, s, FR); CHECK(a.en);
        ctl.set_manual(0, 0.0f, 0u);                     // what the "Off" button sends
        ctl.update(POS, s, FR); CHECK(!a.en);
    }

    SECTION("the bridge publishes what it is doing, bench nudge included");
    {
        FakeBridge a, b;
        HBridgeConfig cfg = base_cfg();
        cfg.half[0].demand_sig = -1; cfg.half[0].enable_sig = -1;   // no producer at all
        HBridge ctl; ctl.init(cfg, &a, &b);
        SignalBus s{};
        ctl.update(POS, s, FR);
        CHECK(s.get(SIG_HBRIDGE_EN_1, -1.0f) == 0.0f);              // off, and says so
        CHECK_NEAR(s.get(SIG_HBRIDGE_DUTY_1, -1.0f), 0.0f, 0.01f);
        ctl.set_manual(0, -40.0f, 4000u);
        ctl.update(POS, s, FR);
        CHECK(s.get(SIG_HBRIDGE_EN_1, -1.0f) == 1.0f);              // the nudge shows on the bridge's own
        CHECK_NEAR(s.get(SIG_HBRIDGE_DUTY_1, 0.0f), -40.0f, 0.01f); // channels, which no producer publishes
    }

    return test_summary();
}
