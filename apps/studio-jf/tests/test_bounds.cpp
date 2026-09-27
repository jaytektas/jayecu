// Can a write put a value somewhere it cannot legally go?
//
// For most of this codebase's life, yes. There were six ways to write a config value and one of them
// clamped; the one every scalar widget used (setConfigValue) resolved the field, looked up its bounds,
// and threw them away — `double mn, mx;` existed only to satisfy a signature. And "no bounds declared"
// was encoded as min == max, which 196 of the 1086 config fields were in, so for those there was
// nothing to enforce even in principle.
//
// The rule now is one sentence: a value is clamped to what the schema declares, or — if it declares
// nothing — to what its datatype can store. Which makes clamping the same thing as saturation, and that
// matters: encodeRaw casts without saturating, so before this an out-of-range write WRAPPED. Writing
// 999999 to an S16 landed on -13617. Nothing reported it, and the number that came back was a plausible
// one, which is the worst way for a tune to be wrong.
//
// Everything here goes through the public door a widget uses, not through a test-only shortcut — the
// point is that the door itself is safe, not that a safe path exists.
//
//   cmake --build build --target bounds_test && ./build/bounds_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/MathEvaluator.h"
#include "model/SigilResolvers.h"
#include "surface/CanvasWidget.h"
#include "surface/WidgetRegistry.h"
#include <j/core/SceneGraph.h>
#include <memory>

#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-64s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// Write through the door every scalar widget uses, then read the bytes back. Raw both ways — the Cache
// speaks the unit the bytes are in.
static double roundTrip(Cache& c, const MetaModel& m, const std::string& path, double raw) {
    c.setConfigValue(path, raw);
    return c.readRaw(m.locate(path)).v;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());
    static ConfigSigilResolver configSigils;      // else every control expression evaluates to 0
    MathEvaluator::instance().registerResolver(&configSigils);
    jf::JSceneGraph graph;

    std::puts("=== bounds: can a write land somewhere illegal? ===");

    // 1 — DECLARED bounds hold. cell_len is 0..96 because the cell array it indexes is 96 long; a longer
    //     pattern is not a big number, it is a read past the end of the run.
    //
    //     96, not 32: the firmware widened MAX_PATTERN_TEETH and the schema followed, and this test did
    //     not. It then failed for three days saying the field declared 0..96 when it should say 0..32 —
    //     which was true, and backwards. A bound is only worth asserting against the thing that owns it.
    {
        const std::string p = "trigger.streams[0].cell_len";
        const auto L = m.locate(p);
        ck(L.valid() && L.minV == 0 && L.maxV == 96, "cell_len declares 0..96",
           "min " + std::to_string(L.minV) + " max " + std::to_string(L.maxV));
        ck(roundTrip(c, m, p, 199) == 96, "…199 clamps to 96, not stored");
        ck(roundTrip(c, m, p, -5) == 0,  "…-5 clamps to 0");
        ck(roundTrip(c, m, p, 12) == 12, "…and a legal value is untouched");
    }

    // 2 — UNDECLARED bounds fall to the datatype, and the write SATURATES rather than wrapping. This is
    //     the case that used to be silent: 300 into a U08 is 44 if you let the cast do it.
    {
        const std::string p = "trigger.streams[0].capture_index";     // U08, declares nothing
        const auto L = m.locate(p);
        double lo = 0, hi = 0;
        Cache::rawBounds(L, lo, hi);
        ck(L.maxV <= L.minV, "capture_index declares no bounds of its own");
        ck(lo == 0 && hi == 255, "…so its range is U08's own, 0..255",
           std::to_string(lo) + ".." + std::to_string(hi));
        const double got = roundTrip(c, m, p, 300);
        ck(got == 255, "…300 saturates to 255", "got " + std::to_string(got) + " (44 = the old wrap)");
        ck(roundTrip(c, m, p, -1) == 0, "…and -1 saturates to 0 rather than wrapping to 255");
    }

    // 3 — A SENTINEL that is declared in range survives, because it is an ordinary value.
    //
    //     This is the case that made the whole design argument: sensors.sensor[].source declares
    //     min: -1, where -1 means "no source assigned". Clamping does not need to know that; -1 is
    //     simply inside the declared range. A sentinel only needs special handling when the schema
    //     fails to admit it exists.
    {
        const std::string p = "sensors.sensor[0].source";
        const auto L = m.locate(p);
        ck(L.valid() && L.minV == -1, "sensor.source declares min -1 — 'unassigned' is a real value",
           "min " + std::to_string(L.minV));
        ck(roundTrip(c, m, p, -1) == -1, "…so writing -1 stores -1");
        ck(roundTrip(c, m, p, 200) == L.maxV, "…while 200 still clamps to its declared max");
    }

    // 4 — A packed BIT GROUP is bounded by its own width, not by the width of the word it shares.
    //     Writing past it would otherwise spill into the flags beside it.
    {
        std::string bg;
        for (const auto& [path, f] : m.config())
            if (!f.bitGroups.empty() && f.bitGroups.front().width() >= 1) {
                bg = path + "." + f.bitGroups.front().name;
                break;
            }
        if (bg.empty()) {
            std::puts("  (no bit groups in this meta — nothing to check)");
        } else {
            const auto L = m.locate(bg);
            double lo = 0, hi = 0;
            Cache::rawBounds(L, lo, hi);
            const double want = std::pow(2.0, L.bits.hi - L.bits.lo + 1) - 1.0;
            ck(L.valid() && L.bits.packed(), "a bit group resolves to a bit RANGE: " + bg);
            ck(hi == want, "…bounded by its own width, not the word's",
               "hi " + std::to_string(hi) + " want " + std::to_string(want));
            ck(roundTrip(c, m, bg, 9999) == want, "…and an over-large write cannot spill into its neighbours");
        }
    }

    // 5 — The neighbours are the point of a read-modify-write. A packed write must leave the rest of the
    //     word exactly as it was; getting this wrong is how a one-bit edit silently clears three others.
    {
        std::string parent, g0, g1;
        for (const auto& [path, f] : m.config())
            if (f.bitGroups.size() >= 2) { parent = path; g0 = f.bitGroups[0].name; g1 = f.bitGroups[1].name; break; }
        if (parent.empty()) {
            std::puts("  (no scalar carries two bit groups — nothing to check)");
        } else {
            const auto La = m.locate(parent + "." + g0), Lb = m.locate(parent + "." + g1);
            double alo = 0, ahi = 0; Cache::rawBounds(La, alo, ahi);
            c.setConfigValue(parent + "." + g1, 1);
            const double before = c.readRaw(Lb).v;
            c.setConfigValue(parent + "." + g0, ahi);          // fill the neighbour to its max
            ck(c.readRaw(Lb).v == before, "writing one bit group leaves the one beside it alone",
               parent + ": " + g1 + " was " + std::to_string(before) + ", now " + std::to_string(c.readRaw(Lb).v));
        }
    }

    // 6 — Nothing declares a range its storage cannot hold. A schema that promised a value the bytes
    //     cannot keep would clamp to the promise and then wrap on the way in.
    {
        int overwide = 0;
        std::string first;
        for (const auto& [path, f] : m.config()) {
            const auto L = m.locate(path);
            if (!L.valid() || L.maxV <= L.minV) continue;
            MetaModel::Location bare = L; bare.minV = bare.maxV = 0.0;   // ask for the datatype's own range
            double lo = 0, hi = 0;
            Cache::rawBounds(bare, lo, hi);
            if (L.minV < lo || L.maxV > hi) { if (!overwide++) first = path; }
        }
        ck(overwide == 0, "no scalar declares a range wider than its datatype",
           std::to_string(overwide) + " do, first: " + first);
    }

    // ---- LAYER 2: the control's own Min/Max ------------------------------------------------------
    // A control may NARROW what its field allows, never widen it — the storage decides what fits and the
    // schema decides what is meaningful, and neither is a layout's business. This is the layer for the
    // limits a fixed number cannot state because they depend on other settings: WIDTH max cannot go
    // below WIDTH min, a GAP cell index is a byte where a SEQUENCE step is an angle.
    {
        PanelElement el;
        el.type = "configedit";
        el.props["signalName"] = "trigger.streams[0].cell_len";     // field range 0..96
        auto probe = [&](const std::string& mn, const std::string& mx) {
            el.props["minExpr"] = mn; el.props["maxExpr"] = mx;
            std::unique_ptr<CanvasWidget> w = makeWidgetInstance(el.type, graph);
            w->setSource(el); w->setData(el); w->setCache(&c);
            const MetaModel::Location L = w->boundLoc();
            double lo = 0, hi = 0; Cache::rawBounds(L, lo, hi);
            return std::make_pair(lo, hi);
        };
        auto [lo0, hi0] = probe("", "");
        ck(lo0 == 0 && hi0 == 96, "with no control bounds, the field's own stand",
           std::to_string(lo0) + ".." + std::to_string(hi0));
        auto [lo1, hi1] = probe("4", "10");
        ck(lo1 == 4 && hi1 == 10, "a control narrows the field",
           std::to_string(lo1) + ".." + std::to_string(hi1));
        auto [lo2, hi2] = probe("-100", "9999");
        ck(lo2 == 0 && hi2 == 96, "…and CANNOT widen it, either end",
           std::to_string(lo2) + ".." + std::to_string(hi2));

        // The narrowing must reach the BYTE. Clamping the spin box alone leaves every other route in —
        // a paste, a bus write, a box ranged before the setting its Max depends on changed.
        el.props["minExpr"] = ""; el.props["maxExpr"] = "10";
        std::unique_ptr<CanvasWidget> w = makeWidgetInstance(el.type, graph);
        w->setSource(el); w->setData(el); w->setCache(&c);
        c.writeRaw(w->boundLoc(), Raw{30}, "trigger.streams[0].cell_len");
        ck(c.readRaw(m.locate("trigger.streams[0].cell_len")).v == 10,
           "a write past the control's Max clamps in the CACHE, not just the box",
           std::to_string(c.readRaw(m.locate("trigger.streams[0].cell_len")).v));

        // An expression, not a number — the case the whole layer exists for.
        c.setConfigValue("trigger.streams[0].width_min", 40);
        el.props["signalName"] = "trigger.streams[0].width_max";
        el.props["minExpr"] = "trigger.streams[0].width_min";
        el.props["maxExpr"] = "";
        std::unique_ptr<CanvasWidget> w2 = makeWidgetInstance(el.type, graph);
        w2->setSource(el); w2->setData(el); w2->setCache(&c);
        c.writeRaw(w2->boundLoc(), Raw{5}, "trigger.streams[0].width_max");
        ck(c.readRaw(m.locate("trigger.streams[0].width_max")).v == 40,
           "a WIDTH band cannot invert: max below min clamps up to min",
           std::to_string(c.readRaw(m.locate("trigger.streams[0].width_max")).v));
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "a write cannot land outside what the field may hold",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
