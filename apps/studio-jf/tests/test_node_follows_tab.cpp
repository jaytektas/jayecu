// A TAB WITH NO VIEWPORT HANDS THE SELECTION BACK TO MAIN.
//
// The report: with Diagnostics in front, picking a node in the tree (or following a link) does nothing
// visible. Diagnostics is a workspace of instruments with no viewport at all, so there is nowhere on that
// tab for the page to appear — the tree moves, the surface does not, and the user is left wondering where
// the page they asked for went.
//
// The test is on the TAB, not the node: a workspace that HAS viewports keeps its selections, including for
// pages it does not happen to carry. Which tabs hold which pages is the workspace's own business, and a
// rule that jumped whenever the current tab lacked a viewport for THIS page would throw the user out of
// Idle Control every time they touched the tree.
//
//   cmake --build build --target node_follows_tab_test && ./build/node_follows_tab_test

#include "surface/SurfaceTabs.h"
#include "tab_by_name.h"
#include "surface/PanelLibrary.h"
#include "model/Cache.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const char* what, const std::string& note = {}) {
    std::printf("[follow] %-62s %s%s\n", what, ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    jf::JSceneGraph graph;
    PanelLibrary lib;
    SurfaceTabs tabs(graph, Cache::instance(), lib);

    const std::string ve       = "Configuration/Fuel/VE Table";
    const std::string idle     = "Configuration/Engine Functions/Idle Control";
    const std::string idleDuty = idle + "/Idle Base Duty";
    const std::string group    = "Configuration/Engine Functions";   // a bare grouping node: no page

    // MAIN — the general tuning tool: a viewport for every page, as the real document has.
    PanelModel* main = tabs.activeModel();
    ck(main != nullptr, "the default tab has a model");
    if (!main) return 1;
    for (const std::string& n : { ve, idle, idleDuty })
        main->add("viewport", 0.f, 0.f, 600.f, 400.f, { { "node", n } });
    const std::string MAIN = tabs.activeName();

    // IDLE CONTROL — a focused workspace: viewports, but only for its own branch.
    tabs.newSurface("Idle Control");
    for (const std::string& n : { idle, idleDuty })
        tabs.activeModel()->add("viewport", 0.f, 0.f, 400.f, 300.f, { { "node", n } });

    // DIAGNOSTICS — instruments only. No viewport, so it can display no page content whatsoever.
    tabs.newSurface("Diagnostics");
    tabs.activeModel()->add("gauge", 0.f, 0.f, 120.f, 120.f, { { "signalName", "clt" } });
    ck(tabs.activeName() == "Diagnostics", "standing on Diagnostics", tabs.activeName());

    // THE REPORT.
    tabs.setActiveNode(ve);
    ck(tabs.activeName() == MAIN, "from Diagnostics, a selection goes to Main", tabs.activeName());

    // Even for a page a focused workspace also carries: Main is the general tool and carries every page.
    reopenNamed(tabs, "Diagnostics");
    ck(tabs.activeName() == "Diagnostics", "back on Diagnostics", tabs.activeName());
    tabs.setActiveNode(idleDuty);
    ck(tabs.activeName() == MAIN, "...and so does a page a focused tab also carries", tabs.activeName());

    reopenNamed(tabs, "Diagnostics");
    tabs.setActiveNode("");
    ck(tabs.activeName() == "Diagnostics", "an empty selection moves nothing", tabs.activeName());

    // THE HALF THAT IS EASY TO GET WRONG. A workspace that HAS viewports keeps every selection.
    reopenNamed(tabs, "Idle Control");
    ck(tabs.activeName() == "Idle Control", "on the Idle Control workspace", tabs.activeName());
    tabs.setActiveNode(idle);
    ck(tabs.activeName() == "Idle Control", "a node the focused tab carries stays put", tabs.activeName());
    tabs.setActiveNode(ve);
    ck(tabs.activeName() == "Idle Control", "a node it does NOT carry also stays put — it has a viewport",
       tabs.activeName());
    tabs.setActiveNode(group);
    ck(tabs.activeName() == "Idle Control", "and a bare grouping node moves nothing", tabs.activeName());

    std::printf("[follow] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
