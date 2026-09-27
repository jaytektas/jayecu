// Pasting a VIEWPORT: what you get is a NEW viewport on the SELECTED node — the same thing dragging that
// node out of the tree makes — at the size and position of the one copied, and the copied viewport's
// widgets duplicated into that node's page as new widgets. Nothing else is carried over.
//
// Pinned because this went wrong repeatedly by hand: the copy first got a private canvas keyed by its uid
// (which "node" also gates visibility on, so it was invisible forever), then kept the SOURCE's node (so it
// appeared on the original's node instead of the one being edited).
//
//   cmake --build build --target viewport_paste_test && ./build/viewport_paste_test

#include "../src/surface/Surface.h"
#include "../src/surface/PanelLibrary.h"
#include "../src/model/Cache.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <set>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("[vp-paste] %-56s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    jf::JSceneGraph graph;
    PanelLibrary lib;
    nodeviewport::resolver() = [&lib](const std::string& n) -> const PanelModel* { return lib.find(n); };

    // SOURCE node "A": a page with two widgets, one of them grouped.
    PanelModel& pageA = lib.forNode("A");
    const int wa = pageA.add("checkbox", 10.f, 20.f, 60.f, 20.f, {{"signalName", "sensors.sensor[*].enabled"}});
    const int wb = pageA.add("label",    10.f, 50.f, 80.f, 18.f, {{"labelText", "Enabled"}});
    pageA.setGroupId(wa, 7); pageA.setGroupId(wb, 7);

    pageA.setCanvasSize(1600.f, 900.f);          // the source page is NOT the default size
    // The surface holds one viewport onto A.
    PanelModel main;
    const int vpA = main.add("viewport", 100.f, 200.f, 640.f, 480.f, {{"node", "A"}, {"title", "A"}});

    Surface surf(graph, Cache::instance(), &main);
    surf.setPageAccess([&lib](const std::string& n) -> PanelModel* { return &lib.forNode(n); }, []{});
    surf.setMode(Surface::Mode::Edit);
    // PINNED TO 1:1, so this test says what it means. The page canvas and the surface bounds are
    // deliberately the same size here; fitting one to the other is exactly scale 1, whatever the
    // user's "static surface size" preference happens to be. Static is no longer 1:1 — it renders a
    // page AT the preference size — so without this a hit-test lands wherever that preference put it.
    surf.setFitToView(true);

    // Copy it, select node B, paste.
    surf.setActiveNode("A");
    surf.menuSelectAll();          // the surface holds only this viewport
    surf.menuCopy();
    surf.setActiveNode("B");
    surf.menuPaste();

    const PanelElement* pasted = nullptr;
    for (const PanelElement& e : main.elements())
        if (e.type == "viewport" && e.id != vpA) pasted = &e;
    check(pasted != nullptr, "a viewport was pasted");
    if (!pasted) { std::printf("\n[vp-paste] FAILURES\n"); return 1; }

    check(pasted->prop("node") == "B",       "it belongs to the SELECTED node, not the source's");
    check(pasted->prop("page").empty(),      "no private canvas — the node's page IS its canvas");
    check(pasted->w == 640.f && pasted->h == 480.f, "same size as the one copied");
    check(pasted->x == 100.f && pasted->y == 200.f, "same position as the one copied");
    check(pasted->uid != main.get(vpA)->uid && !pasted->uid.empty(), "its own uid");
    check(pasted->prop("title") == "B",      "titled after its node, like a tree drag (not the source's)");

    // The widgets are duplicated into B's page — as NEW widgets.
    const PanelModel* pageB = lib.find("B");
    check(pageB && pageB->elements().size() == 2, "B's page holds copies of both widgets");
    if (pageB) {
        std::set<std::string> srcUids, dstUids;
        for (const auto& e : pageA.elements())    srcUids.insert(e.uid);
        for (const auto& e : pageB->elements())   dstUids.insert(e.uid);
        bool shared = false;
        for (const auto& u : dstUids) if (srcUids.count(u)) shared = true;
        check(!shared, "copies carry FRESH uids (no widget on two pages)");
        bool props = false, grouped = false;
        int g0 = 0;
        for (const auto& e : pageB->elements()) {
            if (e.type == "checkbox" && e.prop("signalName") == "sensors.sensor[*].enabled") props = true;
            if (e.groupId) { if (!g0) g0 = e.groupId; else grouped = (e.groupId == g0); }
        }
        check(props,   "widget properties came across");
        check(grouped, "grouping is preserved, re-keyed inside the new page");
        check(g0 != 7, "…and it is NOT the source page's group id");
    }
    // The page's CANVAS comes across too, or the copied widgets sit on a differently-sized page and the
    // copy does not look like the thing it was copied from.
    check(pageB && pageB->canvasW() == 1600.f && pageB->canvasH() == 900.f,
          "the copy's canvas matches the source's, not just its widgets");
    // The source page is untouched — editing the copy must never edit the original.
    check(pageA.elements().size() == 2, "the source page still holds exactly its own two widgets");

    // INSIDE a page, a pasted viewport is a plain copy. Re-pointing it at the active node would aim it at
    // the page it now lives on, and copying the source page's widgets in would duplicate them into the page
    // being edited — which is exactly what happened: the controls again, plus a self-referencing viewport.
    {
        const int pastedId = pasted->id;
        const size_t before = lib.find("B") ? lib.find("B")->elements().size() : 0;
        surf.enterViewport(pastedId);                // edit B's page in place
        surf.menuPaste();                            // the clipboard still holds the viewport
        const PanelModel* b = lib.find("B");
        bool selfRef = false;
        for (const auto& e : b->elements())
            if (e.type == "viewport" && viewportPage(e) == "B") selfRef = true;
        check(!selfRef, "a viewport pasted INSIDE a page does not point at that page");
        check(b && b->elements().size() == before + 1,
              "…and adds ONE element, not a viewport plus a second set of widgets");
    }

    std::printf("\n[vp-paste] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
