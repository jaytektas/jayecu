// A GROUP arranges as ONE item.
//
// A group is documented as "same id => select/move/delete as one unit", and the selection expands to
// every member when you touch one. So an arrange op that walks the selection ELEMENT by element drags
// every member to the same edge and flattens the group's internal layout — aligning left shoved the
// contents of each group on top of each other, which is the opposite of what grouping is for.
//
// Pinned here:
//   - align moves a group by ONE delta: its box lands on the reference, members keep their offsets
//   - an ungrouped element still aligns on its own
//   - the reference is the group's BOX when you point at a member, not that member's own edge
//   - distribute and spacing space the group as one item, not as N items
//
//   cmake --build build --target arrange_groups_test && ./build/arrange_groups_test

#include "../src/surface/Surface.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"

#include <j/core/SceneGraph.h>
#include <cstdio>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[arrange] %-62s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  — " + detail).c_str());
    if (!ok) ++fails;
}

static std::string rectOf(PanelModel& m, int id) {
    const PanelElement* e = m.get(id);
    if (!e) return "(gone)";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.0f,%.0f %.0fx%.0f", e->x, e->y, e->w, e->h);
    return buf;
}

int main() {
    jf::JSceneGraph graph;
    PanelModel model;
    Surface surf(graph, Cache::instance(), &model);
    surf.setMode(Surface::Mode::Edit);

    // A group of two: a control at x=200 with its caption label 40px to its left.
    const int ctrl  = model.add("field", 200, 100, 80, 20, {});
    const int label = model.add("label", 150, 100, 40, 20, {});
    model.setGroupId(ctrl, 1);
    model.setGroupId(label, 1);
    // …and a lone element further right, which is what we will align TO.
    const int solo = model.add("field", 400, 300, 80, 20, {});

    // Align left against the SOLO element: the group must travel as one, keeping the 50px gap between
    // its own members. Before the fix both members landed on x=400 and the group collapsed.
    surf.selectOnly(solo);
    surf.selectAlso(ctrl);          // pulls in the whole group
    surf.alignSelection(Surface::AlignMode::Left);

    const PanelElement* l = model.get(label);
    const PanelElement* c = model.get(ctrl);
    check(l && c && (c->x - l->x) == 50.0f, "the group keeps its internal layout",
          l && c ? (rectOf(model, label) + "  |  " + rectOf(model, ctrl)) : "missing");
    check(l && l->x == 400.0f, "the group's LEFT EDGE lands on the reference", rectOf(model, label));
    check(c && c->x == 450.0f, "…so the member that was 50px in stays 50px in", rectOf(model, ctrl));
    check(l && c && l->y == 100.0f && c->y == 100.0f, "aligning left leaves y alone",
          rectOf(model, label));

    // The reference is the group's box, not the member you happened to click.
    model.setRect(label, 150, 100, 40, 20);
    model.setRect(ctrl, 200, 100, 80, 20);
    model.setRect(solo, 400, 300, 80, 20);
    surf.selectOnly(ctrl);          // point at the control, which sits 50px inside its group
    surf.selectAlso(solo);
    surf.alignSelection(Surface::AlignMode::Left);
    const PanelElement* s = model.get(solo);
    check(s && s->x == 150.0f, "the solo element aligns to the GROUP'S edge, not the clicked member",
          rectOf(model, solo));

    // Distribute needs three units; two of them being one group must not count as three items.
    model.setRect(label, 0, 0, 40, 20);
    model.setRect(ctrl, 50, 0, 80, 20);
    model.setRect(solo, 400, 0, 80, 20);
    const int third = model.add("field", 800, 0, 80, 20, {});
    surf.selectOnly(ctrl);
    surf.selectAlso(solo);
    surf.selectAlso(third);
    surf.distributeSelection(true);
    const PanelElement* g0 = model.get(label);
    const PanelElement* g1 = model.get(ctrl);
    check(g0 && g1 && (g1->x - g0->x) == 50.0f, "distribute moves the group as one, gap intact",
          g0 && g1 ? (rectOf(model, label) + "  |  " + rectOf(model, ctrl)) : "missing");

    // Make Same Size is a WIDGET operation: with a group in the selection it must do nothing (the menu
    // item is disabled, and the op itself refuses, since a shortcut can reach it too).
    model.setRect(label, 0, 0, 40, 20);
    model.setRect(ctrl, 50, 0, 80, 20);
    model.setRect(solo, 400, 0, 200, 60);
    surf.selectOnly(solo);
    surf.selectAlso(ctrl);                 // pulls the group in
    surf.matchSize(Surface::SizeMode::Both);
    const PanelElement* ml = model.get(label);
    const PanelElement* mc = model.get(ctrl);
    check(ml && ml->w == 40.0f && ml->h == 20.0f && mc && mc->w == 80.0f && mc->h == 20.0f,
          "Make Same Size leaves a grouped selection untouched",
          ml && mc ? (rectOf(model, label) + "  |  " + rectOf(model, ctrl)) : "missing");

    // …and still works normally when nothing is grouped.
    const int plain = model.add("field", 700, 0, 30, 10, {});
    surf.selectOnly(solo);
    surf.selectAlso(plain);
    surf.matchSize(Surface::SizeMode::Both);
    const PanelElement* mp = model.get(plain);
    check(mp && mp->w == 200.0f && mp->h == 60.0f, "…but ungrouped widgets still size to the reference",
          rectOf(model, plain));

    std::printf("\n[arrange] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
