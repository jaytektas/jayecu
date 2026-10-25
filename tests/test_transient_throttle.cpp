#include "test_helpers.h"
#include <algorithm>
#include <utility>
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

    SECTION("a lift ends a tip-in (disenrich off), and the throttle's rebound at the bottom does not re-enrich");
    {
        // Drive 7: stab to 36 %, lift to 1.8 %, the ETB rebounds to 2.7 %. The rate swings -528 -> +32 %/s,
        // a huge positive accel, and the old code re-took the table peak: fuel_corr_accel 1.95 on a lift.
        cfg.enabled = 1; cfg.load_source = 0; cfg.enable_disenrich = 0; cfg.enable_decay = 1; cfg.enable_async = 0;
        TransientThrottle tt; tt.init(cfg);
        EngineFrame f{};
        uint32_t cyc = 0;
        auto step = [&](float tps) {
            g_ms += 10; SignalBus b{}; b.set(SIG_TPS, tps); b.set(SIG_CLT, 80.0f); b.set(SIG_RPM, 1800.0f);
            tt.update(pos(cyc++ / 6), b, f);                       // ~1900 rpm: one engine cycle per 6 frames
            return std::make_pair(b.get(SIG_FUEL_CORR_ACCEL, 1.0f), b.get(SIG_TRANSIENT_ACTIVE, 0.0f));
        };
        g_ms = 6000;
        for (int i = 0; i < 5; ++i) step(4.0f);
        float tip = 1.0f;
        for (float t : {12.0f, 24.0f, 36.0f}) tip = std::max(tip, step(t).first);
        step(36.0f);                                               // lift straight after, as on the road
        CHECK(tip > 1.05f);                                        // the stab enriched
        const float at_lift = step(25.0f).first;                  // first lift frame: the rate filter's lag
        // The drive 7 bug was a RE-PEAK on the lift (1.95). Since 25 Oct the lift no longer zeroes what
        // the tip-in put on — it bleeds off at the Decay Rate — so the guard is: never above where it
        // stood when the lift began, never rising frame to frame, and the event over.
        float worst = 0.0f, prev = at_lift, state_after = -1.0f; bool rose = false;
        for (float t : {12.0f, 1.8f, 2.4f, 2.7f, 2.7f, 2.9f, 3.0f, 3.3f, 3.5f, 3.6f, 3.6f}) {
            auto r = step(t); worst = std::max(worst, r.first); state_after = r.second;
            rose = rose || r.first > prev + 1e-4f; prev = r.first;
        }
        fprintf(stdout, "    tip-in peak %.3f, at lift %.3f, worst during/after lift %.3f, state %.0f\n",
                (double)tip, (double)at_lift, (double)worst, (double)state_after);
        CHECK(worst <= at_lift + 1e-4f);                           // no re-peak on the lift or the rebound
        CHECK(!rose);                                              // only ever bleeding off
        CHECK(state_after == 0.0f);                                // the event ended
        float tail = prev;
        for (int i = 0; i < 120; ++i) tail = step(3.6f).first;    // ~20 engine cycles at rest
        CHECK_NEAR(tail, 1.0, 0.001);                              // ...and it runs out
    }

    SECTION("a stab and lift: the enrichment bleeds off through the lift instead of vanishing (25 Oct)");
    {
        // Kelly, 25 Oct: stab 13 -> 22 %, MAP 67 -> 98 kPa, lift at the top. fuel_corr_accel went from 1.08
        // to 1.000 on the lift's first frame while MAP was still 98 kPa, and lambda 0.99 -> 1.34.
        cfg.enabled = 1; cfg.load_source = 0; cfg.enable_disenrich = 0; cfg.enable_decay = 1; cfg.enable_async = 0;
        TransientThrottle tt; tt.init(cfg);
        EngineFrame f{};
        uint32_t cyc = 0;
        auto step = [&](float tps) {
            g_ms += 10; SignalBus b{}; b.set(SIG_TPS, tps); b.set(SIG_CLT, 85.0f); b.set(SIG_RPM, 1500.0f);
            b.set(SIG_MAP, 95.0f);
            tt.update(pos(cyc++ / 8), b, f);                       // 1500 rpm: one engine cycle per 8 frames
            return b.get(SIG_FUEL_CORR_ACCEL, 1.0f);
        };
        g_ms = 9000;
        for (int i = 0; i < 5; ++i) step(13.0f);
        float tip = 1.0f;
        for (float t : {16.0f, 19.0f, 22.0f}) tip = std::max(tip, step(t));
        float before_lift = step(22.0f);
        CHECK(tip > 1.02f);
        float l1 = step(18.0f), l2 = step(12.0f), l3 = step(8.0f);  // the lift
        fprintf(stdout, "    peak %.3f, before lift %.3f, lift frames %.3f %.3f %.3f\n",
                (double)tip, (double)before_lift, (double)l1, (double)l2, (double)l3);
        CHECK(l2 > 1.0f + 1e-3f);                                  // still there on the lift, not zeroed
        CHECK(l3 <= l2 + 1e-4f && l2 <= l1 + 1e-4f);               // and only falling
        float later = l3;
        for (int i = 0; i < 80; ++i) later = step(8.0f);           // 10 engine cycles on
        CHECK(later < l3);                                         // bled off by the Decay Rate
    }

    SECTION("decay switched off: a lift still ends the enrichment at once (nothing to hand it to)");
    {
        cfg.enabled = 1; cfg.load_source = 0; cfg.enable_disenrich = 0; cfg.enable_decay = 0; cfg.enable_async = 0;
        TransientThrottle tt; tt.init(cfg);
        EngineFrame f{};
        auto step = [&](float tps) {
            g_ms += 10; SignalBus b{}; b.set(SIG_TPS, tps); b.set(SIG_CLT, 85.0f); b.set(SIG_RPM, 1500.0f);
            tt.update(pos(0), b, f); return b.get(SIG_FUEL_CORR_ACCEL, 1.0f);
        };
        g_ms = 12000;
        for (int i = 0; i < 5; ++i) step(13.0f);
        for (float t : {16.0f, 19.0f, 22.0f, 22.0f}) step(t);
        step(18.0f); const float l2 = step(12.0f);
        CHECK_NEAR(l2, 1.0, 0.001);
        cfg.enable_decay = 1;
    }

    SECTION("a new stab on the tail of a lifted one is a tip-in of its own: it adds, and arms async");
    {
        cfg.enabled = 1; cfg.load_source = 0; cfg.enable_disenrich = 0; cfg.enable_decay = 1;
        cfg.enable_async = 1; cfg.max_async_pulses = 2; cfg.async_holdoff_ms = 0;
        TransientThrottle tt; tt.init(cfg);
        uint32_t cyc = 0;
        auto step = [&](float tps, EngineFrame& f) {
            g_ms += 10; SignalBus b{}; b.set(SIG_TPS, tps); b.set(SIG_CLT, 85.0f); b.set(SIG_RPM, 1500.0f);
            b.set(SIG_BASE_PW, 3000.0f); b.set(SIG_PW_ADD_DEADTIME, 900.0f);
            tt.update(pos(cyc++ / 8), b, f); return b.get(SIG_FUEL_CORR_ACCEL, 1.0f);
        };
        g_ms = 15000;
        EngineFrame f{};
        for (int i = 0; i < 5; ++i) step(13.0f, f);
        for (float t : {16.0f, 19.0f, 22.0f, 22.0f}) step(t, f);
        step(18.0f, f); step(12.0f, f); const float tail = step(8.0f, f);
        CHECK(tail > 1.0f + 1e-3f);
        float again = tail; bool async = false;
        for (float t : {14.0f, 22.0f, 30.0f}) {
            EngineFrame g{}; again = std::max(again, step(t, g)); async = async || g.async_inj_pulses > 0;
        }
        fprintf(stdout, "    tail %.3f, second stab %.3f, async %d\n", (double)tail, (double)again, (int)async);
        CHECK(again >= tail - 1e-4f);                              // the tail is not thrown away by the new push
        CHECK(async);                                              // and it is a real tip-in: async armed
        cfg.enable_async = 0;
    }

    SECTION("a wiggle on boost, inside an event that began at a closed throttle, is judged from where it began");
    {
        // Drive 7: reopen from 3.6 % (start load latched 4.2), settle at 30 %, then a 30 -> 39 % wiggle.
        // Old: enriched off the from-idle row (+140 %). Now: the push re-latches its start at ~30 %.
        cfg.enabled = 1; cfg.load_source = 0; cfg.enable_disenrich = 0; cfg.enable_decay = 1; cfg.enable_async = 0;
        TransientThrottle tt; tt.init(cfg);
        EngineFrame f{};
        uint32_t cyc = 0;
        float start_seen = -1.0f;
        auto step = [&](float tps) {
            g_ms += 10; SignalBus b{}; b.set(SIG_TPS, tps); b.set(SIG_CLT, 80.0f); b.set(SIG_RPM, 4200.0f);
            tt.update(pos(cyc++ / 3), b, f);
            start_seen = b.get(SIG_TT_START_LOAD, -1.0f);
            return b.get(SIG_FUEL_CORR_ACCEL, 1.0f);
        };
        g_ms = 9000;
        for (int i = 0; i < 5; ++i) step(3.6f);
        for (float t : {10.0f, 16.0f, 20.0f, 21.0f}) step(t);          // the reopen: a real tip-in from closed
        for (int i = 0; i < 12; ++i) step(30.0f);                       // settle, event still decaying
        float peak = 1.0f, start = 0.0f;
        for (float t : {31.9f, 35.8f, 39.0f}) { peak = std::max(peak, step(t)); start = std::max(start, start_seen); }
        fprintf(stdout, "    wiggle 30 -> 39 %%: start load %.1f, peak fuel_corr_accel %.3f\n", (double)start, (double)peak);
        CHECK(start > 25.0f);                                            // judged from ~30 %, not from 3.6 %
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

    SECTION("async pulses: the percentage is of FUEL, asked for as spray time (the scheduler adds the opening price)");
    {
        // base_pw includes the dead time, so scaling it scaled the opening time too. The squirt is SPRAY
        // time: the scheduler adds the opening price only when it opens an injector (test_injector_overlap).
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
        CHECK_NEAR(with, none, 1.0);                   // same spray whatever the dead time: it is not the module's to add
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
