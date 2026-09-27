// A save must not delete pages the tree can still reach.
//
// pruneUnplacedPages() runs inside buildDoc(), so it is on the path of EVERY save. Its old rule was
// "keep a page some viewport names", which held only while every page had a viewport of its own. Give a
// surface ONE viewport that shows whichever page is selected and no page is named by anything — so the
// rule deletes the entire library, and the next load rebuilds each page empty on demand. That is a
// document destroyed by opening and saving it.
//
//   cmake --build build --target page_retention_test && ./build/page_retention_test
#include "../src/surface/SurfaceTabs.h"
#include "../src/surface/PanelLibrary.h"
#include "../src/model/Cache.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <set>
#include <string>

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[retention] %-54s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    jf::JSceneGraph graph;
    Cache& cache = Cache::instance();
    PanelLibrary store;
    SurfaceTabs tabs(graph, cache, store);

    // Three authored pages, as a real document has hundreds of.
    const std::set<std::string> tree = { "Cfg/Fuel/VE Table", "Cfg/Fuel/Target Lambda", "Cfg/Ignition/Advance" };
    for (const std::string& n : tree) store.forNode(n).add("table", 10.f, 10.f, 100.f, 100.f, {});
    check(store.pageKeys().size() == 3, "three pages in the library",
          std::to_string(store.pageKeys().size()));

    // THE TREE IS THE CRITERION. Nothing here names a page — the surface in this fixture has no
    // viewports at all, which is the same position an unbound one leaves the library in.
    tabs.pruneUnplacedPages(tree);
    check(store.pageKeys().size() == 3, "a save keeps every page the tree still has",
          std::to_string(store.pageKeys().size()) + " left");

    // A page whose node is GONE from the tree is genuinely unreachable and should go.
    const std::set<std::string> pruned = { "Cfg/Fuel/VE Table", "Cfg/Ignition/Advance" };
    tabs.pruneUnplacedPages(pruned);
    const auto left = store.pageKeys();
    check(left.size() == 2, "…and drops one whose node was deleted", std::to_string(left.size()));
    bool gone = true;
    for (const auto& k : left) if (k == "Cfg/Fuel/Target Lambda") gone = false;
    check(gone, "…the right one");

    // THE OLD RULE, for the record: with no path set the placement rule still applies, which is what a
    // caller that cannot say what is in the tree gets. It deletes everything here, and that is exactly
    // the behaviour that made one unbound viewport destroy a document.
    tabs.pruneUnplacedPages({});
    check(store.pageKeys().empty(),
          "with no tree to consult, the old placement rule still prunes",
          std::to_string(store.pageKeys().size()));

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All page retention tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
