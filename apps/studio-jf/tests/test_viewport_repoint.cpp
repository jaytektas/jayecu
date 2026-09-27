// A viewport re-pointed at a different page must show THAT page — all of it, and only it.
//
// The children a viewport mirrors are cached in a map keyed by SOURCE ELEMENT ID, and element ids are
// per-page starting at 1. So page B's element 3 finds page A's widget, matches on type, and gets reused
// — while carrying A's bounds, props and bindings. On screen that is a page with widgets from another
// page in it, at the wrong sizes. It never showed while a viewport was pinned to one node for its whole
// life; it appears the moment one viewport serves more than one page.
//
//   cmake --build build --target viewport_repoint_test && ./build/viewport_repoint_test
#include "../src/surface/Surface.h"
#include "../src/surface/PanelLibrary.h"
#include "../src/surface/widgets/ViewportWidget.h"
#include "../src/model/Cache.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[vp-repoint] %-52s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    jf::JSceneGraph graph;
    PanelLibrary lib;
    nodeviewport::resolver() = [&lib](const std::string& n) -> const PanelModel* { return lib.find(n); };

    // TWO PAGES WITH COLLIDING IDS AND MATCHING TYPES — the case the cache cannot tell apart. Both hold
    // one label; only the geometry and the caption differ. Ids are assigned per page, so both are id 1.
    PanelModel& a = lib.forNode("Page A");
    const int ida = a.add("label", 10.f, 10.f, 100.f, 20.f, { { "labelText", "FROM A" } });
    PanelModel& b = lib.forNode("Page B");
    const int idb = b.add("label", 500.f, 400.f, 300.f, 60.f, { { "labelText", "FROM B" } });
    check(ida == idb, "the two pages really do collide on element id",
          std::to_string(ida) + " vs " + std::to_string(idb));

    // A surface with ONE viewport, re-pointed between the two pages.
    PanelModel host;
    const int vpId = host.add("viewport", 0.f, 0.f, 1280.f, 700.f, { { "node", "Page A" } });
    Cache& cache = Cache::instance();
    Surface surf(graph, cache, &host);
    surf.setBounds({ 0.f, 0.f, 1280.f, 700.f });
    surf.setActiveNode("Page A");
    surf.setPageAccess([&lib](const std::string& p) -> PanelModel* { return &lib.forNode(p); }, []{});

    auto mirrored = [&](const char* node) -> const PanelElement* {
        host.setProp(vpId, "node", node);
        surf.setActiveNode(node);
        jf::JPrimitiveBuffer buf;
        surf.populateRenderPrimitives(buf);          // render is what reconciles
        auto* vw = dynamic_cast<ViewportWidget*>(surf.widgetById(vpId));
        if (!vw) return nullptr;
        CanvasWidget* child = vw->mirroredWidget(1);
        return child ? child->element() : nullptr;
    };

    if (const PanelElement* pe = mirrored("Page A")) {
        check(pe->prop("labelText") == "FROM A", "page A's child carries A's caption",
              pe->prop("labelText"));
        check(pe->w == 100.f, "…and A's geometry", std::to_string(pe->w));
    } else {
        check(false, "page A mirrors a child at all");
    }

    // THE MOVE. Same id, same type — so without the page-change reset the old widget is kept.
    if (const PanelElement* pe = mirrored("Page B")) {
        check(pe->prop("labelText") == "FROM B", "…and after re-pointing it carries B's",
              pe->prop("labelText"));
        check(pe->w == 300.f, "…and B's geometry, not A's", std::to_string(pe->w));
    } else {
        check(false, "page B mirrors a child at all");
    }

    // …and back, so the reset is not one-directional.
    if (const PanelElement* pe = mirrored("Page A"))
        check(pe->prop("labelText") == "FROM A", "…and back again", pe->prop("labelText"));

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All viewport re-point tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
