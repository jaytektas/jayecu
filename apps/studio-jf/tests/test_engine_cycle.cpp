// The engine-cycle model: degrees, edges, and the wrap cases that make a dwell readable.
//
// This is the NATIVE model — the canonical form the view draws. A foreign ECU is converted into it
// at the comms boundary, so nothing here knows another protocol exists. What it must get right is
// the arithmetic that makes a cycle a cycle: angles wrap, and a span that crosses the origin is one
// span with a positive length, not two fragments or a negative number.
//
//   cmake --build build --target engine_cycle_test && ./build/engine_cycle_test
#include "model/EngineCycle.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace enginecycle;

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-62s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

int main() {
    std::puts("=== Engine cycle model (degrees, edges) ===");

    std::puts("\n-- the cycle span is data, not 720 --");
    {
        Cycle c;
        ck(near(c.cycleAngle(), 720.0), "defaults to a four-stroke");
        c.setCycleAngle(360.0);
        ck(near(c.wrap(400.0), 40.0), "two-stroke: 400 deg is 40 deg");
        c.setCycleAngle(1080.0);
        ck(near(c.wrap(400.0), 400.0), "rotary: 400 deg is a distinct position");
        ck(near(c.wrap(1100.0), 20.0), "…and 1100 wraps to 20");
        ck(near(c.wrap(-10.0), 1070.0), "negative angles wrap forward");
    }

    std::puts("\n-- a coil trace gives dwell start AND spark from two edges --");
    {
        Cycle c;
        c.setCycleAngle(720.0);
        // Dwell from 330 deg, spark at 385 (i.e. 25 deg before the 410 TDC in this made-up layout).
        c.addEdge("Coil 1", 330.0, true,  Signal::Coil, 1);
        c.addEdge("Coil 1", 385.0, false, Signal::Coil, 1);
        c.sort();

        const auto& t = c.traces().front();
        const auto sp = c.spans(t);
        ck(sp.size() == 1, "one dwell span", std::to_string(sp.size()));
        if (sp.size() != 1) { std::puts("  (skipping span checks)"); return 1; }
        ck(near(sp[0].from, 330.0), "dwell starts where the coil went high");
        ck(near(sp[0].to,   385.0), "…and the SPARK is its falling edge");
        ck(near(c.spanLength(sp[0]), 55.0), "55 deg of dwell",
           std::to_string(c.spanLength(sp[0])));
        ck(!sp[0].wrapped, "no wrap involved");
    }

    std::puts("\n-- a dwell that crosses the cycle origin is ONE span --");
    {
        Cycle c;
        c.setCycleAngle(720.0);
        // Charge at 700 deg, fire at 10 deg — the coil is dwelling across the cycle boundary.
        c.addEdge("Coil 4", 700.0, true,  Signal::Coil, 4);
        c.addEdge("Coil 4", 10.0,  false, Signal::Coil, 4);
        c.sort();

        const auto sp = c.spans(c.traces().front());
        ck(sp.size() == 1, "still one span, not two fragments", std::to_string(sp.size()));
        if (sp.size() != 1) return 1;
        ck(sp[0].wrapped, "marked as wrapping the origin");
        ck(near(c.spanLength(sp[0]), 30.0), "30 deg of dwell, not -690",
           std::to_string(c.spanLength(sp[0])));
    }

    std::puts("\n-- injector open/close, and several events on one trace --");
    {
        Cycle c;
        c.setCycleAngle(720.0);
        c.addEdge("Inj 1", 100.0, true,  Signal::Injector, 1);
        c.addEdge("Inj 1", 140.0, false, Signal::Injector, 1);
        c.addEdge("Inj 1", 500.0, true,  Signal::Injector, 1);   // a second squirt (transient enrich)
        c.addEdge("Inj 1", 515.0, false, Signal::Injector, 1);
        c.sort();

        const auto sp = c.spans(c.traces().front());
        ck(sp.size() == 2, "two open periods", std::to_string(sp.size()));
        if (sp.size() != 2) return 1;
        ck(near(c.spanLength(sp[0]), 40.0), "first is 40 deg");
        ck(near(c.spanLength(sp[1]), 15.0), "second is 15 deg");
    }

    std::puts("\n-- edges may arrive in any order, and duplicates are dropped --");
    {
        Cycle c;
        c.setCycleAngle(720.0);
        c.addEdge("Coil 2", 400.0, false, Signal::Coil, 2);      // out of order
        c.addEdge("Coil 2", 350.0, true,  Signal::Coil, 2);
        c.addEdge("Coil 2", 360.0, true,  Signal::Coil, 2);      // redundant repeat of "high"
        c.sort();

        const auto& t = c.traces().front();
        ck(t.edges.size() == 2, "the repeated state is dropped", std::to_string(t.edges.size()));
        ck(t.edges[0].high && !t.edges[1].high, "rise then fall, in angle order");
        const auto sp = c.spans(t);
        ck(sp.size() == 1 && near(c.spanLength(sp[0]), 50.0), "one 50 deg span");
    }

    std::puts("\n-- a trace is identified by (signal, index), not by its label --");
    {
        Cycle c;
        c.addEdge("Coil 1", 10.0, true,  Signal::Coil, 1);
        c.addEdge("coil1",  20.0, false, Signal::Coil, 1);        // same channel, different label
        ck(c.traces().size() == 1, "one trace, not two", std::to_string(c.traces().size()));
        c.addEdge("Inj 1", 30.0, true, Signal::Injector, 1);      // same index, different signal
        ck(c.traces().size() == 2, "…but a different signal IS a different trace");
    }

    std::puts("\n-- fidelity travels with the data --");
    {
        Cycle c;
        ck(!c.isReconstructed(), "our own data is measured by default");
        c.setFidelity(Fidelity::Reconstructed);
        ck(c.isReconstructed(), "a bridge that interpolated angle says so");
        // The point: only the converter knows it interpolated, so the view cannot infer this and
        // must be told — otherwise a reconstructed angle is presented as if it were measured.
    }

    std::puts("\n-- rotary identity survives, and orders leading before trailing --");
    {
        Cycle c;
        c.setCycleAngle(1080.0);
        Trace& tr = c.trace("Rotor 1 Face 2 Trail", Signal::Coil, 5);
        tr.rotor = 1; tr.face = 2; tr.trailing = true;
        Trace& ld = c.trace("Rotor 1 Face 2 Lead", Signal::Coil, 5);   // same index...
        ck(&ld == &tr, "same (signal,index) resolves to the same trace");

        // Distinct channels for lead and trail, as the scheduler actually assigns them.
        Cycle d;
        d.setCycleAngle(1080.0);
        Trace& lead  = d.trace("R1F1 Lead",  Signal::Coil, 1);
        lead.rotor = 1; lead.face = 1; lead.trailing = false;
        Trace& trail = d.trace("R1F1 Trail", Signal::Coil, 2);
        trail.rotor = 1; trail.face = 1; trail.trailing = true;
        d.sortTracesForDisplay();
        ck(!d.traces()[0].trailing && d.traces()[1].trailing,
           "leading plug sorts before its trailing plug");
        ck(d.traces()[0].rotor == 1 && d.traces()[0].face == 1, "rotor/face identity is kept");
    }

    std::puts("\n-- display order groups by signal, then channel --");
    {
        Cycle c;
        c.addEdge("Inj 2",  10.0, true, Signal::Injector, 2);
        c.addEdge("Coil 2", 20.0, true, Signal::Coil, 2);
        c.addEdge("Crank",  30.0, true, Signal::Crank);
        c.addEdge("Coil 1", 40.0, true, Signal::Coil, 1);
        c.sortTracesForDisplay();
        const auto& t = c.traces();
        ck(t[0].signal == Signal::Crank,    "trigger inputs first");
        ck(t[1].label  == "Coil 1",         "then coils, by channel", t[1].label);
        ck(t[2].label  == "Coil 2",         "…in order", t[2].label);
        ck(t[3].signal == Signal::Injector, "then injectors");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All engine-cycle model tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
