// A BAND BELONGS TO THE CHANNEL, NOT TO THE CONTROL SHOWING IT.
//
// Oil pressure is low below the same number on a watch list, a readout and a gauge. Setting that three
// times is three chances to disagree with yourself, so the threshold is stored once against the channel
// and every control that resolves a colour asks for it.
//
// The order matters as much as the mechanism: a per-widget rule is the SPECIFIC statement about one
// control and must still win, the channel's expressions come next, and the plain warning numbers last.
//
//   cmake --build build --target channel_prefs_test && ./build/channel_prefs_test

#include "model/Cache.h"
#include "model/ChannelPrefs.h"
#include "model/MetaModel.h"
#include "surface/PanelModel.h"
#include "surface/CanvasWidget.h"
#include "model/MathEvaluator.h"
#include "model/SigilResolvers.h"

#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const char* what, const std::string& note = {}) {
    std::printf("[prefs] %-60s %s%s\n", what, ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static std::string hex(const uint8_t* c) {
    char b[16]; std::snprintf(b, sizeof b, "#%02x%02x%02x", c[0], c[1], c[2]); return b;
}

int main() {
    // ---- THE RULES OF AN UNSET THRESHOLD ----------------------------------------------------------
    // Blank is not zero. A channel with no band must paint nothing: the commonest way to break this is
    // to default the numbers to 0 and turn every positive reading into an alarm.
    {
        ChannelPref p;
        ck(p.empty(), "a fresh preference is empty");
        ck(p.severity(-1000.0) == 0 && p.severity(1000.0) == 0, "…and bands nothing at all");
        p.warnHi = 100.0;
        ck(p.severity(99.0) == 0 && p.severity(101.0) == 1, "a warning high bands above it only");
        p.alarmHi = 120.0;
        ck(p.severity(101.0) == 1 && p.severity(121.0) == 2,
           "an alarm outranks the warning it is also past");
        p.warnLo = 20.0;
        ck(p.severity(19.0) == 1 && p.severity(50.0) == 0, "a warning low bands below it");
    }

    // ---- IT SURVIVES THE DOCUMENT ------------------------------------------------------------------
    {
        ChannelPrefs& P = ChannelPrefs::instance();
        P.clear();
        ChannelPref p; p.warnHi = 95.0; p.unit = "psi"; p.ranges = "#000000,#ff0000,,,0,value > 200ms,";
        P.set("oil_pressure", p);
        P.set("nothing_set", ChannelPref{});                 // an empty entry is not worth storing
        const jf::JJson j = P.toJson();
        P.clear();
        ck(P.find("oil_pressure") == nullptr, "cleared is cleared");
        P.load(j);
        const ChannelPref* q = P.find("oil_pressure");
        ck(q && q->warnHi == 95.0 && q->unit == "psi", "unit and threshold survive a save/load");
        ck(q && q->ranges.find("200ms") != std::string::npos,
           "…and so does the banding expression, suffix and all", q ? q->ranges : std::string());
        ck(P.find("nothing_set") == nullptr, "an empty preference is not written at all");
        ck(std::isnan(q->warnLo), "a threshold that was never set comes back UNSET, not 0");
    }

    // ---- THE CASCADE, THROUGH THE REAL COLOUR RESOLVER ---------------------------------------------
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());
    // A binding is an EXPRESSION, resolved through the evaluator — without a resolver registered every
    // one of them reads 0, and a band that never fires looks exactly like a band that does not work.
    static ConfigSigilResolver configSigils;
    MathEvaluator::instance().registerResolver(&configSigils);

    // A config path, so the reading comes from the default image and the test needs no live ECU.
    const std::string path = "engine.cranking_rpm";
    const double v = c.value(path);
    std::printf("[prefs] banding %s, which reads %.1f\n", path.c_str(), v);
    if (!(v > 0.0)) { std::printf("[prefs] no default to band against — nothing to test\n"); return 77; }

    PanelModel page;
    const int id = page.add("value", 0.f, 0.f, 100.f, 30.f, { { "signalName", path } });
    const PanelElement& el = *page.get(id);
    uint8_t buf[4];
    static const uint8_t base[4] = { 0x11, 0x22, 0x33, 0xff };

    ChannelPrefs::instance().clear();
    ck(CanvasWidget::elColor(el, "fgColor", base, buf) == base,
       "with no band, the control keeps its own colour");

    ChannelPref p; p.warnHi = v - 1.0;                 // the reading is now past the warning
    ChannelPrefs::instance().set(path, p);
    // Take a COPY of the colour, not the pointer: every resolve writes into the same scratch buffer, so
    // holding the pointer compares the second answer with itself.
    const uint8_t* warnPtr = CanvasWidget::elColor(el, "fgColor", base, buf);
    const std::string warn = hex(warnPtr);
    std::printf("[prefs] over the warning -> %s\n", warn.c_str());
    ck(warnPtr != base, "a channel warning colours a control that never heard of it", warn);

    p.alarmHi = v - 1.0;
    ChannelPrefs::instance().set(path, p);
    const std::string alarm = hex(CanvasWidget::elColor(el, "fgColor", base, buf));
    std::printf("[prefs] over the alarm   -> %s\n", alarm.c_str());
    ck(alarm != warn, "an alarm reads differently from a warning", alarm + " vs " + warn);

    // …and a rule on the WIDGET still beats the channel: it is the narrower statement.
    const int id2 = page.add("value", 0.f, 40.f, 100.f, 30.f,
                             { { "signalName", path },
                               { "ranges", "#000000,#00ff00,,,0,value > 0," } });
    const std::string own = hex(CanvasWidget::elColor(*page.get(id2), "fgColor", base, buf));
    std::printf("[prefs] the widget's own rule -> %s\n", own.c_str());
    ck(own == "#00ff00", "a per-widget rule outranks the channel's band", own);

    // A band is only about the number, so it must not repaint the box the number sits in.
    const uint8_t* bg = CanvasWidget::elColor(el, "bgColor", base, buf);
    ck(bg == base, "a numeric warning does not repaint the background", hex(bg));

    ChannelPrefs::instance().clear();
    std::printf("[prefs] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
