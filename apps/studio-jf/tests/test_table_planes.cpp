// A CELL OP EDITS THE PLANE YOU ARE LOOKING AT.
//
// TableWidget::tableCellOffset took no z, so every operation built on it — copy, paste, interpolate,
// smooth, "copy table as text" — addressed PLANE 0 whatever plane was on screen. On a 3D table (the VE
// map carries six ethanol planes) that means the ops worked on a different set of cells than the ones
// under the cursor, and nothing looked wrong: the plane you were watching simply did not change.
//
// Pinned by address rather than by driving the menus: for every plane, the offset a cell op computes must
// be the offset the model uses for that same cell on that same plane — and planes must not overlap.
//
//   cmake --build build --target table_planes_test && ./build/table_planes_test

#include "model/Cache.h"
#include "model/MetaModel.h"
#include "surface/PanelModel.h"
#include "surface/widgets/TableWidget.h"

#include <j/core/SceneGraph.h>

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[planes] %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    const std::string T = "fuel_calculator.ve_table";
    // The VE table's third axis is OPTIONAL and ships disabled (a disabled axis collapses to one plane),
    // so switch it on: this test is about the planes, and a 2D table cannot show the bug.
    c.setConfigValue("fuel_calculator.ve_table_z_en", 1);
    c.setConfigValue("fuel_calculator.ve_table_z_axis_n", 4);
    const TableImage t = c.resolveTable(T);
    const int depth = c.liveDepth(T);
    ck(depth > 1, "the VE table really has more than one plane", std::to_string(depth) + " planes");
    if (depth < 2) { std::printf("[planes] SKIPPED — no 3D table to test\n"); return 0; }

    PanelElement el;
    el.id = 7; el.uid = "test-planes-uid"; el.type = "table";
    el.props["signalName"] = T;
    const TableWidget::TableDims d = TableWidget::tableDims(c, t, TableWidget::tableTransposed(el));

    // Every plane gets its own values, so a read from the wrong one is unmistakable.
    for (int z = 0; z < depth; ++z)
        for (int r = 0; r < c.liveRows(T); ++r)
            for (int col = 0; col < c.liveCols(T); ++col)
                c.setTableCell(T, col, r, 1000.0 * (z + 1) + r * 10 + col, z);

    std::set<int> offsets;
    bool matches = true, distinct = true;
    for (int z = 0; z < depth; ++z) {
        TableWidget::tableSetPlane(el.uid, z);                 // what the plane stepper does
        ck(TableWidget::tableCurrentPlane(el.uid) == z, "the element reports the plane it was set to");
        for (int rr = 0; rr < d.rows; ++rr)
            for (int cc = 0; cc < d.cols; ++cc) {
                const int off = TableWidget::tableCellOffset(t, d, rr, cc,
                                                            TableWidget::tableCurrentPlane(el.uid));
                if (!offsets.insert(off).second) distinct = false;     // two planes sharing a cell
                // …and it must be the value the MODEL holds for that cell on that plane.
                const int vIdx = d.rows - 1 - rr, hIdx = cc;
                const int i0 = d.transpose ? hIdx : vIdx, i1 = d.transpose ? vIdx : hIdx;
                if (std::fabs(TableWidget::cellValue(el, t, off) - c.tableCell(T, i0, i1, z)) > 1e-6)
                    matches = false;
            }
    }
    ck(matches, "a cell op reads the plane the element is showing");
    ck(distinct, "…and no two planes address the same cell",
       std::to_string(offsets.size()) + " offsets for "
       + std::to_string(depth * d.rows * d.cols) + " cells");

    std::printf("[planes] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
