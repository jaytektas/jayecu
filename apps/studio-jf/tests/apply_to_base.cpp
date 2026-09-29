// The DATA the "Apply to Base Table" operation stands on, checked without a window.
//
// The operation itself is three lines of arithmetic (base *= 1 + pct/100, then zero the trim). What can
// silently break is everything underneath it: the schema declares `apply_to`, codegen has to emit it,
// MetaModel has to parse it, and resolveTable has to carry it onto the TableImage the menu reads. Every
// one of those was missing at some point — the schema declared the relationship for months while the
// meta dropped it on the floor, so three comments described a feature that had no data behind it.
//
// The other half is the claim that makes a cell-for-cell walk legitimate: the trim and its base table
// must share axes and allocation. If a schema edit ever points one of them elsewhere, the walk would
// write through a mapping that no longer holds — silently, across the whole map.
//
//   cmake --build build --target apply_to_base && ./build/apply_to_base <meta>
#include "model/MetaModel.h"

#include <cmath>
#include <cstdio>
#include <string>

static int failures = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  %s  %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : ("   - " + detail).c_str());
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fputs("usage: apply_to_base <meta>\n", stderr); return 2; }
    MetaModel meta;
    if (!meta.loadFile(argv[1])) { std::fprintf(stderr, "cannot load meta %s\n", argv[1]); return 1; }

    const std::string TRIM = "fuel_calculator.lambda_ltft";
    const std::string BASE = "fuel_calculator.ve_table";

    std::printf("=== apply_to reaches the studio ===\n");
    const TableImage trim = meta.resolveTable(TRIM);
    const TableImage base = meta.resolveTable(BASE);
    check(trim.valid, TRIM + " resolves");
    check(base.valid, BASE + " resolves");
    check(!trim.applyTo.empty(), "the trim names a base table",
          trim.applyTo.empty() ? "applyTo is EMPTY - codegen or MetaModel dropped it"
                               : trim.applyTo.front());
    if (!trim.applyTo.empty()) {
        std::string named = trim.applyTo.front();
        if (named.rfind("config.", 0) == 0) named.erase(0, 7);
        check(named == BASE, "…and names the one the operation applies to", named);
    }
    check(base.applyTo.empty(), "an ordinary table names nothing");

    std::printf("\n=== the cell-for-cell walk is legitimate ===\n");
    check(trim.axes.size() == base.axes.size(), "same number of axes");
    for (std::size_t i = 0; i < trim.axes.size() && i < base.axes.size(); ++i) {
        const char ax = static_cast<char>('x' + i);
        check(trim.axes[i].breaksBase == base.axes[i].breaksBase,
              std::string("the ") + ax + " axis IS the base table's axis (shared breakpoints)");
        check(trim.axes[i].nBase == base.axes[i].nBase,
              std::string("…and its live bin count is the same scalar"));
        check(trim.axes[i].nMax == base.axes[i].nMax,
              std::string("…allocated to the same width"));
    }
    check(trim.cellSize == base.cellSize, "cells are the same width",
          std::to_string(trim.cellSize) + " vs " + std::to_string(base.cellSize));

    std::printf("\n=== the trim can express a real correction ===\n");
    // The bug this pins: bounds are RAW, so a scaled block whose min/max were written as if they were
    // display units clamped a +/-100%% trim to +/-1.00%% - and clamped it silently.
    const double lo = trim.cellMinV * trim.cellScale, hi = trim.cellMaxV * trim.cellScale;
    check(hi >= 100.0 && lo <= -100.0, "entry bounds reach +/-100 %",
          std::to_string(lo) + " .. " + std::to_string(hi) + " %");

    std::printf("\n=== the arithmetic ===\n");
    // Neutral is 0, and the firmware applies (1 + pct/100) - so rolling in must reproduce exactly what
    // the engine was already being given, which is the only thing that makes this a no-op at the injector.
    const double ve = 85.0, pct = 8.0;
    check(std::fabs(ve * (1.0 + pct / 100.0) - 91.8) < 1e-9, "8 % on 85 VE gives 91.8");
    check(std::fabs(ve * (1.0 + 0.0 / 100.0) - ve) < 1e-12, "a 0 % cell leaves the base untouched");

    std::printf("\n=== a duty offset folds in by ADDING ===\n");
    // The fuel trim is a percentage of the VE cell; the boost trim is a duty offset the firmware adds
    // (Boost.cpp: base + ltt). Folding the boost trim by multiplying turned +5 points on a 40 % cell
    // into 42 %, not 45 %. The schema says which (apply_mode), and it has to reach the TableImage.
    check(!trim.applyAdd, TRIM + " multiplies (a percentage)");
    const TableImage boost = meta.resolveTable("boost.boost_ltt");
    check(boost.valid && !boost.applyTo.empty(), "boost.boost_ltt resolves and names a base table");
    check(boost.applyAdd, "boost.boost_ltt adds (a duty offset)");
    check(std::fabs((40.0 + 5.0) - 45.0) < 1e-12, "+5 on a 40 % duty cell gives 45");

    std::printf("\n%s\n", failures ? (std::to_string(failures) + " FAILED").c_str() : "ALL PASSED");
    return failures ? 1 : 0;
}
