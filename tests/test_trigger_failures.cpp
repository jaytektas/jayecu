// What the decoder does when the ENGINE misbehaves — the faults a car actually presents.
//
// The wheel sweep on the rig proves the healthy cases: every wheel acquires, holds through an RPM
// ramp, and reports nothing. This is the other half. A trigger fault on a running engine is not an
// abstraction — a chipped tooth, a cracked sensor mount, ignition crosstalk into a VR lead, a
// stretched chain, a connector that opens over a bump — and for each one the questions are the same
// three: does the decoder NOTICE, does it stop firing on a position it can no longer prove, and does
// it come back cleanly when the fault does.
//
// Faults are injected into a synthesised edge stream so their timing is exact. The rig cannot chip a
// tooth.
#include "test_helpers.h"
#include "GenericTrigger.h"
#include <tuple>
#include <numeric>
#include <vector>
#include <cmath>
#include <algorithm>

static constexpr uint32_t K = 10;          // ticks per 0.1 deg

// A 36-1 crank, optionally with a once-per-cycle WIDTH cam, driven tooth by tooth so a fault can be
// placed on an exact tooth. Pitch is 100 deci-deg; `us_per_dd` sets the speed.
struct Rig {
    GenericTrigger g;
    int cr, cam = -1;
    uint32_t tick = 0;
    int slot = 0;                          // 0..34 present teeth; the gap follows slot 34
    uint32_t pitch;                        // ticks per tooth
    bool started = false;
    uint8_t gi[1] = {0}, gr[1] = {2};

    explicit Rig(uint32_t pitch_ticks = 1000, bool with_cam = false) : pitch(pitch_ticks) {
        cr = g.add_gap(0, 2, 36, gi, gr, 1, 25);
        if (with_cam) {
            cam = g.add_width(2, REPEATS_PHASE, 600, 1100, 2000, true);
            g.set_phase_meta(2, 2000, false, 0);
        }
    }
    uint32_t next_gap() const { return (slot == 34) ? 2 * pitch : pitch; }
    // Advance to the next tooth and feed it. `skip` drops it (the edge never happens); `scale`
    // stretches or shrinks the interval.
    void tooth(bool skip = false, double scale = 1.0) {
        if (!started) { started = true; }
        else {
            tick += static_cast<uint32_t>(next_gap() * scale);
            slot = (slot == 34) ? 0 : slot + 1;
        }
        if (!skip) g.on_edge(static_cast<uint8_t>(cr), tick);
    }
    void spin(int n, double scale = 1.0) { for (int i = 0; i < n; i++) tooth(false, scale); }
    void spin_to(int target) { while (slot != target) tooth(); }
    void extra(uint32_t after) { g.on_edge(static_cast<uint8_t>(cr), tick + after); }
    // Feed the cam pulse (leading + trailing) at the given engine angle offset from now.
    void cam_pulse(uint32_t at_tick) {
        if (cam < 0) return;
        g.on_edge(static_cast<uint8_t>(cam), at_tick, true);
        g.on_edge(static_cast<uint8_t>(cam), at_tick + 800 * K / 10, false);
    }
    bool synced() const { return g.level() != SyncLvl::NONE; }
    // Spin until it re-acquires, up to a bound. Returns teeth taken, or -1 if it never did.
    int reacquire(int max_teeth = 200) {
        for (int i = 0; i < max_teeth; i++) { tooth(); if (synced()) return i + 1; }
        return -1;
    }
};

static const char* KIND[] = {"NONE","TOOTH_WINDOW","GAP_MISMATCH","MISSED_TOOTH",
                             "NOISE_EDGE","SIGNAL_ABSENT","PHASE_LOST"};

int main() {
    fprintf(stdout, "=== decoder under real trigger faults ===\n");

    SECTION("a cam that fires EARLIER gives a more NEGATIVE residual (the HAL negates it into advance)");
    {
        // EnginePositionHal publishes vvt_angle_N = -phase_residual, because the residual is
        // crank-at-the-edge minus nominal and an advanced cam fires earlier. If this convention ever
        // flips, the VVT loop gets positive feedback and drives every phaser to a stop — so it is pinned.
        Rig r(1000, true);
        r.g.set_phase_meta(2, 2000, true, 1800);   // phased, generous authority
        const uint8_t cs = static_cast<uint8_t>(r.cam);
        auto cycle_with_cam = [&](int32_t shift_ticks) {
            for (int rev = 0; rev < 2; ++rev) {
                r.spin_to(10);
                if (rev == 0) {
                    const uint32_t lead  = static_cast<uint32_t>(static_cast<int32_t>(r.tick) + 500 + shift_ticks);
                    const uint32_t trail = lead + 8000;               // an 80 degree pulse
                    bool led = false, trailed = false;
                    for (int k = 0; k < 20; ++k) {                    // edges in time order with the teeth
                        const uint32_t next = r.tick + r.next_gap();
                        if (!led && lead < next)      { r.g.on_edge(cs, lead, true);   led = true; }
                        if (!trailed && trail < next) { r.g.on_edge(cs, trail, false); trailed = true; }
                        r.tooth();
                    }
                }
                r.spin_to(0);
            }
        };
        for (int i = 0; i < 8; ++i) cycle_with_cam(0);
        CHECK(r.g.phase_resid_valid(cs));
        const int base = r.g.phase_residual(cs);
        for (int i = 0; i < 3; ++i) cycle_with_cam(-1000);     // 10 crank degrees earlier
        CHECK_NEAR(r.g.phase_residual(cs) - base, -100, 5);    // 10.0 degrees more negative
    }

    SECTION("a chipped tooth: one tooth missing, every revolution");
    {
        // A tooth broken off the wheel, or a sensor that misses one every turn. The old decoder rode
        // through this for ever — miss_ was cleared by any good tooth, so an intermittent fault never
        // reached the consecutive-miss limit while the angle walked.
        Rig r; r.spin(200); r.spin_to(10);
        int drops = 0;
        for (int rev = 0; rev < 6; rev++) {
            r.spin_to(10);
            r.tooth(/*skip*/true);              // the chipped tooth: no edge happens
            r.tooth();                          // the NEXT tooth carries the doubled interval
            if (!r.synced()) drops++;
            r.reacquire();
        }
        CHECK(drops == 6);                      // noticed EVERY time, not ridden through
        CHECK(r.g.missed_total() >= 6);
    }

    SECTION("...and it re-acquires each time, so the engine keeps restarting rather than running wrong");
    {
        Rig r; r.spin(200); r.spin_to(10);
        r.tooth(true); r.tooth();               // skip, then the tooth that reveals it
        CHECK(!r.synced());
        const int teeth = r.reacquire();
        CHECK(teeth > 0);
        CHECK(teeth < 110);                     // within about three revolutions of a 36-1
        CHECK(r.g.level() == SyncLvl::CRANK);
    }

    SECTION("ignition crosstalk: a burst of noise into the trigger lead");
    {
        // A VR lead run beside a plug wire picks up the spark. The burst is fast and irregular —
        // nothing like a tooth — and the decoder must refuse it rather than decode it.
        Rig r; r.spin(200); r.spin_to(10);
        CHECK(r.synced());
        for (uint32_t k = 1; k <= 12; k++) r.extra(k * 37);   // 12 spurious edges inside one pitch
        CHECK(!r.synced());
        CHECK(r.g.noise_total() > 0);
        // ...and once the burst passes it comes back.
        CHECK(r.reacquire() > 0);
        CHECK(r.g.level() == SyncLvl::CRANK);
    }

    SECTION("crosstalk RECOVERY is bounded however long the burst — it used to be permanent");
    {
        // The one that mattered. The nominal-interval estimate snapped DOWN instantly and only rose
        // on a tooth judged "not a gap", so a burst that dragged it to a fraction of a pitch made
        // every real tooth afterwards read as a gap — and the branch that would have fixed it never
        // ran again. Measured before the fix: three spurious edges and a 36-1 never re-acquired in
        // 4000 teeth, 114 revolutions. One burst of crosstalk and the engine does not restart.
        for (int burst : {1, 3, 12, 40, 100}) {
            Rig r; r.spin(200); r.spin_to(10);
            CHECK(r.synced());
            for (int k = 1; k <= burst; k++) r.extra(static_cast<uint32_t>(k) * 37u);
            const int teeth = r.reacquire(400);
            CHECK(teeth > 0);                    // it comes back AT ALL
            CHECK(teeth <= 150);                 // within about four revolutions of a 36-1
        }
    }

    SECTION("debris on the wheel: one EXTRA edge between two real teeth");
    {
        Rig r; r.spin(200); r.spin_to(10);
        r.extra(r.pitch / 3);                   // a spurious edge at a third of a pitch
        CHECK(!r.synced());                     // refused, not absorbed
        CHECK(r.g.last_error_kind() == (uint8_t)TriggerErrorKind::NOISE_EDGE);
        // The position is DISCARDED, not carried forward. An earlier version of this checked that
        // the angle was unchanged — that the noise edge had not advanced it — which was the right
        // worry and the wrong assertion: it also passes for a decoder that keeps believing a
        // position it can no longer prove. Reset to zero is the answer to both.
        CHECK(r.g.angle() == 0);
    }

    SECTION("a connector that opens: several teeth lost, then it comes back");
    {
        Rig r; r.spin(200); r.spin_to(10);
        for (int i = 0; i < 5; i++) r.tooth(true);   // five teeth simply do not arrive
        r.tooth();                                   // the one that comes back reveals the gap
        CHECK(!r.synced());
        CHECK(r.reacquire() > 0);
        CHECK(r.g.level() == SyncLvl::CRANK);
    }

    SECTION("cranking: the crank speeds and slows every compression stroke, and sync must hold");
    {
        // A starter turns an engine unevenly — it slows into every compression and speeds up over
        // the top. This is where a per-tooth window is most likely to false-trip, and where a
        // decoder that cannot cope simply never starts the engine.
        Rig r(5000);                            // ~200 rpm on a 36-1
        r.spin(200);
        CHECK(r.synced());
        const uint32_t missed_before = r.g.missed_total();
        for (int rev = 0; rev < 8; rev++)
            for (int t = 0; t < 35; t++) {
                // A SMOOTH +/-12% about nominal — a crank slowing into compression and speeding over
                // the top, not stepping between two speeds. Real inertia makes this continuous, and
                // the distinction matters: the window is on the TOOTH-TO-TOOTH ratio, so a square
                // wobble of +/-12% asks for a 27% jump at each flip while a smooth one never asks
                // for more than a few percent between adjacent teeth.
                const double s = 1.0 + 0.12 * std::sin(2.0 * 3.14159265 * t / 35.0);
                r.tooth(false, s);
            }
        CHECK(r.synced());
        CHECK(r.g.missed_total() == missed_before);   // not one false fault
    }

    SECTION("a stall: the engine stops mid-revolution and is restarted");
    {
        Rig r; r.spin(200); r.spin_to(10);
        CHECK(r.synced());
        // Stopping is an ABSENCE, which the decoder cannot see from edges alone — that is the tooth
        // deadline's job and it lives in EnginePositionHal (see test_trigger_liveness). What the
        // decoder must do is come back when the engine is cranked again, with no reconfigure.
        r.g.drop_sync();
        CHECK(!r.synced());
        r.tick += 3 * 1000 * 1000;              // three seconds of nothing
        CHECK(r.reacquire() > 0);
        CHECK(r.g.level() == SyncLvl::CRANK);
    }

    SECTION("the wrong wheel in the tune: a 36-1 decoder on a wheel with no gap");
    {
        // A tune that names the wrong wheel is a common way to spend an evening. It must never
        // pretend: no gap arrives where the schedule says one should, so it can never lock.
        GenericTrigger g;
        uint8_t gi[1] = {0}, gr[1] = {2};
        const int cr = g.add_gap(0, 2, 36, gi, gr, 1, 25);   // told 36-1
        uint32_t t = 0;
        for (int i = 0; i < 400; i++) { t += 1000; g.on_edge((uint8_t)cr, t); }  // fed an EVEN wheel
        CHECK(g.level() == SyncLvl::NONE);
    }

    SECTION("the match window follows RPM: wider cranking, tighter at speed");
    {
        // A tooth 1.7 pitches late. At 6000 rpm that is not a tooth — consecutive teeth on a
        // spinning engine differ by a few percent, so 70% late means something is wrong. At cranking
        // the same interval is an ordinary compression stroke, and a decoder that rejects it is a
        // decoder that never lets the engine start.
        {
            Rig r; r.spin(200); r.spin_to(10);
            r.g.set_window_pcts(0, /*run*/50, /*crank*/75);
            r.g.set_rpm(6000);                          // fully the running window: [0.5, 1.5]
            r.g.on_edge((uint8_t)r.cr, r.tick + 1700);
            CHECK(!r.synced());
            CHECK(r.g.last_error_kind() == (uint8_t)TriggerErrorKind::MISSED_TOOTH);
        }
        {
            Rig r; r.spin(200); r.spin_to(10);
            r.g.set_window_pcts(0, /*run*/50, /*crank*/75);
            r.g.set_rpm(200);                           // cranking: [0.25, 1.75]
            r.g.on_edge((uint8_t)r.cr, r.tick + 1700);
            CHECK(r.synced());                          // the same interval, now an ordinary tooth
        }
    }

    SECTION("...and the blend is monotonic between the two rpm marks");
    {
        // Nothing should get TIGHTER as the engine slows. A band that moved the wrong way would let
        // a wheel pass at speed and fail it at idle, which is the hardest kind of fault to chase.
        int accepted_at = 0;
        for (uint32_t rpm : {200u, 600u, 1000u, 1400u, 3000u}) {
            Rig r; r.spin(200); r.spin_to(10);
            r.g.set_window_pcts(0, 50, 75);
            r.g.set_rpm(rpm);
            r.g.on_edge((uint8_t)r.cr, r.tick + 1650);  // 1.65 pitches
            if (r.synced()) accepted_at++;
        }
        CHECK(accepted_at > 0);                         // the slow end accepts it
        CHECK(accepted_at < 5);                         // the fast end does not
    }

    SECTION("a tooth outside the window resets the decoder to its INITIAL state");
    {
        // Not "drops sync and carries some of it forward". The matcher goes back to acquiring with
        // no nominal, no pattern index and no angle; the fusion drops the anchor, the revolution and
        // the level. Whatever was believed about position is discarded rather than repaired, because
        // an anomaly says the belief was wrong and there is nothing in it worth keeping.
        Rig r; r.spin(200); r.spin_to(10);
        CHECK(r.g.level() == SyncLvl::CRANK);
        CHECK(r.g.angle() != 0);                       // it had a position...

        r.g.on_edge((uint8_t)r.cr, r.tick + 1900);     // 1.9 pitches: outside [0.5, 1.5]
        CHECK(r.g.level() == SyncLvl::NONE);
        CHECK(r.g.angle() == 0);                       // ...and now it has none, not a stale one
        CHECK(!r.g.rev_known());
        CHECK(r.g.velocity() == 0);

        // ...and it acquires again from scratch rather than resuming where it left off.
        CHECK(r.reacquire() > 0);
        CHECK(r.g.level() == SyncLvl::CRANK);
    }

    SECTION("the same is true on a SEQUENCE wheel, which used to ride through four");
    {
        // Odd-fire cranks, the GM 4200 and every multi-pulse cam decode as SEQUENCE. The gap path
        // stopped tolerating misses; this one was left behind, riding through four window violations
        // while advancing the pattern on an assumption each time.
        GenericTrigger g;
        AngleDeg10 cell[2] = {1350, 2250};             // odd-fire 0/135
        const int cr = g.add_seq(0, 2, cell, 2, 25);
        uint32_t t = 0;
        for (int i = 0; i < 40; ++i) { t += (i % 2 ? 2250u : 1350u) * 10u; g.on_edge((uint8_t)cr, t); }
        CHECK(g.level() == SyncLvl::CRANK);
        t += 9000 * 10u;                                // grossly out of window
        g.on_edge((uint8_t)cr, t);
        CHECK(g.level() == SyncLvl::NONE);              // ONE is enough, not four
    }

    SECTION("KICKBACK: a single-gap wheel syncs just as happily backwards");
    {
        // A recorded limitation, pinned so it cannot change silently. During a start an engine can
        // kick back — the crank turns BACKWARDS through a compression — and a single-gap wheel
        // cannot tell. Its inter-tooth spacings are 1,1,...,1,2 and reversed they are 2,1,...,1,1:
        // the same cyclic sequence. Nothing in the interval stream distinguishes the directions, so
        // the decoder locks and reports a confident position that is counting the wrong way.
        //
        // This is not a fault in the matcher; it is what a symmetric wheel can physically tell you.
        // Detecting it needs something the crank alone does not have — an asymmetric gap pattern
        // whose inter-gap sequence differs reversed, a cam whose reference then lands at the wrong
        // crank angle, or direction from the sensor itself. None of that is built: reverse rotation
        // is undesigned, and this test exists so that stays a known gap rather than a surprise.
        for (bool backwards : {false, true}) {
            GenericTrigger g;
            uint8_t gi[1] = {0}, gr[1] = {2};
            const int cr = g.add_gap(0, 2, 36, gi, gr, 1, 50);
            std::vector<int> sp;                       // spacings in tooth pitches, one revolution
            for (int i = 0; i < 34; ++i) sp.push_back(1);
            sp.push_back(2);                            // the missing tooth
            if (backwards) std::reverse(sp.begin(), sp.end());
            uint32_t t = 0;
            for (int i = 0; i < 400; ++i) { t += sp[i % sp.size()] * 1000u; g.on_edge((uint8_t)cr, t); }
            CHECK(g.level() == SyncLvl::CRANK);         // locks EITHER way — that is the finding
        }
    }

    SECTION("NOISE IN THE GAP: a spurious edge landing inside the missing tooth");
    {
        // The classic field symptom, and the reason noise is the fault class worth designing for:
        // a noise pulse arrives while the wheel is crossing its missing tooth, so the one interval
        // the decoder uses as its reference is split into two shorter ones. The gap the anchor
        // depends on simply is not there that revolution.
        //
        // A presence-test decoder reads the two halves as ordinary teeth, misses the gap entirely,
        // and carries on counting from the wrong place — position silently wrong for a whole
        // revolution. The window catches it because neither half is a legal interval: at the tooth
        // where a 2x (or 3x, or 4x) gap is scheduled, anything materially shorter is rejected.
        for (auto w : {std::make_tuple(36, 2, 50), std::make_tuple(36, 2, 25),
                       std::make_tuple(60, 3, 50), std::make_tuple(60, 3, 33),
                       std::make_tuple(12, 4, 50)}) {
            const int slots = std::get<0>(w), ratio = std::get<1>(w), at_pct = std::get<2>(w);
            GenericTrigger g;
            uint8_t gi[1] = {0}, gr[1] = {(uint8_t)ratio};
            const int cr = g.add_gap(0, 2, slots, gi, gr, 1, 50);
            const int present = slots - (ratio - 1);
            uint32_t t = 0; int slot = 0;
            auto tooth = [&] {
                t += (slot == present - 1) ? 1000u * (uint32_t)ratio : 1000u;
                slot = (slot == present - 1) ? 0 : slot + 1;
                g.on_edge((uint8_t)cr, t);
            };
            for (int i = 0; i < 300; ++i) tooth();
            CHECK(g.level() == SyncLvl::CRANK);
            while (slot != present - 1) tooth();        // park on the tooth before the gap
            g.on_edge((uint8_t)cr, t + 1000u * (uint32_t)ratio * (uint32_t)at_pct / 100u);
            CHECK(g.level() == SyncLvl::NONE);          // caught ON the intruder, not a rev later
            CHECK(g.last_error_kind() == (uint8_t)TriggerErrorKind::NOISE_EDGE);
        }
    }

    SECTION("WHEEL DAMAGE: a second broken tooth makes a FALSE gap, and it must not be trusted");
    {
        // A chipped 36-1 that loses a SECOND tooth presents two 2x intervals per revolution. The
        // danger is not that the engine will not run — it is that the decoder anchors to the WRONG
        // gap and runs CONFIDENTLY at an angle that is out by however many teeth separate them.
        // Firing into the wrong part of the cycle is far worse than not firing at all.
        //
        // Intervals are derived from SLOT POSITIONS rather than written by hand, so the pattern is
        // correct by construction and provably sums to one revolution. A malformed stimulus would
        // fail to sync for reasons that have nothing to do with the wheel, and report as "safe".
        auto intervals = [](int slots, std::vector<int> missing) {
            std::vector<int> present, iv;
            for (int s = 0; s < slots; ++s)
                if (std::find(missing.begin(), missing.end(), s) == missing.end()) present.push_back(s);
            for (size_t i = 1; i < present.size(); ++i) iv.push_back(present[i] - present[i-1]);
            iv.push_back(slots - present.back() + present.front());
            return iv;
        };
        auto run = [&](std::vector<int> missing) {
            auto iv = intervals(36, missing);
            CHECK(std::accumulate(iv.begin(), iv.end(), 0) == 36);   // the stimulus is a real wheel
            GenericTrigger g;
            uint8_t gi[1] = {0}, gr[1] = {2};
            const int cr = g.add_gap(0, 2, 36, gi, gr, 1, 50);
            uint32_t t = 0; size_t i = 0; int locked = 0;
            for (int n = 0; n < 3000; ++n) {
                t += (uint32_t)iv[i % iv.size()] * 1000u; ++i;
                g.on_edge((uint8_t)cr, t);
                if (g.level() != SyncLvl::NONE) ++locked;
            }
            return locked;
        };
        CHECK(run({0}) > 2500);                     // CONTROL: a healthy 36-1 locks and stays locked
        CHECK(run({0, 18}) == 0);                   // opposite the gap
        CHECK(run({0, 5})  == 0);                   // near it
        CHECK(run({0, 1})  == 0);                   // adjacent — merges into a 3x gap
        CHECK(run({0, 12, 24}) == 0);               // three gaps
        // Not one edge of false confidence in 3000: it refuses rather than guessing which gap is
        // the real one. The engine will not start on a damaged wheel, which is the correct outcome.
    }

    SECTION("DECEL TO STALL: a normal key-off must not read as a fault while it is still turning");
    {
        // The commonest event in the vehicle's life. The engine coasts down over a few hundred ms,
        // so EVERY tooth interval is longer than the one before it, and the window judges an
        // interval against the last one — a fast enough decay is indistinguishable from a missing
        // tooth. Dropping sync at the END of that is correct; the engine has stopped. Dropping while
        // it is still turning at hundreds of rpm is a false trip on a running engine.
        //
        // THE WINDOW MUST BE BLENDED BY RPM HERE, as EnginePositionHal::service() does once per
        // frame: 25% running, 75% cranking, across 400-1500 rpm. Feeding a flat window tests a
        // decoder the firmware does not have — with a flat 50% this same probe reported a 4-1
        // dropping at 296 rpm, which was the harness, not the decoder.
        auto stall_rpm = [](int slots, int ratio, double tau_s) {
            GenericTrigger g;
            uint8_t gi[1] = {0}, gr[1] = {(uint8_t)ratio};
            const int present = slots - (ratio - 1);
            const int cr = g.add_gap(0, 2, slots, gi, gr, 1, 25);
            g.set_window_pcts(cr, 25, 75);
            double t_us = 0, rpm = 800.0; int slot = 0;
            auto tooth = [&] {
                const double deg = (slot == present-1 ? ratio : 1) * (360.0 / slots);
                t_us += deg / (rpm * 6.0) * 1e6;
                slot = (slot == present-1) ? 0 : slot + 1;
                g.set_rpm((uint32_t)rpm);
                g.on_edge((uint8_t)cr, (uint32_t)t_us);
            };
            for (int i = 0; i < 400; ++i) tooth();
            if (g.level() == SyncLvl::NONE) return -1.0;          // never locked: not a result
            const double t0 = t_us;
            while (rpm > 10.0) {
                tooth();
                rpm = 800.0 * std::exp(-((t_us - t0) / 1e6) / tau_s);
                if (g.level() == SyncLvl::NONE) return rpm;
            }
            return rpm;
        };
        // tau 0.5 s is a key-off; 0.1 s is a stall against compression. Both must hold sync well
        // past anything that could be called running.
        for (double tau : {0.5, 0.2, 0.1}) {
            CHECK(stall_rpm(60, 3, tau) < 100.0);      // 60-2
            CHECK(stall_rpm(36, 2, tau) < 100.0);      // 36-1
            CHECK(stall_rpm(12, 2, tau) < 100.0);      // 12-1
            CHECK(stall_rpm(8,  2, tau) < 100.0);      // 8-1
            CHECK(stall_rpm(4,  2, tau) < 100.0);      // 4-1, the coarsest and the hardest case
        }
        // ...and the fine wheels ride it down to a standstill, not merely to "slow".
        CHECK(stall_rpm(60, 3, 0.5) < 20.0);
        CHECK(stall_rpm(36, 2, 0.5) < 20.0);
    }

    SECTION("INTERMITTENT CONNECTOR: hundreds of make/break cycles, and it must not wedge");
    {
        // One dropout is covered above. A corroded pin or a chafed loom gives HUNDREDS, for minutes,
        // and the questions that only repetition asks are whether every one re-acquires, whether
        // re-acquisition degrades as they accumulate, and whether anything leaks across them.
        struct Res { int relocked, failed, first, last, worst; SyncLvl end; };
        auto flap = [](int slots, int ratio, int good, int lost, int cycles) {
            GenericTrigger g;
            uint8_t gi[1] = {0}, gr[1] = {(uint8_t)ratio};
            const int present = slots - (ratio - 1);
            const int cr = g.add_gap(0, 2, slots, gi, gr, 1, 25);
            g.set_window_pcts(cr, 25, 75);
            double t_us = 0; int slot = 0;
            auto step = [&](bool deliver) {
                const double deg = (slot == present-1 ? ratio : 1) * (360.0 / slots);
                t_us += deg / (1200.0 * 6.0) * 1e6;
                slot = (slot == present-1) ? 0 : slot + 1;
                g.set_rpm(1200);
                if (deliver) g.on_edge((uint8_t)cr, (uint32_t)t_us);
            };
            for (int i = 0; i < 300; ++i) step(true);
            Res r{0, 0, -1, -1, 0, SyncLvl::NONE};
            for (int c = 0; c < cycles; ++c) {
                for (int i = 0; i < lost; ++i) step(false);        // the connector is open
                int n = 0; bool back = false;
                for (int i = 0; i < good; ++i) {
                    step(true); ++n;
                    if (!back && g.level() != SyncLvl::NONE) {
                        back = true; ++r.relocked;
                        if (r.first < 0) r.first = n;
                        r.last = n; if (n > r.worst) r.worst = n;
                    }
                }
                if (!back) ++r.failed;
            }
            r.end = g.level();
            return r;
        };
        for (auto cfg : {std::make_tuple(60, 3, 5), std::make_tuple(60, 3, 1),
                         std::make_tuple(60, 3, 120), std::make_tuple(36, 2, 3),
                         std::make_tuple(4, 2, 2)}) {
            const Res r = flap(std::get<0>(cfg), std::get<1>(cfg), 200, std::get<2>(cfg), 300);
            CHECK(r.relocked == 300);                  // every single one
            CHECK(r.failed == 0);
            CHECK(r.end != SyncLvl::NONE);
            // NO DEGRADATION: the 300th re-acquisition costs what the first did. A creeping cost
            // would mean state surviving a reset that should have cleared it, and 300 cycles is
            // where that shows up and one cycle is not.
            CHECK(r.last <= r.first + 30);
            CHECK(r.worst <= r.first + 30);
        }
        // AND THE PATHOLOGICAL CASE STAYS REFUSED. A lead breaking every other tooth is not a
        // degraded signal, it is not a signal: half the teeth are gone and no pattern survives it.
        // Never re-locking is the right answer, and never FALSELY locking is the important half.
        const Res chatter = flap(36, 2, 1, 1, 3000);
        CHECK(chatter.relocked == 0);
        CHECK(chatter.end == SyncLvl::NONE);
    }

    fprintf(stdout, "  (last error kind seen: %s)\n", KIND[0]);
    return test_summary();
}
