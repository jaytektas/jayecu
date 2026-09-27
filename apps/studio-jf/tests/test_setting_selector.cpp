// Does the setting selector say what the ECU is actually set to?
//
// It is a preset dropdown: each option names a set of config paths and the values they take, picking one
// writes them all, and the option SHOWN is the one whose values the tune currently holds. That last part
// is what makes it a readout as well as a control — and what makes "Custom" necessary, because a tune
// that matches no option is a real state and the alternative is a box displaying an option whose numbers
// are not the ones in the ECU.
//
// It hosts a framework JComboBox now rather than drawing a box and a triangle, so these assertions are
// about the control's own item list and selection, which is what the user sees.
//
//   cmake --build build --target setting_selector_test && ./build/setting_selector_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/MathEvaluator.h"
#include "model/SigilResolvers.h"
#include "model/PresetOptions.h"
#include "surface/WidgetRegistry.h"
#include "surface/widgets/SettingSelectorWidget.h"
#include <j/core/JComboBox.h>
#include <j/core/SceneGraph.h>

#include <cstdio>
#include <memory>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-64s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());
    static ConfigSigilResolver configSigils;
    MathEvaluator::instance().registerResolver(&configSigils);
    jf::JSceneGraph graph;

    // Two engine configurations, as the preset editor would write them.
    const std::string spec =
        "4-cyl 1-3-4-2|engine.cylinder_count=4,engine.cyl[0].tdc_angle=0,engine.cyl[1].tdc_angle=5400\n"
        "6-cyl 1-5-3-6-2-4|engine.cylinder_count=6,engine.cyl[0].tdc_angle=0,engine.cyl[1].tdc_angle=4800";

    PanelElement el;
    el.id = 5;
    el.type = "settingselector";
    el.props["presets"] = spec;

    std::unique_ptr<CanvasWidget> w = makeWidgetInstance(el.type, graph);
    w->setSource(el); w->setData(el); w->setCache(&c);
    auto* sel = static_cast<SettingSelectorWidget*>(w.get());

    std::puts("=== the preset dropdown ===");

    // It is a real framework control, not a drawing of one.
    jf::JControl* ctl = sel->controlForTest();
    auto* combo = dynamic_cast<jf::JComboBox*>(ctl);
    ck(combo != nullptr, "the widget hosts a framework JComboBox");
    if (!combo) return 1;

    sel->syncForTest();
    ck(combo->items().size() == 3, "every preset is an item, plus Custom",
       std::to_string(combo->items().size()));

    // Nothing matches the shipped defaults, so it says so rather than naming an option it is not on.
    c.setConfigValue("engine.cylinder_count", 8);
    sel->syncForTest();
    ck(combo->currentText().rfind("Custom", 0) == 0, "a tune matching no option reads Custom",
       combo->currentText());

    // Set the tune to exactly what an option describes and the control follows — this is the readout half.
    PresetOptions::fromCompact(spec).apply(1);          // the 6-cylinder option
    sel->syncForTest();
    ck(combo->currentText() == "6-cyl 1-5-3-6-2-4", "…and names the option the ECU is sitting on",
       combo->currentText());
    ck(c.configValue("engine.cylinder_count") == 6.0, "applying an option writes all of its pairs",
       std::to_string(c.configValue("engine.cylinder_count")));
    ck(c.configValue("engine.cyl[1].tdc_angle") == 4800.0, "…including the array element ones",
       std::to_string(c.configValue("engine.cyl[1].tdc_angle")));

    // Changing ONE field of the matched set breaks the match: the dropdown is describing the tune, not
    // remembering what was last picked.
    c.setConfigValue("engine.cyl[1].tdc_angle", 1234);
    sel->syncForTest();
    ck(combo->currentText().rfind("Custom", 0) == 0,
       "changing one field of the set drops it back to Custom", combo->currentText());

    // An empty preset list is not an error, and must not offer a row that writes anything.
    {
        PanelElement bare;
        bare.id = 6;
        bare.type = "settingselector";
        std::unique_ptr<CanvasWidget> bw = makeWidgetInstance(bare.type, graph);
        bw->setSource(bare); bw->setData(bare); bw->setCache(&c);
        auto* bs = static_cast<SettingSelectorWidget*>(bw.get());
        auto* bc = dynamic_cast<jf::JComboBox*>(bs->controlForTest());
        bs->syncForTest();
        ck(bc && bc->items().size() == 1 && bc->currentText() == "(no presets)",
           "with no presets it says so and offers nothing",
           bc ? bc->currentText() : std::string("no control"));
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "the dropdown names what the ECU is set to, and sets what it names",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
