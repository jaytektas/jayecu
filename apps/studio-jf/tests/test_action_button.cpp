// A BUTTON THAT WORKS OUT A SETTING AND WRITES IT.
//
// Pulses per kilometre is a measurement, not an opinion: it is whatever the tyre and the tooth count make
// it, and the way to find it is to drive at a known speed and divide. The button does that sum and writes
// the answer where the user would have typed it.
//
// The two things worth asserting are the arithmetic and the refusal. A calibration speed nobody has set
// yet makes the sum a divide by zero, and writing an infinity — or a zero — into a calibration is worse
// than a button that plainly does nothing, so it must both grey itself and decline the click.
//
//   cmake --build build --target action_button_test && ./build/action_button_test

#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/MathEvaluator.h"
#include "model/SigilResolvers.h"
#include "surface/PanelModel.h"
#include "surface/Surface.h"
#include "surface/widgets/ActionButtonWidget.h"

#include <j/core/SceneGraph.h>
#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const char* what, const std::string& note = {}) {
    std::printf("[action] %-58s %s%s\n", what, ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// The button's rect ON SCREEN. The widget knows; the model only knows where it was authored.
static jf::JRect buttonRect(Surface& s, int id) {
    if (CanvasWidget* w = s.widgetById(id)) {
        const auto b = w->getBoundingBox();
        return jf::JRect{ b.x, b.y, b.width, b.height };
    }
    return jf::JRect{ 0.f, 0.f, 0.f, 0.f };
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());
    static ConfigSigilResolver configSigils;
    MathEvaluator::instance().registerResolver(&configSigils);

    const std::string target = "vehicle_speed.source[0].pulses_per_km";
    const std::string speed  = "vehicle_speed.calibration_speed";
    if (!c.isConfig(target)) { std::printf("[action] no %s in this schema\n", target.c_str()); return 77; }

    jf::JSceneGraph graph;
    PanelModel page;
    page.setCanvasSize(400.f, 200.f);
    page.setCanvasStatic(1);
    // The real Calibrate sum: pulses/km = Hz x 3600 / kph. The frequency is a literal here because a host
    // test has no live telemetry — what is under test is the arithmetic and the write, not the bus.
    // ONE `writes` PROP, "path = expression" per line — not the separate target/expr pair this test was
    // written against. A button computes a SET of measurements (one Capture calibrates every pickup that
    // is turning), so the pair could never have said what the control does.
    const int id = page.add("action", 10.f, 10.f, 90.f, 25.f,
                            { { "labelText", "Calibrate" },
                              { "writes", target + " = 40.8333 * 3600 / [#" + speed + "]" } });
    Surface s(graph, c, &page);
    s.setBounds({ 0.f, 0.f, 400.f, 200.f });
    s.setMode(Surface::Mode::Run);
    // PINNED TO 1:1, so this test says what it means. The page canvas and the surface bounds are
    // deliberately the same size here; fitting one to the other is exactly scale 1, whatever the
    // user's "static surface size" preference happens to be. Static is no longer 1:1 — it renders a
    // page AT the preference size — so without this a hit-test lands wherever that preference put it.
    s.setFitToView(true);
    jf::JPrimitiveBuffer buf;

    // 60.0 kph is the schema default (stored x10). 40.8333 Hz at 60 kph is 2450 pulses/km — the donor's.
    std::printf("[action] calibration speed reads %.1f kph\n", c.configValue(speed));
    s.populateRenderPrimitives(buf);
    // WHERE THE BUTTON ACTUALLY IS, asked of the widget that was just laid out — not the authored rect
    // read as if it were a screen coordinate. A surface has not been 1:1 with its page for a while
    // (canvasStatic renders at the interface scale, and fit-to-view scales to the area), so pressing the
    // authored centre pressed empty canvas and the click never reached the button: every assertion about
    // what it wrote then measured a button nobody had clicked.
    const jf::JRect btn = buttonRect(s, id);
    const float cx = btn.x + btn.width * 0.5f, cy = btn.y + btn.height * 0.5f;
    s.handleMousePress(cx, cy);
    s.handleMouseRelease(cx, cy);
    const double got = c.configValue(target);
    std::printf("[action] wrote %s = %.0f\n", target.c_str(), got);
    ck(std::fabs(got - 2450.0) < 1.0, "40.83 Hz at 60 kph calibrates to 2450 pulses/km",
       std::to_string(got));

    // …and the refusal. With the calibration speed at zero the sum is a divide by zero.
    c.setConfigValue(speed, 0.0);
    c.setConfigValue(target, 0.0);
    s.populateRenderPrimitives(buf);
    s.handleMousePress(cx, cy);
    s.handleMouseRelease(cx, cy);
    const double after = c.configValue(target);
    std::printf("[action] with no calibration speed, target reads %.0f\n", after);
    ck(after == 0.0, "with nothing to divide by, the click writes nothing at all",
       std::to_string(after));

    std::printf("[action] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
