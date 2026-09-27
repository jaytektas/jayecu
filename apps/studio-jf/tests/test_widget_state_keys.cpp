// PER-WIDGET STATE IS KEYED BY UID, NOT BY ELEMENT ID.
//
// An element id is unique within ONE model — a page, a panel, a surface. The maps holding per-widget
// state are GLOBAL: table cursors, table scroll, array cursors, curve selection/drag, peak hold, live-
// graph history, the open caption. Keying those by id meant two widgets with the same id on different
// pages wore each other's state, and every authored page numbers its widgets from 1.
//
// The report was a table: pick a column in one and the focused column moved in another, across dozens of
// pages. The cursor was never per-table at all.
//
// This drives the maps directly, because that is where the bug lived — one entry per widget, and two
// widgets that merely share an id must not collide.
//
//   cmake --build build --target widget_state_keys_test && ./build/widget_state_keys_test

#include "surface/widgets/TableWidget.h"
#include "surface/widgets/LabelWidget.h"
#include "surface/widgets/CurveWidget.h"
#include "surface/widgets/Array1DWidget.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[keys] %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    // Two widgets, same element id (page A's table 1 and page B's table 1), different uids — which is
    // exactly what a document full of authored pages looks like.
    const std::string a = "11111111-aaaa-4aaa-8aaa-111111111111";
    const std::string b = "22222222-bbbb-4bbb-8bbb-222222222222";

    TableWidget::tableCursors()[a].col = 7;
    TableWidget::tableCursors()[a].row = 3;
    TableWidget::tableCursors()[b].col = 0;
    ck(TableWidget::tableCursors()[a].col == 7 && TableWidget::tableCursors()[b].col == 0,
       "two tables keep their own cursor",
       std::to_string(TableWidget::tableCursors()[b].col));

    TableWidget::tableSetPlane(a, 3);
    TableWidget::tableSetPlane(b, 1);
    ck(TableWidget::tableCurrentPlane(a) == 3 && TableWidget::tableCurrentPlane(b) == 1,
       "…and its own Z plane",
       std::to_string(TableWidget::tableCurrentPlane(a)) + "/" + std::to_string(TableWidget::tableCurrentPlane(b)));

    TableWidget::tableScrolls()[a].y = -40.f;
    ck(TableWidget::tableScrolls()[b].y == 0.f, "…and its own scroll offset");

    CurveWidget::setCurveSelIndex(a, 5);
    CurveWidget::setCurveDragIndex(a, 2);
    ck(CurveWidget::curveSelIndex(b) == -1 && CurveWidget::curveDragIndex(b) == -1,
       "two curves keep their own selection and drag");

    Array1DWidget::array1dCursors()[a].sel = 9;
    ck(Array1DWidget::array1dCursors()[b].sel == 0, "two arrays keep their own cursor");

    LabelWidget::labelEdits()[a].active = true;
    ck(!LabelWidget::labelEdits()[b].active, "an open caption belongs to ONE label");

    // The keys are strings for a reason: the state must survive an instance being rebuilt (which is why
    // it is not held on the widget), while still naming exactly one widget in the document.
    ck(TableWidget::tableCursors().count(a) && TableWidget::tableCursors().count(b),
       "both entries exist side by side", std::to_string(TableWidget::tableCursors().size()) + " entries");

    std::printf("[keys] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
