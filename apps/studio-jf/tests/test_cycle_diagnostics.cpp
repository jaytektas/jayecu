// CycleDiagnostics — does the frame on screen deserve to be believed?
//
// The case that motivated this is real: a capture bug served a 116-tooth engine cycle as 37 + 79
// after the engine restarted. It reported Complete, every angle was plausible, and the view drew it
// without complaint. It was caught by counting teeth in a bench script. These tests exist to make
// sure the studio counts too.
#include "../src/model/CycleDiagnostics.h"

#include <cstdio>
#include <string>

using namespace enginecycle;

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-68s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

static bool has(const Diagnosis& d, const std::string& needle) {
    for (const Finding& f : d.findings)
        if (f.text.find(needle) != std::string::npos) return true;
    return false;
}

// A steady 60-2 + cam frame: 116 crank teeth, 72 grid marks, 2 cam edges, one coil, one injector.
static Cycle steadyFrame(int crankTeeth = 116) {
    Cycle c;
    c.setCycleAngle(720.0);
    c.setRpm(3000.0);
    for (int i = 0; i < crankTeeth; ++i) c.addEdge("Crank", 0.1 + i * 6.0, true, Signal::Crank, -1);
    for (int i = 0; i < 72; ++i)         c.addEdge("Grid",  i * 10.0,      true, Signal::Virtual, -1);
    c.addEdge("Cam 1", 213.0, true,  Signal::Cam, 1);
    c.addEdge("Cam 1", 216.0, false, Signal::Cam, 1);
    c.addEdge("Coil 1", 677.6, true,  Signal::Coil, 1);      // dwell start
    c.addEdge("Coil 1", 698.8, false, Signal::Coil, 1);      // spark
    c.addEdge("Inj 1",  480.0, true,  Signal::Injector, 1);
    c.addEdge("Inj 1",  520.0, false, Signal::Injector, 1);
    c.sort();
    return c;
}

int main() {
    std::puts("=== CycleDiagnostics (is this frame trustworthy?) ===");

    std::puts("\n-- a steady frame reports its lanes and finds nothing wrong --");
    {
        const Cycle prev = steadyFrame();
        const Cycle now  = steadyFrame();
        const Diagnosis d = diagnose(now, &prev);
        ck(d.trustworthy, "trustworthy");
        ck(d.findings.empty(), "no findings on an unchanged frame",
           d.findings.empty() ? "" : d.findings[0].text);
        ck(d.totalEdges == 116 + 72 + 2 + 2 + 2, "every edge counted",
           std::to_string(d.totalEdges));
        ck(d.summary().find("116 crank") != std::string::npos, "summary leads with the crank count",
           d.summary());
        ck(d.summary().find("72 grid") != std::string::npos, "and carries the grid count", d.summary());
    }

    std::puts("\n-- THE bug: a split cycle is called out, not drawn silently --");
    {
        // Exactly what the firmware served after a restart: the same engine, a third of the teeth.
        const Cycle prev = steadyFrame(116);
        const Cycle now  = steadyFrame(37);
        const Diagnosis d = diagnose(now, &prev);
        ck(!d.findings.empty(), "the frame is flagged");
        ck(has(d, "crank 37 edges, was 116"), "and it says exactly what changed",
           d.findings.empty() ? "-" : d.findings[0].text);
        ck(d.worst() == Severity::Warning, "as a warning — a changed count may be real engine behaviour");
    }

    std::puts("\n-- a lane that vanishes is louder than a lane that shrinks --");
    {
        Cycle prev = steadyFrame();
        Cycle now;                                    // cam lost: sync dropped to crank-only
        now.setCycleAngle(720.0);
        for (int i = 0; i < 116; ++i) now.addEdge("Crank", 0.1 + i * 6.0, true, Signal::Crank, -1);
        for (int i = 0; i < 72; ++i)  now.addEdge("Grid",  i * 10.0,      true, Signal::Virtual, -1);
        now.addEdge("Coil 1", 677.6, true,  Signal::Coil, 1);
        now.addEdge("Coil 1", 698.8, false, Signal::Coil, 1);
        now.addEdge("Inj 1",  480.0, true,  Signal::Injector, 1);
        now.addEdge("Inj 1",  520.0, false, Signal::Injector, 1);
        now.sort();
        const Diagnosis d = diagnose(now, &prev);
        ck(has(d, "cam missing"), "a missing lane is named as missing, not just absent");
    }

    std::puts("\n-- an unpaired level edge is REPORTED, not condemned --");
    {
        // At 15000 rpm a cycle is 8 ms and an injection pulse runs past the boundary, so an odd edge
        // count is what a healthy engine produces. It is also what a lost edge looks like, and one
        // frame cannot tell them apart — so it is stated, not judged. Calling it an error marked
        // every high-rpm frame untrustworthy, and a warning that fires on healthy data is worse than
        // no warning at all.
        Cycle now = steadyFrame();
        now.trace("Coil 1", Signal::Coil, 1).edges.pop_back();
        const Diagnosis d = diagnose(now, nullptr);
        ck(has(d, "crosses the cycle boundary"), "flagged, and says what it might be");
        ck(d.trustworthy, "the frame is still believable — the model closes a wrapped span");
        ck(d.worst() == Severity::Warning, "a warning, not an error");
    }

    std::puts("\n-- event lanes are never 'unpaired': a tooth has no off --");
    {
        // 35 rising teeth and no falling ones is what a trigger lane IS. Treating that as an odd
        // count would put a permanent error on every healthy capture.
        Cycle now = steadyFrame(35);
        const Diagnosis d = diagnose(now, nullptr);
        ck(d.trustworthy, "an odd number of teeth is not a fault");
        ck(!has(d, "odd"), "and is not reported as one");
    }

    std::puts("\n-- the first frame has nothing to compare against, and says nothing --");
    {
        const Cycle now = steadyFrame();
        const Diagnosis d = diagnose(now, nullptr);
        ck(d.findings.empty(), "no reference, no comparison findings",
           d.findings.empty() ? "" : d.findings[0].text);
        ck(d.trustworthy, "and it is not called suspect for being first");
    }

    std::puts("\n-- what the producer said about itself is carried through --");
    {
        Cycle now = steadyFrame();
        now.setFidelity(Fidelity::Reconstructed);
        now.setNote("stalled — last cycle before the trigger was lost");
        const Diagnosis d = diagnose(now, nullptr);
        ck(has(d, "reconstructed"), "a reconstructed frame says so");
        ck(has(d, "stalled"), "and the producer's own note is shown, not dropped");
        ck(d.trustworthy, "neither makes the DATA untrustworthy — they qualify how to read it");
    }

    std::puts("\n-- a changed span is flagged: 360 and 720 are different pictures --");
    {
        Cycle prev = steadyFrame();
        Cycle now  = steadyFrame();
        now.setCycleAngle(360.0);
        const Diagnosis d = diagnose(now, &prev);
        ck(has(d, "span changed"), "flagged, because every angle in the frame means something else");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All CycleDiagnostics tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
