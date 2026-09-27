// CHANGING A TABLE'S SIZE MOVES NO DATA.
//
// A table is a fixed grid — 32x32x6 of storage with its origin at cell 0 — and "<table>_<axis>_n" says
// only how much of it is in play: how far a lookup may search, not where a row begins. So a cell's
// address is (z * Xalloc * Yalloc + y * Xalloc + x), and changing a size changes a search bound and
// nothing else.
//
// It used to be addressed at the LIVE count on both sides (the studio's comment even said "matches
// firmware"), which made a size a layout: a resize moved every cell's meaning, so the map had to be
// resampled to stay where it was, and a restore that brought back cells without their counts came back
// scrambled. This pins the fixed-stride contract from the studio's side.
//
// Insert and remove are a different thing and still move data: taking a column out shifts the rest left
// along each row, at the physical stride. That is data following its breakpoints, not a re-layout.
//
//   cmake --build build --target axis_stride_test && ./build/axis_stride_test

#include "model/Cache.h"
#include "model/MetaModel.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[stride] %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
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
    const auto ti = c.resolveTable(T);
    const int alloc = c.tiAllocN(ti, 0);
    ck(alloc > cols0, "the allocation is wider than the live grid",
       std::to_string(cols0) + " live of " + std::to_string(alloc));

    for (int r = 0; r < rows0; ++r)
        for (int col = 0; col < cols0; ++col)
            c.setTableCell(T, col, r, 100.0 + r * 100 + col, 0);
    auto cell = [&](int col, int r) { return c.tableCell(T, col, r, 0); };

    // Raw bytes, so the test can say a cell did not move rather than that it still reads the same.
    const std::vector<uint8_t> before = c.configImage();

    // THE CONTRACT: widen the live size. Not one byte of the image may change but the count itself.
    c.setConfigValue(N, cols0 + 4);
    ck(c.liveCols(T) == cols0 + 4, "the live size follows the number", std::to_string(c.liveCols(T)));
    const std::vector<uint8_t>& after = c.configImage();
    int changed = 0, firstDiff = -1;
    for (size_t i = 0; i < before.size() && i < after.size(); ++i)
        if (before[i] != after[i]) { ++changed; if (firstDiff < 0) firstDiff = int(i); }
    ck(changed == 1, "exactly ONE byte changed — the count", std::to_string(changed) + " bytes");
    ck(changed == 1 && firstDiff == m.locate(N).offset, "…and it is the count's own byte");

    bool kept = true;
    for (int r = 0; r < rows0 && kept; ++r)
        for (int col = 0; col < cols0 && kept; ++col)
            if (cell(col, r) != 100.0 + r * 100 + col) kept = false;
    ck(kept, "every cell still reads what it read before the resize");

    // Shrink it again: same rule, and the cells that drop out of play are still there when it grows back.
    c.setConfigValue(N, cols0);
    kept = true;
    for (int r = 0; r < rows0 && kept; ++r)
        for (int col = 0; col < cols0 && kept; ++col)
            if (cell(col, r) != 100.0 + r * 100 + col) kept = false;
    ck(kept, "…and shrinking moves nothing either");

    // The other half of the contract: an INSERT is a data move, on purpose. A column inserted at 0
    // pushes the row right, at the physical stride.
    const double c0r1 = cell(0, 1), c1r1 = cell(1, 1);
    c.tiInsertBin(c.resolveTable(T), 0, 0);
    ck(c.liveCols(T) == cols0 + 1, "insert grows the live size", std::to_string(c.liveCols(T)));
    ck(std::fabs(cell(1, 1) - c0r1) < 1e-6 && std::fabs(cell(2, 1) - c1r1) < 1e-6,
       "…and shifts the row along, which is what insert is FOR",
       std::to_string(cell(1, 1)) + " / " + std::to_string(cell(2, 1)));

    std::printf("[stride] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
