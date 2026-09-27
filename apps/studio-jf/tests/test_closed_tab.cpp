// ONLY THE TAB IN FRONT ANSWERS A RIGHT-CLICK.
//
// The report: two widgets selected (the inspector agreed — "2 widgets"), right-click one, and the context
// menu offers "Add control" and nothing else. Every other root item greys off the SELECTION, and "Add
// control" is the one whose state is never set — so the menu that opened had been built for an EMPTY
// selection. It belonged to a different surface.
//
// A closed tab is why. Every pooled surface owns a live Surface widget with a context menu; JTabWidget
// hides the pages it owns, but removeTab hands a closed page back VISIBLE (deliberately — it is the
// caller's widget and must stay paintable), and a doc restored-but-not-opened was never hidden at all.
// The window's right-click walk goes newest-widget-first and opens the menu of the first VISIBLE one whose
// box contains the click, so a closed surface still sitting at its last bounds answered ahead of the tab
// actually in front.
//
// So this counts what that walk would see: how many Surface widgets are visible at once.
//
//   cmake --build build --target closed_tab_test && ./build/closed_tab_test

#include "surface/SurfaceTabs.h"
#include "tab_by_name.h"
#include "surface/PanelLibrary.h"
#include "model/Cache.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const char* what, const std::string& note = {}) {
    std::printf("[closed-tab] %-58s %s%s\n", what, ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// Exactly the test the window's right-click walk makes: every live widget, visible, that could answer.
static int visibleSurfaces() {
    int n = 0;
    for (jf::JWidget* w : jf::JWidget::s_activeWidgets) {
        if (!w || !w->isVisible() || !w->contextMenu()) continue;
        if (w->getRef("role").toString() == "Surface") ++n;
    }
    return n;
}

int main() {
    jf::JSceneGraph graph;
    PanelLibrary lib;
    SurfaceTabs tabs(graph, Cache::instance(), lib);

    ck(visibleSurfaces() == 1, "one surface open: one surface answers clicks",
       std::to_string(visibleSurfaces()));

    tabs.newSurface("Surface");
    ck(visibleSurfaces() == 1, "a second tab opens: still only the one in front",
       std::to_string(visibleSurfaces()));

    tabs.newSurface("Surface");
    ck(visibleSurfaces() == 1, "…and a third", std::to_string(visibleSurfaces()));

    // THE REPORT. Closing a tab leaves its surface in the pool so it can be reopened — but it must stop
    // answering for the tab that is now in front.
    tabs.closeActive();
    ck(visibleSurfaces() == 1, "closing a tab does not leave TWO surfaces answering",
       std::to_string(visibleSurfaces()) + " visible");

    // Reopening it is the same question from the other side: the one that comes back is the one in front.
    const std::vector<std::string> closed = closedSurfaceNames(tabs);
    ck(!closed.empty(), "the closed surface is still reopenable");
    if (!closed.empty()) ck(reopenNamed(tabs, closed.front()), "…by the name it was closed under");
    ck(visibleSurfaces() == 1, "reopening it leaves exactly one answering",
       std::to_string(visibleSurfaces()));

    // A RESTORE builds a doc for every pooled surface, open or not — the shape a saved document with a
    // closed tab comes back in.
    jf::JJson doc = tabs.surfacesToJson();
    tabs.surfacesFromJson(doc);
    ck(visibleSurfaces() == 1, "a restored document leaves only the open tab answering",
       std::to_string(visibleSurfaces()));

    std::printf("[closed-tab] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
