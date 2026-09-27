// IgnitionTrim — the slow advance-correction producer. Computes one table per frame (round-robin)
// and publishes wk::ign_advance_trim, which Ignition reads. This is where the CLT/IAT/etc
// corrections live now (they used to be inline in Ignition). Round-robin means a full
// refresh takes N (=9) update() calls.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/IgnitionTrim.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../generated/signal_ids.h"

static IgnitionConfig make_cfg() {
    IgnitionConfig c{};
    // The corrections each have a switch now, and a zero-initialised struct has them all OFF — a real
    // tune ships them on (the tables are neutral until given values), so a test that wants a correction
    // evaluated has to say so, exactly as the fuel tests do.
    c.enable_clt = c.enable_iat = c.enable_fuelcomp = c.enable_gear = c.enable_poststart = 1;
    c.enable_generic1 = c.enable_generic2 = c.enable_generic3 = c.enable_generic4 = 1;
    c.enable_revlimit = 1;
    c.clt_ign_corr_table_x_src = SIG_CLT; c.clt_ign_corr_table_y_src = SIG_MAP; c.clt_ign_corr_table_y_en = 0;
    c.clt_axis_n = 14; c.clt_adv_map_axis_n = 8;
    for (int i = 0; i < 16; i++) { c.clt_ign_corr_table[i] = 0; c.clt_axis[i] = static_cast<float>(-40 + i * 10); }
    return c;
}
static EnginePosition make_pos(float rpm = 3000.0f) { EnginePosition p{}; p.rpm = rpm; return p; }
static SignalBus make_bus(float clt_c) { SignalBus b{}; b.set(SIG_MAP, 100.0f); b.set(SIG_CLT, clt_c); return b; }

int main() {
    fprintf(stdout, "=== IgnitionTrim ===\n");

    SECTION("publishes wk::ign_advance_trim; CLT cold retards via the trim");
    {
        auto cfg = make_cfg();
        cfg.clt_ign_corr_table[0] = -50;   // -5.0° at clt_axis[0] = -40°C

        IgnitionTrim it; it.init(cfg);
        EngineFrame f{};

        SignalBus warm = make_bus(80.0f);
        for (int k = 0; k < 9; k++) it.update(make_pos(), warm, f);   // round-robin: refresh all tables
        const float t_warm = warm.get(SIG_IGN_ADVANCE_TRIM);

        SignalBus cold = make_bus(-40.0f);
        for (int k = 0; k < 9; k++) it.update(make_pos(), cold, f);
        const float t_cold = cold.get(SIG_IGN_ADVANCE_TRIM);

        fprintf(stdout, "    trim warm=%g  cold=%g\n", (double)t_warm, (double)t_cold);
        CHECK(t_cold < t_warm);                 // cold pulls the trim negative (retard)
        CHECK_NEAR(t_cold, -5.0f, 0.1f);        // exactly the -5° CLT cell, others 0
        CHECK_NEAR(t_warm,  0.0f, 0.1f);
    }

    SECTION("one table per frame — trim builds up over the round-robin, not all at once");
    {
        auto cfg = make_cfg();
        cfg.clt_ign_corr_table[0] = -50;   // CLT is round-robin index 0
        IgnitionTrim it; it.init(cfg);
        EngineFrame f{};
        SignalBus b = make_bus(-40.0f);
        it.update(make_pos(), b, f);              // first frame: only index 0 (CLT) refreshed
        CHECK_NEAR(b.get(SIG_IGN_ADVANCE_TRIM), -5.0f, 0.1f);   // CLT already in on frame 1
    }

    SECTION("every term is published on its own channel, and they add up to the total");
    {
        auto cfg = make_cfg();
        cfg.clt_ign_corr_table[0]     = -50;   // -5.0 deg at the cold end
        cfg.igngen1_ign_corr_table[0] =  20;   // +2.0 deg from a generic table
        IgnitionTrim it; it.init(cfg);
        EngineFrame f{};
        SignalBus b = make_bus(-40.0f);
        for (int k = 0; k < 9; k++) it.update(make_pos(), b, f);

        // The two that were given values say so, by name — which is the whole point: the total alone
        // cannot tell a tuner that coolant pulled 5 degrees while a generic table gave 2 back.
        CHECK_NEAR(b.get(SIG_IGN_CORR_CLT),      -5.0f, 0.1f);
        CHECK_NEAR(b.get(SIG_IGN_CORR_GENERIC1),  2.0f, 0.1f);
        CHECK_NEAR(b.get(SIG_IGN_CORR_IAT),       0.0f, 0.1f);   // untouched tables contribute nothing

        // And the parts are the total: these are the same values the sum was built from, not a second
        // evaluation that could drift from it.
        const float parts = b.get(SIG_IGN_CORR_CLT) + b.get(SIG_IGN_CORR_IAT)
                          + b.get(SIG_IGN_CORR_FUELCOMP) + b.get(SIG_IGN_CORR_GEAR)
                          + b.get(SIG_IGN_CORR_GENERIC1) + b.get(SIG_IGN_CORR_GENERIC2)
                          + b.get(SIG_IGN_CORR_GENERIC3) + b.get(SIG_IGN_CORR_GENERIC4)
                          + b.get(SIG_IGN_CORR_POSTSTART);
        CHECK_NEAR(parts, b.get(SIG_IGN_ADVANCE_TRIM), 0.01f);
    }

    return test_summary();
}
