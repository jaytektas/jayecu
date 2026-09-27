// Host test for TorqueModel — table-based brake-torque estimate + spark/lambda trims + power.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/TorqueModel.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"
#include "../generated/ecu_config.h"
#include <cmath>

static TorqueModelConfig make_cfg() {
    TorqueModelConfig c = g_config.torque_model;   // the SHIPPED defaults, not zero — a zeroed axis has no live bin count
    c.enabled = 1;
    c.best_torque_lambda = 880;                        // 0.88
    // base torque table: rpm x MAP, uniform 200 Nm for a predictable check
    c.torque_table_x_src = SIG_RPM;
    c.torque_table_y_src = SIG_MAP;
    c.torque_rpm_axis_n  = 8;
    for (int i=0;i<8;i++) c.torque_rpm_axis[i]  = 800.0f + i*950.0f;   // 800..7450
    for (int i=0;i<8;i++) c.torque_load_axis[i] = 20.0f + i*30.0f;     // 20..230
    for (int i=0;i<128;i++) c.torque_table[i] = 200;
    // spark trim: index by spark_retard_total, default flat 100% (no effect)
    c.torque_spark_trim_x_src = SIG_SPARK_RETARD_TOTAL;
    c.torque_spark_trim_y_en  = 0;
    for (int i=0;i<8;i++) c.torque_spark_axis[i] = i*5.0f;             // 0,5,10..35
    for (int i=0;i<8;i++) c.torque_spark_trim[i] = 100;
    // lambda trim: index by lambda_torque_ratio, default flat 100%
    c.torque_lambda_trim_x_src = SIG_LAMBDA_TORQUE_RATIO;
    c.torque_lambda_trim_y_en  = 0;
    const float lax[8] = {0.70f,0.85f,0.95f,1.00f,1.10f,1.25f,1.50f,1.80f};
    for (int i=0;i<8;i++) c.torque_lambda_axis[i] = lax[i];
    for (int i=0;i<8;i++) c.torque_lambda_trim[i] = 100;
    return c;
}
struct R { float t; float p; float ratio; };
static R step(TorqueModel& m, SignalBus& b, float rpm, float map, float retard, float lambda) {
    EnginePosition pos{}; pos.rpm=rpm; EngineFrame f{};
    b.set(SIG_RPM, rpm); b.set(SIG_MAP, map); b.set(wk::rpm, rpm); b.set(wk::map, map);
    b.set(SIG_SPARK_RETARD_TOTAL, retard);
    if (lambda > 0) b.set(wk::lambda, lambda);
    m.update(pos,b,f);
    return { b.get(SIG_ENGINE_TORQUE_NM,-1.0f), b.get(SIG_ENGINE_POWER_KW,-1.0f), b.get(SIG_LAMBDA_TORQUE_RATIO,-1.0f) };
}
int main() {
    fprintf(stdout, "=== TorqueModel ===\n");

    SECTION("base torque from table; power = T*omega (flat trims)");
    { auto c=make_cfg(); TorqueModel m; m.init(c); SignalBus b{};
      R r = step(m,b,3000,100,0,0.88f);
      CHECK_NEAR(r.t, 200.0f, 1.0f);
      const float expect_kw = 200.0f * (3000.0f*0.104719755f) * 0.001f;  // ~62.8 kW
      CHECK_NEAR(r.p, expect_kw, 0.5f); }

    SECTION("power scales with rpm at constant torque");
    { auto c=make_cfg(); TorqueModel m; m.init(c); SignalBus b{};
      float p3 = step(m,b,3000,100,0,0.88f).p;
      float p6 = step(m,b,6000,100,0,0.88f).p;
      CHECK_NEAR(p6, 2.0f*p3, 0.5f); }

    SECTION("spark retard trims torque");
    { auto c=make_cfg();
      for (int i=0;i<8;i++) c.torque_spark_trim[i] = 100 - i*10;   // 100,90,80.. over 0,5,10..
      TorqueModel m; m.init(c); SignalBus b{};
      R r = step(m,b,3000,100,10,0.88f);      // 10 deg -> axis bin[2]=80% -> 160 Nm
      CHECK_NEAR(r.t, 160.0f, 1.0f); }

    SECTION("lambda ratio published + trims torque; peak set by best_torque_lambda");
    { auto c=make_cfg();
      const uint16_t lt[8] = {85,95,99,100,96,85,65,45};
      for (int i=0;i<8;i++) c.torque_lambda_trim[i] = lt[i];
      TorqueModel m; m.init(c); SignalBus b{};
      // lambda == best_torque_lambda -> ratio 1.0 -> 100%
      R r0 = step(m,b,3000,100,0,0.88f);
      CHECK_NEAR(r0.ratio, 1.0f, 0.001f);
      CHECK_NEAR(r0.t, 200.0f, 1.0f);
      // lean to lambda 1.10 -> ratio 1.25 -> curve bin[5]=85% -> 170 Nm
      R r1 = step(m,b,3000,100,0,1.10f);
      CHECK_NEAR(r1.ratio, 1.25f, 0.001f);
      CHECK_NEAR(r1.t, 170.0f, 1.0f);
      // Different fuel: shift best_torque_lambda; the same *ratio* curve now peaks at the new lambda.
      auto c85 = c; c85.best_torque_lambda = 800;   // 0.80
      TorqueModel m85; m85.init(c85); SignalBus b85{};
      R r2 = step(m85,b85,3000,100,0,0.80f);        // lambda == peak -> ratio 1.0 -> 100%
      CHECK_NEAR(r2.ratio, 1.0f, 0.001f);
      CHECK_NEAR(r2.t, 200.0f, 1.0f); }

    SECTION("trims compound (spark x lambda)");
    { auto c=make_cfg();
      for (int i=0;i<8;i++) c.torque_spark_trim[i]  = 80;   // flat 80% spark
      for (int i=0;i<8;i++) c.torque_lambda_trim[i] = 50;   // flat 50% lambda
      TorqueModel m; m.init(c); SignalBus b{};
      R r = step(m,b,3000,100,10,0.88f);                    // 200 * 0.8 * 0.5 = 80 Nm
      CHECK_NEAR(r.t, 80.0f, 1.0f); }

    SECTION("near-zero rpm -> zero power");
    { auto c=make_cfg(); TorqueModel m; m.init(c); SignalBus b{};
      CHECK_NEAR(step(m,b,50,100,0,0.88f).p, 0.0f, 0.01f); }

    SECTION("disabled -> zeros, ratio neutral");
    { auto c=make_cfg(); c.enabled=0; TorqueModel m; m.init(c); SignalBus b{};
      R r = step(m,b,4000,120,0,0.88f);
      CHECK_NEAR(r.t, 0.0f, 0.01f); CHECK_NEAR(r.p, 0.0f, 0.01f); CHECK_NEAR(r.ratio, 1.0f, 0.01f); }

    return test_summary();
}
