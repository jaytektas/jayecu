#include "test_helpers.h"
#include "../firmware/Engine/Modules/Misfire.h"
#include "../firmware/Diagnostics/DtcManager.h"
#include "../firmware/Scheduler/SegmentTimer.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"
#include "../generated/learned_layout.h"

extern "C" uint32_t g_stub_tick_ms;

void platform_learned_reset();   // the one host learned region (platform_hal_stub)
static void reset_region() { platform_learned_reset(); }

static const AngleDeg10 CYCLE = 7200;
static const AngleDeg10 TDC4[4] = { 0, 1800, 3600, 5400 };

static MisfireConfig make_cfg(bool enabled) {
    MisfireConfig c{};
    c.enabled             = enabled ? 1 : 0;
    c.threshold_pct       = 80;     // 8.0% longer than its neighbours (scale 0.1 -> fraction 0.08)
    c.min_rpm             = 600;
    c.max_rpm             = 7000;
    c.events_to_dtc       = 3;
    c.window_cycles       = 1000;   // long enough that decay does not interfere with these tests
    c.multi_cyl_for_p0300 = 2;
    c.learn_enabled       = 1;
    c.learn_rate          = 500;    // 0.5 — fast, so a test converges in a few cycles
    c.cut_fuel            = 0;
    return c;
}

// Feed `cycles` engine cycles of segments straight into the timer, with per-cylinder length factors.
// factor[c] > 1 makes cylinder c's segment longer — which is what a misfire looks like, and also what
// a narrow tooth looks like. Telling those apart is the point of the whole module.
struct Rig {
    SegmentTimer st;
    Misfire      mf;
    DtcManager   dtc;
    SignalBus    bus{};
    EnginePosition pos{};
    EngineFrame  frame{};
    uint32_t tick = 0;
    AngleDeg10 angle = 0;

    void configure(const MisfireConfig& cfg) {
        st.configure(TDC4, 4, CYCLE);
        dtc.init(1);
        mf.init(cfg);
        mf.set_timer(&st);
        mf.set_dtc(&dtc);
        pos.rpm = 2000;
        bus.set(wk::rpm, 2000.0f);
    }
    void spin(int cycles, const float* factor, bool overrun = false) {
        const AngleDeg10 pitch = 200;                 // 20 deg -> 36 teeth/cycle -> 9 per segment
        for (int cy = 0; cy < cycles; ++cy) {
            for (int t = 0; t < 36; ++t) {
                // Which segment are we in? boundaries at 0/180/360/540 -> cylinder index by angle.
                const int seg = angle / 1800;
                const float f = factor ? factor[seg] : 1.0f;
                tick += static_cast<uint32_t>(1000.0f * f);
                angle = static_cast<AngleDeg10>((angle + pitch) % CYCLE);
                st.on_tooth(angle, tick, pitch);
            }
            if (overrun) bus.set(wk::fuel_cut, 1.0f, true, g_stub_tick_ms, 1000);
            else         bus.invalidate(wk::fuel_cut);
            mf.update(pos, bus, frame);
        }
    }
};

int main() {
    fprintf(stdout, "=== Misfire ===\n");
    const float even[4]      = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float cyl1_slow[4] = { 1.0f, 1.30f, 1.0f, 1.0f };   // cylinder 1 not firing
    const float cyl2_bias[4] = { 1.0f, 1.0f, 1.12f, 1.0f };   // cylinder 2's segment always reads long

    SECTION("a healthy engine produces no misfire events");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Rig r; r.configure(cfg);
        r.spin(20, even);
        fprintf(stdout, "  seen=%u total=%u rough0=%.4f\n", r.mf.segments_seen(), r.mf.total(), r.mf.roughness(0));
        CHECK(r.mf.segments_seen() > 40);
        CHECK(r.mf.total() == 0);
    }

    SECTION("switching the module off retires the code it raised");
    {
        // Misfire's codes LATCH — a tally of events that happened, which nothing re-asserts once the
        // engine runs cleanly — so the table's ttl cannot retire them and this module has to. It never
        // did: P0300/P030x stayed current until the next key-off, switched off or not.
        reset_region();
        auto cfg = make_cfg(true);
        Rig r; r.configure(cfg);
        r.spin(20, cyl1_slow);
        fprintf(stdout, "  active while enabled: %u\n", r.dtc.active_count());
        CHECK(r.dtc.active_count() > 0);
        // Untick it the way the ECU does — the module holds a pointer to the live config, so this IS
        // what a tune write looks like from in here. Re-init would reset the edge state and prove nothing.
        cfg.enabled = 0;
        r.spin(1, even);
        fprintf(stdout, "  active after unticking: %u\n", r.dtc.active_count());
        CHECK(r.dtc.active_count() == 0);
    }

    SECTION("a cylinder that does not fire is caught, and named");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Rig r; r.configure(cfg);
        r.spin(20, cyl1_slow);
        fprintf(stdout, "  rough: %.3f %.3f %.3f %.3f  events1=%u\n",
                r.mf.roughness(0), r.mf.roughness(1), r.mf.roughness(2), r.mf.roughness(3), r.mf.events(1));
        CHECK(r.mf.events(1) > 0);
        CHECK(r.mf.events(0) == 0);
        CHECK(r.mf.events(2) == 0);
        CHECK(r.mf.roughness(1) > 0.08f);
    }

    SECTION("ACCELERATION is not a misfire -- every segment shortens together");
    {
        // The reason the metric is relative. Under acceleration every segment gets shorter at once,
        // so a detector comparing against an absolute or a lagging average would call it a misfire on
        // every cylinder. Comparing against the NEIGHBOURS makes the common-mode change cancel.
        reset_region();
        auto cfg = make_cfg(true);
        Rig r; r.configure(cfg);
        const AngleDeg10 pitch = 200;
        float speed = 1.0f;
        for (int cy = 0; cy < 30; ++cy) {
            for (int t = 0; t < 36; ++t) {
                speed *= 0.995f;                       // continuous, hard acceleration
                r.tick += static_cast<uint32_t>(1000.0f * speed);
                r.angle = static_cast<AngleDeg10>((r.angle + pitch) % CYCLE);
                r.st.on_tooth(r.angle, r.tick, pitch);
            }
            r.mf.update(r.pos, r.bus, r.frame);
        }
        fprintf(stdout, "  under acceleration: total=%u rough=%.4f\n", r.mf.total(), r.mf.roughness(0));
        CHECK(r.mf.total() == 0);
    }

    // ---- The wheel correction: geometry is not combustion ----------------------------------------

    SECTION("an uncorrected wheel bias reads EXACTLY like a permanent misfire");
    {
        // Establish the problem before asserting the fix, so the fix is measured against something.
        reset_region();
        auto cfg = make_cfg(true);
        cfg.learn_enabled = 0;
        Rig r; r.configure(cfg);
        r.spin(20, cyl2_bias);
        fprintf(stdout, "  uncorrected: events2=%u rough2=%.3f\n", r.mf.events(2), r.mf.roughness(2));
        CHECK(r.mf.events(2) > 0);          // a cylinder that is firing perfectly well
    }

    SECTION("overrun teaches the geometry, and the bias stops reading as a misfire");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Rig r; r.configure(cfg);
        // OVERRUN: nothing is firing, so the 12% spread is pure geometry. Learn it, judge nothing.
        r.spin(40, cyl2_bias, /*overrun=*/true);
        fprintf(stdout, "  learned corrections: %.3f %.3f %.3f %.3f\n",
                r.mf.correction(0), r.mf.correction(1), r.mf.correction(2), r.mf.correction(3));
        CHECK(r.mf.total() == 0);                            // overrun never counts as a misfire
        CHECK(r.mf.correction(2) > 1.05f);                   // and cylinder 2's bias was learned
        CHECK(r.mf.correction(0) < 1.05f);

        // Back on throttle with the SAME physical bias: now corrected away.
        const uint32_t before = r.mf.total();
        r.spin(20, cyl2_bias, /*overrun=*/false);
        fprintf(stdout, "  after learning: new events=%u rough2=%.4f\n", r.mf.total() - before, r.mf.roughness(2));
        CHECK(r.mf.total() == before);
    }

    SECTION("and a REAL misfire on top of a corrected bias is still caught");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Rig r; r.configure(cfg);
        r.spin(40, cyl2_bias, true);                         // learn the geometry
        const float bias_and_misfire[4] = { 1.0f, 1.30f, 1.12f, 1.0f };
        r.spin(20, bias_and_misfire, false);
        fprintf(stdout, "  events: %u %u %u %u\n",
                r.mf.events(0), r.mf.events(1), r.mf.events(2), r.mf.events(3));
        CHECK(r.mf.events(1) > 0);        // the genuinely dead cylinder
        CHECK(r.mf.events(2) == 0);       // the merely crooked one
    }

    // ---- Gates ------------------------------------------------------------------------------------

    SECTION("below the rpm floor nothing is judged");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Rig r; r.configure(cfg);
        r.pos.rpm = 300;                                     // cranking
        r.spin(20, cyl1_slow);
        CHECK(r.mf.total() == 0);
    }

    SECTION("disabled drains without judging, so it cannot bank a backlog");
    {
        reset_region();
        auto cfg = make_cfg(false);
        Rig r; r.configure(cfg);
        r.spin(20, cyl1_slow);
        CHECK(r.mf.total() == 0);
        CHECK(r.st.dropped() == 0);      // it kept the ring empty rather than letting it overflow
    }

    SECTION("with no timer assigned the module is simply inert");
    {
        reset_region();
        auto cfg = make_cfg(true);
        Misfire mf; mf.init(cfg);
        SignalBus bus{}; EnginePosition pos{}; EngineFrame fr{};
        pos.rpm = 2000;
        mf.update(pos, bus, fr);
        CHECK(mf.total() == 0);
        CHECK(mf.segments_seen() == 0);
    }

    return test_summary();
}
