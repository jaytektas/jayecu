// EngineCycleView — the hover readout resolves a cursor position to a span.
//
// This exists because the readout was written, was correct, and was invisible for as long as it
// existed. handleMouseMove receives ABSOLUTE window coordinates — JWidget::isPointInside compares
// the same values against the node's absolute boundingBox — and the view added bounds() on top,
// offsetting every hit test by the widget's own origin. The lane search then never matched, the
// readout was cleared on every move, and setTooltip() was handed an empty string, so neither the
// view's own box nor the framework tooltip could ever appear.
//
// Nothing caught it: the arithmetic compiles either way and a GUI that shows no tooltip looks like
// a GUI that has no tooltip. So the coordinate contract is pinned here.
//
//   cmake --build build --target cycle_hover_test && ./build/cycle_hover_test
#include "ui/EngineCycleView.h"

#include <j/core/SceneGraph.h>

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-68s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    std::printf("=== EngineCycleView hover ===\n\n");
    jf::JSceneGraph g;
    EngineCycleView v(g);

    // Put the widget somewhere with a NON-ZERO ORIGIN. At (0,0) the buggy and correct arithmetic
    // agree, which is exactly how this survived: any test placing the view at the origin passes
    // against both.
    const float X = 300.f, Y = 200.f, W = 900.f, H = 400.f;
    v.setBounds(jf::JRect{ X, Y, W, H });

    enginecycle::Cycle c;
    c.setCycleAngle(720.0);
    c.setRpm(3000.0);
    c.addEdge("Coil 1", 100.0, true,  enginecycle::Signal::Coil, 1);   // dwell 100 -> 120 deg
    c.addEdge("Coil 1", 120.0, false, enginecycle::Signal::Coil, 1);
    v.setCycle(c);

    // The lane rects are recorded during paint, so the view has to have drawn once before a hover
    // can resolve. That is the real sequence too — you cannot hover what has not been shown.
    jf::JPrimitiveBuffer buf;
    v.populateRenderPrimitives(buf);

    // Sweep in ABSOLUTE coordinates across the whole widget and collect whatever the readout says.
    // Asking "does some point inside the dwell produce a readout" rather than computing the exact
    // pixel keeps this about the coordinate contract instead of about the layout constants.
    std::string seen;
    for (float ax = X; ax < X + W && seen.empty(); ax += 2.f)
        for (float ay = Y; ay < Y + H && seen.empty(); ay += 2.f) {
            v.handleMouseMove(ax, ay);
            if (!v.readout().empty()) seen = v.readout();
        }

    ck(!seen.empty(), "a cursor somewhere over the widget resolves to a readout",
       "swept the whole widget and every point came back empty");
    if (!seen.empty()) {
        ck(seen.find("Coil 1") != std::string::npos, "…which names the channel", seen);
        ck(seen.find("100") != std::string::npos && seen.find("120") != std::string::npos,
           "…and states the dwell's start and spark angles", seen);
    }

    // Off the widget entirely: nothing to report, and nothing left showing from a previous position.
    v.handleMouseMove(X - 50.f, Y - 50.f);
    ck(v.readout().empty(), "a cursor outside the widget clears the readout");

    std::printf("\n%s\n", fails ? "FAILURES" : "All EngineCycleView hover tests passed (0 failures)");
    return fails ? 1 : 0;
}
