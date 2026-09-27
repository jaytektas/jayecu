// The wiring row's TRIP POINTS: shown for a switch on an analog pin, and for nothing else.
//
// A switch has no calibration curve — the firmware decodes it to 0/1 against two voltages
// (Stages.h, decode_switch_thresh), and those two numbers are the first two breakpoints of the
// slot's cal axis. This checks the widget agrees with the firmware about WHEN that applies, and
// that the boxes read and write the breakpoints the firmware will actually read.
//
//   cmake --build build --target wiring_thresh_test && ./build/wiring_thresh_test
#include "../src/surface/widgets/WiringWidget.h"
#include "../src/surface/widgets/CurveWidget.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"
#include "../src/model/TableImage.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// The boxes are the widget's own children, so "is the row showing" is asked the way a user asks it:
// how many controls the scene graph got.
static int boxCount(jf::JSceneGraph&, WiringWidget& w) { return w.threshBoxCountForTest(); }

int main(int argc, char** argv) {
    MetaModel meta;
    const char* cands[] = { argc > 1 ? argv[1] : nullptr,
#ifdef REAL_META
                            REAL_META,
#endif
                            "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::puts("[wiring-thresh] (no meta found — skipped)"); return 0; }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    std::vector<uint8_t> image = meta.defaultImage();
    if (image.empty()) image.assign((size_t)meta.configSize(), 0);
    C.setConfigImage(image);

    const std::string slot = "sensors.sensor[start_sw]";
    if (!meta.locate(slot + ".interface").valid()) {
        std::puts("[wiring-thresh] (meta has no start_sw slot — skipped)");
        return 0;
    }

    jf::JSceneGraph graph;
    jf::JPrimitiveBuffer buf;
    const jf::JRect r{ 0.f, 0.f, 320.f, 80.f };

    std::puts("=== Wiring row: the two trip points ===");

    // A switch on its own SWITCH interface reads a digital pin — no thresholds, nothing to show.
    {
        PanelElement el;
        el.type = "wiring";
        el.props["signalName"] = "[#" + slot + ".source]";
        WiringWidget w(graph);
        w.bind(&el, &C);          // exactly what the Surface does to a dropped widget
        C.setConfigValue(slot + ".interface", 4.0);      // IFSEL_SWITCH
        w.render(buf, r, C);
        ck(boxCount(graph, w) == 0, "a switch on a digital pin shows no trip points");
    }

    // …and on an ANALOG interface it does, reading the cal axis's first two breakpoints.
    {
        PanelElement el;
        el.type = "wiring";
        el.props["signalName"] = "[#" + slot + ".source]";
        WiringWidget w(graph);
        w.bind(&el, &C);          // exactly what the Surface does to a dropped widget
        C.setConfigValue(slot + ".interface", 0.0);      // IFSEL_ANALOG_VOLTAGE
        const TableImage ti = C.resolveTable(slot + ".cal");
        ck(ti.valid && !ti.axes.empty() && ti.axes[0].nMax >= 2,
           "the calibration resolves to a table with a raw axis");
        const TableImage::Axis& ax = ti.axes[0];
        C.writeAt(ax.breaksBase,                  ax.breakType, 1.0, 800.0);    // off
        C.writeAt(ax.breaksBase + ax.breakSize,   ax.breakType, 1.0, 1600.0);   // on
        w.render(buf, r, C);
        ck(boxCount(graph, w) == 2, "a switch on an analog pin shows both trip points");

        // ASCENDING AXIS: [0] is the low point, so it is OFF and [1] is ON — the same rule the
        // firmware's builder relies on to tell them apart.
        ck(w.threshRawForTest(false) == 800.0,  "…the OFF box reads breakpoint 0",
           std::to_string(w.threshRawForTest(false)));
        ck(w.threshRawForTest(true)  == 1600.0, "…and the ON box reads breakpoint 1",
           std::to_string(w.threshRawForTest(true)));
    }

    // A sensor that is NOT a switch keeps the plain wiring row, however it is wired.
    {
        const std::string clt = "sensors.sensor[clt]";
        PanelElement el;
        el.type = "wiring";
        el.props["signalName"] = "[#" + clt + ".source]";
        WiringWidget w(graph);
        w.bind(&el, &C);          // exactly what the Surface does to a dropped widget
        C.setConfigValue(clt + ".interface", 0.0);       // analog, but a temperature
        w.render(buf, r, C);
        ck(boxCount(graph, w) == 0, "an analog sensor that is not a switch shows none");
    }

    // …and the calibration itself is HIDDEN on a switch, whichever way it is read. Nothing decodes a
    // switch through a curve, so a curve editor over those bytes offers a shape that has no effect —
    // and on the analog interface it would be drawing over the two trip points.
    {
        auto calVisible = [&](const std::string& slotPath) {
            PanelElement el;
            el.type = "curve";
            el.props["signalName"] = "[#" + slotPath + ".cal]";
            CurveWidget w(graph);
            w.bind(&el, &C);
            return w.visibleNow();
        };
        C.setConfigValue(slot + ".interface", 4.0);      // switch on a digital pin
        ck(!calVisible(slot), "a switch's calibration is hidden on the switch interface");
        C.setConfigValue(slot + ".interface", 0.0);      // …and on an analog one, where the bytes are the trip points
        ck(!calVisible(slot), "…and on the analog interface too");
        ck(calVisible("sensors.sensor[clt]"), "a real sensor's calibration still shows");
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "All wiring trip-point tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
