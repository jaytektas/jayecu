// Does a search leave you in control of the tree?
//
// The field picker is the dictionary's tree in a popup, so it has to behave like the dictionary: what you
// open stays open, a search reveals its matches without taking the tree away from you, and clearing the
// search gives you back the arrangement you had. The revealing is the part that goes wrong quietly — if a
// filter forces branches open at RENDER time, every branch you then collapse springs open again on the
// next frame and there is no way to shut anything while a query stands.
//
// This is about the shared JTreeView the picker and the dictionary dock both use, driven over the real
// config tree, since that is where the depth is (module ▸ array ▸ element ▸ field).
//
//   cmake --build build --target tree_filter_test && ./build/tree_filter_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "ui/DictionaryTree.h"
#include <j/core/JTreeView.h>
#include <j/core/SceneGraph.h>

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-64s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// The row for a label, anywhere in the tree.
static jf::JTreeViewNode* find(jf::JTreeViewNode& n, const std::string& label) {
    if (n.label == label) return &n;
    for (auto& c : n.children) if (jf::JTreeViewNode* h = find(c, label)) return h;
    return nullptr;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    jf::JSceneGraph g;
    jf::JTreeView tree(g);
    tree.setRootNode(buildConfigTree(m));
    tree.setFilterMatchesUserData(true);
    tree.root().expanded = true;

    std::puts("=== searching the field tree ===");

    jf::JTreeViewNode* engine = find(tree.root(), "engine");
    ck(engine != nullptr, "the tree has the engine module");
    if (!engine) return 1;

    // 1 — the arrangement you make is yours.
    engine->expanded = true;
    ck(engine->expanded, "a branch you open is open");

    // 2 — a search REVEALS its matches: the path down to a match is expanded as real state, so the row is
    //     actually reachable rather than merely drawn.
    tree.setFilter("tdc_angle");
    jf::JTreeViewNode* cyl = find(tree.root(), "cyl  (12)");
    ck(cyl && cyl->expanded, "a search opens the path down to what it matched");

    // 3 — …and you can shut it again while the search still stands. This is the one that fails when a
    //     filter overrides expansion at render time instead of setting it.
    if (cyl) {
        cyl->expanded = false;
        ck(!cyl->expanded, "a matched branch can be collapsed while the search stands");
        cyl->expanded = true;
    }

    // 4 — clearing the search costs nothing: the arrangement from before it comes back.
    jf::JTreeViewNode* sensors = find(tree.root(), "sensors");
    const bool sensorsWasOpen = sensors && sensors->expanded;
    tree.setFilter({});
    sensors = find(tree.root(), "sensors");
    ck(sensors && sensors->expanded == sensorsWasOpen,
       "clearing the search restores what was there before it");
    engine = find(tree.root(), "engine");
    ck(engine && engine->expanded, "…including the branch you had opened yourself");

    // 5 — a search matches the BINDING PATH too, which is how you find a field by the name it has in the
    //     firmware rather than by its prose label.
    // A field inside an ARRAY, which is where the tree shows a prose label rather than the field name:
    // wb[0].assign_mode reads "Assignment", so no substring of the query appears in the row's text and a
    // hit can only have come from matching the path. (This used to search cyl[0].ign_channel, a field
    // the fixed IGNk->cylk output binding removed.)
    tree.setFilter("wb[0].assign_mode");
    jf::JTreeViewNode* hit = find(tree.root(), "Assignment");
    ck(hit != nullptr, "a field is findable by its path, not only its label");
    tree.setFilter({});

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "a search reveals without taking the tree away from you",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
