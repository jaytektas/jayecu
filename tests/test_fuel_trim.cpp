#include "test_helpers.h"
#include "../firmware/Engine/Modules/FuelTrim.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../generated/signal_ids.h"
#include "../generated/ecu_config.h"   // g_config — default-seeded warmup/iat/... correction tables

int main() {
    fprintf(stdout, "=== FuelTrim ===\n");
    auto& cfg = g_config.fuel_calculator;        // the slow correction tables live in FuelCalculator's config

    SECTION("round-robin publishes all slow corrections; warmup tracks CLT");
    {
        FuelTrim ft; ft.init(cfg);
        EngineFrame f{};
        EnginePosition pos{}; pos.rpm = 3000.0f;
        SignalBus b{};
        // Run a full round-robin pass + margin (9 tables, run 12 frames). Cold CLT → warmup enrichment.
        for (int i = 0; i < 12; i++) { b.set(SIG_CLT, 20.0f); b.set(SIG_IAT, 20.0f); ft.update(pos, b, f); }
        const float warmup = b.get(SIG_FUEL_CORR_WARMUP, 0.0f);
        const float clt_c  = b.get(SIG_CLT_CORR, -999.0f);
        fprintf(stdout, "    warmup@CLT20 = %.3f (clt_corr=%.1f%%)\n", (double)warmup, (double)clt_c);
        CHECK(warmup > 1.0f);                    // default warmup curve enriches when cold
        CHECK(clt_c > 0.0f);                     // raw clt_corr telemetry also published
        // Every slow correction is published every frame (≈neutral 1.0 by default, NOT the -1 default).
        for (SignalId s : {SIG_FUEL_CORR_IAT, SIG_FUEL_CORR_FUELCOMP, SIG_FUEL_CORR_BARO, SIG_FUEL_CORR_GEAR,
                           SIG_FUEL_CORR_GENERIC1, SIG_FUEL_CORR_GENERIC2, SIG_FUEL_CORR_GENERIC3, SIG_FUEL_CORR_GENERIC4})
            CHECK(b.get(s, -1.0f) >= 0.5f);
    }

    SECTION("re-publishes every frame (no staleness between round-robin refreshes)");
    {
        FuelTrim ft; ft.init(cfg);
        EngineFrame f{};
        EnginePosition pos{}; pos.rpm = 3000.0f;
        SignalBus b{};
        for (int i = 0; i < 12; i++) { b.set(SIG_CLT, 60.0f); ft.update(pos, b, f); }
        const float w1 = b.get(SIG_FUEL_CORR_WARMUP, 0.0f);
        // A single further frame (warmup is NOT the round-robin target most frames) still re-publishes it.
        SignalBus b2{}; b2.set(SIG_CLT, 60.0f); ft.update(pos, b2, f);
        const float w2 = b2.get(SIG_FUEL_CORR_WARMUP, 0.0f);
        fprintf(stdout, "    warmup re-published: %.3f -> %.3f\n", (double)w1, (double)w2);
        CHECK_NEAR(w2, w1, 0.05);                // fresh on a brand-new bus despite not being re-evaluated
        CHECK(w2 > 0.5f);                        // published, not stale/default
    }

    SECTION("warmup table scale + range honored (clt_corr_table -> fuel_corr_warmup)");
    {
        cfg.clt_corr_table_y_en = 0;                 // 1D over CLT
        cfg.clt_corr_table[0] = 50;                  // 5.0% (scale 0.1) at the coldest cell
        FuelTrim ft; ft.init(cfg);
        EngineFrame f{}; EnginePosition pos{}; pos.rpm = 3000.0f;
        SignalBus b{};
        for (int i = 0; i < 12; i++) { b.set(SIG_CLT, -50.0f); ft.update(pos, b, f); }
        const float w = b.get(SIG_FUEL_CORR_WARMUP, 1.0f);
        fprintf(stdout, "    cell 50 -> warmup %.4f (expect 1.05, NOT 1.50)\n", (double)w);
        CHECK_NEAR(w, 1.05, 0.001);                  // scale honored: 50 = 5.0%, not 50%

        cfg.clt_corr_table[0] = 1000;                // 100.0% — impossible under the old uint8 cap
        FuelTrim ft2; ft2.init(cfg);
        SignalBus b2{};
        for (int i = 0; i < 12; i++) { b2.set(SIG_CLT, -50.0f); ft2.update(pos, b2, f); }
        const float w2 = b2.get(SIG_FUEL_CORR_WARMUP, 1.0f);
        fprintf(stdout, "    cell 1000 -> warmup %.4f (expect 2.0)\n", (double)w2);
        CHECK_NEAR(w2, 2.0, 0.001);                  // +100%
    }

    return test_summary();
}
