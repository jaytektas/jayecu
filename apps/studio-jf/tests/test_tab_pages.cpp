// A tab carries its own page.
//
// Every surface used to be pushed the same node, so all tabs showed one page and switching tabs
// changed the framing and nothing else. Park the VE table on one tab and the trigger scope on another
// and switching between them should switch the page — and the tree should follow, or the selection
// says one thing while the surface shows another.
//
//   cmake --build build --target tab_pages_test && ./build/tab_pages_test
#include "../src/surface/SurfaceTabs.h"
#include "tab_by_name.h"
#include "../src/surface/PanelLibrary.h"
#include "../src/model/Cache.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[tab-pages] %-52s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    jf::JSceneGraph graph;
    Cache& cache = Cache::instance();
    PanelLibrary store;
    SurfaceTabs tabs(graph, cache, store);
    tabs.setBounds({ 0.f, 0.f, 1280.f, 800.f });

    for (const char* n : { "Fuel/VE Table", "Ignition/Advance" })
        store.forNode(n).add("label", 10.f, 10.f, 100.f, 20.f, {});

    const int ia = tabs.newSurface("Workspace A");
    const int ib = tabs.newSurface("Workspace B");
    (void)ia;
    Surface* b = tabs.activeSurface();        // newSurface leaves the new tab active
    reopenNamed(tabs, "Workspace A");
    Surface* a = tabs.activeSurface();
    check(a && b && a != b, "two workspaces, each with its own surface",
          std::to_string(ia) + "/" + std::to_string(ib));
    if (!a || !b) return 1;

    // Whichever tab is active takes the selection; the other keeps what it had.
    tabs.setActiveNode("Fuel/VE Table");
    const std::string bAfterFirst = b->activeNode();

    reopenNamed(tabs, "Workspace A");
    tabs.setActiveNode("Ignition/Advance");

    check(a->activeNode() == "Ignition/Advance", "the active tab takes the selection",
          a->activeNode());
    check(b->activeNode() == bAfterFirst, "…and the other tab keeps its own page",
          b->activeNode() + " (was " + bAfterFirst + ")");
    check(a->activeNode() != b->activeNode(), "two tabs really are on different pages");

    // THE TREE FOLLOWS. Arriving on a tab announces the page it is on, so the app can move the
    // highlight — without which the selection and the surface disagree.
    std::string announced;
    tabs.nodeFollowed.connect([&](const std::string& n) { announced = n; });
    reopenNamed(tabs, "Workspace B");
    check(announced == b->activeNode(), "switching tabs announces that tab's page",
          "announced '" + announced + "', tab is on '" + b->activeNode() + "'");

    // NAVIGATING IS NOT AN EDIT. Where a tab is looking is where you are, not what the document says,
    // so moving between pages must not dirty it — otherwise browsing raises a save prompt on the way out
    // and, worse, a save would write wherever you happened to stop over the tab's configured default.
    bool dirtied = false;
    Surface::onModified = [&dirtied] { dirtied = true; };
    tabs.setActiveNode("Fuel/VE Table");
    check(!dirtied, "changing the selected page does not dirty the document");
    Surface::onModified = nullptr;

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All tab page tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
