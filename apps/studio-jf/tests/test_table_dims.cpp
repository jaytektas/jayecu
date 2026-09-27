// The LIVE shape of every table: how many bins each axis is laid out at, and whether an optional axis
// is switched on.
//
// These are two questions, and they were sharing one return value. axisEffLen() answered "how many
// bins" but returned 0 for a disabled optional axis, so liveCols() could clamp it back to 1 while
// axisActive() read the 0 as "off" — and the per-element-table branch went through tiLiveN(), which
// returns 1 for a disabled axis and cannot express "off" at all. tiLiveN is the stride (always >= 1)
// and tiEnabled is the switch; axisEffLen and axisActive are gone.
//
// A table's live dimensions are not in the meta (they come from the <axis>_n scalars in the config
// image), so the locations golden cannot pin them. This does: every table instance the meta declares,
// against the DEFAULT config image, so the numbers are a property of the layout and not of a tune.
//
//   cmake --build build --target table_dims_test && ./build/table_dims_test
//   ./build/table_dims_test --emit tests/golden/table_dims.txt
#include "model/Cache.h"
#include "model/MetaModel.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::string emitTo;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--emit" && i + 1 < argc) emitTo = argv[++i];

    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    std::vector<std::string> lines;
    for (const std::string& path : m.tableInstancePaths()) {
        const TableImage t = c.resolveTable(path);
        char buf[320];
        // …and the CELL PRECISION, because a table that reports none is drawn with none: the target
        // lambda map showed a grid of 1s over a heat map that was plainly not flat, since only a sensor
        // calibration ever carried its digits through resolveTable. A number the schema states and the
        // screen loses is exactly what a golden is for.
        std::snprintf(buf, sizeof(buf),
                      "%-64s cols=%-4d rows=%-4d depth=%-4d  on=[%d%d%d]  digits=%-3d units=%s",
                      path.c_str(), c.liveCols(path), c.liveRows(path), c.liveDepth(path),
                      c.tiEnabled(t, 0) ? 1 : 0, c.tiEnabled(t, 1) ? 1 : 0, c.tiEnabled(t, 2) ? 1 : 0,
                      t.cellDigits, t.cellUnits.c_str());
        lines.push_back(buf);
    }
    std::sort(lines.begin(), lines.end());
    std::ostringstream dump;
    for (const auto& l : lines) dump << l << "\n";

    if (!emitTo.empty()) {
        std::ofstream f(emitTo, std::ios::binary);
        if (!f) { std::fprintf(stderr, "cannot write %s\n", emitTo.c_str()); return 1; }
        f << dump.str();
        std::printf("wrote %s (%zu tables)\n", emitTo.c_str(), lines.size());
        return 0;
    }

    std::ifstream gf(GOLDEN, std::ios::binary);
    if (!gf) { std::fprintf(stderr, "no golden at %s — emit one first\n", GOLDEN); return 1; }
    std::stringstream want; want << gf.rdbuf();
    if (want.str() == dump.str()) {
        std::printf("=== %zu tables, live shape identical to the golden ===\n", lines.size());
        return 0;
    }
    std::vector<std::string> a, b, diff;
    for (std::string l; std::getline(want, l);) a.push_back(l);
    { std::istringstream d(dump.str()); for (std::string l; std::getline(d, l);) b.push_back(l); }
    std::set_symmetric_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(diff));
    std::printf("=== %zu tables, %zu line(s) DIFFER ===\n", lines.size(), diff.size());
    for (size_t i = 0; i < diff.size() && i < 40; ++i) std::printf("  %s\n", diff[i].c_str());
    return 1;
}
