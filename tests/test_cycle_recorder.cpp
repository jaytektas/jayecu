// CycleRecorder — the ISRs record, the READER decides which cycle.
//
// The property under test is exactness. A 60-2 wheel produces exactly 116 teeth per engine cycle;
// 115 is not "close", it is a tooth in the wrong cycle. The first design gated recording on a state
// flag flipped at the boundary by the GRID ISR (prio 2) and read by the CAPTURE ISR (prio 1) which
// preempts it, so an edge arriving inside that window landed in the outgoing cycle. Measured on the
// bench: 309 captures of 116, 2 of 117, 1 of 115 — always the tooth at angle ~0.
//
// So the ISRs decide nothing about STATE. They append to ONE ring, each edge carrying a marker when
// it opens its own lane's cycle — a fact the recording ISR already has (the decoder's counted wrap,
// or grid index 0), not a flag shared across priorities. The reader cuts each lane at its own marker.
//
// Cutting by ANGLE instead was tried and measured: 3753 captures at 3000 rpm gave 3743 of 116, five
// of 117 and five of 115. Real tooth 0 and virtual tooth 0 both sit at 0 deg and arrive microseconds
// apart from two interrupt priorities, so which lands first jitters, and a window bounded by one
// stream's marker gains a tooth when that order flips at one end and loses one at the other. The
// equal 5/5 is the signature. No single cut point is exact for two streams that share an angle.
//
// These tests drive the appends in orders a real ISR pair can produce — including a boundary tooth
// arriving either side of the grid mark, and a sparse output lane beside a dense trigger one — and
// require the selected cycle to be exact every time, not usually.
//
//   cmake --build build --target cycle_recorder_test && ./build/cycle_recorder_test
#include "../firmware/Scheduler/CycleRecorder.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

static const CycleHeader& hdr_of(const uint8_t* p) {
    return *reinterpret_cast<const CycleHeader*>(p);
}

// Read the whole selected cycle out, paging exactly as the host does.
static std::vector<CycleEdge> read_all(const CycleRecorder& r, CycleHeader* out_hdr = nullptr,
                                      uint8_t back = 0) {
    std::vector<CycleEdge> edges;
    uint8_t buf[sizeof(CycleHeader) + CycleRecorder::PAGE_MAX * sizeof(CycleEdge)];
    uint16_t first = 0;
    for (int guard = 0; guard < 64; ++guard) {
        const uint16_t n = r.serialize(buf, sizeof(buf), first, back);
        if (n < sizeof(CycleHeader)) break;
        const CycleHeader& h = hdr_of(buf);
        if (out_hdr) *out_hdr = h;
        for (uint16_t i = 0; i < h.count; ++i) {
            CycleEdge e{};
            std::memcpy(&e, buf + sizeof(CycleHeader) + i * sizeof(CycleEdge), sizeof(e));
            edges.push_back(e);
        }
        if (h.count == 0 || edges.size() >= h.total) break;
        first = static_cast<uint16_t>(first + h.count);
    }
    return edges;
}

// Feed one engine cycle of a 60-2 at PHASE: 116 teeth, 0.1 deg then every 6 deg.
static void feed_crank_cycle(CycleRecorder& r, int teeth = 116) {
    for (int i = 0; i < teeth; ++i)
        r.record_trigger(CycleSignal::Crank, 0, static_cast<AngleDeg10>(1 + i * 60), true, i == 0);
}

int main() {
    std::puts("=== CycleRecorder (reader-selected cycles, no ISR decision) ===");

    std::puts("\n-- disabled: the ISR fast path stores nothing --");
    {
        CycleRecorder r;
        ck(r.state() == CycleRecorder::State::Idle, "starts Idle");
        r.record_trigger(CycleSignal::Crank, 0, 100, true, true);
        r.record_output(CycleSignal::Coil, 0, 200, true);
        r.record_virtual(300, true);
        CycleHeader h{};
        (void)read_all(r, &h);
        ck(h.total == 0, "nothing recorded while disabled", std::to_string(h.total));
    }

    std::puts("\n-- one cycle is not enough: a partial cycle is never served --");
    {
        // The reader needs a wrap on BOTH sides. Serving the newest partial cycle would draw half an
        // engine cycle as though it were a whole one.
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_720, 20000, 2);
        feed_crank_cycle(r);
        CycleHeader h{};
        (void)read_all(r, &h);
        ck(h.total == 0, "one cycle in the ring yields nothing yet", std::to_string(h.total));
        ck(h.state == static_cast<uint8_t>(CycleRecorder::State::Recording),
           "and it reports Recording, not Complete");
    }

    std::puts("\n-- three cycles: EXACTLY 116 teeth come back --");
    {
        // A COMPLETE cycle needs a wrap on BOTH sides, so three cycles are fed to bound the middle
        // one. That is a real property of the design, not a quirk of the test: after enabling, the
        // first complete cycle becomes available on the third boundary. At 1200 rpm that is 300 ms.
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_720, 20000, 2);
        feed_crank_cycle(r);
        feed_crank_cycle(r);
        feed_crank_cycle(r);
        CycleHeader h{};
        auto e = read_all(r, &h);
        ck(h.state == static_cast<uint8_t>(CycleRecorder::State::Complete), "reports Complete");
        ck(e.size() == 116, "exactly 116 teeth, not 115 or 117", std::to_string(e.size()));
        ck(!e.empty() && e.front().angle == 1, "first tooth at 0.1 deg",
           e.empty() ? "-" : std::to_string(e.front().angle));
        ck(!e.empty() && e.back().angle == 1 + 115 * 60, "last tooth at 690.1 deg",
           e.empty() ? "-" : std::to_string(e.back().angle));
    }

    std::puts("\n-- the boundary tooth: the case that broke the old design --");
    {
        // A crank tooth at angle ~0 arrives microseconds after virtual tooth 0. Under the old design
        // it landed in whichever cycle won the race. Here the boundary callback is deliberately made
        // LATE — after the tooth — and the answer must not change, because the callback no longer
        // decides anything.
        CycleRecorder a, b;
        for (CycleRecorder* r : { &a, &b }) { r->enable(); r->on_cycle_boundary(0, ANGLE_720, 20000, 2); }

        feed_crank_cycle(a);
        a.record_trigger(CycleSignal::Crank, 0, 1, true, true);    // first tooth of cycle 2
        a.on_cycle_boundary(0, ANGLE_720, 20000, 2);               // boundary arrives LATE (after it)
        for (int i = 1; i < 116; ++i) a.record_trigger(CycleSignal::Crank, 0, static_cast<AngleDeg10>(1 + i * 60), true, false);
        feed_crank_cycle(a);                                       // third cycle bounds the second

        feed_crank_cycle(b);
        b.on_cycle_boundary(0, ANGLE_720, 20000, 2);               // boundary arrives EARLY (before it)
        feed_crank_cycle(b);
        feed_crank_cycle(b);

        CycleHeader ha{}, hb{};
        auto ea = read_all(a, &ha);
        auto eb = read_all(b, &hb);
        ck(ea.size() == 116, "late boundary  -> 116 teeth", std::to_string(ea.size()));
        ck(eb.size() == 116, "early boundary -> 116 teeth", std::to_string(eb.size()));
        ck(ea.size() == eb.size(), "boundary TIMING cannot change the count — the race is gone");
    }

    std::puts("\n-- THE bench failure: the boundary tooth stamped either side of 0 --");
    {
        // Reproduces what 3753 bench captures showed. Real tooth 0 is stamped by the FIRING CLOCK,
        // which near the boundary reads 719.9 deg as readily as 0.1, and it arrives either side of
        // virtual tooth 0 depending on which interrupt got there first. Both are alternated here.
        // The tooth's identity never changes, so neither may the count.
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_720, 20000, 2);
        for (int cyc = 0; cyc < 6; ++cyc) {
            const bool early = (cyc & 1);                  // flip the race every cycle
            // The disputed tooth: cycle-start by IDENTITY either way, stamped either way.
            if (early) {
                r.record_trigger(CycleSignal::Crank, 0, 7199, true, true);   // clock hasn't wrapped
                r.record_virtual(0, true);
            } else {
                r.record_virtual(0, true);
                r.record_trigger(CycleSignal::Crank, 0, 1, true, true);      // clock has wrapped
            }
            for (int i = 1; i < 116; ++i) {
                r.record_trigger(CycleSignal::Crank, 0, static_cast<AngleDeg10>(1 + i * 60), true, false);
                if (i <= 71) r.record_virtual(static_cast<AngleDeg10>(i * 100), false);   // 71 + the start mark
            }
        }
        // Every capture across the alternating race must be identical and exact.
        std::vector<size_t> counts;
        for (int k = 0; k < 4; ++k) {
            CycleHeader h{};
            auto e = read_all(r, &h);
            size_t crank = 0, virt = 0;
            for (const auto& x : e) {
                if (((x.flags >> 1) & 7) == 2) ++crank;
                if (((x.flags >> 1) & 7) == 4) ++virt;
            }
            counts.push_back(crank);
            if (k == 0) ck(virt == 72, "grid lane exact too — 72 marks", std::to_string(virt));
        }
        bool all116 = true;
        for (size_t c : counts) if (c != 116) all116 = false;
        ck(all116, "116 teeth whichever side of the grid mark the tooth lands",
           std::to_string(counts.empty() ? 0 : counts[0]));
    }

    std::puts("\n-- all three lanes describe the SAME cycle --");
    {
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_720, 20000, 2);
        // Interleaved in TIME, as the real ISRs produce them — a coil edge is appended when the pin
        // moves, so it lands between the teeth that bracket it, not after the whole cycle.
        for (int cyc = 0; cyc < 5; ++cyc) {
            for (int i = 0; i < 8; ++i) {
                r.record_virtual(static_cast<AngleDeg10>(i * 900), i == 0);
                r.record_trigger(CycleSignal::Crank, 0, static_cast<AngleDeg10>(10 + i * 800), true,
                                 i == 0);
                if (i == 4) r.record_output(CycleSignal::Coil, 0, 3700, true);    // dwell start
                if (i == 5) r.record_output(CycleSignal::Coil, 0, 4600, false);   // spark
            }
        }
        auto e = read_all(r);
        int crank = 0, virt = 0, coil = 0;
        for (const auto& x : e) {
            switch ((x.flags >> 1) & 7) {
                case 2: ++crank; break;
                case 4: ++virt;  break;
                case 0: ++coil;  break;
                default: break;
            }
        }
        ck(crank == 8, "8 crank teeth", std::to_string(crank));
        ck(virt  == 8, "8 grid marks",  std::to_string(virt));
        // THE sparse-lane case. Two coil edges 25 deg apart cannot show a cycle boundary on their
        // own; selecting them per-lane returned nothing at all. They are carried by the ring's
        // boundaries, so a sparse lane is as exact as a dense one.
        ck(coil  == 2, "2 coil edges — a SPARSE lane rides the ring's boundaries",
           std::to_string(coil));
    }

    std::puts("\n-- edges come back in the order they HAPPENED --");
    {
        // One ring means one timeline: a coil edge between two teeth is stored between them, so the
        // record reads as the engine ran rather than as three lanes concatenated.
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_720, 20000, 2);
        // Sparse on purpose, so more cycles are fed: the reader backs off READ_GUARD entries from the
        // head, and here a whole cycle is only five of them. A real cycle is hundreds.
        for (int cyc = 0; cyc < 6; ++cyc) {
            r.record_virtual(0, true);                              // opens the grid lane
            r.record_trigger(CycleSignal::Crank, 0,  100, true, true);
            r.record_output(CycleSignal::Coil,   0, 2000, true);    // between the two teeth
            r.record_trigger(CycleSignal::Crank, 0, 3600, true, false);
            r.record_output(CycleSignal::Coil,   0, 5000, false);
            r.record_trigger(CycleSignal::Crank, 0, 6900, true, false);
        }
        auto e = read_all(r);
        bool ordered = e.size() == 6;
        for (size_t i = 1; i < e.size(); ++i) if (e[i].angle < e[i - 1].angle) ordered = false;
        ck(ordered, "6 edges, angles ascending across lanes", std::to_string(e.size()));
        ck(e.size() == 6 && ((e[2].flags >> 1) & 7) == 0,
           "the coil edge sits BETWEEN the teeth that bracket it");
    }

    std::puts("\n-- a ring that has rolled still returns an exact cycle --");
    {
        // Many cycles pushed through a ring that holds only a few. The reader must still hand back a
        // whole one, because it selects from the newest end.
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_720, 20000, 2);
        for (int c = 0; c < 40; ++c) feed_crank_cycle(r);
        CycleHeader h{};
        auto e = read_all(r, &h);
        ck(e.size() == 116, "still exactly 116 after 40 cycles", std::to_string(e.size()));
        ck(h.state == static_cast<uint8_t>(CycleRecorder::State::Complete), "still Complete");
    }

    std::puts("\n-- header carries the capture's own labels --");
    {
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_1080, 65000, 1);      // rotary, 6500 rpm, CRANK-only
        feed_crank_cycle(r, 20);
        feed_crank_cycle(r, 20);
        feed_crank_cycle(r, 20);
        CycleHeader h{};
        (void)read_all(r, &h);
        ck(h.magic == CYCLE_MAGIC && h.version == CYCLE_VERSION, "magic + version");
        ck(h.cycle_angle == 10800, "rotary span travels with the data", std::to_string(h.cycle_angle));
        ck(h.rpm_x10 == 65000, "rpm is the capture's", std::to_string(h.rpm_x10));
        ck(h.sync_level == 1, "CRANK-only sync is declared");
        ck(h.cycle_seq > 0, "cycle_seq labels the frame", std::to_string(h.cycle_seq));
    }

    std::puts("\n-- the cycle number is STAMPED, so it describes the frame returned --");
    {
        // It used to be the live boundary counter read at serialize() time, so it ran ahead of the
        // frame on screen and changed when the SAME cycle was re-read. Now every edge carries its
        // own cycle's low 3 bits, written by the lane's own ISR, and the header reports the number of
        // the cycle actually being shipped.
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_720, 20000, 2);
        feed_crank_cycle(r); feed_crank_cycle(r); feed_crank_cycle(r);

        CycleHeader h1{}, h2{};
        auto e = read_all(r, &h1);
        (void)read_all(r, &h2);
        ck(h1.cycle_seq == h2.cycle_seq, "re-reading the SAME cycle gives the SAME number",
           std::to_string(h1.cycle_seq) + " then " + std::to_string(h2.cycle_seq));

        // Every trigger edge in the frame belongs to one cycle, so they all carry one stamp.
        bool one = !e.empty();
        const uint8_t s0 = cycle_seq3_of(e.front().flags);
        for (const auto& x : e)
            if (cycle_lane_of(x.flags) == CycleLane::Trigger && cycle_seq3_of(x.flags) != s0) one = false;
        ck(one, "every edge in the frame carries that one cycle's stamp");
        ck((h1.cycle_seq & CYCLE_SEQ_MASK) == s0,
           "the header's number and the stamped edges agree",
           std::to_string(h1.cycle_seq & CYCLE_SEQ_MASK) + " vs " + std::to_string(s0));

        // Advance one cycle: the number must advance by exactly one.
        feed_crank_cycle(r);
        CycleHeader h3{};
        (void)read_all(r, &h3);
        ck(h3.cycle_seq == h1.cycle_seq + 1, "the next cycle numbers one higher",
           std::to_string(h1.cycle_seq) + " -> " + std::to_string(h3.cycle_seq));
    }

    std::puts("\n-- a stall is a discontinuity: never spanned, never passed off as live --");
    {
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_720, 20000, 2);
        feed_crank_cycle(r); feed_crank_cycle(r); feed_crank_cycle(r);
        CycleHeader before{};
        auto eb = read_all(r, &before);
        ck(before.state == static_cast<uint8_t>(CycleRecorder::State::Complete),
           "running: Complete");
        ck(eb.size() == 116, "and exact", std::to_string(eb.size()));

        r.on_trigger_lost();                       // the engine stops
        CycleHeader after{};
        auto ea = read_all(r, &after);
        ck(after.state == static_cast<uint8_t>(CycleRecorder::State::Stale),
           "stalled: the SAME frame, now reported Stale not Complete");
        ck(ea.size() == eb.size(), "the last good cycle is still served, not hidden",
           std::to_string(ea.size()));
        bool no_stall_edge = true;
        for (const auto& x : ea) if (((x.flags >> 1) & 7) == 5) no_stall_edge = false;
        ck(no_stall_edge, "the discontinuity itself is never shipped as an edge");

        // Restart. Until a WHOLE cycle has been turned since the stall, there is no complete frame:
        // a window reaching back across the gap would splice two different runs of the engine and
        // look exact while being nonsense.
        feed_crank_cycle(r);
        CycleHeader mid{};
        (void)read_all(r, &mid);
        ck(mid.state != static_cast<uint8_t>(CycleRecorder::State::Complete) &&
           mid.state != static_cast<uint8_t>(CycleRecorder::State::Stale),
           "one cycle after the restart: no frame spans the gap",
           std::to_string(mid.state));

        feed_crank_cycle(r); feed_crank_cycle(r);
        CycleHeader fresh{};
        auto ef = read_all(r, &fresh);
        ck(fresh.state == static_cast<uint8_t>(CycleRecorder::State::Complete),
           "once it has turned again: Complete", std::to_string(fresh.state));
        ck(ef.size() == 116, "and exact again", std::to_string(ef.size()));
    }

    std::puts("\n-- reading BACKWARDS returns older cycles, each whole and each its own --");
    {
        // Above ~12000 rpm the engine completes cycles faster than a round trip fetches one, and the
        // ring is holding them the whole time. Reading back is how a host collects the run it would
        // otherwise skip, so each step back must be a DIFFERENT and still-exact cycle.
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_720, 20000, 2);
        for (int c = 0; c < 8; ++c) feed_crank_cycle(r);

        std::vector<uint32_t> seqs;
        for (uint8_t back = 0; back <= CycleRecorder::BACK_MAX; ++back) {
            CycleHeader h{};
            auto e = read_all(r, &h, back);
            ck(e.size() == 116, "back " + std::to_string(back) + ": still exactly 116 teeth",
               std::to_string(e.size()));
            seqs.push_back(h.cycle_seq);
        }
        bool descending = true;
        for (size_t i = 1; i < seqs.size(); ++i)
            if (seqs[i] + 1 != seqs[i - 1]) descending = false;
        ck(descending, "each step back is the PREVIOUS engine cycle, consecutively",
           std::to_string(seqs.front()) + " down to " + std::to_string(seqs.back()));

        CycleHeader h{};
        (void)read_all(r, &h, static_cast<uint8_t>(CycleRecorder::BACK_MAX + 1));
        ck(h.total == 0, "a back index beyond the limit is EMPTY, not a nearer cycle substituted",
           std::to_string(h.total));
    }

    std::puts("\n-- paging a back cycle does not drift onto the newest --");
    {
        // The latch keys on `back` as well as `first`; without that, page 2 of an older cycle would
        // silently come from the newest one and the frame would be spliced from two.
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_720, 20000, 2);
        for (int c = 0; c < 8; ++c) feed_crank_cycle(r);
        CycleHeader h0{}, h1{};
        (void)read_all(r, &h0, 0);
        auto e1 = read_all(r, &h1, 2);
        ck(h1.cycle_seq + 2 == h0.cycle_seq, "back 2 is two cycles older",
           std::to_string(h1.cycle_seq) + " vs " + std::to_string(h0.cycle_seq));
        ck(e1.size() == 116, "and it paged out whole", std::to_string(e1.size()));
    }

    std::puts("\n-- reset stops the writers and clears --");
    {
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_720, 20000, 2);
        feed_crank_cycle(r); feed_crank_cycle(r); feed_crank_cycle(r);
        r.reset();
        ck(r.state() == CycleRecorder::State::Idle, "Idle after reset");
        r.record_trigger(CycleSignal::Crank, 0, 100, true, true);
        CycleHeader h{};
        (void)read_all(r, &h);
        ck(h.total == 0, "and records nothing further", std::to_string(h.total));
    }

    std::puts("\n-- paging terminates and a short buffer is refused --");
    {
        CycleRecorder r;
        r.enable();
        r.on_cycle_boundary(0, ANGLE_720, 20000, 2);
        feed_crank_cycle(r); feed_crank_cycle(r); feed_crank_cycle(r);
        CycleHeader h{};
        auto e = read_all(r, &h);
        ck(e.size() == h.total, "every edge came out exactly once",
           std::to_string(e.size()) + " of " + std::to_string(h.total));

        uint8_t buf[sizeof(CycleHeader) + 8];
        const uint16_t n = r.serialize(buf, sizeof(buf), h.total);
        ck(hdr_of(buf).count == 0 && n == sizeof(CycleHeader), "a read past the end is empty");
        uint8_t tiny[8];
        ck(r.serialize(tiny, sizeof(tiny), 0) == 0, "no header, no page — and no overrun");
        ck(r.serialize(nullptr, 4096, 0) == 0, "a null destination writes nothing");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All CycleRecorder tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
