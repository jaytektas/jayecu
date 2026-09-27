// Tests build_generic() — the schema-config → GenericTrigger bridge. Builds a decoder from a
// synthetic StreamsConfig[] and confirms it decodes the same as a hand-wired one (crank GAP 36-1 +
// cam SEQUENCE → PHASE).
//
// Cells are PER STREAM. They were a shared pool addressed by a cell_off, which meant these tests had
// to hand-partition a pool and keep the offsets in step with the lengths; now each stream carries its
// own run from 0 and there is no partition to get wrong.

#include "test_helpers.h"
#include "GenericTriggerBuilder.h"
#include <vector>
#include <algorithm>
#include <cstdint>

static constexpr uint32_t K = 10;

int main() {
    fprintf(stdout, "=== build_generic (config → GenericTrigger) ===\n");

    SECTION("build crank GAP(36-1) + cam SEQUENCE from config → PHASE");
    {
        // THE INDEX IS THE ROLE: 0 Crank Primary, 1 Crank Secondary, 2..5 the cams. The cam sits at
        // slot 2 and slot 1 is left EMPTY, which is the case that used to be unrepresentable — the
        // builder appended, so a hole shifted every later stream's decoder index away from the config
        // index the position HAL feeds on_edge.
        StreamsConfig streams[MAX_STREAMS] = {};
        streams[0].enabled = 1; streams[0].capture_index = 0; streams[0].edge = 0;
        streams[0].primitive = 0;
        streams[0].slots = 36; streams[0].gap_ratio = 2; streams[0].cell_len = 1;
        streams[0].window_pct = 25;
        streams[0].cell[0].v = 0;                       // GAP gap index
        streams[2].enabled = 1; streams[2].capture_index = 4; streams[2].edge = 0;
        streams[2].primitive = 1;
        streams[2].cell_len = 3; streams[2].window_pct = 20;
        streams[2].cell[0].v = 2400; streams[2].cell[1].v = 2700; streams[2].cell[2].v = 2100;  // Σ=7200

        GenericTrigger g;
        build_generic(g, streams, MAX_STREAMS);

        // feed interleaved crank + cam edges on a tick timeline
        struct E { uint32_t tick; int idx; };
        std::vector<E> ev;
        for (int c=0;c<8;c++) {
            for (int rev=0;rev<2;rev++) for (int slot=0;slot<36;slot++) {
                if (slot==35) continue;
                uint32_t a = c*7200 + rev*3600 + slot*100; ev.push_back({a*K, 0});
            }
            for (int A : {0,2400,5100}) ev.push_back({(uint32_t)(c*7200+A)*K, 2});   // slot 2 = Cam Intake B1
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){return a.tick<b.tick;});
        for (auto& e : ev) g.on_edge((uint8_t)e.idx, e.tick);

        CHECK(g.level() == SyncLvl::PHASE);
        CHECK(g.rev_known());
    }

    SECTION("a DISABLED slot is not built, and a hole does not shift its neighbours");
    {
        // The structural claim of role-indexed streams: slot 2 is Cam Intake B1 whether or not slot 1
        // is populated. When the builder appended, a hole shifted every later stream's decoder index
        // away from the config index the position HAL feeds on_edge — silently, so the stream simply
        // stopped decoding. This is the case that could not previously be expressed at all.
        StreamsConfig streams[MAX_STREAMS] = {};
        streams[0].enabled = 1; streams[0].primitive = 0; streams[0].slots = 36;
        streams[0].gap_ratio = 2; streams[0].cell_len = 1;
        streams[0].window_pct = 25;
        streams[0].cell[0].v = 0;
        // slot 1 (Crank Secondary) deliberately LEFT OFF.
        streams[2].enabled = 1; streams[2].primitive = 1;
        streams[2].cell_len = 3; streams[2].window_pct = 20;
        streams[2].cell[0].v = 2400; streams[2].cell[1].v = 2700; streams[2].cell[2].v = 2100;
        // slot 3 configured but DISABLED — it must be ignored entirely, not merely skipped in binding.
        // It carries the SAME cells as slot 2 now, which is the point: with a private pool that is a
        // duplicate description, not a shared run, so nothing about it can disturb slot 2.
        streams[3].enabled = 0; streams[3].primitive = 1;
        streams[3].cell_len = 3; streams[3].window_pct = 20;
        streams[3].cell[0].v = 2400; streams[3].cell[1].v = 2700; streams[3].cell[2].v = 2100;

        GenericTrigger g;
        build_generic(g, streams, MAX_STREAMS);

        struct E { uint32_t tick; int idx; };
        std::vector<E> ev;
        for (int c=0;c<8;c++) {
            for (int rev=0;rev<2;rev++) for (int slot=0;slot<36;slot++) {
                if (slot==35) continue;
                uint32_t a = c*7200 + rev*3600 + slot*100; ev.push_back({a*K, 0});
            }
            for (int A : {0,2400,5100}) ev.push_back({(uint32_t)(c*7200+A)*K, 2});
        }
        std::stable_sort(ev.begin(), ev.end(), [](const E&a,const E&b){return a.tick<b.tick;});
        for (auto& e : ev) g.on_edge((uint8_t)e.idx, e.tick);

        CHECK(g.level() == SyncLvl::PHASE);   // the cam at slot 2 was reached at index 2
        CHECK(g.rev_known());

        // And an edge on the DISABLED slot changes nothing. Without the enabled check in on_edge it
        // would be matched against a default-constructed Stream — a wheel nobody described.
        const SyncLvl before = g.level();
        for (int k = 0; k < 40; ++k) g.on_edge(3, (uint32_t)(100000 + k*137)*K);
        CHECK(g.level() == before);
        // Slot 1 is a hole inside n_, so it must reject too.
        for (int k = 0; k < 40; ++k) g.on_edge(1, (uint32_t)(200000 + k*211)*K);
        CHECK(g.level() == before);
    }

    SECTION("build a SEQUENCE crank (odd-fire) from config → CRANK");
    {
        StreamsConfig streams[MAX_STREAMS] = {};
        streams[0].enabled = 1; streams[0].primitive = 1;   // slot 0 = Crank Primary (crank-rate SEQUENCE)
        streams[0].cell_len = 2; streams[0].window_pct = 25;
        streams[0].cell[0].v = 1350; streams[0].cell[1].v = 2250;   // 135/225 V-twin
        GenericTrigger g; build_generic(g, streams, MAX_STREAMS);
        // feed the 135/225 stream
        uint32_t t=0; const AngleDeg10 cell[2]={1350,2250};
        for (int e=0;e<14;e++){ t += cell[e%2]*K; g.on_edge(0, t); }
        CHECK(g.level() == SyncLvl::CRANK);              // crank-only → CRANK
    }

    return test_summary();
}
