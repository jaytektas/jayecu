// A CALIBRATION LEAVES THE STUDIO AND COMES BACK UNCHANGED.
//
// A sensor's transfer curve is the sensor's own data sheet, not a fact about this engine: the same part
// is the same curve in every car it is fitted to, so it is worth typing once and keeping. That is only
// true if a saved file reproduces the table exactly — a round trip that quietly rounds a breakpoint or
// drops the last point is worse than no export at all, because the loss is invisible until the engine
// runs badly on a curve nobody thinks they changed.
//
//   cmake --build build --target table_file_test && ./build/table_file_test

#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/TableFile.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const char* what, const std::string& note = {}) {
    std::printf("[tablefile] %-56s %s%s\n", what, ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    const std::string cal = "sensors.sensor[clt].cal";
    const TableImage t = c.resolveTable(cal);
    if (!t.valid) { std::printf("[tablefile] no %s in this schema\n", cal.c_str()); return 77; }
    const std::string file = std::string(TMP_DIR) + "/clt_cal.csv";

    // A curve worth keeping: a real-looking thermistor shape, not a straight line — a bug that dropped
    // the interior points would pass on a line.
    const std::vector<double> raw = { 100, 500, 1200, 2000, 2800, 3600 };
    const std::vector<double> deg = { 130, 100, 60, 20, -5, -30 };
    for (int have = c.tiLiveN(t, 0); have < 6; ++have) c.tiInsertBin(t, 0, have);
    ck(c.tiLiveN(t, 0) == 6, "a six-point curve fits the axis");
    c.tiWriteBins(t, 0, raw);
    for (size_t i = 0; i < deg.size(); ++i) c.tiSetCell(t, static_cast<int>(i), 0, 0, deg[i]);

    const tablefile::Result w = tablefile::save(c, cal, file);
    std::printf("[tablefile] %s\n", w.message.c_str());
    ck(w.ok, "a calibration writes to a file");

    // Wreck it — cells zeroed AND the axis shrunk back — then load the file over the wreckage. The
    // shrink is the part that matters: a load that writes six cells into an axis still saying it holds
    // two leaves four of them where nothing reads them, and the curve on screen is still the old one.
    for (size_t i = 0; i < deg.size(); ++i) c.tiSetCell(t, static_cast<int>(i), 0, 0, 0.0);
    for (int have = c.tiLiveN(t, 0); have > 2; --have) c.tiRemoveBin(t, 0, have - 1);
    c.tiWriteBins(t, 0, std::vector<double>(static_cast<size_t>(c.tiLiveN(t, 0)), 0.0));
    ck(std::fabs(c.tiCell(t, 0, 0, 0)) < 0.001 && c.tiLiveN(t, 0) == 2, "…and the table is then wiped");

    const tablefile::Result r = tablefile::load(c, cal, file);
    std::printf("[tablefile] %s\n", r.message.c_str());
    ck(r.ok, "the file loads back");

    ck(c.tiLiveN(t, 0) == 6, "the axis grew back to the file's six points");
    bool cells = true, bins = true;
    const std::vector<double> got = c.tiBins(t, 0);
    for (size_t i = 0; i < deg.size(); ++i) {
        if (std::fabs(c.tiCell(t, static_cast<int>(i), 0, 0) - deg[i]) > 0.01) cells = false;
        if (i < got.size() && std::fabs(got[i] - raw[i]) > 0.5) bins = false;
    }
    ck(bins, "every raw breakpoint came back");
    ck(cells, "every reading came back");

    // A file bigger than the table can hold is REFUSED, not truncated: a calibration cut short is a
    // calibration that lies, and it would read as a working curve.
    {
        FILE* f = std::fopen((std::string(TMP_DIR) + "/too_big.csv").c_str(), "w");
        std::fprintf(f, "# jayecu table 1\n,");
        const int n = c.tiAllocN(t, 0) + 4;
        for (int i = 0; i < n; ++i) std::fprintf(f, "%d%s", i * 10, i + 1 < n ? "," : "\n");
        std::fprintf(f, ",");
        for (int i = 0; i < n; ++i) std::fprintf(f, "%d%s", i, i + 1 < n ? "," : "\n");
        std::fclose(f);
        const tablefile::Result big = tablefile::load(c, cal, std::string(TMP_DIR) + "/too_big.csv");
        std::printf("[tablefile] oversize: %s\n", big.message.c_str());
        ck(!big.ok, "a file with more points than the table holds is refused");
        ck(std::fabs(c.tiCell(t, 2, 0, 0) - deg[2]) < 0.01, "…and the table it refused is untouched");
    }

    std::printf("[tablefile] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
