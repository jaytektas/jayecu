#include "test_helpers.h"
#include "../firmware/Engine/Modules/TransientThrottle.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../generated/signal_ids.h"
#include "../generated/ecu_config.h"   // g_config — default-seeded TransientThrottle tables + axes

static uint32_t g_ms = 0;
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }

static EnginePosition pos(uint32_t cyc) { EnginePosition p{}; p.rpm = 3000.0f; p.cycle_count = cyc; return p; }

int main() {
    fprintf(stdout, "=== TransientThrottle ===\n");
    auto& cfg = g_config.transient_throttle;   // seeded defaults (8 tables: rate/sync/decay/ign/disenrich*/clt)

    SECTION("tip-in (rising TPS) enriches fuel_corr_accel > 1, publishes load rate, decays per cycle");
    {
        cfg.enabled = 1; cfg.load_source = 0; cfg.tps_src = SIG_TPS; cfg.map_src = SIG_IMAP;
        cfg.enable_decay = 1; cfg.enable_disenrich = 0;
        TransientThrottle tt; tt.init(cfg);
        EngineFrame f{};
        g_ms = 1000;
        { SignalBus b{}; b.set(SIG_TPS, 10.0f); b.set(SIG_CLT, 80.0f); b.set(SIG_RPM, 3000.0f); tt.update(pos(0), b, f); }
        g_ms = 1010;                                   // +10 ms; TPS 10->30 = 2000 %/s (filtered)
        float peak = 1.0f, rate = 0.0f;
        { SignalBus b{}; b.set(SIG_TPS, 30.0f); b.set(SIG_CLT, 80.0f); b.set(SIG_RPM, 3000.0f); tt.update(pos(1), b, f);
          peak = b.get(SIG_FUEL_CORR_ACCEL, 1.0f); rate = b.get(SIG_TT_LOAD_RATE, 0.0f); }
        fprintf(stdout, "    tip-in peak=%.3f rate=%.0f/s active=%.0f\n",
                (double)peak, (double)rate, (double)0.0f);
        CHECK(rate > 25.0f);                           // load rate published, past the dead band
        CHECK(peak > 1.02f);                           // enrichment applied
        // Hold TPS flat, advance engine cycles -> per-ECyc decay bleeds it back toward neutral.
        float decayed = peak;
        for (uint32_t c = 2; c < 40; ++c) {
            g_ms += 10; SignalBus b{}; b.set(SIG_TPS, 30.0f); b.set(SIG_CLT, 80.0f); b.set(SIG_RPM, 3000.0f);
            tt.update(pos(c), b, f); decayed = b.get(SIG_FUEL_CORR_ACCEL, 1.0f);
        }
        fprintf(stdout, "    after decay=%.3f\n", (double)decayed);
        CHECK(decayed < peak);
        CHECK(decayed >= 1.0f - 1e-3f);                // enrichment never undershoots neutral
    }

    SECTION("tip-out (falling TPS) with disenrich enabled drops fuel_corr_accel < 1");
    {
        cfg.enabled = 1; cfg.load_source = 0; cfg.enable_disenrich = 1; cfg.enable_decay = 1;
        TransientThrottle tt; tt.init(cfg);
        EngineFrame f{};
        g_ms = 2000;
        { SignalBus b{}; b.set(SIG_TPS, 60.0f); b.set(SIG_RPM, 3000.0f); tt.update(pos(0), b, f); }
        g_ms = 2010;                                   // TPS 60->20 = -4000 %/s
        float v = 1.0f, act = 0.0f;
        { SignalBus b{}; b.set(SIG_TPS, 20.0f); b.set(SIG_RPM, 3000.0f); tt.update(pos(1), b, f);
          v = b.get(SIG_FUEL_CORR_ACCEL, 1.0f); act = b.get(SIG_TRANSIENT_ACTIVE, 0.0f); }
        fprintf(stdout, "    tip-out fuel_corr_accel=%.3f active=%.0f\n", (double)v, (double)act);
        CHECK(v < 1.0f);
        CHECK(act == 2.0f);                            // DISENRICH state published
    }

    SECTION("disenrich gate off -> closing throttle does nothing");
    {
        cfg.enabled = 1; cfg.load_source = 0; cfg.enable_disenrich = 0;
        TransientThrottle tt; tt.init(cfg);
        EngineFrame f{};
        g_ms = 2500;
        { SignalBus b{}; b.set(SIG_TPS, 60.0f); b.set(SIG_RPM, 3000.0f); tt.update(pos(0), b, f); }
        g_ms = 2510;
        float v = 1.0f;
        { SignalBus b{}; b.set(SIG_TPS, 20.0f); b.set(SIG_RPM, 3000.0f); tt.update(pos(1), b, f);
          v = b.get(SIG_FUEL_CORR_ACCEL, 1.0f); }
        CHECK_NEAR(v, 1.0, 0.001);                     // disenrich disabled -> neutral
    }

    SECTION("async enabled -> tip-in arms an async injection burst (pulses + per-pulse pw)");
    {
        cfg.enabled = 1; cfg.load_source = 0; cfg.enable_async = 1; cfg.max_async_pulses = 2;
        cfg.async_holdoff_ms = 0;
        TransientThrottle tt; tt.init(cfg);
        g_ms = 4000;
        { EngineFrame f0{}; SignalBus b{}; b.set(SIG_TPS, 10.0f); b.set(SIG_RPM, 3000.0f);
          b.set(SIG_BASE_PW, 10000.0f); tt.update(pos(0), b, f0); }
        g_ms = 4010;                                   // tip-in edge
        EngineFrame f{};
        { SignalBus b{}; b.set(SIG_TPS, 30.0f); b.set(SIG_RPM, 3000.0f); b.set(SIG_BASE_PW, 10000.0f);
          tt.update(pos(1), b, f); }
        fprintf(stdout, "    async pulses=%u per-pulse pw=%u us\n",
                (unsigned)f.async_inj_pulses, (unsigned)f.async_inj_pw_us);
        CHECK(f.async_inj_pulses == 2);                // max_async_pulses
        CHECK(f.async_inj_pw_us > 0);                  // non-zero squirt
        // A second tick (still ENRICH, no fresh trigger) must NOT re-arm — async is edge-triggered.
        g_ms = 4020;
        EngineFrame f2{};
        { SignalBus b{}; b.set(SIG_TPS, 30.0f); b.set(SIG_RPM, 3000.0f); b.set(SIG_BASE_PW, 10000.0f);
          tt.update(pos(2), b, f2); }
        CHECK(f2.async_inj_pulses == 0);               // no re-fire without a new tip-in edge
    }

    SECTION("async pulses: the percentage is of FUEL, and every pulse carries its own dead time");
    {
        // base_pw includes the dead time, so scaling it scaled the opening time too; and each async
        // pulse is a separate opening, so each needs the dead time or a short burst delivers nothing.
        cfg.enabled = 1; cfg.load_source = 0; cfg.enable_async = 1; cfg.max_async_pulses = 2;
        cfg.async_holdoff_ms = 0;
        auto burst = [&](float dead) {
            TransientThrottle tt; tt.init(cfg);
            g_ms += 1000;
            { EngineFrame f0{}; SignalBus b{}; b.set(SIG_TPS, 10.0f); b.set(SIG_RPM, 3000.0f);
              b.set(SIG_BASE_PW, 5000.0f + dead); b.set(SIG_PW_ADD_DEADTIME, dead); tt.update(pos(0), b, f0); }
            g_ms += 10;
            EngineFrame f{};
            { SignalBus b{}; b.set(SIG_TPS, 30.0f); b.set(SIG_RPM, 3000.0f);
              b.set(SIG_BASE_PW, 5000.0f + dead); b.set(SIG_PW_ADD_DEADTIME, dead); tt.update(pos(1), b, f); }
            return static_cast<double>(f.async_inj_pw_us);
        };
        const double none = burst(0.0f), with = burst(1000.0f);
        fprintf(stdout, "    per-pulse pw: no dead time %g us, 1000 us dead time %g us\n", none, with);
        CHECK(none > 0.0);
        CHECK_NEAR(with - none, 1000.0, 1.0);          // same fuel, plus one dead time per pulse
    }

    SECTION("disabled -> neutral fuel_corr_accel 1.0 + ign_corr_transient 0");
    {
        cfg.enabled = 0;
        TransientThrottle tt; tt.init(cfg);
        EngineFrame f{}; g_ms = 3000;
        SignalBus b{}; b.set(SIG_TPS, 50.0f); tt.update(pos(0), b, f);
        CHECK_NEAR(b.get(SIG_FUEL_CORR_ACCEL, 1.0f), 1.0, 0.001);
        CHECK_NEAR(b.get(SIG_IGN_CORR_TRANSIENT, 0.0f), 0.0, 0.001);
    }

    return test_summary();
}
