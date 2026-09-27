// The MULTI-POSITION SWITCH's band editor (BandsWidget): shown for that calibration and no other, the curve
// hidden in its place, Capture centring a band on the reading and keeping clear of its neighbours and of
// the 0 V floor — and, the one that matters, that what it stores is a calibration the FIRMWARE accepts:
// the stored element is run through firmware's own multi_switch_bands(), so the editor and the ECU cannot
// disagree about what a valid set of bands is.
//
//   cmake --build build --target bands_widget_test && ./build/bands_widget_test
#include "../src/surface/widgets/BandsWidget.h"
#include "../src/surface/widgets/CurveWidget.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <cstring>
#include <string>

// The firmware's own band rules (tests/firmware_bands.cpp — a separate unit, see there).
int firmware_multi_switch_bands(const unsigned char* element, unsigned size);

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    MetaModel meta;
    const char* cands[] = { argc > 1 ? argv[1] : nullptr,
#ifdef REAL_META
                            REAL_META,
#endif
                            "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::puts("[bands] no meta found"); return 1; }   // a test with no input FAILS

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    std::vector<uint8_t> image = meta.defaultImage();
    if (image.empty()) image.assign(static_cast<size_t>(meta.configSize()), 0);
    C.setConfigImage(image);

    int multi = -1;
    for (size_t i = 0; i < meta.sensorTypes().size(); ++i)
        if (meta.sensorTypes()[i].id == "multi_switch") multi = static_cast<int>(i);
    if (multi < 0) { std::puts("[bands] the meta has no multi_switch type"); return 1; }

    const std::string base = "sensors.sensor[aux_1]";
    C.setConfigValue(base + ".type", multi);
    C.setConfigValue(base + ".interface", 0.0);   // analog voltage
    C.setConfigValue(base + ".source", 4.0);      // AV5

    jf::JSceneGraph graph;
    jf::JPrimitiveBuffer buf;
    const jf::JRect r{ 0.f, 0.f, 600.f, 360.f };
    PanelElement el;
    el.type = "bands";
    el.props["signalName"] = base + ".cal";
    BandsWidget w(graph);
    w.bind(&el, &C);

    // The firmware's verdict on what is stored right now: the element's bytes, as the ECU would hold them.
    auto firmwareBands = [&]() -> int {
        const MetaModel::Location L = meta.locate(base + ".enabled");   // the element's first field
        if (!L.valid()) return -1;
        const auto& img = C.configImage();
        return firmware_multi_switch_bands(img.data() + L.offset, static_cast<unsigned>(img.size() - L.offset));
    };

    std::puts("=== Multi-position switch: the band editor ===");

    // ---- it replaces the curve, and only for this type ----
    {
        PanelElement ce;
        ce.type = "curve";
        ce.props["signalName"] = base + ".cal";
        CurveWidget curve(graph);
        curve.bind(&ce, &C);
        ck(w.visibleNow(), "the band editor shows for a multi-position switch's calibration");
        ck(!curve.visibleNow(), "…and the curve over the same bytes is hidden");
        PanelElement be;
        be.type = "bands";
        be.props["signalName"] = "sensors.sensor[clt].cal";
        BandsWidget other(graph);
        other.bind(&be, &C);
        ck(!other.visibleNow(), "a coolant sensor's calibration shows no band editor");
    }

    // ---- the default curve left behind by the previous type is not a valid set of bands ----
    // A two-point 0-5 V ramp is not a set of positions, but it breaks no STRUCTURAL rule: it is one band
    // spanning the range. The editor used to refuse it and the firmware used to answer 0, both because of
    // a fixed 0 V floor that no longer exists (78bbd30 — a stalk that grounds the line is a real position,
    // and where a fault starts is the user's own Detect Raw Low). What still matters is that the two agree.
    ck(w.problemForTest().empty(), "a leftover 0-5 V curve breaks no structural rule", w.problemForTest());
    ck(firmwareBands() == 1, "…and the firmware reads it as the one band it is",
       std::to_string(firmwareBands()));

    // ---- capture: re-centre band 1 on the pin, then add a second position ----
    const std::string e1 = w.captureForTest(0, 3200.0);
    ck(e1.empty(), "Capture on row 1 centres it on the reading", e1);
    const std::string e2 = w.captureForTest(-1, 1200.0);
    ck(e2.empty(), "+ Band at a second voltage adds a position", e2);
    auto b = w.bandsForTest();
    ck(b.size() == 2, "two bands stored", std::to_string(b.size()));
    if (b.size() == 2) {
        ck(b[0].lo < 1200 && b[0].hi > 1200 && b[1].lo < 3200 && b[1].hi > 3200,
           "stored in ascending order, each around its reading");
        ck(b[0].hi - b[0].lo >= 150 && b[0].hi - b[0].lo <= 170, "a free band is +-100 mV wide (about 164 counts)",
           std::to_string(b[0].hi - b[0].lo));
        ck(b[1].pos == 0 && b[0].pos == 1, "the recaptured band kept its position; the new one took the next free");
    }
    ck(w.problemForTest().empty(), "the editor has nothing to complain about", w.problemForTest());
    ck(firmwareBands() == 2, "…and the FIRMWARE accepts exactly those two bands",
       std::to_string(firmwareBands()));

    // ---- what Capture refuses ----
    ck(!w.captureForTest(-1, 1250.0).empty(), "a reading inside an existing band is not a new position");
    ck(!w.captureForTest(-1, 1300.0).empty(), "a reading too close to a band to keep a gap is refused");
    ck(w.bandsForTest().size() == 2, "…and neither refusal changed what is stored",
       std::to_string(w.bandsForTest().size()));

    // ---- THE FLOOR IS THE USER'S, NOT A CONSTANT ----
    // A reading near 0 V used to be refused outright. It is a position now: a button that GROUNDS the
    // line is how most cruise stalks send MAIN (Toyota's has no resistor at all), and a short to ground
    // is the same ambiguity every other sensor has at 0 V. Where a fault starts is Detect Raw Low, which
    // the user arms — so the refusal is still there, it just answers to the tune instead of to a constant.
    ck(w.captureForTest(-1, 100.0).empty(), "with Detect Raw Low off, a reading near 0 V is a position");
    ck(w.bandsForTest().size() == 3 && firmwareBands() == 3, "…stored, and the firmware takes it",
       std::to_string(w.bandsForTest().size()) + "/" + std::to_string(firmwareBands()));
    ck(w.removeForTest(0), "…removed again (it is the lowest band)");

    C.setConfigValue(base + ".diag_raw_min", 200.0);
    C.setConfigValue(base + ".diag_enable", 1.0);            // DIAG_RAW_MIN
    const std::string refused = w.captureForTest(-1, 100.0);
    ck(!refused.empty(), "…and with it ARMED the same reading is a fault, not a button", refused);
    ck(refused.find("Raw Low") != std::string::npos, "…named as the threshold the user armed", refused);
    C.setConfigValue(base + ".diag_enable", 0.0);
    ck(w.bandsForTest().size() == 2, "…leaving the two real positions",
       std::to_string(w.bandsForTest().size()));

    // ---- a band squeezed between neighbours is narrowed to keep the gap on both sides ----
    ck(w.captureForTest(-1, 2200.0).empty(), "a third position between the two is added");
    b = w.bandsForTest();
    ck(b.size() == 3 && firmwareBands() == 3, "three bands, and the firmware takes all three",
       std::to_string(b.size()) + "/" + std::to_string(firmwareBands()));

    // ---- the editor names the same faults the firmware refuses ----
    {
        const MetaModel::Location L = meta.locate(base + ".enabled");
        const TableImage ti = C.resolveTable(base + ".cal");
        const auto& ax = ti.axes[0];
        const double saved = C.readAt(ax.breaksBase + 2 * ax.breakSize, ax.breakType, 1.0);
        C.writeAt(ax.breaksBase + 2 * ax.breakSize, ax.breakType, 1.0, b[0].hi);   // band 2 starts where band 1 ends
        ck(!w.problemForTest().empty() && firmwareBands() == 0,
           "touching bands: the editor says so and the firmware refuses", w.problemForTest());
        C.writeAt(ax.breaksBase + 2 * ax.breakSize, ax.breakType, 1.0, saved);
        ck(w.problemForTest().empty() && firmwareBands() == 3, "…restored, both accept it again");
        (void)L;
    }

    // ---- remove, down to the last band ----
    ck(w.removeForTest(1), "a band can be removed");
    ck(w.bandsForTest().size() == 2 && firmwareBands() == 2, "…leaving two the firmware still accepts");
    ck(w.removeForTest(0), "…and another");
    ck(!w.removeForTest(0), "the last band cannot be removed (the calibration needs a point pair)");

    // ---- one row of controls per band ----
    w.render(buf, r, C);
    ck(w.rowControlsForTest() == static_cast<int>(w.bandsForTest().size()), "one row of controls per band");

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All band editor tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
