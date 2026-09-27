// Where does an inserted bin GO?
//
// An axis is a sorted ladder and every lookup assumes it. The Insert in the axis-setup dialog chose the
// slot from the CURSOR (or the end, with nothing selected) and then wrote the typed value into it — so
// typing 3 on an axis running 0..100 appended it after 100 and left the ladder unsorted. That is not a
// cosmetic problem: tiInsertBin creates and interpolates a CELL at the slot the bin went to, so from the
// first out-of-order bin onward every cell reads against the wrong breakpoint. It happened on the real
// etb[0].ff_table, and nothing in the strip said a word, because the legality colouring checked range
// and precision and never order.
//
// The rule these pin: the position comes from the VALUE, after snapping, and the axis stays sorted.
//
//   cmake --build build --target axis_insert_test && ./build/axis_insert_test
#include "model/Cache.h"
#include "model/MetaModel.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static std::string join(const std::vector<double>& v) {
    std::string s;
    for (double d : v) { char b[32]; std::snprintf(b, sizeof(b), "%g ", d); s += b; }
    return s;
}
static bool sorted(const std::vector<double>& v) {
    if (v.size() < 2) return true;
    const bool desc = v.back() < v.front();
    for (std::size_t i = 1; i < v.size(); ++i)
        if (desc ? (v[i] >= v[i - 1]) : (v[i] <= v[i - 1])) return false;
    return true;
}

// The dialog's rule, verbatim in behaviour: snap, refuse a duplicate, insert at the ordered slot.
// (AxisSetupDialog::orderedSlot + _insertAt — a header-only dialog that cannot be linked headless,
// so the rule is exercised through the same Cache calls it makes, in the same order.)
static int orderedSlot(const std::vector<double>& bins, double v) {
    if (bins.size() < 2) return static_cast<int>(bins.size());
    const bool desc = bins.back() < bins.front();
    for (std::size_t i = 1; i < bins.size(); ++i)
        if (desc ? (bins[i] > bins[i - 1]) : (bins[i] < bins[i - 1]))
            return static_cast<int>(bins.size());
    for (std::size_t i = 0; i < bins.size(); ++i)
        if (desc ? (v > bins[i]) : (v < bins[i])) return static_cast<int>(i);
    return static_cast<int>(bins.size());
}
static bool insertValue(Cache& c, const std::string& path, int axis, double v) {
    const TableImage t0 = c.resolveTable(path);
    const double sv = c.snapBin(t0, axis, v);
    const std::vector<double> before = c.tiBins(t0, axis);
    for (double b : before) if (std::fabs(b - sv) < 1e-9) return false;
    const int at = orderedSlot(before, sv);
    if (!c.tiInsertBin(t0, axis, at)) return false;
    const TableImage t = c.resolveTable(path);
    std::vector<double> b = c.tiBins(t, axis);
    if (at >= 0 && at < static_cast<int>(b.size())) { b[at] = sv; c.tiWriteBins(t, axis, b); }
    return true;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);

    // The very table it happened on: a 1-D spring feed-forward curve, X in % of plate travel.
    const std::string PATH = "electronic_throttle.etb[0].ff_table";
    std::puts("=== an inserted bin lands where its VALUE belongs ===");

    c.setConfigImage(m.defaultImage());
    {
        const TableImage t = c.resolveTable(PATH);
        const std::vector<double> bins = c.tiBins(t, 0);
        ck(bins.size() >= 4 && sorted(bins), "the shipped axis is a sorted ladder to begin with", join(bins));
    }

    // Mark every cell with its own index so a cell that ends up beside the wrong bin is visible.
    {
        const TableImage t = c.resolveTable(PATH);
        for (int i = 0; i < c.tiLiveN(t, 0); ++i) c.tiSetCell(t, i, 0, 0, i * 1.0);
    }

    // 3 belongs between 0 and 5 — near the START, which is the case the old code got most wrong
    // because with nothing selected it appended.
    {
        const std::vector<double> before = c.tiBins(c.resolveTable(PATH), 0);
        ck(insertValue(c, PATH, 0, 3.0), "inserting 3.0 succeeds");
        const TableImage t = c.resolveTable(PATH);
        const std::vector<double> after = c.tiBins(t, 0);
        ck(after.size() == before.size() + 1, "the axis grew by exactly one bin",
           std::to_string(after.size()));
        ck(sorted(after), "…and is STILL sorted (the whole bug: 3 used to land after 100)", join(after));
        int at = -1;
        for (std::size_t i = 0; i < after.size(); ++i) if (std::fabs(after[i] - 3.0) < 1e-9) at = int(i);
        ck(at == 1, "…in its ordered slot, not at the cursor and not at the end", std::to_string(at));

        // The cells were marked 0,1,2,… — so every ORIGINAL cell must still sit against its original
        // bin, and only the new slot may hold something interpolated.
        bool paired = true;
        for (std::size_t i = 0; i < after.size(); ++i) {
            if (int(i) == at) continue;
            const double want = (int(i) < at) ? double(i) : double(i - 1);
            if (std::fabs(c.tiCell(t, int(i), 0, 0) - want) > 0.051) paired = false;
        }
        ck(paired, "…and every cell moved WITH its own bin");
        const double nc = c.tiCell(t, at, 0, 0);
        ck(nc > 0.0 && nc < 1.0, "…the new cell interpolates between its neighbours",
           std::to_string(nc));
    }

    // The end and the start are still reachable — an insert past either edge is an append/prepend.
    {
        const std::vector<double> before = c.tiBins(c.resolveTable(PATH), 0);
        insertValue(c, PATH, 0, before.back() - 0.5);       // just inside the top
        const std::vector<double> after = c.tiBins(c.resolveTable(PATH), 0);
        ck(sorted(after), "a value just below the top slots in below it", join(after));
    }

    // A duplicate is not an insert: two identical breakpoints make the cell behind them unreachable.
    {
        const std::vector<double> before = c.tiBins(c.resolveTable(PATH), 0);
        const bool did = insertValue(c, PATH, 0, before[2]);
        const std::vector<double> after = c.tiBins(c.resolveTable(PATH), 0);
        ck(!did, "inserting a value the axis already has is refused");
        ck(after.size() == before.size(), "…and the table is untouched", std::to_string(after.size()));
    }

    // Snapping happens BEFORE the slot is chosen: a value past the ceiling belongs where it LANDS.
    {
        c.setConfigImage(m.defaultImage());
        const TableImage t0 = c.resolveTable(PATH);
        const Cache::ChannelDomain d = c.axisDomain(t0, 0);
        if (d.hasRange) {
            insertValue(c, PATH, 0, d.hi + 500.0);          // way over the top
            const std::vector<double> after = c.tiBins(c.resolveTable(PATH), 0);
            ck(sorted(after) || after.size() == c.tiBins(t0, 0).size(),
               "an over-range value cannot land out of order (it clamps, then slots)", join(after));
        } else {
            ck(true, "(axis declares no range — clamp case not applicable)");
        }
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "an insert keeps the ladder sorted and the cells paired",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
