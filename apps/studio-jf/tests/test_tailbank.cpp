// Repro: the Bank [@expr] cell on an UNUSED firing-order slot shows a value (30) instead of blank/0.
// Drives the real ConfigEditWidget the dashboard uses, against a real config image whose firing_order
// tail is 0 (positions 7..12 unused) — exactly the live tune.
//   cmake --build build --target tailbank_test && ./build/tailbank_test
#include "../src/surface/widgets/ConfigEditWidget.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"
#include "../src/model/MathEvaluator.h"
#include "../src/model/SigilResolvers.h"

#include <j/core/SceneGraph.h>
#include <j/core/JDoubleSpinBox.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-56s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

static double bankCell(jf::JSceneGraph& g, int foIndex) {
    PanelElement el;
    el.type = "configedit";
    el.props["signalName"] = "engine.cyl[@engine.firing_order[" + std::to_string(foIndex) + "].cyl - 1].bank";
    el.props["scale"] = "0";
    el.props["decimals"] = "-1";
    el.props["displayUnit"] = "Auto";
    ConfigEditWidget w(g);
    w.bind(&el, &Cache::instance());
    w.syncForTest();
    auto* sb = dynamic_cast<jf::JDoubleSpinBox*>(w.controlForTest());
    return sb ? sb->value() : -12345.0;
}

// The decimals a Bank [@expr] cell formats with, for a given firing-order row. An unused slot binds
// blank but must still format as a Bank (0 decimals -> "0"), not the generic "0.00" fallback.
static int bankDecimals(jf::JSceneGraph& g, int foIndex) {
    PanelElement el;
    el.type = "configedit";
    el.props["signalName"] = "engine.cyl[@engine.firing_order[" + std::to_string(foIndex) + "].cyl - 1].bank";
    el.props["scale"] = "0";
    el.props["decimals"] = "-1";               // AUTO -> the field's precision
    el.props["displayUnit"] = "Auto";
    ConfigEditWidget w(g);
    w.bind(&el, &Cache::instance());
    return w.decimalsForTest();
}

int main(int argc, char** argv) {
    MetaModel meta;
    const char* cands[] = { argc > 1 ? argv[1] : nullptr,
                            "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json",
                            "shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::puts("[tailbank] (no meta found — skipped)"); return 0; }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    std::vector<uint8_t> image = meta.defaultImage();
    if (image.empty()) image.assign((size_t)meta.configSize(), 0);
    C.setConfigImage(image);

    // The live tune: firing order 1-5-3-6-2-4, tail unused (0); every cylinder on bank 1.
    const int fo[12] = { 1,5,3,6,2,4, 0,0,0,0,0,0 };
    for (int i = 0; i < 12; ++i)
        C.writeRaw(meta.locate("engine.firing_order[" + std::to_string(i) + "].cyl"), fo[i],
                   "engine.firing_order[" + std::to_string(i) + "].cyl");
    for (int i = 0; i < 12; ++i)
        C.writeRaw(meta.locate("engine.cyl[" + std::to_string(i) + "].bank"), 1,
                   "engine.cyl[" + std::to_string(i) + "].bank");

    // Wire the evaluator exactly as the app does: the '#' config resolver reads the image.
    static ConfigSigilResolver cfg;
    MathEvaluator::instance().registerResolver(&cfg);

    jf::JSceneGraph g;
    std::puts("=== Bank [@expr] cell value, per firing-order row ===");
    // Sanity: a USED row points at its cylinder's bank (all banks are 1 here).
    ck(bankCell(g, 0) == 1.0, "row 1 (used, cyl 1) -> bank 1", std::to_string(bankCell(g, 0)));
    // The bug: an UNUSED tail row (firing_order[i].cyl = 0) must be blank/0, not 30.
    for (int i = 6; i <= 11; ++i) {
        const double v = bankCell(g, i);
        ck(v == 0.0, "row " + std::to_string(i + 1) + " (unused, cyl 0) -> 0", std::to_string(v));
    }
    // …and it must FORMAT as a Bank (0 decimals), so it reads "0", not the 2-decimal "0.00" that
    // clipped to look like "30". Used and unused rows alike take the field's own precision.
    ck(bankDecimals(g, 0) == 0, "row 1 (used) Bank decimals = 0",   std::to_string(bankDecimals(g, 0)));
    ck(bankDecimals(g, 6) == 0, "row 7 (unused) Bank decimals = 0", std::to_string(bankDecimals(g, 6)));

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "clean",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
