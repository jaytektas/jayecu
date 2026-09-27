// AN OUTPUT PIN'S PAGE OFFERS WHAT THAT PIN AND THIS ENGINE CAN USE — asked of the real ComboBoxWidget the
// page draws, because that is what the user reads. (The pin-capability rule was once wired into the enum
// picker only, while the pages draw these fields as combo boxes, so it reached no page at all.)
//
//   Function   IGN pins offer Ignition, LS pins Injector, every pin None and Generic (meta element_options).
//   Cylinder   the cylinders this engine has for a coil or a sequential injector; All for a distributor or
//              a multi-point injector; the banks for a bank-fired one (EngineOutputLayout::cylinderOptions).
//
//   cmake --build build --target output_row_pickers_test && ./build/output_row_pickers_test
#include "../src/surface/widgets/ComboBoxWidget.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"
#include "../src/model/EngineOutputLayout.h"
#include "../src/model/MetaModel.h"
#include "../src/model/MathEvaluator.h"
#include "../src/model/SigilResolvers.h"

#include <j/core/SceneGraph.h>
#include <j/core/JComboBox.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-62s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

static std::string shown(jf::JSceneGraph& g, const std::string& path) {
    PanelElement el;
    el.type = "combobox";
    el.props["signalName"] = path;
    ComboBoxWidget w(g);
    w.bind(&el, &Cache::instance());
    w.syncForTest();
    auto* cb = dynamic_cast<jf::JComboBox*>(w.controlForTest());
    std::string s;
    if (cb) for (const std::string& it : cb->items()) s += it + "|";
    return s;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());
    // The page reads a bound field through the evaluator, as the app does.
    static ConfigSigilResolver cfg;
    MathEvaluator::instance().registerResolver(&cfg);
    engine_outputs::install(c);
    ComboBoxWidget::optionFilter = [](const std::string& b) { return engine_outputs::cylinderOptions(Cache::instance(), b); };
    jf::JSceneGraph g;

    ck(shown(g, "outputs.output[0].function") == "None|Ignition|Generic|", "IGN1 offers a coil, not an injector",
       shown(g, "outputs.output[0].function"));
    ck(shown(g, "outputs.output[12].function") == "None|Injector|Generic|", "LS1 offers an injector, not a coil",
       shown(g, "outputs.output[12].function"));
    ck(shown(g, "outputs.output[34].function") == "None|Generic|", "HS1 offers neither",
       shown(g, "outputs.output[34].function"));

    // A four-cylinder, coil-on-plug, one sequential stage: the edits lay the rows out.
    c.setConfigValue("engine.num_inj_stages", 1);
    c.setConfigValue("engine.ign_mode", 2);
    c.setConfigValue("engine.cylinder_count", 2);
    c.setConfigValue("engine.cylinder_count", 4);
    const std::string four = "Cylinder 1|Cylinder 2|Cylinder 3|Cylinder 4|";
    ck(shown(g, "outputs.output[0].cylinder") == four, "a coil's Cylinder offers the four cylinders",
       shown(g, "outputs.output[0].cylinder"));
    ck(shown(g, "outputs.output[12].cylinder") == four, "a sequential injector's Cylinder offers the four",
       shown(g, "outputs.output[12].cylinder"));
    c.setConfigValue("engine.inj_stage[0].num_outputs", 1);
    c.setConfigValue("engine.inj_stage[0].mode", engine_outputs::MultiPoint);
    ck(shown(g, "outputs.output[12].cylinder") == "All Cylinders|", "a multi-point injector offers All",
       shown(g, "outputs.output[12].cylinder"));
    c.setConfigValue("engine.ign_mode", 0);
    ck(shown(g, "outputs.output[0].cylinder") == "All Cylinders|", "a distributor coil offers All",
       shown(g, "outputs.output[0].cylinder"));

    std::printf(fails ? "\n[row-pickers] %d FAILED\n" : "\n[row-pickers] all passed\n", fails);
    return fails ? 1 : 0;
}
