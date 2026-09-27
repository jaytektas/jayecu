// CycleWire — the firmware's capture bytes -> the native enginecycle::Cycle.
//
// This is the comms boundary, so it is where a protocol detail can quietly become a lie on screen.
// The cases that matter are the ones where the bytes are perfectly valid and the DRAWING would still
// be wrong: a crank-folded capture presented against a 720 axis, a truncated buffer presented as a
// complete cycle, a still-running capture presented as finished.
//
//   cmake --build build --target cycle_wire_test && ./build/cycle_wire_test
#include "model/CycleWire.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-68s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static bool near(double a, double b, double eps = 1e-6) { return (a > b ? a - b : b - a) < eps; }

// Build a wire image the way the firmware lays it out, so the test exercises the real byte layout
// rather than a helper that agrees with the decoder by construction.
struct Wire {
    std::vector<uint8_t> b;
    Wire(uint8_t state, uint16_t cycle_angle_dd, uint32_t rpm_x10, uint8_t sync, uint8_t dropped,
         uint16_t total, uint16_t count, uint16_t magic = cyclewire::kMagic) {
        auto u16 = [&](uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); };
        u16(magic);
        b.push_back(1);            // version
        b.push_back(state);
        u16(cycle_angle_dd);
        u16(total);
        u16(0);                    // first
        u16(count);
        for (int i = 0; i < 4; ++i) b.push_back(uint8_t(rpm_x10 >> (8 * i)));
        b.push_back(sync);
        b.push_back(dropped);
        u16(0);                    // reserved
    }
    // sig: 0 Coil, 1 Injector, 2 Crank, 3 Cam
    void edge(uint16_t angle_dd, uint8_t channel, uint8_t sig, bool active) {
        b.push_back(uint8_t(angle_dd)); b.push_back(uint8_t(angle_dd >> 8));
        b.push_back(channel);
        b.push_back(uint8_t((active ? 1 : 0) | (sig << 1)));
    }
};

static const enginecycle::Trace* find(const enginecycle::Cycle& c, const std::string& label) {
    for (const auto& t : c.traces()) if (t.label == label) return &t;
    return nullptr;
}

int main() {
    using namespace enginecycle;
    std::puts("=== CycleWire (capture bytes -> native cycle) ===");

    std::puts("\n-- a PHASE capture keeps the ECU's span --");
    {
        Wire w(3, 7200, 20000, 2, 0, 2, 2);
        w.edge(3300, 0, 0, true);      // coil ch0 dwell start at 330 deg
        w.edge(3550, 0, 0, false);     // spark at 355
        Cycle c;
        const auto r = cyclewire::decode(w.b.data(), w.b.size(), c);
        ck(r.complete(), "decodes a Complete capture", r.message);
        ck(near(c.cycleAngle(), 720.0), "720 deg span survives", std::to_string(c.cycleAngle()));
        ck(near(c.rpm(), 2000.0), "rpm comes from the header", std::to_string(c.rpm()));
        ck(!c.isReconstructed(), "our own capture is MEASURED, not reconstructed");
        ck(c.note().empty(), "nothing to qualify, so no note", c.note());

        const Trace* t = find(c, "Coil 1");
        ck(t != nullptr, "channel 0 is labelled 'Coil 1' (1-based on screen)");
        if (t) {
            const auto sp = c.spans(*t);
            ck(sp.size() == 1 && near(c.spanLength(sp[0]), 25.0),
               "the two edges resolve to a 25 deg dwell",
               sp.empty() ? "no span" : std::to_string(c.spanLength(sp[0])));
        }
    }

    std::puts("\n-- a CRANK-only capture is folded to 360, and SAYS so --");
    {
        // This is the case that draws wrong while every byte is valid. The ECU reports a 720 span in
        // the header but folds every angle into one revolution when it has no phase; presenting that
        // against a 720 axis squeezes the whole engine into the left half and makes companion
        // cylinders look unrelated.
        Wire w(3, 7200, 12030, 1, 0, 2, 2);
        w.edge(3388, 0, 0, false);
        w.edge(1588, 1, 0, false);
        Cycle c;
        const auto r = cyclewire::decode(w.b.data(), w.b.size(), c);
        ck(r.complete(), "decodes");
        ck(near(c.cycleAngle(), 360.0), "span is the ONE REVOLUTION the angles live in, not 720",
           std::to_string(c.cycleAngle()));
        ck(c.note().find("crank sync") != std::string::npos,
           "and the note says why the span is short", c.note());
    }

    std::puts("\n-- a truncated capture is declared, not quietly short --");
    {
        Wire w(3, 7200, 20000, 2, 7, 1, 1);
        w.edge(100, 0, 0, true);
        Cycle c;
        const auto r = cyclewire::decode(w.b.data(), w.b.size(), c);
        ck(r.dropped == 7, "the drop count survives the decode", std::to_string(r.dropped));
        ck(c.note().find("TRUNCATED") != std::string::npos,
           "the view is told the ECU's buffer filled", c.note());
    }

    std::puts("\n-- a capture still in progress is NOT complete --");
    {
        for (auto st : {0, 1, 2}) {
            Wire w(uint8_t(st), 7200, 0, 2, 0, 0, 0);
            const auto r = cyclewire::peek(w.b.data(), w.b.size());
            ck(r.ok && !r.complete(), "state " + std::to_string(st) + " is not Complete", r.message);
        }
        Wire done(3, 7200, 0, 2, 0, 0, 0);
        ck(cyclewire::peek(done.b.data(), done.b.size()).complete(), "state 3 is Complete");
        // Stale is drawable too — it is the last cycle the engine turned, and hiding it would lose
        // the one frame worth having after a stall. It must be flagged, not withheld.
        {
            auto st = done; st.b[3] = 4;
            auto r = cyclewire::peek(st.b.data(), st.b.size());
            ck(r.complete(), "state 4 (Stale) still yields a drawable frame", r.message);
            ck(r.stale(),    "and it is flagged stale, not passed off as live");
        }
    }

    std::puts("\n-- every signal kind lands in its own lane --");
    {
        Wire w(3, 7200, 20000, 2, 0, 4, 4);
        w.edge(100, 0, 0, true);    // Coil 1
        w.edge(200, 0, 1, true);    // Inj 1
        w.edge(300, 0, 2, true);    // Crank 1
        w.edge(400, 1, 3, true);    // Cam 2
        Cycle c;
        cyclewire::decode(w.b.data(), w.b.size(), c);
        ck(c.traces().size() == 4, "four distinct traces", std::to_string(c.traces().size()));
        ck(find(c, "Coil 1")  && find(c, "Coil 1")->signal  == Signal::Coil,     "Coil 1");
        ck(find(c, "Inj 1")   && find(c, "Inj 1")->signal   == Signal::Injector, "Inj 1");
        ck(find(c, "Crank 1") && find(c, "Crank 1")->signal == Signal::Crank,    "Crank 1");
        ck(find(c, "Cam 2")   && find(c, "Cam 2")->signal   == Signal::Cam,      "Cam 2");
        // Display order groups trigger inputs first — the reference the outputs are read against.
        ck(c.traces()[0].signal == Signal::Crank, "trigger inputs sort to the top");
    }

    std::puts("\n-- the ECU's duplicate edges collapse to one dwell --");
    {
        // Real behaviour, seen on the bench: at CRANK sync both half-buckets dispatch and a
        // wasted-spark mask carries both companions, so the ECU drives a coil twice at the identical
        // angle. The record keeps both (it reports what happened); the display must show ONE dwell.
        Wire w(3, 7200, 12030, 1, 0, 4, 4);
        w.edge(3172, 0, 0, true);
        w.edge(3172, 0, 0, true);
        w.edge(3388, 0, 0, false);
        w.edge(3388, 0, 0, false);
        Cycle c;
        cyclewire::decode(w.b.data(), w.b.size(), c);
        const Trace* t = find(c, "Coil 1");
        ck(t && t->edges.size() == 2, "four recorded edges become two state changes",
           t ? std::to_string(t->edges.size()) : "no trace");
        if (t) {
            const auto sp = c.spans(*t);
            ck(sp.size() == 1 && near(c.spanLength(sp[0]), 21.6),
               "one 21.6 deg dwell, not two hairlines",
               sp.empty() ? "no span" : std::to_string(c.spanLength(sp[0])));
        }
    }

    std::puts("\n-- a trigger stream survives as TEETH, not one collapsed mark --");
    {
        // The decoder captures ONE polarity, so a wheel arrives as N rising edges and no falling
        // ones. Treated as a level, the same-state collapse reduces the whole wheel to its first
        // tooth and the lane draws as a single bar across the cycle — which is what it did.
        Wire w(3, 3600, 12030, 1, 0, 12, 12);
        for (int i = 0; i < 12; ++i) w.edge(uint16_t(i * 100), 0, 2, true);   // 12 teeth, 10 deg apart
        Cycle c;
        cyclewire::decode(w.b.data(), w.b.size(), c);
        const Trace* t = find(c, "Crank 1");
        ck(t && t->edges.size() == 12, "all 12 teeth survive",
           t ? std::to_string(t->edges.size()) : "no trace");
        if (t) {
            ck(near(t->edges[0].angle, 0.0) && near(t->edges[11].angle, 110.0),
               "…in angle order, spanning the wheel");
            // A coil in the same capture must STILL collapse — the rule is per signal kind, not global.
            ck(isEventSignal(t->signal), "the crank trace is an event trace");
        }

        Wire lvl(3, 7200, 20000, 2, 0, 3, 3);
        lvl.edge(1000, 0, 0, true);
        lvl.edge(1000, 0, 0, true);      // redundant repeat of "high"
        lvl.edge(1200, 0, 0, false);
        Cycle d;
        cyclewire::decode(lvl.b.data(), lvl.b.size(), d);
        const Trace* coil = find(d, "Coil 1");
        ck(coil && coil->edges.size() == 2, "a LEVEL trace still collapses its repeat",
           coil ? std::to_string(coil->edges.size()) : "no trace");
    }

    std::puts("\n-- malformed input leaves an EMPTY cycle, never a half-built one --");
    {
        Cycle c;
        c.addEdge("stale", 10.0, true, Signal::Coil, 1);      // pre-existing content must be dropped

        const auto shortR = cyclewire::decode(nullptr, 0, c);
        ck(!shortR.ok && c.empty(), "a null buffer clears the cycle and reports", shortR.message);

        Wire bad(3, 7200, 20000, 2, 0, 1, 1, 0x1234);         // wrong magic
        bad.edge(100, 0, 0, true);
        c.addEdge("stale", 10.0, true, Signal::Coil, 1);
        const auto magicR = cyclewire::decode(bad.b.data(), bad.b.size(), c);
        ck(!magicR.ok && c.empty(), "a bad magic is refused, not parsed anyway", magicR.message);

        // A header promising more edges than the payload carries must not read past the end.
        Wire lying(3, 7200, 20000, 2, 0, 50, 50);
        lying.edge(100, 0, 0, true);                           // …but only one edge is present
        const auto r = cyclewire::decode(lying.b.data(), lying.b.size(), c);
        ck(r.ok && c.traces().size() == 1,
           "a header over-promising edges is clamped to what arrived",
           std::to_string(c.traces().size()));
    }

    std::puts("\n-- an unknown signal kind is skipped, not mislabelled --");
    {
        Wire w(3, 7200, 20000, 2, 0, 2, 2);
        w.edge(100, 0, 6, true);    // a kind this build does not know
        w.edge(200, 0, 0, true);    // and a coil
        Cycle c;
        cyclewire::decode(w.b.data(), w.b.size(), c);
        ck(c.traces().size() == 1 && find(c, "Coil 1"),
           "the known edge lands, the unknown one is dropped",
           std::to_string(c.traces().size()));
    }

    std::puts("\n-- a stalled capture says so, because the angles cannot --");
    {
        Wire w(4 /* Stale */, 7200, 0, 2, 0, 0, 0);
        enginecycle::Cycle c;
        const auto r = cyclewire::decode(w.b.data(), w.b.size(), c);
        ck(r.ok && r.stale(), "decoded and flagged stale", r.message);
        ck(c.note().find("STALLED") != std::string::npos,
           "and the cycle carries the caveat into the view", c.note());
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All CycleWire tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
