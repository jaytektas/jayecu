// One viewport per surface, showing whichever page the tree has selected.
//
// A page is authored once and lives in the library keyed by its tree path; a viewport is a window onto
// the current one. The old model bound every viewport to a node and hid it unless that node was
// selected, so a surface needed one viewport PER PAGE — and a page whose viewport had never been added
// to a given surface was unreachable there.
//
// The gates this covers are the ones that actually broke: viewportElAt() decides WHICH viewport a
// double-click hit, and enterViewport() decides what it opens. Both asked viewportPage(), which answers
// "" for an unbound viewport — so the click was rejected before it reached the code that would have
// handled it, and fixing only the second one changed nothing.
//
//   cmake --build build --target viewport_unbound_test && ./build/viewport_unbound_test
#include "../src/surface/Surface.h"
#include "../src/surface/PanelLibrary.h"
#include "../src/surface/widgets/ViewportWidget.h"
#include "../src/model/Cache.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[unbound] %-56s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    jf::JSceneGraph graph;
    PanelLibrary lib;
    nodeviewport::resolver() = [&lib](const std::string& n) -> const PanelModel* { return lib.find(n); };

    lib.forNode("Fuel/VE Table").add("label", 20.f, 20.f, 200.f, 30.f, { { "labelText", "VE" } });
    lib.forNode("Ignition/Advance").add("label", 40.f, 40.f, 400.f, 50.f, { { "labelText", "ADV" } });
    lib.forNode("Fuel/VE Table").setTitle("VE Table");
    lib.forNode("Ignition/Advance").setTitle("Ignition/Advance");

    // ONE viewport, carrying no node of its own.
    PanelModel host;
    const int vpId = host.add("viewport", 0.f, 0.f, 1280.f, 700.f, {});
    Cache& cache = Cache::instance();
    Surface surf(graph, cache, &host);
    surf.setBounds({ 0.f, 0.f, 1280.f, 700.f });
    surf.setPageAccess([&lib](const std::string& p) -> PanelModel* { return &lib.forNode(p); }, []{});
    surf.setMode(Surface::Mode::Edit);

    auto shows = [&](const char* node) -> std::string {
        surf.setActiveNode(node);
        jf::JPrimitiveBuffer buf;
        surf.populateRenderPrimitives(buf);
        auto* vw = dynamic_cast<ViewportWidget*>(surf.widgetById(vpId));
        if (!vw) return "<no viewport widget>";
        CanvasWidget* child = vw->mirroredWidget(1);
        const PanelElement* pe = child ? child->element() : nullptr;
        return pe ? pe->prop("labelText") : std::string("<nothing mirrored>");
    };

    check(shows("Fuel/VE Table") == "VE", "an unbound viewport shows the selected page",
          shows("Fuel/VE Table"));
    check(shows("Ignition/Advance") == "ADV", "…and follows the selection to another",
          shows("Ignition/Advance"));

    // THE HIT-TEST. It used to reject a viewport whose viewportPage() was empty, which is every unbound
    // one — so the double-click never even identified what it had hit.
    surf.setActiveNode("Fuel/VE Table");
    { jf::JPrimitiveBuffer b; surf.populateRenderPrimitives(b); }
    check(surf.viewportElAt(640.f, 350.f) == vpId,
          "a click inside it finds the viewport", std::to_string(surf.viewportElAt(640.f, 350.f)));

    // …AND THE DRILL-IN. Entering scopes the surface onto that page, which is what the page editor is.
    surf.enterViewport(vpId);
    check(surf.inScope(), "double-click enters the page editor");
    check(surf.model() == &lib.forNode("Fuel/VE Table"), "…on the page it was showing");

    // THE PAGE'S OWN TITLE. It is drawn in the post-content pass, which resolved the page through
    // viewportPage() rather than pageKey() — empty for an unbound viewport, so the model came back null
    // and the caption vanished along with the page's border settings. Asserting on the resolution
    // rather than on pixels: if pageKey() names the page, the title bar has something to draw.
    surf.setActiveNode("Ignition/Advance");
    { jf::JPrimitiveBuffer b; surf.populateRenderPrimitives(b); }
    if (auto* vw = dynamic_cast<ViewportWidget*>(surf.widgetById(vpId))) {
        check(vw->mirroredPage() != nullptr, "the viewport resolves the page it is showing");
        check(vw->titleShown() == "Ignition/Advance", "…and draws that page's own title",
              "'" + vw->titleShown() + "'");
    } else {
        check(false, "the viewport widget exists");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All unbound viewport tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
