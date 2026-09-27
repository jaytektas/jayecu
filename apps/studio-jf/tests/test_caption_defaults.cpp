// A caption the studio places is a Label, so it is born like one.
//
// Dropping a binding creates the control AND its caption. The control starts from its type's Preferences ▸
// Widget Defaults (addWidgetAt); the caption started from nothing but its text, so a Label default the user
// had set — a font, an alignment, a colour — reached every label on the page except the ones the studio
// placed itself. Which are most of them.
//
//   cmake --build build --target caption_defaults_test && ./build/caption_defaults_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/EditorSettings.h"
#include "surface/Surface.h"
#include "surface/PanelModel.h"
#include <j/core/SceneGraph.h>

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-62s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m); c.setConfigImage(m.defaultImage());
    jf::JSceneGraph graph;

    // A Label default, as the Widget Defaults form persists one.
    EditorSettings::instance().setWidgetDefaultProps("label", {
        { "align", "Left" }, { "bold", "1" }, { "fgColor", "#ff8c28" } });

    PanelModel page;
    Surface s(graph, c, &page);
    s.setMode(Surface::Mode::Edit);
    s.addWidgetAt("field", "Coolant Temperature");

    const PanelElement* cap = nullptr;
    for (const PanelElement& e : page.elements())
        if (e.type == "label") cap = &e;
    ck(cap != nullptr, "the drop places a caption");
    if (cap) {
        ck(cap->prop("labelText") == "Coolant Temperature", "…with the caption text", cap->prop("labelText"));
        ck(cap->prop("align")   == "Left",    "…and the Label default alignment", cap->prop("align"));
        ck(cap->prop("bold")    == "1",       "…and bold",                        cap->prop("bold"));
        ck(cap->prop("fgColor") == "#ff8c28", "…and the colour",                  cap->prop("fgColor"));
    }
    EditorSettings::instance().clearWidgetDefaults("label");

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "a placed caption is born with the Label defaults",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
