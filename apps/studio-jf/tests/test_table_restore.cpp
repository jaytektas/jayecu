// Restore Defaults / Restore to Connect Point must cover the SAME regions — the only difference is the
// source image. The bug: "Restore Defaults" went through restoreTableFrom (config-table only), so on an
// element table / curve (ETB feed-forward, sensor calibration) it silently did nothing, while "Restore to
// Connect Point" restored it. Proof: restore-from-defaults and restore-from-an-identical-baseline must
// land on byte-identical images for BOTH a full config table and a curve.
//   cmake --build build --target table_restore_test && ./build/table_restore_test <meta>
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// Restore `path` from defaults, and separately from a baseline == the default image; the two must agree,
// and must actually change a scribbled-over image (proving the region was restored, not left untouched).
static void checkRestore(Cache& C, const std::string& kind, const std::string& path,
                         const std::vector<uint8_t>& def) {
    const std::vector<uint8_t> scribble(def.size(), 0xAB);

    C.setConfigImage(scribble);
    C.restoreTableDefaults(path);
    const std::vector<uint8_t> fromDefaults = C.configImage();

    C.setConfigImage(scribble);
    C.setBaseline(def);
    C.restoreFromBaseline(path);
    const std::vector<uint8_t> fromBaseline = C.configImage();

    ck(fromDefaults != scribble, kind + ": Restore Defaults changed the image (not a no-op)");
    ck(fromDefaults == fromBaseline,
       kind + ": Restore Defaults covers the same region as Connect Point");
}

int main(int argc, char** argv) {
    MetaModel meta;
    const char* mc[] = { argc > 1 ? argv[1] : nullptr, "../../shared/tuneit-meta.json",
                         "../../../shared/tuneit-meta.json" };
    bool ok = false;
    for (const char* c : mc) if (c && meta.loadFile(c)) { ok = true; break; }
    if (!ok) { std::puts("[table-restore] (no meta — skipped)"); return 0; }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    const std::vector<uint8_t> def = meta.defaultImage();
    if (def.empty()) { std::puts("[table-restore] (no default image — skipped)"); return 0; }

    std::puts("=== Restore Defaults == Restore Connect Point (same source) ===");

    // A full config table (2D cells; axes are separate shared fields, unchanged by restore).
    const std::string tbl = "fuel_calculator.stage1_dead_time_table";
    ck(C.isTable(tbl) && meta.configTables().count(tbl), "found a full config table", tbl);
    if (C.isTable(tbl)) checkRestore(C, "config table", tbl, def);

    // An element table / curve — the case Restore Defaults used to skip.
    const char* curveCands[] = { "electronic_throttle.etb[0].ff_table",
                                 "electronic_throttle.etb[1].ff_table" };
    std::string curve;
    for (const char* p : curveCands)
        if (C.isTable(p) && !meta.configTables().count(p)) { curve = p; break; }
    ck(!curve.empty(), "found an element-table (curve) binding", curve);
    if (!curve.empty()) checkRestore(C, "curve", curve, def);

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "clean", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
