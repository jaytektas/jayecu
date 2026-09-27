// EVERY table in the definition: does the number a page shows match the cells in the grid?
//
// A readout bound to a table reads Cache::value() and the widget multiplies by configScale(). That
// product is what lands on the page, and it must equal what the grid editor draws in the cell the live
// trace is sitting on — the two are the same number or one of them is lying.
//
// They disagreed by a factor of the cell scale on all 176 table readouts in the document, because
// solveTable cooked its answer and configValue's contract says raw. This is the net under that.
//
// EACH TABLE IS FILLED WITH A KNOWN, NON-ZERO VALUE FIRST. Read against the default tune, 113 of the
// 173 tables are all-equal grids and 60-odd of those are all zero — and zero times any scale is zero,
// so a sweep over the shipped values cannot tell a correct scale from a wrong one. The value written
// is a scale step off the bottom of the table's own range, so it is representable exactly and no
// clamp can move it.
//
//   cmake --build build --target table_scale_sweep_test && ./build/table_scale_sweep_test
#include "model/Cache.h"
#include "model/MetaModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m); c.setConfigImage(m.defaultImage());
    // THE LEARNED TABLES LIVE OUTSIDE THE CONFIG IMAGE — the knock noise floors and the VVT long-term
    // trim are in RAM-backed segments the cache allocates separately. Without this their buffers do not
    // exist, every write is dropped on the floor and every read comes back 0: 35 tables "failed" with
    // "wrote -63, grid 0, page 0", which is the harness reporting its own omission as a product fault.
    c.initSegments();

    std::vector<std::string> paths;
    for (const auto& kv : m.configTables()) paths.push_back(kv.first);
    std::sort(paths.begin(), paths.end());

    int checked = 0, bad = 0, skipped = 0;
    for (const std::string& p : paths) {
        const MetaModel::Location L = m.locate(p);
        if (L.kind != MetaModel::Location::Kind::Table) { ++skipped; continue; }
        const int cols = c.liveCols(p), rows = c.liveRows(p), dep = c.liveDepth(p);
        if (cols < 1 || rows < 1 || dep < 1) { ++skipped; continue; }

        // A VALUE THE SCALE CANNOT HIDE. Not zero (0 x anything is 0, so a wrong scale still matches)
        // and not 1 (1 x anything looks right at a glance in a log). Thirty-seven storage counts up
        // from the table's floor: representable exactly, and inside the range so nothing clamps it.
        //
        // THE BOUNDS ARE IN COUNTS, THE CELL IS IN ENGINEERING. A table's meta min/max are the RAW
        // limits (specific gravity is 500..2000 at scale 0.001, i.e. 0.5..2.0), so a target built by
        // adding scale steps to `min` mixes the two: it asked for 500.037 on a table that stops at 2.0,
        // and eight tables reported a "mismatch" that was only ever the harness clamping its own
        // nonsense. Convert the floor first, then step.
        const double sc   = c.configScale(p);
        const double step = (sc > 0.0) ? sc : 1.0;
        const double loEng = c.configMin(p) * (sc > 0.0 ? sc : 1.0);
        const double hiEng = c.configMax(p) * (sc > 0.0 ? sc : 1.0);
        if (!(hiEng > loEng)) { ++skipped; continue; }
        double want = loEng + 37.0 * step;
        if (want > hiEng) want = loEng + std::floor((hiEng - loEng) / step / 2.0) * step;
        if (std::fabs(want) < step * 0.5) want = loEng + step;        // never land on zero
        if (std::fabs(want) < step * 0.5) { ++skipped; continue; }

        for (int z = 0; z < dep; ++z)
            for (int r = 0; r < rows; ++r)
                for (int q = 0; q < cols; ++q) c.setTableCell(p, q, r, want, z);

        // A FLAT GRID READS AS ITS OWN VALUE WHEREVER THE TRACE LANDS, which is what makes this exact
        // rather than a range check: no axis, no channel and no interpolation can change the answer.
        const double cell  = c.tableCell(p, 0, 0, 0);
        const double shown = c.value(p) * c.configScale(p);
        ++checked;
        const double tol = std::max(step * 0.51, std::fabs(want) * 1e-6);
        if (std::fabs(shown - want) > tol || std::fabs(cell - want) > tol) {
            ++bad;
            std::printf("  MISMATCH  %-50s wrote %.4f  grid %.4f  page %.4f  (scale %g)\n",
                        p.c_str(), want, cell, shown, sc);
        }
    }
    std::printf("\n%d tables checked, %d skipped (no range to write into), %d MISMATCH\n",
                checked, skipped, bad);
    std::printf("%s\n", bad ? "FAILED" : "all good");
    return bad ? 1 : 0;
}
