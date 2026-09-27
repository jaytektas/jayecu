#include "test_helpers.h"
void platform_learned_reset();   // the one host learned region (platform_hal_stub)

#include "../firmware/Engine/Modules/Idle.h"
#include "../firmware/Engine/EngineStateMachine.h"   // EngineRunState
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../generated/signal_ids.h"
#include "../generated/ecu_config.h"
#include "../generated/learned_layout.h"   // LEARNED_REGION_USED / LEARNED_IDLE_LTT_OFFSET

static uint32_t g_ms = 0;
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }

// Build a config that mirrors the reference default surfaces (1D X curves; every 2nd axis off).
static IdleConfig make_cfg() {
    IdleConfig c = g_config.idle;   // the SHIPPED defaults, not zero — a zeroed axis has no live bin count
    c.enabled = 1;
    c.mode = 0;                       // open loop unless a test flips it
    c.tps_closed_pct = 20;
    c.idle_lockout_rpm = 2000;
    c.cl_activation_offset_rpm = 150;
    c.pid_scaler_pct = 1000;
    c.max_duty_pct = 900;

    // axis channels (X = primary; Y off everywhere)
    c.base_duty_table_x_src = SIG_CLT;          c.base_duty_table_y_src = SIG_CLT;          c.base_duty_table_y_en = 0;
    c.target_rpm_table_x_src = SIG_CLT;         c.target_rpm_table_y_src = SIG_VEHICLE_SPD; c.target_rpm_table_y_en = 0;
    c.start_target_offset_table_x_src = SIG_RUN_TIME; c.start_target_offset_table_y_src = SIG_CLT; c.start_target_offset_table_y_en = 0;
    c.start_base_offset_table_x_src = SIG_RUN_TIME;   c.start_base_offset_table_y_src = SIG_CLT;   c.start_base_offset_table_y_en = 0;
    c.min_output_table_x_src = SIG_RPM;         c.min_output_table_y_src = SIG_CLT;         c.min_output_table_y_en = 0;
    c.p_gain_table_x_src = SIG_IDLE_RPM_ERROR;  c.p_gain_table_y_src = SIG_CLT;             c.p_gain_table_y_en = 0;
    c.i_gain_table_x_src = SIG_IDLE_RPM_ERROR;  c.i_gain_table_y_src = SIG_CLT;             c.i_gain_table_y_en = 0;
    c.d_gain_table_x_src = SIG_IDLE_RPM_ERROR;  c.d_gain_table_y_src = SIG_CLT;             c.d_gain_table_y_en = 0;
    c.idle_ign_corr_table_x_src = SIG_IDLE_RPM_ERROR; c.idle_ign_corr_table_y_src = SIG_CLT;          c.idle_ign_corr_table_y_en = 0; c.idle_ign_corr_table_x_en = 1;  // X now optional -> enable it (default tune ships on)
    c.throttle_follower_target_table_x_src = SIG_RPM;  c.throttle_follower_target_table_y_src = SIG_TPS;   // always-2D
    c.throttle_follower_decay_table_x_src = SIG_RPM;   c.throttle_follower_decay_table_y_src = SIG_CLT;  c.throttle_follower_decay_table_y_en = 0;

    // resizable X axis live bin counts
    c.idle_x_axis_n = 16;   // base/start-offset CLT axis (16-point reference surface below)
    c.start_runtime_axis_n = 6;
    c.min_rpm_axis_n = 7;
    c.gain_err_axis_n = 7;
    c.igncorr_err_axis_n = 7;
    c.tf_tps_axis_n = 5;
    c.tfd_rpm_axis_n = 6;

    // --- breakpoints + row-0 cells (reference surfaces) ---
    const float xclt[16] = {-30,-20,-10,0,10,20,30,40,50,60,70,75,80,85,90,100};
    const uint16_t base[16] = {830,775,720,667,577,488,455,371,351,351,319,300,280,286,301,301}; // x0.1 %
    for (int i = 0; i < 16; i++) { c.idle_x_axis[i] = xclt[i]; c.base_duty_table[i] = base[i]; }

    const float tclt[8] = {-10,0,20,40,60,70,80,100};
    const uint16_t tgt[8] = {1400,1350,1200,950,950,950,950,950};
    for (int i = 0; i < 8; i++) { c.target_clt_axis[i] = tclt[i]; c.target_rpm_table[i] = tgt[i]; }

    const float rt[6] = {0,2,4,6,8,10};
    const uint16_t st[6] = {350,350,300,100,50,0};
    const uint16_t sb[6] = {40,20,20,10,10,0};   // x0.1 %
    for (int i = 0; i < 6; i++) { c.start_runtime_axis[i] = rt[i]; c.start_target_offset_table[i] = st[i]; c.start_base_offset_table[i] = sb[i]; }

    const float mr[7] = {1000,1500,2000,3000,4000,6000,8000};
    const uint16_t mo[7] = {0,50,100,150,200,300,450};   // x0.1 %
    for (int i = 0; i < 7; i++) { c.min_rpm_axis[i] = mr[i]; c.min_output_table[i] = mo[i]; }

    const float ge[7] = {-250,-50,-20,0,20,50,250};     // zero error ON a breakpoint
    const uint16_t pg[7] = {20,20,20,20,20,20,20};      // 0.20 %/100rpm
    const uint16_t ig[7] = {500,250,150,100,150,250,500};  // %/(100rpm·s), symmetric about target
    for (int i = 0; i < 7; i++) { c.gain_err_axis[i] = ge[i]; c.p_gain_table[i] = pg[i]; c.i_gain_table[i] = ig[i]; c.d_gain_table[i] = 0; }

    const float ce[7] = {-300,-200,-100,-20,20,100,200};
    const int16_t ic[7] = {-50,-20,-10,-2,2,20,60};    // deg x0.1; rpm low(+err)->advance
    for (int i = 0; i < 7; i++) { c.igncorr_err_axis[i] = ce[i]; c.idle_ign_corr_table[i] = ic[i]; }

    const float tfr[8] = {750,1000,1500,2000,3000,4000,5000,6000};
    for (int i = 0; i < 8; i++) c.tf_rpm_axis[i] = tfr[i];
    const float tft[5] = {0,5,10,25,50}; for (int i = 0; i < 5; i++) c.tf_tps_axis[i] = tft[i];
    const uint16_t tfc[40] = {   // x0.1 %, row-major y(TPS) outer / x(RPM) inner (reference follower surface)
        0,0,10,20,31,50,70,90,            10,16,37,48,74,112,170,190,
        24,39,61,80,127,190,230,270,      55,69,84,99,155,250,280,350,
        63,86,102,119,175,270,370,450 };
    for (int i = 0; i < 40; i++) c.throttle_follower_target_table[i] = tfc[i];
    const float tfdr[6] = {1000,2000,3000,4000,5000,6000};
    for (int i = 0; i < 6; i++) { c.tfd_rpm_axis[i] = tfdr[i]; c.throttle_follower_decay_table[i] = 150; }  // 15.0 %/s

    const float lca[8] = {-20,0,20,40,60,80,100,110};
    for (int i = 0; i < 8; i++) c.ltt_clt_axis[i] = lca[i];
    return c;
}

static EnginePosition make_pos(float rpm) { EnginePosition p{}; p.rpm = rpm; return p; }

// run_time high so the decaying start offsets are 0 (steady idle); rpm bus signal == pos.rpm.
static SignalBus make_bus(float clt, float tps, EngineRunState st, float rpm, float run_time = 20.0f) {
    SignalBus b{};
    b.set(SIG_CLT, clt);
    b.set(SIG_TPS, tps);
    b.set(SIG_ENGINE_STATE, static_cast<float>(st));
    b.set(SIG_RPM, rpm);
    b.set(SIG_RUN_TIME, run_time);
    return b;
}

int main() {
    fprintf(stdout, "=== Idle (Stage 1b) ===\n");

    SECTION("open-loop base position table -> idle_duty across CLT (reference base surface)");
    {
        auto cfg = make_cfg(); Idle ic; ic.init(cfg); EngineFrame f{};
        auto duty = [&](float clt){ SignalBus b = make_bus(clt, 0.0f, EngineRunState::RUNNING, 850.0f);
            ic.update(make_pos(850.0f), b, f); return b.get(SIG_IDLE_DUTY, -1.0f); };
        CHECK_NEAR(duty(-30.0f), 83.0, 1e-3);   // coldest cell
        CHECK_NEAR(duty(0.0f),   66.7, 1e-3);
        CHECK_NEAR(duty(80.0f),  28.0, 1e-3);
        CHECK_NEAR(duty(100.0f), 30.1, 1e-3);   // hottest cell
        CHECK_NEAR(duty(5.0f),   62.2, 1e-3);   // interp halfway 0(66.7)->10(57.7)
    }

    SECTION("start offsets decay over run-time (added to base/target)");
    {
        auto cfg = make_cfg(); Idle ic; ic.init(cfg); EngineFrame f{};
        // at run_time 0: base += start_base(0)=4, target += start_target(0)=350
        SignalBus b0 = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 850.0f, /*run_time*/0.0f);
        ic.update(make_pos(850.0f), b0, f);
        CHECK_NEAR(b0.get(SIG_IDLE_DUTY, -1.0f), 32.0, 1e-3);          // base 28 + start_base 4
        CHECK_NEAR(b0.get(SIG_IDLE_TARGET_RPM, -1.0f), 950.0 + 350.0, 1e-3);
        // at run_time 10: both offsets decayed to 0
        SignalBus b1 = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 850.0f, /*run_time*/10.0f);
        ic.update(make_pos(850.0f), b1, f);
        CHECK_NEAR(b1.get(SIG_IDLE_DUTY, -1.0f), 28.0, 1e-3);
        CHECK_NEAR(b1.get(SIG_IDLE_TARGET_RPM, -1.0f), 950.0, 1e-3);
    }

    SECTION("min-output floor clamps duty up vs RPM");
    {
        auto cfg = make_cfg(); Idle ic; ic.init(cfg); EngineFrame f{};
        // hot (base ~28) but spin at 6000 RPM -> min_output(6000)=30 forces the floor up.
        SignalBus b = make_bus(80.0f, 50.0f, EngineRunState::RUNNING, 6000.0f);  // tps open: open loop
        ic.update(make_pos(6000.0f), b, f);
        CHECK_NEAR(b.get(SIG_IDLE_DUTY, -1.0f), 30.0, 1e-3);   // base 28 clamped up to floor 30
    }

    SECTION("min output set ABOVE max duty: the ceiling wins, never the floor");
    {
        // Both settings run to 100% and nothing in the schema relates them, so a tune can order them
        // backwards. That fed std::clamp(v, lo, hi) with lo > hi — undefined, and in practice it
        // returned the FLOOR, driving the valve past the ceiling that exists to protect it.
        auto cfg = make_cfg();
        cfg.max_duty_pct = 500;              // 50% ceiling
        cfg.min_output_table[0] = 1000;      // 100% floor at the bottom RPM bin — above the ceiling
        Idle ic; ic.init(cfg); EngineFrame f{};
        SignalBus b = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 850.0f);   // base@80 = 28
        ic.update(make_pos(850.0f), b, f);
        CHECK_NEAR(b.get(SIG_IDLE_DUTY, -1.0f), 50.0, 1e-3);   // collapsed to the ceiling, not 100
    }

    SECTION("closed loop drives duty up when RPM is below target");
    {
        auto cfg = make_cfg(); cfg.mode = 1; Idle ic; ic.init(cfg); EngineFrame f{};
        // clt 80 -> target 950, base 28; idle at 850 (100 RPM low). Step the loop and watch the trim grow.
        float duty = 0.0f;
        for (int i = 0; i <= 50; i++) {
            g_ms = 1000 + i * 30;
            SignalBus b = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 850.0f);
            ic.update(make_pos(850.0f), b, f);
            duty = b.get(SIG_IDLE_DUTY, -1.0f);
            CHECK(b.get(SIG_IDLE_RPM_ERROR, 0.0f) == 100.0f);
        }
        CHECK(duty > 28.0f);            // integral wound positive -> more air than base
        CHECK(duty <= 90.0f);           // within authority
    }

    SECTION("closed loop pulls duty toward the floor when RPM is above target");
    {
        auto cfg = make_cfg(); cfg.mode = 1; Idle ic; ic.init(cfg); EngineFrame f{};
        // clt 80 -> target 950; sit at 1000 (50 high, still within the 150 activation offset).
        float duty = 0.0f;
        for (int i = 0; i <= 80; i++) {
            g_ms = 5000 + i * 30;
            SignalBus b = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 1000.0f);
            ic.update(make_pos(1000.0f), b, f);
            duty = b.get(SIG_IDLE_DUTY, -1.0f);
        }
        CHECK(duty < 28.0f);            // negative trim -> less air than base
        // min_output(1000) = 0, so it can fall below base toward the floor
    }

    SECTION("activation offset: closed loop disengaged far above target -> stays at base");
    {
        auto cfg = make_cfg(); cfg.mode = 1; Idle ic; ic.init(cfg); EngineFrame f{};
        // 1500 RPM, target 950 -> 550 above, beyond the 150 offset and the lockout: loop frozen.
        float duty = 0.0f;
        for (int i = 0; i <= 20; i++) {
            g_ms = 9000 + i * 30;
            SignalBus b = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 1500.0f);
            ic.update(make_pos(1500.0f), b, f);
            duty = b.get(SIG_IDLE_DUTY, -1.0f);
        }
        CHECK_NEAR(duty, 28.0, 1e-3);   // base only, integrator never stepped
    }

    SECTION("idle_active: true only when RUNNING + throttle closed + below lockout RPM");
    {
        auto cfg = make_cfg(); Idle ic; ic.init(cfg); EngineFrame f{};
        auto active = [&](EngineRunState st, float tps, float rpm){ SignalBus b = make_bus(80.0f, tps, st, rpm);
            ic.update(make_pos(rpm), b, f); return b.get_bool(SIG_IDLE_ACTIVE); };
        CHECK(active(EngineRunState::RUNNING,  0.0f,  800.0f)  == true);
        CHECK(active(EngineRunState::CRANKING, 0.0f,  800.0f)  == false);   // not running
        CHECK(active(EngineRunState::RUNNING,  10.0f, 800.0f)  == false);   // throttle open
        CHECK(active(EngineRunState::RUNNING,  0.0f,  3000.0f) == false);   // above lockout
    }

    SECTION("drive-by-wire: the PEDAL decides 'off the throttle', not the plate held open at idle");
    {
        // The plate sits a few percent open at idle on DBW, so "plate below 2 %" never happened and idle
        // control never engaged on any DBW car.
        auto cfg = make_cfg(); Idle ic; ic.init(cfg); EngineFrame f{};
        SignalBus b = make_bus(80.0f, 5.0f, EngineRunState::RUNNING, 800.0f);   // plate 5 % (the idle floor)
        b.set(SIG_PEDAL_DEMAND, 0.0f);                                           // foot off
        ic.update(make_pos(800.0f), b, f);
        CHECK(b.get_bool(SIG_IDLE_ACTIVE) == true);
        SignalBus b2 = make_bus(80.0f, 5.0f, EngineRunState::RUNNING, 800.0f);
        b2.set(SIG_PEDAL_DEMAND, 20.0f);                                         // foot on
        ic.update(make_pos(800.0f), b2, f);
        CHECK(b2.get_bool(SIG_IDLE_ACTIVE) == false);
    }

    SECTION("long-term trim: learns the steady PI trim into the CLT cell + persists across re-init");
    {
        platform_learned_reset();         // neutral/cold region (no learned trim yet)
        auto cfg = make_cfg(); cfg.mode = 1; cfg.ltt_enabled = 1;
        cfg.ltt_min_runtime_s = 0; cfg.ltt_dwell_ms = 0; cfg.ltt_learn_pct = 500; cfg.ltt_authority_pct = 200;
        Idle ic; ic.init(cfg); EngineFrame f{};
        // clt 80 -> target 950; hold rpm 850 (err +200) closed-loop so the PI winds positive and LTT
        // migrates it into the clt=80 cell (rpm is forced, so the error never closes -> LTT saturates).
        for (int i = 0; i <= 500; i++) { g_ms = 1000 + i * 30;
            SignalBus b = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 850.0f);
            ic.update(make_pos(850.0f), b, f); }
        // read the retained trim with learning gated OFF (rpm above the activation offset -> not closed)
        g_ms += 30; SignalBus br = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 2000.0f);
        ic.update(make_pos(2000.0f), br, f);
        const float ltt = br.get(SIG_IDLE_LTT_PCT, -99.0f);
        CHECK(ltt > 10.0f);            // learned a substantial positive base correction
        CHECK(ltt <= 20.0f + 1e-3f);   // clamped to authority

        // Persistence: a fresh controller mapping the same RAM slice keeps the learned trim (not re-zeroed).
        Idle ic2; ic2.init(cfg);
        g_ms += 30; SignalBus b2 = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 2000.0f);
        ic2.update(make_pos(2000.0f), b2, f);
        CHECK_NEAR(b2.get(SIG_IDLE_LTT_PCT, -99.0f), ltt, 1e-3);   // same cell, retained across re-init
    }

    SECTION("throttle follower: rises with TPS, decays on tip-out (dashpot)");
    {
        auto cfg = make_cfg(); cfg.throttle_follower_enabled = 1; Idle ic; ic.init(cfg); EngineFrame f{};
        auto step = [&](float tps, uint32_t ms){ g_ms = ms;
            SignalBus b = make_bus(80.0f, tps, EngineRunState::RUNNING, 2000.0f);
            ic.update(make_pos(2000.0f), b, f); return b.get(SIG_IDLE_FOLLOWER, -1.0f); };
        step(50.0f, 1000);
        CHECK_NEAR(step(50.0f, 1030), 11.9, 1e-3);   // rpm2000 x tps50 follower target -> rises instantly
        // tip-out: tps -> 0 (target at rpm2000/tps0 = 2.0); decays at 15 %/s, not a snap
        float v300 = step(0.0f, 1330);               // 300 ms later: 11.9 - 15*0.3 = 7.4
        CHECK_NEAR(v300, 7.4, 1e-2);
        CHECK_NEAR(step(0.0f, 2030), 2.0, 1e-2);     // long after: bled down to the tps0 floor (2.0)
        CHECK_NEAR(step(50.0f, 2060), 11.9, 1e-3);   // throttle re-opened -> instant rise again
    }

    SECTION("idle ignition correction: spark trim vs RPM error, gated on idle_active");
    {
        auto cfg = make_cfg(); Idle ic; ic.init(cfg); EngineFrame f{};
        // clt 80 -> target 950. error = target - rpm.
        auto corr = [&](float rpm, float tps){ SignalBus b = make_bus(80.0f, tps, EngineRunState::RUNNING, rpm);
            ic.update(make_pos(rpm), b, f); return b.get(SIG_IDLE_IGN_CORR, -99.0f); };
        CHECK_NEAR(corr(750.0f, 0.0f),  6.0,  1e-3);   // err +200 (rpm low) -> +6.0 deg advance
        CHECK_NEAR(corr(1150.0f, 0.0f), -2.0, 1e-3);   // err -200 (rpm high) -> -2.0 deg retard
        CHECK_NEAR(corr(950.0f, 0.0f),  0.0,  1e-3);   // on target -> ~0 (interp 20->-20 through 0)
        CHECK_NEAR(corr(750.0f, 10.0f), 0.0,  1e-3);   // throttle open -> not idle_active -> 0
    }

    SECTION("idle-up: bound signal active adds rpm + base offset (with on-delay + decay)");
    {
        auto cfg = make_cfg();                       // open loop; clt 80 -> base 28.0, target 950
        cfg.idle_up[0].enabled = 1;
        cfg.idle_up[0].input_sig = SIG_IAT;          // any bus signal; iat as a stand-in switch
        cfg.idle_up[0].threshold_x10 = 5;            // active when iat > 0.5
        cfg.idle_up[0].base_offset_x10 = 100;        // +10.0 % duty
        cfg.idle_up[0].rpm_offset = 200;             // +200 RPM target
        cfg.idle_up[0].on_delay_ms = 500;
        cfg.idle_up[0].decay_ms = 1000;
        Idle ic; ic.init(cfg); EngineFrame f{};
        auto step = [&](float iat, uint32_t ms){ g_ms = ms;
            SignalBus b = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 850.0f);
            b.set(SIG_IAT, iat); ic.update(make_pos(850.0f), b, f);
            return std::pair<float,float>{b.get(SIG_IDLE_DUTY,-1), b.get(SIG_IDLE_TARGET_RPM,-1)}; };

        auto a = step(0.0f, 1000);   CHECK_NEAR(a.first, 28.0, 1e-3);  CHECK_NEAR(a.second, 950.0, 1e-3);   // inactive
        step(1.0f, 1100);                                                                                   // active, within on-delay
        auto b = step(1.0f, 1300);   CHECK_NEAR(b.first, 28.0, 1e-3);                                       // 200ms < 500ms on-delay -> not engaged yet
        auto c = step(1.0f, 1700);   CHECK_NEAR(c.first, 38.0, 1e-3);  CHECK_NEAR(c.second, 1150.0, 1e-3);  // past on-delay -> +10% / +200rpm
        step(0.0f, 2000);            auto d = step(0.0f, 2300);                                             // released -> decay over 1000ms
        CHECK(d.first > 28.0f && d.first < 38.0f);                                                          // mid-decay (300ms -> ~70% remaining)
        auto e = step(0.0f, 3500);   CHECK_NEAR(e.first, 28.0, 1e-3);                                       // fully decayed back to base
    }

    SECTION("target rate-limit slews the target instead of stepping");
    {
        auto cfg = make_cfg(); cfg.rpm_rate_rising_limit = 100; cfg.rpm_rate_falling_limit = 100;
        Idle ic; ic.init(cfg); EngineFrame f{};
        g_ms = 1000; SignalBus b0 = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 900.0f);
        ic.update(make_pos(900.0f), b0, f);
        CHECK_NEAR(b0.get(SIG_IDLE_TARGET_RPM, -1.0f), 950.0, 1e-3);    // snaps to raw on first frame
        // jump the table target to 1400 (clt -30); rising limit 100 RPM/s, 100 ms steps -> +10/frame
        g_ms = 1100; SignalBus b1 = make_bus(-30.0f, 0.0f, EngineRunState::RUNNING, 900.0f);
        ic.update(make_pos(900.0f), b1, f);
        CHECK_NEAR(b1.get(SIG_IDLE_TARGET_RPM, -1.0f), 960.0, 1e-3);    // slewed, not stepped to 1400
        g_ms = 1200; SignalBus b2 = make_bus(-30.0f, 0.0f, EngineRunState::RUNNING, 900.0f);
        ic.update(make_pos(900.0f), b2, f);
        CHECK_NEAR(b2.get(SIG_IDLE_TARGET_RPM, -1.0f), 970.0, 1e-3);
    }

    SECTION("decel offset: tip-out air kick that decays");
    {
        auto cfg = make_cfg(); cfg.decel_offset_pct = 100; cfg.decel_decay_ms = 1000;   // open loop, base@80=28
        Idle ic; ic.init(cfg); EngineFrame f{};
        auto duty = [&](float tps, uint32_t ms){ g_ms = ms;
            SignalBus b = make_bus(80.0f, tps, EngineRunState::RUNNING, 850.0f);
            ic.update(make_pos(850.0f), b, f); return b.get(SIG_IDLE_DUTY, -1.0f); };
        duty(50.0f, 1000);                              // throttle open
        CHECK_NEAR(duty(0.0f, 1030), 38.0, 1e-3);       // tip-out edge -> base 28 + 10
        CHECK_NEAR(duty(0.0f, 1530), 33.0, 1e-2);       // 500 ms decay -> +5
        CHECK_NEAR(duty(0.0f, 2100), 28.0, 1e-2);       // fully decayed -> base
    }

    SECTION("stall save: RPM below floor -> full air");
    {
        auto cfg = make_cfg(); cfg.stall_offset_rpm = 900;   // open loop, base@80=28, max 90
        Idle ic; ic.init(cfg); EngineFrame f{};
        auto duty = [&](float rpm){ SignalBus b = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, rpm);
            ic.update(make_pos(rpm), b, f); return b.get(SIG_IDLE_DUTY, -1.0f); };
        CHECK_NEAR(duty(800.0f),  90.0, 1e-3);   // below stall floor -> full air
        CHECK_NEAR(duty(1000.0f), 28.0, 1e-3);   // above -> normal base
    }

    SECTION("vehicle-speed check forces idle_active off above the limit");
    {
        auto cfg = make_cfg(); cfg.vss_check_enabled = 1; cfg.max_vehicle_speed = 10;
        Idle ic; ic.init(cfg); EngineFrame f{};
        auto active = [&](float vss){ SignalBus b = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 850.0f);
            b.set(SIG_VEHICLE_SPD, vss); ic.update(make_pos(850.0f), b, f); return b.get_bool(SIG_IDLE_ACTIVE); };
        CHECK(active(5.0f)  == true);    // below the limit -> idling
        CHECK(active(20.0f) == false);   // moving -> not idling (gates CL/learn/ign-corr)
    }

    SECTION("close-on-boost forces the idle valve shut above the MAP threshold");
    {
        auto cfg = make_cfg(); cfg.close_on_boost_enabled = 1; cfg.close_on_boost_kpa = 105;  // open loop, base@80=28
        Idle ic; ic.init(cfg); EngineFrame f{};
        auto duty = [&](float map){ SignalBus b = make_bus(80.0f, 0.0f, EngineRunState::RUNNING, 850.0f);
            b.set(SIG_MAP, map); ic.update(make_pos(850.0f), b, f); return b.get(SIG_IDLE_DUTY, -1.0f); };
        CHECK_NEAR(duty(50.0f),  28.0, 1e-3);   // vacuum -> normal base
        CHECK_NEAR(duty(150.0f), 0.0,  1e-3);   // boost -> valve closed
    }

    SECTION("disabled -> publishes nothing");
    {
        auto cfg = make_cfg(); cfg.enabled = 0; Idle ic; ic.init(cfg); EngineFrame f{};
        SignalBus b = make_bus(0.0f, 0.0f, EngineRunState::RUNNING, 850.0f); ic.update(make_pos(850.0f), b, f);
        CHECK(b.get(SIG_IDLE_DUTY, -1.0f) == -1.0f);   // never set -> fallback returned
    }

    return test_summary();
}
