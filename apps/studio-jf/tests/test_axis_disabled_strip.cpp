// A table only labels the directions it HAS.
//
// An optional axis that is switched off collapses to one bin — tiLiveN says 1 — but it still has a
// stored breakpoint sitting in the config. The grid asked for that breakpoint anyway, so a table whose
// optional axis was off drew a row-header strip labelling an axis nothing indexes by, and a permanently
// blank corner cell above it (a header column exists only to be labelled; the X header row has nothing
// to put over it). On the bench ECU, etb[0].ff_table with its clt axis disabled showed a "60.0" row
// header — a coolant bin in play nowhere — and a blank cell above it, which is what it was reported as.
//
// The name bar had the same fault from the other end: it named the disabled direction "— (°C)".
//
//   cmake --build build --target axis_disabled_strip_test && ./build/axis_disabled_strip_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "surface/PanelModel.h"
#include "surface/widgets/TableWidget.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-68s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    // The table it was found on: X = plate %, Y = clt, and Y is OPTIONAL (it has an enable).
    const std::string PATH = "electronic_throttle.etb[0].ff_table";
    const jf::JRect r{ 0.f, 0.f, 812.f, 112.f };

    // WHICH SCREEN DIRECTION a storage axis lands on is the "axisMode" combo's business, so the
    // orientation is pinned rather than left to the global default — otherwise this test asserts the
    // preference as much as the rule. "1" = combo 0, transposed (the dashboard element this was found
    // on); "5" = combo 4 "left & top", the default. The rule must hold either way round.
    PanelElement tr;  tr.type  = "table"; tr.props["signalName"] = PATH; tr.props["axisMode"] = "1";
    PanelElement lt;  lt.type  = "table"; lt.props["signalName"] = PATH; lt.props["axisMode"] = "5";

    std::puts("=== a switched-off axis draws no strip and names nothing ===");

    const TableImage t0 = c.resolveTable(PATH);
    ck(t0.valid && t0.axes.size() == 2, "the ff_table has two declared axes",
       std::to_string(t0.axes.size()));

    // --- Y ON: both strips, both names, either way round ------------------------------------------
    c.tiSetEnabled(t0, 1, true);
    for (const PanelElement* el : { &tr, &lt }) {
        const std::string which = el->props.at("axisMode") == "1" ? "transposed" : "left&top";
        const TableWidget::TableGeom g = TableWidget::tableGeom(*el, r, c);
        ck(!g.xb.empty() && !g.yb.empty(),
           "with the optional axis ON both strips have breakpoints (" + which + ")",
           std::to_string(g.xb.size()) + " x " + std::to_string(g.yb.size()));
        ck(!g.vName.empty() && !g.hName.empty(), "…and both directions are named (" + which + ")");
        ck(g.hdrW > 0.f, "…and a header column is reserved (" + which + ")");
    }

    // --- Y OFF: the disabled axis loses its strip, whichever direction it is on -------------------
    c.tiSetEnabled(t0, 1, false);
    {
        const TableImage t = c.resolveTable(PATH);
        ck(!c.tiEnabled(t, 1), "the optional axis is now off");
        ck(c.tiLiveN(t, 1) == 1, "…and collapses to a single bin, which is why it still HAD one to draw",
           std::to_string(c.tiLiveN(t, 1)));
        ck(!c.tiBins(t, 1).empty(), "…the stored breakpoint is still there in the config");
        const int liveX = c.tiLiveN(t, 0);

        // Transposed: storage X runs across, the dead clt axis is the vertical one — the dashboard case.
        {
            const TableWidget::TableGeom g = TableWidget::tableGeom(tr, r, c);
            ck(int(g.xb.size()) == liveX, "transposed: the live axis keeps its strip across the top",
               std::to_string(g.xb.size()));
            ck(g.yb.empty(), "transposed: the switched-off axis draws NO strip",
               std::to_string(g.yb.size()));
            ck(g.hdrW == 0.f, "…so no header column, and no blank corner above it",
               std::to_string(g.hdrW));
            ck(g.vName.empty(), "…and the name bar does not name it", g.vName);
            ck(!g.hName.empty(), "…while the live direction is still named", g.hName);
            ck(g.rows == 1 && g.cols == liveX, "…one row of cells, one per live bin",
               std::to_string(g.rows) + "x" + std::to_string(g.cols));
        }
        // Left & top: the same table the other way up — storage X down the left, dead axis across.
        {
            const TableWidget::TableGeom g = TableWidget::tableGeom(lt, r, c);
            ck(int(g.yb.size()) == liveX, "left&top: the live axis keeps its strip down the left",
               std::to_string(g.yb.size()));
            ck(g.xb.empty(), "left&top: the switched-off axis draws NO strip",
               std::to_string(g.xb.size()));
            ck(g.hName.empty(), "…and is not named", g.hName);
            ck(!g.vName.empty(), "…while the live direction is", g.vName);
            ck(g.cols == 1 && g.rows == liveX, "…one column of cells, one per live bin",
               std::to_string(g.rows) + "x" + std::to_string(g.cols));
        }
    }

    c.tiSetEnabled(t0, 1, true);   // leave the image as we found it

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "only the directions the table has are drawn and named",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
