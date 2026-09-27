// A RULE'S `value` MEANS WHAT THE CONTROL SHOWS.
//
// m_value is the RAW bound reading. Every gauge draws dispV(value) — the field's scale applied, then
// converted to the display unit — so a rule tested against the raw number meant something other than
// what was on screen the moment either differed. Author "value > 100" against a gauge reading °F, or
// against a field stored ×0.1, and the band landed nowhere near where it was drawn. On a field with
// scale 1 and no conversion the two coincide, which is exactly why it did not look broken.
//
// Pinned here:
//   - with no scale and no conversion, display == raw (the case that always worked stays working)
//   - a scaled field's rule compares against the SCALED number, the one the gauge prints
//   - the raw value is still available unchanged for anything that wants it
//
//   cmake --build build --target rule_units_test && ./build/rule_units_test

#include "../src/surface/CanvasWidget.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <cmath>
#include <string>

static int fails = 0;
static void ck(bool ok, const char* what, const std::string& note = {}) {
    std::printf("[rule-units] %-56s %s%s\n", what, ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

struct Probe : CanvasWidget {
    using CanvasWidget::CanvasWidget;
    void bind(PanelElement& el) { setData(el); }
    std::string paletteTitle() const override { return "Probe"; }
    float defaultW() const override { return 100.f; }
    float defaultH() const override { return 40.f; }
};

int main() {
    jf::JSceneGraph graph;

    // No scale, no unit conversion: display and raw are the same number, so every rule authored before
    // this keeps meaning what it meant.
    {
        PanelElement el; el.type = "field";
        Probe w(graph); w.bind(el);
        w.setValue(42.0);
        ck(near(w.value(), 42.0),        "the raw reading is unchanged");
        ck(near(w.displayValue(), 42.0), "unscaled + unconverted -> display == raw",
           std::to_string(w.displayValue()));
    }

    // A SCALED field. The gauge prints value × scale, so that is what the rule must compare. Before this,
    // a rule saying "value > 100" on a ×0.1 field was really asking about 1000 on the dial.
    //
    // THE SCALE IS THE FIELD'S, ASKED OF THE DEFINITION. This block used to set a per-widget `scale`
    // prop; that override is gone (units own format and precision now, and fourteen widgets carrying
    // their own scale is fourteen chances to disagree with the field they are bound to). So the test
    // binds a REAL field that is stored ×0.1 and lets dispV ask the meta, which is the only path a
    // gauge has.
    {
        MetaModel meta;
        if (!meta.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
        Cache& c = Cache::instance();
        c.setMeta(&meta);
        c.setConfigImage(meta.defaultImage());
        const std::string scaled = "alternator.max_duty_pct";          // U16, scale 0.1
        if (!near(c.configScale(scaled), 0.1)) {
            std::printf("[rule-units] %s is no longer a x0.1 field (%g) — pick another\n",
                        scaled.c_str(), c.configScale(scaled));
            return 1;
        }
        PanelElement el; el.type = "field"; el.props["signalName"] = scaled;
        Probe w(graph); w.bind(el);
        w.setValue(1000.0);
        ck(near(w.value(), 1000.0),      "raw stays raw on a scaled field");
        ck(near(w.displayValue(), 100.0), "display is the number the gauge prints",
           std::to_string(w.displayValue()));
    }

    // setValue before an element is bound must not explode or invent a conversion.
    {
        Probe w(graph);
        w.setValue(7.0);
        ck(near(w.displayValue(), 7.0), "unbound -> display falls back to the raw value");
    }

    std::printf("[rule-units] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
