// THE TABLE WIDGET READS WHAT THE CACHE WROTE.
//
// Cells are laid out at the ALLOCATION's stride — a table is a fixed grid and <axis>_n only bounds the
// search. The studio had THREE copies of that rule: Cache::tiCellOffset, TableGeom::cellOffset (the one
// the render and every cell op use) and TableWidget::tableCellOffset (the clipboard ops). Fixing the
// first and not the others is exactly the shape of the bug this pins: the widget drew a VE table that
// walked ten cells further off its row every line — the bottom row read correctly and the rest slid into
// the padding — while everything else in the studio read it correctly.
//
// So this writes through the model and reads back through the WIDGET's own geometry. Nothing here
// re-derives an address: if the two disagree by so much as one cell the values stop matching.
//
//   cmake --build build --target table_addressing_test && ./build/table_addressing_test

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
    std::printf("[addr] %-60s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
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
    const int cols = c.liveCols(T), rows = c.liveRows(T);
    const TableImage ti = c.resolveTable(T);
    ck(c.tiAllocN(ti, 0) > cols, "the VE table is live-smaller than its allocation",
       std::to_string(cols) + " of " + std::to_string(c.tiAllocN(ti, 0)));

    // A distinct, non-zero value in every live cell: a cell read from the padding shows up as a zero,
    // and a cell read from the wrong row shows up as a duplicate.
    std::set<double> written;
    for (int r = 0; r < rows; ++r)
        for (int col = 0; col < cols; ++col) {
            const double v = 100.0 + r * 100.0 + col;
            c.setTableCell(T, col, r, v, 0);
            written.insert(v);
        }

    // …now read every cell back the way the widget draws it.
    for (const char* mode : { "1", "5" }) {          // transposed and not: the mapping differs, the ADDRESS must not
        PanelElement el;
        el.id = 1; el.type = "table";
        el.props["signalName"] = T;
        el.props["axisMode"] = mode;
        const TableWidget::TableGeom g =
            TableWidget::tableGeom(el, jf::JRect{ 0.f, 0.f, 900.f, 600.f }, c);
        ck(g.rows * g.cols == rows * cols, std::string("axisMode ") + mode + ": the grid is the live grid",
           std::to_string(g.rows) + "x" + std::to_string(g.cols));

        std::set<double> read;
        int zeros = 0;
        for (int rr = 0; rr < g.rows; ++rr)
            for (int cc = 0; cc < g.cols; ++cc) {
                const double v = TableWidget::cellValue(el, g.t, g.cellOffset(rr, cc));
                if (std::fabs(v) < 1e-9) ++zeros;
                read.insert(v);
            }
        ck(zeros == 0, std::string("axisMode ") + mode + ": no cell reads out of the padding",
           std::to_string(zeros) + " zero cells");
        ck(read == written, std::string("axisMode ") + mode + ": every live cell is read exactly once",
           std::to_string(read.size()) + " distinct of " + std::to_string(written.size()));
    }

    // ---- A TABLE SCALES WITH ITS VIEW ---------------------------------------------------------------
    // Every cell in the grid is sized from TEXT: the row height is a line height, the column width is the
    // width of a formatted value. So the view scale has to reach the font, or the grid draws at 1:1
    // whatever the surface is doing — which is what it did. Scaling a page up handed the table more room
    // and it answered with MORE cells of the same size, so the "static surface size" preference resized
    // the frame around a fuel map and nothing inside it. Drop the zoom back out of tableGeom's lh and
    // this goes red.
    {
        PanelElement el;
        el.id = 1; el.type = "table";
        el.props["signalName"] = T;
        el.props["hSectionMode"] = "3";        // Fixed: cells take their natural size, not a stretched one
        el.props["vSectionMode"] = "3";
        const jf::JRect r{ 0.f, 0.f, 900.f, 600.f };
        const TableWidget::TableGeom g1 = TableWidget::tableGeom(el, r, c, {}, 1.f);
        const TableWidget::TableGeom g2 = TableWidget::tableGeom(el, r, c, {}, 2.f);
        ck(g1.cw > 0.f && g1.ch > 0.f, "a table has a cell size at all",
           std::to_string(g1.cw) + "x" + std::to_string(g1.ch));
        ck(std::fabs(g2.ch - 2.f * g1.ch) < 1.5f, "twice the view scale is twice the row height",
           std::to_string(g1.ch) + " -> " + std::to_string(g2.ch));
        ck(g2.cw > g1.cw * 1.6f, "…and a wider column with it",
           std::to_string(g1.cw) + " -> " + std::to_string(g2.cw));
        ck(g2.rows == g1.rows && g2.cols == g1.cols,
           "…over the same grid: a scaled table shows bigger cells, not more of them",
           std::to_string(g2.rows) + "x" + std::to_string(g2.cols));
        // The default is 1:1, so the hit-test and every existing caller are untouched.
        const TableWidget::TableGeom g0 = TableWidget::tableGeom(el, r, c);
        ck(std::fabs(g0.ch - g1.ch) < 0.01f, "…and an unstated scale is 1:1");
    }

    // ---- AND A DENSITY OF ITS OWN ------------------------------------------------------------------
    // A table is read as a field of numbers, not as a label, so it carries a cell size independent of
    // the interface scale: a 22x23 ignition map wants to be compact enough to see as a shape while the
    // rest of the UI stays comfortable. It MULTIPLIES the view scale rather than replacing it, so the
    // two compose instead of fighting — and an untouched table is exactly what it always was.
    {
        PanelElement el;
        el.id = 1; el.type = "table";
        el.props["signalName"] = T;
        el.props["hSectionMode"] = "3";
        el.props["vSectionMode"] = "3";
        const jf::JRect r{ 0.f, 0.f, 900.f, 600.f };
        const TableWidget::TableGeom base = TableWidget::tableGeom(el, r, c, {}, 1.f);

        el.props["cellScale"] = "1";                       // 70 %
        const TableWidget::TableGeom tight = TableWidget::tableGeom(el, r, c, {}, 1.f);
        ck(tight.ch < base.ch, "a tighter cell size makes shorter rows",
           std::to_string(base.ch) + " -> " + std::to_string(tight.ch));

        el.props["cellScale"] = "6";                       // 200 %
        const TableWidget::TableGeom big = TableWidget::tableGeom(el, r, c, {}, 1.f);
        ck(big.ch > base.ch * 1.6f, "…and a looser one makes taller rows",
           std::to_string(base.ch) + " -> " + std::to_string(big.ch));
        ck(big.rows == base.rows && big.cols == base.cols,
           "…over the same grid, either way");

        // COMPOSES WITH THE VIEW SCALE rather than overriding it: 200 % cells at 2x view is 4x the row.
        const TableWidget::TableGeom bigZoom = TableWidget::tableGeom(el, r, c, {}, 2.f);
        ck(std::fabs(bigZoom.ch - 2.f * big.ch) < 2.f, "…and the two multiply",
           std::to_string(big.ch) + " -> " + std::to_string(bigZoom.ch));

        el.props["cellScale"] = "3";                       // an explicit 100 %
        const TableWidget::TableGeom same = TableWidget::tableGeom(el, r, c, {}, 1.f);
        ck(std::fabs(same.ch - base.ch) < 0.01f, "…and 100 % is what an untouched table already was");
    }

    std::printf("[addr] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
