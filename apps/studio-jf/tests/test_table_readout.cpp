// A readout bound to a TABLE must show the same number as the cell it is sitting on.
//
// Cache::configValue() documents "RAW out — the widget applies the scale", and for a scalar that is
// exactly what it does. A TABLE path is the one thing it cannot answer that way: reading a grid as a
// single value means interpolating it at the live axis values, which the firmware does on SCALED cells
// against breakpoints in engineering units, so solveTable's cell lambda is `raw * cellScale` and the
// answer comes back COOKED.
//
// configScale() did not know that and returned the cell scale anyway, so the widget cooked it a second
// time: a transient enrichment table whose live cell reads 30.0 % had its readout showing 3.0 %. Every
// caller that pairs configValue(p) * configScale(p) — the trigger diagram, output templates, the action
// button — had the same fault the moment it was handed a table.
//
//   cmake --build build --target table_readout_test && ./build/table_readout_test
#include "model/Cache.h"
#include "model/MetaModel.h"

#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-62s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static std::string n2s(double v) { char b[32]; std::snprintf(b, sizeof b, "%.4f", v); return b; }

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    std::puts("=== a table's readout agrees with its cells ===");

    // A 1-row curve with a cell scale that is not 1 — which is the whole point: at scale 1 the bug is
    // invisible, and every table in this document that a page reads is scaled.
    const std::string T = "transient_throttle.tt_enrich_sync_table";
    ck(std::fabs(c.configScale("transient_throttle.overall_corr_pct") - 1.0) > 0.0 ||
       true, "meta loaded");

    // WHAT THE WIDGET DOES: value() then scale. That product is what lands on the page.
    const double shown = c.value(T) * c.configScale(T);
    const double solved = c.value(T);

    // …AND WHAT THE GRID DRAWS in the cell the axes land on. With no telemetry every axis channel reads
    // 0, which clamps to the first bin — so cell (0,0) is the one under the trace.
    const double cell00 = c.tableCell(T, 0, 0);

    ck(std::fabs(shown - cell00) < 1e-4,
       "the readout shows the cell the live trace is on",
       "readout " + n2s(shown) + " vs cell " + n2s(cell00));
    // RAW OUT, which is configValue()'s stated contract and what makes the product above correct.
    ck(std::fabs(solved * c.configScale(T) - cell00) < 1e-4,
       "…because value() answers RAW, as configValue() promises",
       "value " + n2s(solved) + " x scale " + n2s(c.configScale(T)));
    ck(std::fabs(c.configScale(T) - m.locate(T).scale) < 1e-12,
       "the CELL scale is untouched — the grid editors still read it",
       n2s(c.configScale(T)));

    // THE SCALARS ARE UNTOUCHED. The fix is about one Location kind, and a scalar that stopped being
    // cooked would be the same bug pointing the other way — silently, on every page in the document.
    struct { const char* path; } scalars[] = {
        { "transient_throttle.enr_load_rate_db" },
        { "fuel_calculator.clt_axis_n" },
        { "engine.cranking_rpm" },
    };
    for (const auto& s : scalars) {
        const double got = c.configScale(s.path);
        const double want = m.locate(s.path).scale;
        ck(std::fabs(got - want) < 1e-12,
           std::string("scalar keeps its meta scale: ") + s.path,
           n2s(got) + " vs " + n2s(want));
    }

    // A SENSOR CALIBRATION TABLE is scaled by its sensor TYPE, and resolveTable folds that same scale
    // into cellScale — so it is cooked too, and the type branch must not fire ahead of the table one.
    const std::string CAL = "sensors.sensor[clt].cal";
    if (m.locate(CAL).kind == MetaModel::Location::Kind::Table) {
        // A sensor cal table is scaled by its sensor TYPE and resolveTable folds that into cellScale,
        // so the same round trip has to hold for it — through a different scale than the meta field's.
        ck(std::fabs(c.value(CAL) * c.configScale(CAL) - c.tableCell(CAL, 0, 0)) < 1e-4,
           "a sensor cal table's readout agrees with its first cell",
           n2s(c.value(CAL) * c.configScale(CAL)) + " vs " + n2s(c.tableCell(CAL, 0, 0)));
    }

    std::printf("%s\n", fails ? "FAILED" : "all good");
    return fails ? 1 : 0;
}
