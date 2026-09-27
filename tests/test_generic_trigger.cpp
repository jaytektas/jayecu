// Integration tests for GenericTrigger — the generic multi-stream decoder + fusion. Feeds
// interleaved crank/cam edges on a common tick timeline and checks the fused sync level +
// engine angle (incl. correct revolution tracking across the 360° boundary).

#include "test_helpers.h"
#include "GenericTrigger.h"
#include <vector>
#include <algorithm>
#include <cstdint>

static constexpr uint32_t K = 10;   // ticks per 0.1°

// Build a sorted (tick, stream) event timeline.
struct Ev { uint32_t tick; int idx; };
static void feed_all(GenericTrigger& g, std::vector<Ev>& ev) {
    std::stable_sort(ev.begin(), ev.end(), [](const Ev&a,const Ev&b){ return a.tick<b.tick; });
    for (auto& e : ev) g.on_edge(static_cast<uint8_t>(e.idx), e.tick);
}
static AngleDeg10 adist(AngleDeg10 a, AngleDeg10 b) {
    int d = a - b; d %= 7200; if (d < 0) d += 7200; return static_cast<AngleDeg10>(d > 3600 ? 7200 - d : d);
}

int main() {
    fprintf(stdout, "=== GenericTrigger (multi-stream fusion) ===\n");

    SECTION("single crank GAP (36-1) → CRANK");
    {
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        int cr = g.add_gap(0, 2 /*repeats: once per crank rev on a four-stroke*/, 36, gi, gr, 1, 25);
        std::vector<Ev> ev;
        for (int c=0;c<5;c++) for (int rev=0;rev<2;rev++) for (int slot=0;slot<36;slot++) {
            if (slot==35) continue;                       // 1 missing/rev
            uint32_t a = c*7200 + rev*3600 + slot*100; ev.push_back({a*K, cr});
        }
        feed_all(g, ev);
        CHECK(g.level() == SyncLvl::CRANK);               // no cam → no rev → CRANK
    }

    SECTION("single crank EVEN (distributor) alone → NONE: it cannot know where it is");
    {
        GenericTrigger g;
        int cr = g.add_gap(0, 2 /*repeats: once per crank rev on a four-stroke*/, 3, nullptr, nullptr, 0, 25);  // 3 even teeth
        std::vector<Ev> ev;
        for (int c=0;c<6;c++) for (int t=0;t<3;t++){ uint32_t a=c*3600 + t*1200; ev.push_back({a*K,cr}); }
        feed_all(g, ev);
        // An even wheel carries no unique feature, so its matcher locks RELATIVE: it knows the pitch
        // and can count, and the origin is whichever tooth happened to confirm first. That is speed
        // and distance, never position. This used to report CRANK — and then fire on an angle that
        // was out by an arbitrary number of teeth, permanently, with full confidence. Position is
        // established or it is not, and NONE is what "not" is called.
        CHECK(g.level() == SyncLvl::NONE);
        CHECK(g.velocity() > 0);            // ...but the velocity it does know is still good
    }

    SECTION("single crank EVEN in SYNC ALWAYS (distributor mode) → PHASE from the first measured tooth");
    {
        GenericTrigger g;
        g.set_sync_always(true);                          // what the config check says for a dizzy
        int cr = g.add_gap(0, 2, 2, nullptr, nullptr, 0, 25);   // 4cyl dizzy: 2 teeth per crank rev
        CHECK(g.level() == SyncLvl::NONE);
        g.on_edge(static_cast<uint8_t>(cr), 0);            // first tooth: primes the period only
        CHECK(g.level() == SyncLvl::NONE);
        g.on_edge(static_cast<uint8_t>(cr), 1800 * K);     // second: one steady interval -> synced
        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(g.velocity() > 0);
        for (int t = 2; t < 12; ++t) g.on_edge(static_cast<uint8_t>(cr), static_cast<uint32_t>(t) * 1800 * K);
        CHECK(g.level() == SyncLvl::PHASE);                // and stays synced on every tooth
        g.on_edge(static_cast<uint8_t>(cr), 11 * 1800 * K + 200 * K);   // a noise edge 20° after tooth 11
        CHECK(g.level() == SyncLvl::NONE);                 // still judged: a wrong edge drops sync
    }

    SECTION("cam-only SEQUENCE (CAS) → PHASE");
    {
        GenericTrigger g;
        // cam pulses at 0,2400,5100 → intervals [2400,2700,2100] (Σ=7200), asymmetric
        AngleDeg10 cell[3] = {2400,2700,2100};
        int cam = g.add_seq(2, REPEATS_PHASE, cell, 3, 20);
        std::vector<Ev> ev;
        for (int c=0;c<8;c++) for (int A : {0,2400,5100}) ev.push_back({(uint32_t)(c*7200+A)*K, cam});
        feed_all(g, ev);
        CHECK(g.level() == SyncLvl::PHASE);
    }

    SECTION("crank GAP (36-1) + cam SEQUENCE → PHASE, angle tracks the TRUE engine angle");
    {
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        int cr  = g.add_gap(0, 2 /*repeats: once per crank rev on a four-stroke*/, 36, gi, gr, 1, 25);
        AngleDeg10 cell[3] = {2400,2700,2100};            // cam pulses at 0,2400,5100
        int cam = g.add_seq(2, REPEATS_PHASE, cell, 3, 20);
        struct E { uint32_t tick; int idx; AngleDeg10 tru; bool crank; };
        std::vector<E> ev;
        for (int c=0;c<8;c++) {
            for (int rev=0;rev<2;rev++) for (int slot=0;slot<36;slot++) {
                if (slot==35) continue;
                uint32_t a = c*7200 + rev*3600 + slot*100;
                ev.push_back({a*K, cr, (AngleDeg10)(a%7200), true});
            }
            for (int A : {0,2400,5100}) ev.push_back({(uint32_t)(c*7200+A)*K, cam, (AngleDeg10)A, false});
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){return a.tick<b.tick;});
        bool seen_phase=false; int checked=0, rev0=0, rev1=0;
        for (auto& e : ev) {
            g.on_edge((uint8_t)e.idx, e.tick);
            if (g.level()==SyncLvl::PHASE) seen_phase=true;
            // once PHASE-locked, every crank edge's fused angle must equal the true angle
            if (seen_phase && e.crank && e.tick > 30*7200*K/10) {   // settle a few cycles
                CHECK(adist(g.angle(), e.tru) <= 2*100);            // within one tooth
                ++checked; if (e.tru < 3600) ++rev0; else ++rev1;
            }
        }
        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(g.rev_known());
        CHECK(checked > 0);
        CHECK(rev0 > 0 && rev1 > 0);                       // tracked through BOTH revolutions
    }

    SECTION("phase-sync rpm band: gate blocks ACQUISITION but preserves RETENTION");
    {
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        g.add_gap(0, 2, 36, gi, gr, 1, 25);
        AngleDeg10 cell[3] = {2400,2700,2100};
        int cam = g.add_seq(2, REPEATS_PHASE, cell, 3, 20);
        struct Ed { uint32_t tick; int idx; };
        auto feed = [&](int c) {
            std::vector<Ed> ev;
            for (int rev=0;rev<2;rev++) for (int slot=0;slot<36;slot++) {
                if (slot==35) continue;
                ev.push_back({(uint32_t)((c*7200 + rev*3600 + slot*100)*K), 0});
            }
            for (int A : {0,2400,5100}) ev.push_back({(uint32_t)((c*7200+A)*K), cam});
            std::stable_sort(ev.begin(), ev.end(), [](const Ed&a,const Ed&b){return a.tick<b.tick;});
            for (auto& e : ev) g.on_edge((uint8_t)e.idx, e.tick);
        };

        // Out of band: crank locks, but the cam is gated -> no revolution, capped at CRANK.
        g.set_phase_stream_valid(false);
        for (int c=0;c<8;c++) feed(c);
        CHECK(g.level() == SyncLvl::CRANK);
        CHECK(!g.rev_known());

        // Enter the band: the cam is trusted -> it acquires PHASE.
        g.set_phase_stream_valid(true);
        for (int c=8;c<16;c++) feed(c);
        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(g.rev_known());

        // Drop below the band AFTER lock: retention holds PHASE on crank counting, cam ignored.
        g.set_phase_stream_valid(false);
        for (int c=16;c<24;c++) feed(c);
        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(g.rev_known());
    }

    SECTION("after drop_sync() the CRANK is still the fine-angle source (stall -> re-acquire)");
    {
        // drop_sync() is the lost-trigger watchdog's path: it exists to forget SYNC while KEEPING
        // the stream config, so the decoder re-acquires from the next real teeth. What must survive
        // it is the TOPOLOGY — that this engine has a crank stream at all. That decides which
        // promotion applies: with a crank present the cam supplies the REVOLUTION and PHASE is
        // gated on the crank being locked, while with no crank the cam is the whole angle and
        // promotes to PHASE on its own.
        //
        // NOTE, so nobody reads more coverage into this than it has: this section PASSES both with
        // and without drop_sync() clearing has_crank_, so it does NOT reproduce the field failure it
        // was written to chase (a 60-2 + cam capture splitting a 116-tooth cycle into 37 + 79 after
        // a restart). The cam here fires three times per cycle, at 0/240/510 deg, so A/360 yields
        // both revolutions; the failing hardware has a SINGLE cam pulse at 213 deg, where A/360 is
        // always 0. That difference is the untested ground, and the mechanism remains unproven.
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        int cr  = g.add_gap(0, 2 /*repeats: once per crank rev on a four-stroke*/, 36, gi, gr, 1, 25);
        AngleDeg10 cell[3] = {2400,2700,2100};
        int cam = g.add_seq(2, REPEATS_PHASE, cell, 3, 20);

        struct E { uint32_t tick; int idx; AngleDeg10 tru; bool crank; };
        auto cycles = [&](int from, int to, std::vector<E>& ev) {
            for (int c=from;c<to;c++) {
                for (int rev=0;rev<2;rev++) for (int slot=0;slot<36;slot++) {
                    if (slot==35) continue;
                    uint32_t a = c*7200 + rev*3600 + slot*100;
                    ev.push_back({a*K, cr, (AngleDeg10)(a%7200), true});
                }
                for (int A : {0,2400,5100}) ev.push_back({(uint32_t)(c*7200+A)*K, cam, (AngleDeg10)A, false});
            }
            std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){return a.tick<b.tick;});
        };

        std::vector<E> before; cycles(0, 8, before);
        for (auto& e : before) g.on_edge((uint8_t)e.idx, e.tick);
        CHECK(g.level() == SyncLvl::PHASE);                // locked before the stall

        g.drop_sync();                                     // the engine stops; teeth cease
        CHECK(g.level() == SyncLvl::NONE);

        std::vector<E> after; cycles(8, 20, after);        // it turns again, same wheel
        bool seen_phase=false; int checked=0, rev0=0, rev1=0, wrong=0;
        for (auto& e : after) {
            g.on_edge((uint8_t)e.idx, e.tick);
            if (g.level()==SyncLvl::PHASE) seen_phase=true;
            if (seen_phase && e.crank && e.tick > 16*7200*K/10) {
                if (adist(g.angle(), e.tru) > 2*100) ++wrong;
                ++checked; if (e.tru < 3600) ++rev0; else ++rev1;
            }
        }
        CHECK(g.level() == SyncLvl::PHASE);                // re-acquires
        CHECK(g.rev_known());
        CHECK(checked > 0);
        CHECK(wrong == 0);                                 // crank edges still carry the TRUE angle
        CHECK(rev0 > 0 && rev1 > 0);                       // and BOTH revolutions are still distinct
    }

    SECTION("ROTARY: a 1080 deg cycle resolves THREE revolutions, not two");
    {
        // The decoder used to hardcode a cam-rate period of 7200 and track the revolution in a
        // single BIT. Neither works for a rotary: its phase reference repeats every 1080 deg (one
        // rotor revolution = three eccentric-shaft revolutions), and one bit cannot say which of
        // three you are in — so the three rotor faces would be indistinguishable.
        GenericTrigger g;
        g.set_cycle(ANGLE_1080);
        CHECK(g.cycle() == 10800);
        CHECK(g.revs_per_cycle() == 3);

        uint8_t gi[1]={0}, gr[1]={2};
        // THREE, not two. A rotary's cycle is 1080 deg, so an e-shaft wheel that repeats once per
        // revolution repeats three times per cycle. The old CRANK/CAM enum could not say this — it
        // meant "period 360" and the rotary case worked by that coincidence rather than by stating
        // the count, which is exactly what generalising to a repeat count makes explicit.
        int cr  = g.add_gap(0, 3 /*repeats per 1080 cycle*/, 36, gi, gr, 1, 25);     // 36-1 on the e-shaft
        // The cam pulses must be UNEVENLY spaced or there is no unique reference: three equal
        // intervals look the same from any of them, so the decoder could never say WHICH e-shaft
        // revolution it is in — which is the entire job of a rotary phase sensor.
        AngleDeg10 cell[3] = {3000,3600,4200};                          // sums to 10800 = the cycle
        int cam = g.add_seq(2, REPEATS_PHASE, cell, 3, 20);

        struct E { uint32_t tick; int idx; AngleDeg10 tru; bool crank; };
        std::vector<E> ev;
        for (int c=0;c<10;c++) {
            for (int rev=0;rev<3;rev++) for (int slot=0;slot<36;slot++) {   // THREE revs per cycle
                if (slot==35) continue;
                uint32_t a = c*10800 + rev*3600 + slot*100;
                ev.push_back({a*K, cr, (AngleDeg10)(a%10800), true});
            }
            for (int A : {0,3000,6600}) ev.push_back({(uint32_t)(c*10800+A)*K, cam, (AngleDeg10)A, false});
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){return a.tick<b.tick;});

        // Distance between two angles in the 1080 deg space.
        auto adist1080 = [](AngleDeg10 a, AngleDeg10 b) {
            int d = a - b; d %= 10800; if (d < 0) d += 10800;
            return d > 5400 ? 10800 - d : d;
        };
        bool seen_phase=false; int checked=0, r0=0, r1=0, r2=0;
        for (auto& e : ev) {
            g.on_edge((uint8_t)e.idx, e.tick);
            if (g.level()==SyncLvl::PHASE) seen_phase=true;
            if (seen_phase && e.crank && e.tick > 5*10800*K) {
                // Assert on what the DECODER reports, never on the stimulus we generated — counting
                // our own truth would pass no matter what the decoder did.
                const AngleDeg10 got = g.angle();
                CHECK(adist1080(got, e.tru) <= 2*100);          // within one tooth of the true angle
                if (got < 3600)      ++r0;
                else if (got < 7200) ++r1;
                else                 ++r2;
                ++checked;
            }
        }
        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(g.rev_known());
        CHECK(checked > 0);
        // The point of the whole change: the decoder REPORTS angles in all three e-shaft
        // revolutions. A single revolution bit cannot represent the third — it would top out at
        // 7200 and every face past that would be mistaken for one in an earlier revolution.
        CHECK(r0 > 0 && r1 > 0 && r2 > 0);
    }

    SECTION("a cam-rate GAP stream's period IS the cycle (this was hardcoded 7200)");
    {
        // The line under test: add_gap() used `rate == CAM ? 7200 : 3600`. On a rotary that decodes
        // a 1080 deg cam wheel as though it spanned 720, so its angles are wrong by 2/3 and the
        // wheel never matches. Feed the SAME 12-slot, one-gap cam wheel to two decoders that differ
        // only in cycle, and the tooth pitch they resolve must differ with it.
        auto run = [](AngleDeg10 cycle) {
            GenericTrigger g;
            g.set_cycle(cycle);
            uint8_t gi[1]={0}, gr[1]={2};
            const int cam = g.add_gap(2, REPEATS_PHASE, 12, gi, gr, 1, 25);
            const AngleDeg10 tooth = static_cast<AngleDeg10>(cycle / 12);
            // 11 present teeth: the gap spans two slots, the rest one each.
            uint32_t t = 0;
            for (int c = 0; c < 6; ++c) {
                t += static_cast<uint32_t>(2 * tooth) * K;            // the gap
                g.on_edge(static_cast<uint8_t>(cam), t);
                for (int k = 0; k < 10; ++k) {
                    t += static_cast<uint32_t>(tooth) * K;
                    g.on_edge(static_cast<uint8_t>(cam), t);
                }
            }
            return g.stream_locked_absolute(static_cast<uint8_t>(cam))
                 ? g.stream_angle(static_cast<uint8_t>(cam)) : (AngleDeg10)-1;
        };

        const AngleDeg10 four = run(ANGLE_720);
        const AngleDeg10 rot  = run(ANGLE_1080);
        CHECK(four >= 0);                                  // both wheels lock...
        CHECK(rot  >= 0);
        CHECK(four < ANGLE_720);                           // ...each inside ITS OWN cycle
        CHECK(rot  < ANGLE_1080);
        // The rotary wheel resolves angles the four-stroke span cannot even represent: its teeth are
        // 90 deg apart over 1080, not 60 deg over 720.
        CHECK(rot != four);
    }

    SECTION("the cycle defaults to four-stroke and survives a reset");
    {
        GenericTrigger g;
        CHECK(g.cycle() == 7200);                       // unchanged default — the old hardcoded value
        CHECK(g.revs_per_cycle() == 2);
        g.set_cycle(ANGLE_360);
        CHECK(g.revs_per_cycle() == 1);                 // two-stroke: one revolution IS the cycle
        g.reset_all();
        CHECK(g.cycle() == 3600);                       // engine config, not decode state
        g.drop_sync();
        CHECK(g.cycle() == 3600);
    }

    SECTION("crank GAP (36-1) + cam WIDTH (unique wide pulse) → PHASE");
    {
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        int cr  = g.add_gap(0, 2 /*repeats: once per crank rev on a four-stroke*/, 36, gi, gr, 1, 25);
        // cam: WIDTH stream, reference pulse 60..110° wide, target 5000° (rev1)
        int cam = g.add_width(2, REPEATS_PHASE, 600, 1100, 5000, /*leading_rising*/true);
        struct E { uint32_t tick; int idx; bool rising; };
        std::vector<E> ev;
        for (int c=0;c<8;c++) {
            for (int rev=0;rev<2;rev++) for (int slot=0;slot<36;slot++) {
                if (slot==35) continue;
                uint32_t a = c*7200 + rev*3600 + slot*100; ev.push_back({a*K, cr, true});
            }
            // cam pulses this cycle: two narrow (20°) decoys + one wide (80°) reference @5000°
            for (auto pr : {std::pair<int,int>{1000,200}, {3000,200}, {5000,800}}) {
                uint32_t s = (c*7200 + pr.first)*K, e = (c*7200 + pr.first + pr.second)*K;
                ev.push_back({s, cam, true});   // leading (rising)
                ev.push_back({e, cam, false});  // trailing (falling)
            }
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){return a.tick<b.tick;});
        for (auto& e : ev) g.on_edge((uint8_t)e.idx, e.tick, e.rising);
        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(g.rev_known());
    }

    // Audit T6. The revolution used to come from the cam's CONFIGURED angle alone. A wide cam pulse
    // configured at 320 deg is only recognised at its trailing edge, 400 deg — after the crank has
    // wrapped into revolution 1 — so the configured angle said "revolution 0" and the engine was placed
    // 360 deg out. The crank's own angle decides now.
    SECTION("a cam judged just across a revolution boundary still lands in the right revolution");
    {
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        int cr  = g.add_gap(0, 2, 36, gi, gr, 1, 25);
        int cam = g.add_width(2, REPEATS_PHASE, 600, 1100, 3200, /*leading_rising*/true);
        struct E { uint32_t tick; int idx; bool rising; AngleDeg10 tru; bool crank; };
        std::vector<E> ev;
        for (int c=0;c<8;c++) {
            for (int rev=0;rev<2;rev++) for (int slot=0;slot<36;slot++) {
                if (slot==35) continue;
                uint32_t a = c*7200 + rev*3600 + slot*100;
                ev.push_back({a*K, cr, true, (AngleDeg10)(a%7200), true});
            }
            for (auto pr : {std::pair<int,int>{1000,200}, {3200,800}}) {   // decoy + wide ref 320..400
                ev.push_back({(uint32_t)(c*7200 + pr.first)*K, cam, true, 0, false});
                ev.push_back({(uint32_t)(c*7200 + pr.first + pr.second)*K, cam, false, 0, false});
            }
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){return a.tick<b.tick;});
        bool seen=false; int checked=0, bad=0;
        for (auto& e : ev) {
            g.on_edge((uint8_t)e.idx, e.tick, e.rising);
            if (g.level()==SyncLvl::PHASE) seen=true;
            if (seen && e.crank && e.tick > 4*7200*K) { ++checked; if (adist(g.angle(), e.tru) > 200) ++bad; }
        }
        fprintf(stdout, "    checked %d crank edges, %d out of place\n", checked, bad);
        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(checked > 0);
        CHECK(bad == 0);
    }

    // ---------------------------------------------------------------------------------------
    // THE TIME DIMENSION. A decoder that only ever reacts to edges cannot notice a tooth that came
    // too soon, or one that never came at all. These lock down the parts of that which live in the
    // decoder itself; the deadline that catches a wheel going silent is in test_trigger_liveness
    // (it needs a clock, which this class deliberately does not own).
    //
    // A 36-1 spun tooth by tooth on a continuous timeline, tracking WHERE ON THE WHEEL it is —
    // which matters, because the interval the decoder expects next is 1000 ticks everywhere except
    // across the missing tooth, where it is 2000. A test that does not know which tooth it is on
    // cannot tell a correct prediction from a lucky one.
    struct Wheel36_1 {
        GenericTrigger* g; uint8_t cr;
        uint32_t tick = 0; int slot = 0; bool started = false;
        enum { LAST_PRESENT = 34 };                  // slots 0..34 present, 35 missing
        void tooth() {
            if (!started) started = true;
            else {
                tick += (slot == LAST_PRESENT) ? 2000 : 1000;
                slot  = (slot == LAST_PRESENT) ? 0 : slot + 1;
            }
            g->on_edge(cr, tick);
        }
        void spin(int n)        { for (int i = 0; i < n; i++) tooth(); }
        void spin_to(int target){ while (slot != target) tooth(); }
    };

    SECTION("an edge arriving too soon drops sync — it is not filtered out and forgiven");
    {
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        int cr = g.add_gap(0, 2, 36, gi, gr, 1, 25);
        Wheel36_1 w{&g, (uint8_t)cr};
        w.spin(200); w.spin_to(10);                    // locked, and nowhere near the gap
        CHECK(g.level() == SyncLvl::CRANK);

        // A pulse 300 ticks after a tooth, where the schedule predicted 1000. An earlier version
        // rejected this and carried on decoding, which treats noise as something to tolerate. There
        // is no acceptable amount of noise on a trigger input: an edge that is not where the wheel
        // says it should be means the trigger cannot be trusted, and a decoder that cannot trust its
        // trigger does not know where the engine is.
        g.on_edge((uint8_t)cr, w.tick + 300);
        CHECK(g.last_error_kind() == (uint8_t)TriggerErrorKind::NOISE_EDGE);
        CHECK(g.noise_total() == 1);
        CHECK(g.level() == SyncLvl::NONE);
    }

    SECTION("a tooth that does not arrive reports MISSED_TOOTH, not a gap fault");
    {
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        int cr = g.add_gap(0, 2, 36, gi, gr, 1, 25);
        Wheel36_1 w{&g, (uint8_t)cr};
        w.spin(200); w.spin_to(10);
        CHECK(g.level() == SyncLvl::CRANK);

        // Drop one tooth where the schedule does NOT expect a gap: the next edge lands two pitches
        // out. Retrospectively that looks exactly like a gap, which is why the decoder used to call
        // it one — and why a dropped tooth was indistinguishable from a wheel described wrongly.
        // They have different causes and different fixes.
        g.on_edge((uint8_t)cr, w.tick + 2000);
        CHECK(g.last_error_kind() == (uint8_t)TriggerErrorKind::MISSED_TOOTH);
        CHECK(g.missed_total() == 1);
        // ...and sync is DROPPED. A tooth that did not arrive means the decoder cannot prove where
        // the engine is, and an engine whose position may be a tooth out must not keep firing.
        CHECK(g.level() == SyncLvl::NONE);
    }

    SECTION("the prediction knows the gap is coming");
    {
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        int cr = g.add_gap(0, 2, 36, gi, gr, 1, 25);
        Wheel36_1 w{&g, (uint8_t)cr};
        w.spin(200); w.spin_to(0);
        CHECK(g.level() == SyncLvl::CRANK);

        // Walk exactly one revolution. A 36-1 has 35 present teeth, so exactly ONE of them must
        // predict a double interval — the gap. "About the same as last time" would be wrong at that
        // one tooth, the only place on the wheel where being wrong costs you the sync.
        int doubles = 0, singles = 0, other = 0;
        for (int i = 0; i < 35; i++) {
            w.tooth();
            const uint32_t e = g.expected_next_ticks((uint8_t)cr);
            if      (e > 1900 && e < 2100) doubles++;
            else if (e >  900 && e < 1100) singles++;
            else                           other++;
        }
        CHECK(doubles == 1);
        CHECK(singles == 34);
        CHECK(other   == 0);
    }

    // ---------------------------------------------------------------------------------------
    // THE SYNC LADDER. Phase is measured against the crank, so it cannot outlive it — but the crank
    // proves itself tooth by tooth without any help from the cam, so losing the cam must not stop
    // the engine. The asymmetry is the whole point of having a ladder.
    // ---------------------------------------------------------------------------------------

    // A 36-1 crank plus a 3-pulse cam on one timeline, from cycle `c0` for `cycles` cycles. The cam
    // can be silenced to model one that has died while the crank keeps turning.
    auto spin_cc = [](GenericTrigger& g, int cr, int cam, int c0, int cycles, bool with_cam) {
        struct E { uint32_t tick; int idx; };
        std::vector<E> ev;
        for (int c = c0; c < c0 + cycles; c++) {
            for (int rev = 0; rev < 2; rev++) for (int slot = 0; slot < 36; slot++) {
                if (slot == 35) continue;
                ev.push_back({(uint32_t)(c*7200 + rev*3600 + slot*100) * K, cr});
            }
            if (with_cam) for (int A : {0, 2400, 5100})
                ev.push_back({(uint32_t)(c*7200 + A) * K, cam});
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a, const E&b){ return a.tick < b.tick; });
        for (auto& e : ev) g.on_edge((uint8_t)e.idx, e.tick);
    };

    SECTION("the cam stops: PHASE is lost, CRANK is kept, the engine keeps running");
    {
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        int cr  = g.add_gap(0, 2, 36, gi, gr, 1, 25);
        AngleDeg10 cell[3] = {2400,2700,2100};
        int cam = g.add_seq(2, REPEATS_PHASE, cell, 3, 20);
        spin_cc(g, cr, cam, 0, 8, true);
        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(g.rev_known());

        // The cam dies; the crank keeps turning. Nothing checked this before — rev_known_ was set by
        // the last pulse ever seen and stayed set, so the decoder went on claiming PHASE, and firing
        // sequentially on a revolution bit with nothing behind it, for as long as the crank ran.
        // The crank teeth themselves are what notice: past one cycle's worth (70 present teeth on a
        // 36-1 four-stroke) plus a tooth, the sync did not land where the crank says it must.
        spin_cc(g, cr, cam, 8, 2, false);
        CHECK(g.level() == SyncLvl::CRANK);               // demoted, not cut
        CHECK(!g.rev_known());
        CHECK(g.last_error_kind() == (uint8_t)TriggerErrorKind::PHASE_LOST);
    }

    SECTION("the crank stops: PHASE goes with it — phase cannot outlive what it is measured against");
    {
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        int cr  = g.add_gap(0, 2, 36, gi, gr, 1, 25);
        AngleDeg10 cell[3] = {2400,2700,2100};
        int cam = g.add_seq(2, REPEATS_PHASE, cell, 3, 20);
        spin_cc(g, cr, cam, 0, 8, true);
        CHECK(g.level() == SyncLvl::PHASE);

        // One crank tooth out of place unlocks the crank stream while the CAM is still locked. The
        // tail used to ask any_locked(), which the cam satisfies, so level_ kept its last value and
        // the decoder reported full PHASE confidence in a position nothing was still measuring.
        g.on_edge((uint8_t)cr, (uint32_t)(8*7200 + 500) * K);
        CHECK(g.level() == SyncLvl::NONE);
        CHECK(!g.rev_known());
    }

    SECTION("outside its rpm band a silent cam does NOT demote — it is being ignored on purpose");
    {
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        int cr  = g.add_gap(0, 2, 36, gi, gr, 1, 25);
        AngleDeg10 cell[3] = {2400,2700,2100};
        int cam = g.add_seq(2, REPEATS_PHASE, cell, 3, 20);
        spin_cc(g, cr, cam, 0, 8, true);
        CHECK(g.level() == SyncLvl::PHASE);

        // The band exists because a VR cam glitches at low rpm; outside it the cam is ignored for
        // acquisition AND correction. Demoting for the absence of something deliberately ignored
        // would make the band worse than useless — it would turn "do not trust this" into "cut".
        g.set_phase_stream_valid(false);
        spin_cc(g, cr, cam, 8, 4, false);
        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(g.rev_known());
    }

    // ---------------------------------------------------------------------------------------
    // THE ANCHOR MODEL. A stream that is locked but not ABSOLUTE knows speed and distance, never
    // position. This is the regression that forced the redesign: an even crank reported PHASE with
    // a permanently wrong angle, because fuse() took the fine stream's position within its own
    // pattern to BE the engine's position — true only for a wheel with a unique feature.
    // ---------------------------------------------------------------------------------------

    SECTION("an even crank is anchored BY THE CAM, to within half a tooth");
    {
        GenericTrigger g;
        int cr  = g.add_gap(0, 2, 24, nullptr, nullptr, 0, 25);          // 24 even teeth, 150 dd pitch
        int cam = g.add_width(2, REPEATS_PHASE, 600, 1100, 1000, true);  // 80 deg pulse, reference @100 deg
        struct E { uint32_t tick; int idx; AngleDeg10 tru; bool rising; };
        std::vector<E> ev;
        for (int c = 0; c < 8; c++) {
            for (int rev = 0; rev < 2; rev++) for (int t = 0; t < 24; t++) {
                uint32_t a = c*7200 + rev*3600 + t*150;
                ev.push_back({a*K, cr, (AngleDeg10)(a % 7200), true});
            }
            ev.push_back({(uint32_t)(c*7200 + 1000)*K,     cam, 1000, true});
            ev.push_back({(uint32_t)(c*7200 + 1000+800)*K, cam, 1000, false});
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){return a.tick<b.tick;});

        AngleDeg10 worst = 0; int measured = 0;
        for (auto& e : ev) {
            g.on_edge((uint8_t)e.idx, e.tick, e.rising);
            if (e.idx == cr && g.level() != SyncLvl::NONE) {
                const AngleDeg10 err = adist(g.angle(), e.tru);
                if (err > worst) worst = err;
                measured++;
            }
        }
        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(measured > 100);
        // EXACT, not half a tooth. The carry from the cam edge to the tooth that applies the anchor
        // is measured against the pitch that just closed rather than guessed at half a pitch, so
        // nothing is left over. This read `worst <= 75` (half of the 150 dd pitch) while the anchor
        // was centred; before the anchor model existed at all it was 45.0 deg on this exact wheel,
        // steady, at PHASE, and 165.0 deg after a single missing tooth re-locked the pattern
        // somewhere else.
        CHECK(worst == 0);
    }

    SECTION("...and a missing tooth does not re-randomise it");
    {
        GenericTrigger g;
        int cr  = g.add_gap(0, 2, 24, nullptr, nullptr, 0, 25);
        int cam = g.add_width(2, REPEATS_PHASE, 600, 1100, 1000, true);
        struct E { uint32_t tick; int idx; AngleDeg10 tru; bool rising; };
        std::vector<E> ev;
        for (int c = 0; c < 12; c++) {
            for (int rev = 0; rev < 2; rev++) for (int t = 0; t < 24; t++) {
                if (c == 4 && rev == 0 && t == 7) continue;       // <-- one tooth never arrives
                uint32_t a = c*7200 + rev*3600 + t*150;
                ev.push_back({a*K, cr, (AngleDeg10)(a % 7200), true});
            }
            ev.push_back({(uint32_t)(c*7200 + 1000)*K,     cam, 1000, true});
            ev.push_back({(uint32_t)(c*7200 + 1000+800)*K, cam, 1000, false});
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){return a.tick<b.tick;});

        AngleDeg10 worst_after = 0;
        for (auto& e : ev) {
            g.on_edge((uint8_t)e.idx, e.tick, e.rising);
            // Measure only well after the fault, once it has re-acquired and been re-anchored.
            if (e.idx == cr && g.level() != SyncLvl::NONE && e.tick > (uint32_t)(7*7200)*K) {
                const AngleDeg10 err = adist(g.angle(), e.tru);
                if (err > worst_after) worst_after = err;
            }
        }
        // The strict window drops the lock on the long interval, the even wheel re-locks its pattern
        // at a DIFFERENT arbitrary tooth — and that no longer matters, because the arbitrary origin
        // is not the position any more. The cam re-anchors and the error returns to half a tooth
        // instead of to somewhere new.
        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(worst_after <= 75);
    }

    // ---------------------------------------------------------------------------------------
    // PHASED CAMS. A phaser moves the cam relative to the crank by tens of degrees, continuously,
    // under closed-loop control — so a check that expects the cam at a FIXED position drops phase
    // forever on a healthy VVT engine. The rig cannot move a cam, so this is the only place the
    // behaviour is provable.
    // ---------------------------------------------------------------------------------------

    // 36-1 crank + WIDTH cam whose pulse can be placed at any engine angle, to model a phaser.
    // Returns the level reached after `cycles` cycles with the cam displaced by `disp`.
    auto run_phased = [](AngleDeg10 nominal, bool phased, AngleDeg10 authority,
                         AngleDeg10 disp, int cycles, uint32_t* lost_out,
                         AngleDeg10 allowance = 0) {
        static GenericTrigger g;                 // static: the lambda outlives each call's frame
        g = GenericTrigger{};
        uint8_t gi[1]={0}, gr[1]={2};
        int cr  = g.add_gap(0, 2, 36, gi, gr, 1, 25);
        int cam = g.add_width(2, REPEATS_PHASE, 600, 1100, nominal, true);
        g.set_phase_meta(2, nominal, phased, authority, allowance);
        struct E { uint32_t tick; int idx; bool rising; };
        std::vector<E> ev;
        for (int c = 0; c < cycles; c++) {
            for (int rev = 0; rev < 2; rev++) for (int slot = 0; slot < 36; slot++) {
                if (slot == 35) continue;
                ev.push_back({(uint32_t)(c*7200 + rev*3600 + slot*100) * K, cr, true});
            }
            // The cam pulse, displaced from nominal by `disp` — a phaser doing its job.
            const int at = c*7200 + nominal + disp;
            ev.push_back({(uint32_t)at * K,       cam, true});
            ev.push_back({(uint32_t)(at+800) * K, cam, false});
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a, const E&b){ return a.tick < b.tick; });
        // Count phase losses only AFTER acquisition has settled: the first cycles legitimately
        // include the moment before the crank is anchored, and a sticky last_error_kind cannot tell
        // that apart from a check which is failing every cycle.
        uint32_t lost_at_settle = 0; bool settled = false;
        for (auto& e : ev) {
            g.on_edge((uint8_t)e.idx, e.tick, e.rising);
            if (!settled && g.level() == SyncLvl::PHASE) { settled = true; lost_at_settle = g.phase_lost_total(); }
        }
        if (lost_out) *lost_out = g.phase_lost_total() - lost_at_settle;
        return g.level();
    };

    SECTION("a phased cam holds PHASE right across its authority");
    {
        uint32_t lost = 0;
        // 60 deg of authority; walk the cam through it. Every one of these is a healthy engine.
        for (AngleDeg10 d : {(AngleDeg10)0, (AngleDeg10)150, (AngleDeg10)300,
                             (AngleDeg10)450, (AngleDeg10)600}) {
            const SyncLvl lvl = run_phased(2000, /*phased*/true, /*authority*/600, d, 10, &lost);
            CHECK(lvl == SyncLvl::PHASE);
            CHECK(lost == 0);            // not one downgrade once running
        }
    }

    SECTION("...and the SAME displacement on a FIXED cam is a fault");
    {
        uint32_t lost = 0;
        // 30 deg out on a cam that cannot move is a chain that has jumped, not a phaser working.
        const SyncLvl lvl = run_phased(2000, /*phased*/false, /*authority*/0, 300, 8, &lost);
        CHECK(lvl == SyncLvl::CRANK);            // demoted, crank kept — the ladder holds
        CHECK(lost > 0);
    }

    SECTION("a FIXED cam's band is its configured allowance, not the crank's tooth pitch");
    {
        // The band used to be fine_pitch_ alone, and that was a structural defect rather than a
        // number that happened to be wrong: pitch is a property of the CRANK WHEEL, while what is
        // being bounded is CAM DRIVE error. Coupling them means a coarse crank silently disables cam
        // validation — a 6g72's three teeth make a 120 deg band, wide enough for the cam to slip a
        // whole tooth and pass — and it would still be the wrong derivation even where the number
        // came out tolerable.
        //
        // Here the wheel is a 36-1, so the floor is 10 deg. A fixed cam 30 deg out is a jumped chain
        // and must fault; the same engine told that 40 deg of mechanical slop is expected must not.
        // Chain stretch, gear lash and torsional wind-up are engine properties nothing in the
        // trigger layer can derive, so they are configured.
        uint32_t lost = 0;
        CHECK(run_phased(2000, false, 0, 300, 8, &lost, /*allowance*/0)   == SyncLvl::CRANK);
        CHECK(lost > 0);                                  // 30 deg out, no allowance: a fault
        lost = 0;
        CHECK(run_phased(2000, false, 0, 300, 8, &lost, /*allowance*/400) == SyncLvl::PHASE);
        CHECK(lost == 0);                                 // the same 30 deg, declared expected
        // ...and the allowance only ever WIDENS. Setting it below the measurement floor cannot make
        // the check tighter than the decoder can honestly measure.
        lost = 0;
        CHECK(run_phased(2000, false, 0, 50, 8, &lost, /*allowance*/1)    == SyncLvl::PHASE);
        CHECK(lost == 0);                                 // 5 deg is inside the 10 deg floor
    }

    SECTION("CHAIN STRETCH: the residual measures the wear, to better than a crank tooth");
    {
        // A timing chain wears and the cam RETARDS, a degree or two at a time, over thousands of
        // miles. A chain that JUMPS a sprocket tooth steps instead. Both put the cam away from
        // nominal and only one is a reason to stop the engine, so the decoder's job is to MEASURE
        // the displacement honestly and let policy above it decide; phase_resid_ is that number.
        //
        // It could not do that while the comparison used angle_, which only advances on a crank
        // tooth: on a 36-1 the residual was quantised to 10 deg, so 2, 5 and 9 deg of stretch all
        // measured 0.0 and were invisible — and the rounding went the FAULT'S way, so 45 deg
        // measured 40.0 and passed a 40 deg band. It is now interpolated to the cam edge.
        auto resid_for = [&](AngleDeg10 disp, AngleDeg10 allowance) {
            GenericTrigger g;
            uint8_t gi[1] = {0}, gr[1] = {2};
            const int cr  = g.add_gap(0, 2, 36, gi, gr, 1, 25);
            const int cam = g.add_width(2, REPEATS_PHASE, 600, 1100, 2000, true);
            (void)cr; (void)cam;
            g.set_phase_meta(2, 2000, /*phased*/false, /*authority*/0, allowance);
            struct E { uint32_t tick; int idx; bool rising; };
            std::vector<E> ev;
            for (int c = 0; c < 10; c++) {
                for (int rev = 0; rev < 2; rev++) for (int slot = 0; slot < 36; slot++) {
                    if (slot == 35) continue;
                    ev.push_back({(uint32_t)(c*7200 + rev*3600 + slot*100) * K, cr, true});
                }
                const int at = c*7200 + 2000 + disp;
                ev.push_back({(uint32_t)at * K,       cam, true});
                ev.push_back({(uint32_t)(at+800) * K, cam, false});
            }
            std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){return a.tick<b.tick;});
            for (auto& e : ev) { g.set_rpm(2000); g.on_edge((uint8_t)e.idx, e.tick, e.rising); }
            struct Out { int resid; SyncLvl lvl; } o{ (int)(int16_t)g.phase_residual(2), g.level() };
            return o;
        };
        // SUB-TOOTH RESOLUTION. Every one of these was 0.0 before, on a 10 deg pitch.
        CHECK(resid_for(20,  400).resid == 20);          //  2.0 deg of stretch
        CHECK(resid_for(50,  400).resid == 50);          //  5.0
        CHECK(resid_for(90,  400).resid == 90);          //  9.0
        CHECK(resid_for(150, 400).resid == 150);         // 15.0 — was 10.0, a whole tooth low
        CHECK(resid_for(450, 400).resid == 450);         // 45.0 — was 40.0, and so PASSED a 40 band
        // ...and the band now means what it says, in both directions.
        CHECK(resid_for(400, 400).lvl == SyncLvl::PHASE);   // 40.0 inside a 40.0 allowance
        CHECK(resid_for(450, 400).lvl == SyncLvl::CRANK);   // 45.0 outside it — was passing
        CHECK(resid_for(100, 0).lvl   == SyncLvl::PHASE);   // 10.0 inside the 10 deg floor
        CHECK(resid_for(110, 0).lvl   == SyncLvl::CRANK);   // 11.0 outside it — was passing
    }

    SECTION("...and wear that CREEPS is held while a chain that JUMPS is not");
    {
        // The two failures differ in how they arrive, not in what they look like at any instant, so
        // the decoder cannot tell them apart from one reading — and should not try. What it must do
        // is keep reporting a truthful number as the wear accumulates, so that something with
        // history can. Here the cam retards a degree per cycle: PHASE is held while it is inside the
        // declared mechanical allowance, the residual tracks it the whole way, and it faults once —
        // and only once — the wear passes what the engine was said to tolerate.
        GenericTrigger g;
        uint8_t gi[1] = {0}, gr[1] = {2};
        const int cr  = g.add_gap(0, 2, 36, gi, gr, 1, 25);
        const int cam = g.add_width(2, REPEATS_PHASE, 600, 1100, 2000, true);
        (void)cr; (void)cam;
        g.set_phase_meta(2, 2000, /*phased*/false, /*authority*/0, /*allowance*/200);   // 20.0 deg
        int last_resid = -1, faulted_at = -1;
        uint32_t t_base = 0;
        for (int c = 0; c < 40; c++) {
            const int disp = c * 10;                       // 1.0 deg more retarded each cycle
            struct E { uint32_t tick; int idx; bool rising; };
            std::vector<E> ev;
            for (int rev = 0; rev < 2; rev++) for (int slot = 0; slot < 36; slot++) {
                if (slot == 35) continue;
                ev.push_back({t_base + (uint32_t)(rev*3600 + slot*100) * K, cr, true});
            }
            ev.push_back({t_base + (uint32_t)(2000 + disp) * K,       cam, true});
            ev.push_back({t_base + (uint32_t)(2000 + disp + 800) * K, cam, false});
            std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){return a.tick<b.tick;});
            for (auto& e : ev) { g.set_rpm(2000); g.on_edge((uint8_t)e.idx, e.tick, e.rising); }
            t_base += 7200 * K;
            if (c >= 3) {                                  // past acquisition
                const int r = (int)(int16_t)g.phase_residual(2);
                CHECK(r == disp);                          // the number tracks the wear, exactly
                if (r > last_resid) last_resid = r;
                if (faulted_at < 0 && g.level() != SyncLvl::PHASE) faulted_at = disp;
            }
        }
        CHECK(last_resid >= 350);                          // it kept measuring past the fault
        CHECK(faulted_at > 200);                           // held right up to the declared allowance
        CHECK(faulted_at <= 220);                          // and gave way immediately after it
    }

    SECTION("a phased cam BEYOND its authority is still a fault");
    {
        uint32_t lost = 0;
        // 90 deg on a 60 deg phaser: the phaser cannot have done this.
        const SyncLvl lvl = run_phased(2000, /*phased*/true, /*authority*/600, 900, 8, &lost);
        CHECK(lvl == SyncLvl::CRANK);
        CHECK(lost > 0);
    }

    SECTION("a PHASED cam cannot anchor an even crank — that engine never knows where it is");
    {
        GenericTrigger g;
        int cr  = g.add_gap(0, 2, 24, nullptr, nullptr, 0, 25);           // even: no feature of its own
        int cam = g.add_width(2, REPEATS_PHASE, 600, 1100, 1000, true);
        g.set_phase_meta(2, 1000, /*phased*/true, /*authority*/600);
        struct E { uint32_t tick; int idx; bool rising; };
        std::vector<E> ev;
        for (int c = 0; c < 8; c++) {
            for (int rev = 0; rev < 2; rev++) for (int t = 0; t < 24; t++)
                ev.push_back({(uint32_t)(c*7200 + rev*3600 + t*150) * K, cr, true});
            ev.push_back({(uint32_t)(c*7200 + 1000) * K,       cam, true});
            ev.push_back({(uint32_t)(c*7200 + 1000+800) * K,   cam, false});
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a, const E&b){ return a.tick < b.tick; });
        for (auto& e : ev) g.on_edge((uint8_t)e.idx, e.tick, e.rising);
        // An anchor has to be a fixed reference. Anchoring to a phaser would inject its current
        // displacement into the engine's position and move it every time the cam moved — a wrong
        // answer that changes while you watch, which is worse than no answer.
        // rev_known() may well be true — the cam really did identify the revolution. What it cannot
        // do is say WHERE IN that revolution the engine is, and a revolution number attached to an
        // unknown position is not a position. The level is the thing that must not lie.
        CHECK(g.level() == SyncLvl::NONE);
    }

    // ---------------------------------------------------------------------------------------
    // TWO CAMS, ONE PER BANK — a V engine. Both can identify the cycle and both move independently
    // under their own phasers, so the cycle needs one spokesman; and a chain is per bank, so a fault
    // has to name the bank rather than say "the trigger is unhappy".
    // ---------------------------------------------------------------------------------------

    // 36-1 crank + two WIDTH cams, bank 1 in slot 2 and bank 2 in slot 4, each with its own nominal
    // and its own displacement. Returns the level reached.
    auto run_two_cam = [](AngleDeg10 nom1, AngleDeg10 disp1,
                          AngleDeg10 nom2, AngleDeg10 disp2,
                          AngleDeg10 authority, uint32_t* lost, uint8_t* who) {
        static GenericTrigger g;
        g = GenericTrigger{};
        uint8_t gi[1]={0}, gr[1]={2};
        int cr = g.add_gap(0, 2, 36, gi, gr, 1, 25);
        int b1 = g.add_width(2, REPEATS_PHASE, 600, 1100, nom1, true);   // Cam Intake B1
        int b2 = g.add_width(4, REPEATS_PHASE, 600, 1100, nom2, true);   // Cam Intake B2
        g.set_phase_meta(2, nom1, true, authority);
        g.set_phase_meta(4, nom2, true, authority);
        struct E { uint32_t tick; int idx; bool rising; };
        std::vector<E> ev;
        for (int c = 0; c < 10; c++) {
            for (int rev = 0; rev < 2; rev++) for (int slot = 0; slot < 36; slot++) {
                if (slot == 35) continue;
                ev.push_back({(uint32_t)(c*7200 + rev*3600 + slot*100) * K, cr, true});
            }
            const int a1 = c*7200 + nom1 + disp1, a2 = c*7200 + nom2 + disp2;
            ev.push_back({(uint32_t)a1 * K, b1, true});  ev.push_back({(uint32_t)(a1+800) * K, b1, false});
            ev.push_back({(uint32_t)a2 * K, b2, true});  ev.push_back({(uint32_t)(a2+800) * K, b2, false});
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a, const E&b){ return a.tick < b.tick; });
        uint32_t at_settle = 0; bool settled = false;
        for (auto& e : ev) {
            g.on_edge((uint8_t)e.idx, e.tick, e.rising);
            if (!settled && g.level() == SyncLvl::PHASE) { settled = true; at_settle = g.phase_lost_total(); }
        }
        if (lost) *lost = g.phase_lost_total() - at_settle;
        if (who)  *who  = g.last_error_stream();
        return g.level();
    };

    SECTION("two healthy cams in DIFFERENT revolutions is normal, not a disagreement");
    {
        uint32_t lost = 0; uint8_t who = 0;
        // Bank 1 fires in revolution 0, bank 2 in revolution 1 — which is how a V engine is built.
        // Comparing the revolution each cam reports would call this a fault every single cycle.
        const SyncLvl lvl = run_two_cam(2000, 0, 5600, 0, 600, &lost, &who);
        CHECK(lvl == SyncLvl::PHASE);
        CHECK(lost == 0);
    }

    SECTION("both banks' phasers moving independently is still normal");
    {
        uint32_t lost = 0;
        // Bank 1 advanced 40 deg, bank 2 advanced 15 the other way. Both inside authority.
        const SyncLvl lvl = run_two_cam(2000, 400, 5600, (AngleDeg10)-150, 600, &lost, nullptr);
        CHECK(lvl == SyncLvl::PHASE);
        CHECK(lost == 0);
    }

    SECTION("ONE bank out of position is a fault that NAMES that bank");
    {
        uint32_t lost = 0; uint8_t who = 0;
        // Bank 2's chain has jumped 90 deg on a 60 deg phaser; bank 1 is perfect.
        const SyncLvl lvl = run_two_cam(2000, 0, 5600, 900, 600, &lost, &who);
        CHECK(lost > 0);
        CHECK(who == 4);                         // Cam Intake B2, not "the trigger"
        // Phase is KEPT. The engine's position comes from the crank and from bank 1, and both are
        // fine — bank 2's chain having jumped is a mechanical fault to report, not a reason to throw
        // away position knowledge that is still correct and stop firing sequentially on it. Whether
        // it should cut is a protection decision made from the fault, not one the decoder makes by
        // forgetting what it knows.
        CHECK(lvl == SyncLvl::PHASE);
    }

    SECTION("the cycle has ONE spokesman, and it is the lowest-numbered locked cam");
    {
        GenericTrigger g;
        uint8_t gi[1]={0}, gr[1]={2};
        int cr = g.add_gap(0, 2, 36, gi, gr, 1, 25);
        int b2 = g.add_width(4, REPEATS_PHASE, 600, 1100, 5600, true);   // ONLY bank 2 wired
        g.set_phase_meta(4, 5600, true, 600);
        struct E { uint32_t tick; int idx; bool rising; };
        std::vector<E> ev;
        for (int c = 0; c < 10; c++) {
            for (int rev = 0; rev < 2; rev++) for (int slot = 0; slot < 36; slot++) {
                if (slot == 35) continue;
                ev.push_back({(uint32_t)(c*7200 + rev*3600 + slot*100) * K, cr, true});
            }
            ev.push_back({(uint32_t)(c*7200 + 5600) * K,     b2, true});
            ev.push_back({(uint32_t)(c*7200 + 5600+800) * K, b2, false});
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a, const E&b){ return a.tick < b.tick; });
        for (auto& e : ev) g.on_edge((uint8_t)e.idx, e.tick, e.rising);
        // With bank 1 absent, bank 2 is the lowest LOCKED phase slot and speaks for the cycle. The
        // nomination follows a defined order rather than whichever edge arrived last, so it does not
        // change under scheduling — but it does fall back when a bank is simply not there.
        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(g.rev_known());
    }

    // ---------------------------------------------------------------------------------------
    // TWO CRANK SENSORS. Common since the 1980s and never two clocks: a Motronic flywheel is a
    // relative ring gear plus a single TDC mark, a GM 3800 a fine even track plus a coarse
    // asymmetric one. One stream provides the angle; the other provides the thing the first cannot.
    // ---------------------------------------------------------------------------------------

    SECTION("Motronic flywheel: an even ring gear anchored by a TDC mark on the second crank slot");
    {
        GenericTrigger g;
        // Slot 0, Crank Primary: 24 even teeth, 150 dd apart. Relative — it can never locate itself.
        int ring = g.add_gap(0, 2, 24, nullptr, nullptr, 0, 25);
        // Slot 1, Crank Secondary: one reference pulse per revolution, identified by width.
        int tdc  = g.add_width(1, 2 /*crank rate*/, 300, 900, 900, true);
        struct E { uint32_t tick; int idx; bool rising; };
        std::vector<E> ev;
        for (int c = 0; c < 8; c++)
            for (int rev = 0; rev < 2; rev++) {
                for (int t = 0; t < 24; t++)
                    ev.push_back({(uint32_t)(c*7200 + rev*3600 + t*150) * K, ring, true});
                // the mark: a 60 deg pulse whose reference sits at 90 deg into each revolution
                const int a = c*7200 + rev*3600 + 900;
                ev.push_back({(uint32_t)a * K, tdc, true});
                ev.push_back({(uint32_t)(a + 600) * K, tdc, false});
            }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a, const E&b){ return a.tick < b.tick; });
        for (auto& e : ev) g.on_edge((uint8_t)e.idx, e.tick, e.rising);
        // CRANK, not NONE: the ring gear counts and the mark says where it is counting from. An
        // earlier version of the config rule refused this arrangement outright, which would have
        // turned away a Porsche 944; an earlier version of the fusion let both streams drive the
        // position and they fought over it.
        CHECK(g.level() == SyncLvl::CRANK);
        CHECK(g.velocity() > 0);
    }

    SECTION("...and the fine track alone, with the mark removed, cannot reach CRANK");
    {
        GenericTrigger g;
        int ring = g.add_gap(0, 2, 24, nullptr, nullptr, 0, 25);
        std::vector<Ev> ev;
        for (int c = 0; c < 8; c++) for (int rev = 0; rev < 2; rev++) for (int t = 0; t < 24; t++)
            ev.push_back({(uint32_t)(c*7200 + rev*3600 + t*150) * K, ring});
        feed_all(g, ev);
        // Same wheel, same teeth, same velocity — and no idea where it is. That is the difference
        // the second sensor exists to make.
        CHECK(g.level() == SyncLvl::NONE);
        CHECK(g.velocity() > 0);
    }

    SECTION("THE ANCHOR IS INDEPENDENT OF WHICH TOOTH APPLIES IT");
    {
        // This is the property the measured carry buys, and it is worth more than the accuracy.
        //
        // The anchor is queued at the reference edge and applied at the next crank tooth. When that
        // reference lands ON a crank tooth, whether the capture ISR services the cam or the tooth
        // first decides between applying on THAT tooth and applying on the NEXT — and while the
        // carry was a fixed half pitch, those two answers were a WHOLE PITCH apart. Measured before
        // the fix: 15.0 deg on this 24-tooth wheel, 2.0 deg on a 180-slit Nissan, and a single tick
        // of jitter flipped between them. A tuner cancels a constant offset with a timing light;
        // an offset that alternates between two values cannot be cancelled by anything.
        //
        // Measuring the carry makes both orderings agree, because applying one tooth later also
        // measures one more pitch of travel.
        auto anchored_angle = [](int teeth, bool cam_first) {
            const int pitch = 3600 / teeth;
            const AngleDeg10 cam_start = 1800;          // 1800 % pitch == 0 for 24 and 180: ON a tooth
            GenericTrigger g;
            int cr  = g.add_gap(0, 2, teeth, nullptr, nullptr, 0, 25);
            int cam = g.add_width(2, REPEATS_PHASE, 600, 1100, cam_start, true);
            struct E { uint32_t tick; int idx; AngleDeg10 tru; bool rising; int prio; };
            std::vector<E> ev;
            for (int c = 0; c < 12; c++) {
                for (int rev = 0; rev < 2; rev++) for (int t = 0; t < teeth; t++) {
                    uint32_t a = c*7200 + rev*3600 + t*pitch;
                    ev.push_back({a*K, cr, (AngleDeg10)(a % 7200), true, 1});
                }
                const uint32_t trail = (uint32_t)(c*7200 + cam_start)*K;
                ev.push_back({trail - 800*K, cam, 0, true,  0});
                // prio decides the tie at an identical tick: the cam edge serviced before the
                // crank tooth, or after it. Nothing else about the stimulus changes.
                ev.push_back({trail,         cam, 0, false, cam_first ? 0 : 2});
            }
            std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){
                return a.tick != b.tick ? a.tick < b.tick : a.prio < b.prio; });
            AngleDeg10 last = 0; AngleDeg10 tru = 0;
            for (auto& e : ev) {
                g.on_edge((uint8_t)e.idx, e.tick, e.rising);
                if (e.idx == cr && g.level() != SyncLvl::NONE) { last = g.angle(); tru = e.tru; }
            }
            return std::make_pair(last, tru);
        };
        for (int teeth : {24, 180}) {
            const auto before = anchored_angle(teeth, true);
            const auto after  = anchored_angle(teeth, false);
            CHECK(before.first == after.first);        // THE PROPERTY: order cannot change the answer
            CHECK(before.second == after.second);      // (and both ran to the same ground truth)
        }
    }

    SECTION("...and on a COARSE even crank, where the old guess was worth 60 degrees");
    {
        // A 6g72 has three crank teeth: a 120 deg pitch, so centring was worth up to +/-60 deg of
        // anchor error depending purely on where the cam happened to sit between teeth. It was
        // constant for a given engine and so absorbed by trigger_offset_btdc, which is why it never
        // showed up as a running fault — but it made the offset a property of the wheel geometry
        // rather than of the engine, and it is exactly the case where an ISR-order flip would have
        // cost a whole 120 deg.
        for (int off : {0, 300, 600, 900, 1190}) {
            GenericTrigger g;
            const int teeth = 3, pitch = 1200;
            const AngleDeg10 cam_start = (AngleDeg10)(1800 + off);
            int cr  = g.add_gap(0, 2, teeth, nullptr, nullptr, 0, 25);
            int cam = g.add_width(2, REPEATS_PHASE, 600, 1100, cam_start, true);
            struct E { uint32_t tick; int idx; AngleDeg10 tru; bool rising; };
            std::vector<E> ev;
            for (int c = 0; c < 10; c++) {
                for (int rev = 0; rev < 2; rev++) for (int t = 0; t < teeth; t++) {
                    uint32_t a = c*7200 + rev*3600 + t*pitch;
                    ev.push_back({a*K, cr, (AngleDeg10)(a % 7200), true});
                }
                ev.push_back({(uint32_t)(c*7200 + cam_start)*K,       cam, 0, true});
                ev.push_back({(uint32_t)(c*7200 + cam_start+800)*K,   cam, 0, false});
            }
            std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){return a.tick<b.tick;});
            AngleDeg10 worst = 0; int measured = 0;
            for (auto& e : ev) {
                g.on_edge((uint8_t)e.idx, e.tick, e.rising);
                if (e.idx == cr && g.level() != SyncLvl::NONE) {
                    const AngleDeg10 err = adist(g.angle(), e.tru);
                    if (err > worst) worst = err;
                    measured++;
                }
            }
            CHECK(measured > 20);
            CHECK(worst == 0);           // was up to 550 dd across this sweep before the fix
        }
    }

    return test_summary();
}
