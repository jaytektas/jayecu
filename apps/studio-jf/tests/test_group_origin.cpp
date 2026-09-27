// A GROUP HAS A POSITION, and typing it moves the group.
//
// The properties inspector offers a row only when every selected element agrees on its value, and the
// members of a group agree on their own x/y roughly never — so X and Y simply did not appear for a
// selection, and a group could not be placed by typing at all. Where they DID appear (identical
// coordinates), writing them assigned the same absolute x to every member and collapsed the selection
// into one column: the arrangement destroyed by the box that claimed to describe it.
//
// This is the same rule test_arrange_groups pins for align/distribute — a group moves by ONE delta and
// its members keep their offsets. Pinned here for the inspector:
//   - X/Y of a multi-selection READ the group's bounding-box origin
//   - writing X/Y TRANSLATES every member, preserving relative layout
//   - W/H still assign per member (making a selection one size is deliberate, and encodes no layout)
//
//   cmake --build build --target group_origin_test && ./build/group_origin_test

#include "../src/surface/PropertiesDock.h"
#include "../src/surface/PanelModel.h"
#include "../src/surface/CanvasWidget.h"   // PropertiesDock holds unique_ptr<CanvasWidget>: needs the complete type
#include "../src/model/Cache.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <cmath>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[group-origin] %-58s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  - " + detail).c_str());
    if (!ok) ++fails;
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main() {
    jf::JSceneGraph graph;
    PanelModel model;
    PropertiesDock dock(graph);

    // A group of two: a control at x=200 and its caption 50px to its left, on the same row.
    const int ctrl  = model.add("field", 200, 100, 80, 20, {});
    const int label = model.add("label", 150, 100, 40, 20, {});
    model.setGroupId(ctrl, 1);
    model.setGroupId(label, 1);

    dock.showFor(&model, { ctrl, label }, /*editing=*/true);

    // The inspector's X/Y for the pair is the ORIGIN of what they form (150,100) -- the label's left
    // edge -- not the primary member's own x.
    check(near(dock.commonGeometryValue("x"), 150.f), "X reads the group origin, not the primary member",
          "got " + std::to_string(dock.commonGeometryValue("x")));
    check(near(dock.commonGeometryValue("y"), 100.f), "Y reads the group origin");

    // Move the group to x=400: the shape travels, the 50px internal gap survives.
    dock.setCommonGeometry("x", 400);
    const PanelElement* c = model.get(ctrl);
    const PanelElement* l = model.get(label);
    check(c && l && near(l->x, 400.f) && near(c->x, 450.f),
          "writing X translates the group, members keep their offsets",
          c && l ? ("label=" + std::to_string(l->x) + " ctrl=" + std::to_string(c->x)) : "null");
    check(c && l && near(l->y, 100.f) && near(c->y, 100.f), "an X move leaves Y alone");

    // And down 60. Origin tracks the move, so a second edit is relative to where it now is.
    check(near(dock.commonGeometryValue("x"), 400.f), "origin follows the group after a move");
    dock.setCommonGeometry("y", 160);
    check(c && l && near(l->y, 160.f) && near(c->y, 160.f), "writing Y translates the group");

    // W is unchanged by any of this, and still obeys the older shared-value rule: a row is offered only
    // when every member already agrees. These two are 80 and 40 wide, so there is no W row to write --
    // which is why X/Y needed the origin treatment rather than the same rule.
    check(dock.commonGeometryValue("w") == 0.f && (c->w != l->w),
          "no W row while members differ (the shared-value rule, untouched)");

    // Equalise and re-select: now W appears, and writing it ASSIGNS to each member rather than
    // translating -- one common size is deliberate, and unlike position it encodes no arrangement.
    model.setRect(ctrl,  c->x, c->y, 40, c->h);
    dock.showFor(&model, { ctrl, label }, /*editing=*/true);
    dock.setCommonGeometry("w", 64);
    c = model.get(ctrl); l = model.get(label);
    check(c && l && near(l->w, 64.f) && near(c->w, 64.f), "W assigns per member (not a translate)");
    check(c && l && near(l->x, 400.f) && near(c->x, 450.f), "a W change does not disturb positions");

    std::printf("[group-origin] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
