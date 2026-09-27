// A TABLE IS ITS CELLS *AND* ITS AXES — restoring must bring back both.
//
// The report: restore to the connect point, and the table comes back looking corrupted.
//
// It restored the CELL region and nothing else. A config table's breakpoints and its live bin counts sit
// in their own regions of the image, nowhere near the cells, so they kept whatever they had become since
// the baseline was taken. Restore after resizing or re-laying an axis and the baseline's numbers came
// back underneath TODAY's grid: right cells, wrong stride, wrong breakpoints — which reads as corruption
// because that is what it is.
//
// The element-table branch beside it had always restored the lot; this pins that a config table does too.
//
//   cmake --build build --target table_restore_test && ./build/table_restore_test

#include "model/Cache.h"
#include "model/MetaModel.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[restore] %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
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
    const std::string N = "fuel_calculator.ve_table_x_axis_n";

    const int cols0 = c.liveCols(T), rows0 = c.liveRows(T);
    for (int r = 0; r < rows0; ++r)
        for (int col = 0; col < cols0; ++col)
            c.setTableCell(T, col, r, 100.0 + r * 100 + col, 0);
    const auto ti0 = c.resolveTable(T);
    const std::vector<double> bins0 = c.tiBins(ti0, 0);
    ck(int(bins0.size()) == cols0, "baseline: the axis has one breakpoint per live column",
       std::to_string(bins0.size()) + " vs " + std::to_string(cols0));

    // THIS is the baseline — the image as it stood when we connected.
    c.setBaseline(c.configImage());
    ck(c.hasBaseline(), "a connect-point baseline exists");

    // Now do what a tuner does between connecting and changing their mind: move the axis and the cells.
    c.setConfigValue(N, cols0 - 3);                     // fewer columns (routed through the resize)
    const auto ti1 = c.resolveTable(T);
    std::vector<double> moved = c.tiBins(ti1, 0);
    for (size_t i = 0; i < moved.size(); ++i) moved[i] = c.snapBin(ti1, 0, 500.0 + 250.0 * i);
    c.beginEdit(); c.tiWriteBins(ti1, 0, moved); c.endEdit("test");
    for (int r = 0; r < c.liveRows(T); ++r)
        for (int col = 0; col < c.liveCols(T); ++col)
            c.setTableCell(T, col, r, 7.0, 0);          // and scribble over every cell
    ck(c.liveCols(T) == cols0 - 3, "the axis really did change", std::to_string(c.liveCols(T)));

    // Restore to the connect point.
    c.restoreFromBaseline(T);

    ck(c.liveCols(T) == cols0, "restore brings the LIVE BIN COUNT back",
       std::to_string(c.liveCols(T)) + " (was " + std::to_string(cols0) + ")");
    const auto ti2 = c.resolveTable(T);
    const std::vector<double> bins2 = c.tiBins(ti2, 0);
    bool binsOk = bins2.size() == bins0.size();
    for (size_t i = 0; binsOk && i < bins0.size(); ++i)
        if (std::fabs(bins2[i] - bins0[i]) > 1e-6) binsOk = false;
    ck(binsOk, "…and the breakpoints with it",
       binsOk ? "" : (std::to_string(bins2.empty() ? 0.0 : bins2[0]) + " vs "
                      + std::to_string(bins0.empty() ? 0.0 : bins0[0])));

    bool cellsOk = true;
    for (int r = 0; r < rows0 && cellsOk; ++r)
        for (int col = 0; col < cols0 && cellsOk; ++col)
            if (std::fabs(c.tableCell(T, col, r, 0) - (100.0 + r * 100 + col)) > 1e-6) cellsOk = false;
    ck(cellsOk, "…so every cell is back at the bin it was tuned at",
       cellsOk ? "" : "cell (2,1) reads " + std::to_string(c.tableCell(T, 2, 1, 0)));

    std::printf("[restore] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
