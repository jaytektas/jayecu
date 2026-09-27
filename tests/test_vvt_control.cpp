// Host test for VvtControl — 4-loop cam phasing with table-driven gains/base-duty (Coolant-temp),
// target scalar, overall correction; plus mode/bank gating, direction, dead band, slew, duty min/max.
#include "test_helpers.h"
#include <algorithm>
#include "../firmware/Engine/Modules/VvtControl.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../generated/signal_ids.h"
#include "../generated/ecu_config.h"   // g_config — the SHIPPED defaults, so a test starts where a user does

static uint32_t g_ms = 0;
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }

// Learned region stub — a live RAM buffer of 4×CLT_BINS floats (no per-block magic); VVT's block is at
// region offset 0. On real HW the SD totem restore fills it before init; here the test seeds it.
void platform_learned_reset();   // the one host learned region (platform_hal_stub)
static void resetRegion() { platform_learned_reset(); }   // zero the LTT cells → neutral/cold

static void fill(uint16_t* t, unsigned n, uint16_t v) { for (unsigned i = 0; i < n; i++) t[i] = v; }
// The exhaust target is SIGNED (degrees of advance; an exhaust phaser usually moves retard-wards).
static void fill(int16_t* t, unsigned n, int16_t v) { for (unsigned i = 0; i < n; i++) t[i] = v; }

// mode=Both, 2 banks, P-only (P=2.0 over CLT, I=D=0), base duty 0, target scalar 100%, no slew.
// Intake target 25°, exhaust 20°.
static VvtControlConfig make_cfg(uint16_t duty_max = 1000, uint16_t delta = 0) {
    VvtControlConfig c{};
    // The per-cam LTT store is a table; its coolant columns come from its own axis array. It used to be
    // a hard-coded 0..140 span inside the module, which folded every sub-zero reading into bin 0.
    {
        const float clt[] = { 0.0f, 17.5f, 35.0f, 52.5f, 70.0f, 87.5f, 105.0f, 122.5f };
        for (unsigned i = 0; i < sizeof(clt)/sizeof(*clt); ++i) c.vvt_ltt_x_axis[i] = clt[i];
    }
    c.enabled = 1; c.mode = 2; c.num_banks = 2; c.max_delta_rate = delta;
    c.intake_duty_min = 0;  c.intake_duty_max = duty_max;  c.intake_direction = 0;  c.intake_dead_band = 0;  c.intake_overall_corr = 0;
    c.exhaust_duty_min = 0; c.exhaust_duty_max = duty_max; c.exhaust_direction = 0; c.exhaust_dead_band = 0; c.exhaust_overall_corr = 0;
    c.vvt_rpm_axis_n = 8;
    for (int i = 0; i < 8; i++) {
        c.vvt_rpm_axis[i] = 1000.0f * (i + 1); c.vvt_load_axis[i] = 20.0f + i * 20;
        c.vvt_clt_axis[i] = 20.0f + i * 10;    c.vvt_aux_rpm_axis[i] = 1000.0f * (i + 1);
        c.vvt_scalar_clt_axis[i] = 20.0f + i * 10;
    }
    // EVERY AXIS IS RESIZABLE NOW, so every axis has a LIVE BIN COUNT — and zero is not "small", it is
    // unconfigured: TableEval asserts on breakpoints with no count, because a populated axis nobody can
    // look anything up in is a bug rather than an empty table. Declared here rather than seeded from
    // g_config so this stays a CONTROLLED config: the defaults carry real target tables, and a test
    // about the controller would then be measuring those.
    {
        c.vvt_rpm_axis_n = 8; c.vvt_load_axis_n = 8; c.vvt_clt_axis_n = 8;
        c.vvt_aux_rpm_axis_n = 8; c.vvt_scalar_clt_axis_n = 8; c.vvt_ct_axis_n = 8;
        c.vvt_ltt_x_axis_n = 8; c.vvt_ltt_y_axis_n = 8;
    }
    // target tables (1D over rpm)
    c.intake_target_table_x_src  = SIG_RPM; c.intake_target_table_y_en  = 0; fill(c.intake_target_table,  VVT_CONTROL_INTAKE_TARGET_TABLE_ALLOC,  250);
    c.exhaust_target_table_x_src = SIG_RPM; c.exhaust_target_table_y_en = 0; fill(c.exhaust_target_table, VVT_CONTROL_EXHAUST_TARGET_TABLE_ALLOC, 200);
    // gains over CLT: P=2.0, I=D=0
    c.intake_p_gain_x_src  = SIG_CLT; c.intake_p_gain_y_en  = 0; fill(c.intake_p_gain,  VVT_CONTROL_INTAKE_P_GAIN_ALLOC,  2000);
    c.intake_i_gain_x_src  = SIG_CLT; c.intake_i_gain_y_en  = 0;
    c.intake_d_gain_x_src  = SIG_CLT; c.intake_d_gain_y_en  = 0;
    c.exhaust_p_gain_x_src = SIG_CLT; c.exhaust_p_gain_y_en = 0; fill(c.exhaust_p_gain, VVT_CONTROL_EXHAUST_P_GAIN_ALLOC, 2000);
    c.exhaust_i_gain_x_src = SIG_CLT; c.exhaust_i_gain_y_en = 0;
    c.exhaust_d_gain_x_src = SIG_CLT; c.exhaust_d_gain_y_en = 0;
    // base duty over CLT: 0
    c.intake_base_duty_1_x_src  = SIG_CLT; c.intake_base_duty_1_y_en  = 0;
    c.intake_base_duty_2_x_src  = SIG_CLT; c.intake_base_duty_2_y_en  = 0;
    c.exhaust_base_duty_1_x_src = SIG_CLT; c.exhaust_base_duty_1_y_en = 0;
    c.exhaust_base_duty_2_x_src = SIG_CLT; c.exhaust_base_duty_2_y_en = 0;
    // target scalar over CLT: 100%
    c.target_scalar_x_src = SIG_CLT; c.target_scalar_y_en = 0; fill(c.target_scalar, VVT_CONTROL_TARGET_SCALAR_ALLOC, 1000);
    return c;
}

static float step(VvtControl& v, SignalBus& b, uint32_t dt, float a1, float a2, float a3, float a4, int loop) {
    g_ms += dt; EnginePosition p{}; p.rpm = 4000; EngineFrame f{};
    b.set(SIG_MAP, 100.0f); b.set(SIG_CLT, 90.0f);   // warm → target scalar 100%, gains from table
    b.set(SIG_VVT_ANGLE_1, a1); b.set(SIG_VVT_ANGLE_2, a2);
    b.set(SIG_VVT_ANGLE_3, a3); b.set(SIG_VVT_ANGLE_4, a4);
    v.update(p, b, f); return v.duty(loop);
}

int main() {
    fprintf(stdout, "=== VvtControl (table-driven, 4-loop) ===\n");
    g_ms = 1000;

    SECTION("Intake B1 behind target -> P duty (gain from CLT table)");
    { auto c = make_cfg(); VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,15,15,15,15,0);
      CHECK_NEAR(step(v,b,10,15,15,15,15,0), 20.0f, 0.5f); }   // (25-15)*2.0

    SECTION("Exhaust B1 uses the exhaust target + gain tables on loop 1");
    { auto c = make_cfg(); VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,0,10,0,0,1);
      CHECK_NEAR(step(v,b,10,0,10,0,0,1), 20.0f, 0.5f); }      // (20-10)*2.0

    SECTION("Intake B2 runs on loop 2 (bank 2 enabled)");
    { auto c = make_cfg(); VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,0,0,20,0,2);
      CHECK_NEAR(step(v,b,10,0,0,20,0,2), 10.0f, 0.5f); }      // (25-20)*2.0

    SECTION("per-bank base-duty table adds; at target -> just base");
    { auto c = make_cfg(); fill(c.intake_base_duty_1, VVT_CONTROL_INTAKE_BASE_DUTY_1_ALLOC, 120);   // 12%
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,25,0,0,0,0);
      CHECK_NEAR(step(v,b,10,25,0,0,0,0), 12.0f, 0.3f); }

    SECTION("overall correction shifts the target");
    { auto c = make_cfg(); c.intake_overall_corr = 50;        // +5.0° -> target 30
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,20,0,0,0,0);
      CHECK_NEAR(step(v,b,10,20,0,0,0,0), 20.0f, 0.5f); }      // (25+5-20)*2.0

    SECTION("target scalar (warm-up ramp) scales the output");
    { auto c = make_cfg(); fill(c.target_scalar, VVT_CONTROL_TARGET_SCALAR_ALLOC, 500);   // 50%
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,15,0,0,0,0);
      CHECK_NEAR(step(v,b,10,15,0,0,0,0), 10.0f, 0.5f); }      // (25-15)*2.0=20, x0.5 = 10

    SECTION("scaled (warm-up) loop does not wind up: full authority returns without an overshoot");
    { auto c = make_cfg(); c.intake_i_gain_x_src = SIG_CLT;
      for (auto& g : c.intake_i_gain) g = 500;                         // a real integrator
      fill(c.target_scalar, VVT_CONTROL_TARGET_SCALAR_ALLOC, 500);   // 50% authority
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,15,0,0,0,0);
      for (int i = 0; i < 300; i++) step(v,b,10,15,0,0,0,0);          // 3 s behind target, scaled
      fill(c.target_scalar, VVT_CONTROL_TARGET_SCALAR_ALLOC, 1000);  // warm: full authority
      const float d = step(v,b,10,25,0,0,0,0);                         // and now AT target
      fprintf(stdout, "    duty at target after a scaled stretch = %.2f\n", (double)d);
      CHECK(std::fabs(d) < 1.0f); }                                   // no wound-up integrator behind it

    SECTION("closed on a phaser: intake settles on a positive advance, exhaust on a negative one");
    {
        // The measured angle is ADVANCE from home (EnginePositionHal negates the decoder's residual).
        // A phaser integrates: above its hold duty it moves one way, below it the other. The intake
        // parks retarded and duty ADVANCES it; the exhaust parks advanced and duty RETARDS it, so it is
        // set to Direction = Retard and asked for -20 degrees. Both must settle on their targets.
        auto c = make_cfg();
        c.mode = 2; c.num_banks = 1; c.exhaust_direction = 1;
        fill(c.exhaust_target_table, VVT_CONTROL_EXHAUST_TARGET_TABLE_ALLOC, static_cast<int16_t>(-200));
        fill(c.intake_base_duty_1,  VVT_CONTROL_INTAKE_BASE_DUTY_1_ALLOC,  300);   // hold duty 30 %
        fill(c.exhaust_base_duty_1, VVT_CONTROL_EXHAUST_BASE_DUTY_1_ALLOC, 300);
        VvtControl v; v.init(c); SignalBus b{};
        float in = 0.0f, ex = 0.0f;                     // both start parked on their stops
        step(v, b, 0, in, ex, 0, 0, 0);
        for (int i = 0; i < 2000; i++) {                // 20 s at 10 ms
            const float di = step(v, b, 10, in, ex, 0, 0, 0);
            const float de = v.duty(1);
            in = std::clamp(in + (di - 30.0f) * 0.02f, 0.0f, 50.0f);    // duty advances the intake
            ex = std::clamp(ex - (de - 30.0f) * 0.02f, -50.0f, 0.0f);   // duty retards the exhaust
        }
        CHECK_NEAR(in,  25.0f, 0.5f);
        CHECK_NEAR(ex, -20.0f, 0.5f);
    }

    SECTION("no cam angle -> loop holds (no full-error drive)");
    { auto c = make_cfg(); VvtControl v; v.init(c); SignalBus b{};
      g_ms += 10; EnginePosition p{}; p.rpm = 4000; EngineFrame f{};
      b.set(SIG_MAP, 100.0f); b.set(SIG_CLT, 90.0f);                  // no VVT angle published at all
      v.update(p, b, f); g_ms += 10; v.update(p, b, f);
      CHECK_NEAR(v.duty(0), 0.0f, 0.01f); }                           // base 0 + nothing: not (25-0)*2

    SECTION("target slew limit ramps the target");
    { auto c = make_cfg(1000, 500);                            // 50°/s
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,0,0,0,0,0);
      CHECK_NEAR(step(v,b,10,0,0,0,0,0), 1.0f, 0.3f); }        // 0.5° error * 2.0

    SECTION("cam direction Retard flips the loop sign");
    { auto c = make_cfg(); fill(c.intake_base_duty_1, VVT_CONTROL_INTAKE_BASE_DUTY_1_ALLOC, 500);   // 50% base
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,15,0,0,0,0);
      CHECK_NEAR(step(v,b,10,15,0,0,0,0), 70.0f, 0.5f);        // Advance
      auto c2 = make_cfg(); fill(c2.intake_base_duty_1, VVT_CONTROL_INTAKE_BASE_DUTY_1_ALLOC, 500);
      c2.intake_direction = 1;
      VvtControl v2; v2.init(c2); SignalBus b2{};
      step(v2,b2,0,15,0,0,0,0);
      CHECK_NEAR(step(v2,b2,10,15,0,0,0,0), 30.0f, 0.5f); }    // Retard

    SECTION("dead band holds duty within tolerance");
    { auto c = make_cfg(); fill(c.intake_base_duty_1, VVT_CONTROL_INTAKE_BASE_DUTY_1_ALLOC, 400);
      c.intake_dead_band = 50;                                 // 5.0°
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,22,0,0,0,0);
      CHECK_NEAR(step(v,b,10,22,0,0,0,0), 40.0f, 0.3f);        // err 3 < 5 -> base only
      CHECK(step(v,b,10,15,0,0,0,0) > 55.0f); }               // err 10 -> correction

    SECTION("mode = Intake -> exhaust loops off");
    { auto c = make_cfg(); c.mode = 0;
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,0,0,0,0,1);
      CHECK_NEAR(step(v,b,10,0,0,0,0,1), 0.0f, 0.1f);
      CHECK(step(v,b,10,0,0,0,0,0) > 0.0f); }

    SECTION("num_banks = 1 -> bank-2 loops off");
    { auto c = make_cfg(); c.num_banks = 1;
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,0,0,0,0,2);
      CHECK_NEAR(step(v,b,10,0,0,0,0,2), 0.0f, 0.1f); }

    SECTION("LTT learns the hold-trim when settled + enabled");
    { resetRegion();
      auto c = make_cfg();
      c.enable_ltt = 1; c.intake_ltt_gain = 100; c.ltt_authority_pct = 500;   // learn 1.0/s, auth 50%
      fill(c.intake_i_gain, VVT_CONTROL_INTAKE_I_GAIN_ALLOC, 5000);           // I=5.0 → integral builds
      fill(c.intake_p_gain, VVT_CONTROL_INTAKE_P_GAIN_ALLOC, 0);              // P off → trim is the integral
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,24,0,0,0,0);
      for (int k = 0; k < 60; k++) step(v,b,20,24,0,0,0,0);   // ~1.2 s settled 1° behind target
      CHECK(v.ltt(0, 90.0f) > 0.5f); }                        // learned a positive hold trim

    SECTION("LTT disabled -> nothing learned");
    { resetRegion();
      auto c = make_cfg(); c.enable_ltt = 0;
      fill(c.intake_i_gain, VVT_CONTROL_INTAKE_I_GAIN_ALLOC, 5000);
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,24,0,0,0,0);
      for (int k = 0; k < 60; k++) step(v,b,20,24,0,0,0,0);
      CHECK_NEAR(v.ltt(0, 90.0f), 0.0f, 0.01f); }

    SECTION("LTT does not learn outside the settle window");
    { resetRegion();
      auto c = make_cfg();
      c.enable_ltt = 1; c.intake_ltt_gain = 100; c.ltt_authority_pct = 500;
      fill(c.intake_i_gain, VVT_CONTROL_INTAKE_I_GAIN_ALLOC, 5000);
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,0,0,0,0,0);                                  // measured 0 vs target 25 -> err 25 >> 3°
      for (int k = 0; k < 60; k++) step(v,b,20,0,0,0,0,0);
      CHECK_NEAR(v.ltt(0, 90.0f), 0.0f, 0.01f); }

    SECTION("LTT clamps to authority");
    { resetRegion();
      auto c = make_cfg();
      c.enable_ltt = 1; c.intake_ltt_gain = 5000; c.ltt_authority_pct = 50;   // fast learn, auth 5%
      fill(c.intake_i_gain, VVT_CONTROL_INTAKE_I_GAIN_ALLOC, 5000);
      VvtControl v; v.init(c); SignalBus b{};
      step(v,b,0,24,0,0,0,0);
      for (int k = 0; k < 200; k++) step(v,b,20,24,0,0,0,0);
      CHECK(v.ltt(0, 90.0f) <= 5.0f + 0.01f); }               // never exceeds the 5% authority

    SECTION("disabled -> all loops 0");
    { auto c = make_cfg(); c.enabled = 0; VvtControl v; v.init(c); SignalBus b{};
      CHECK_NEAR(step(v,b,10,0,0,0,0,0), 0.0f, 0.1f);
      CHECK_NEAR(step(v,b,10,0,0,0,0,1), 0.0f, 0.1f); }

    return test_summary();
}
