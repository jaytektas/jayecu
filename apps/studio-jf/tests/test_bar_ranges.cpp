// VALUE RANGES REACH THE BAR — the fill, not just the card behind it.
//
// A bar gauge's colours are its OWN named props (dialColor / fillColor / bezelColor / glowColor), and it
// read them through the plain elColor(), which knows nothing about value bands. So a rule set on a bar was
// parsed, evaluated, matched — and then painted nothing, on the part an alert most wants to recolour. The
// dial was given the band cascade for exactly this reason and the bar was left behind, so a hot-coolant
// alert had to be faked by recolouring the card behind the bar instead of the bar itself.
//
// Asserted through the REAL render rather than by calling the resolver the way the widget is supposed to:
// the widget draws into a primitive buffer, and the buffer keeps every rectangle's fill colour, so the
// test can ask what actually got painted. A test that mirrors the mapping would pass just as happily with
// the mapping wrong.
//
//   cmake --build build --target bar_ranges_test && ./build/bar_ranges_test

#include "../src/surface/widgets/LinearWidget.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"

#include <j/core/SceneGraph.h>
#include <j/graphics/RenderPrimitive.h>

#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[bar-range] %-56s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  - " + detail).c_str());
    if (!ok) ++fails;
}
static std::string hex(const uint8_t* c) {
    char b[16]; std::snprintf(b, sizeof b, "%02x%02x%02x", c[0], c[1], c[2]); return b;
}

// The bound value is sampled into the widget once per frame by the surface, which this test does not run —
// and a rule asks about `value`, so without it every band is comparing against nought. setValue is the same
// call that sampling makes.
struct Bar : LinearWidget {
    using LinearWidget::LinearWidget;
    void at(double v) { setValue(v); }
};

// Every rectangle the widget painted, as hex — the bar's parts are rectangles, so this is what it drew.
static std::vector<std::string> paint(Bar& bar, const PanelElement& el) {
    bar.setSource(el);
    bar.at(50.0);                       // half full, matching the literal source below
    jf::JPrimitiveBuffer buf;
    bar.render(buf, jf::JRect{ 0.f, 0.f, 300.f, 40.f }, Cache::instance());
    std::vector<std::string> out;
    for (const auto& c : buf.getCommands())
        if (c.kind == jf::JPrimitiveBuffer::JDrawCommand::JKind::JRect) out.push_back(hex(c.rect.color));
    return out;
}
static bool drew(const std::vector<std::string>& v, const std::string& want) {
    for (const std::string& c : v) if (c == want) return true;
    return false;
}
static std::string all(const std::vector<std::string>& v) {
    std::string s; for (const std::string& c : v) { if (!s.empty()) s += " "; s += c; } return s;
}

int main() {
    jf::JSceneGraph graph;
    Bar bar(graph);

    // A bar half full: the source is a literal, so the reading needs no meta and no ECU. Peak is off —
    // it paints a rectangle of its own and stays deliberately unbanded, so it would only be noise here.
    PanelElement el;
    el.id = 1; el.type = "gauge";
    el.props["signalName"]   = "50";
    el.props["minValue"]     = "0";
    el.props["maxValue"]     = "100";
    el.props["orientation"]  = "Horizontal";
    el.props["fillColor"]    = "#00c8ff";
    el.props["dialColor"]    = "#202020";
    el.props["showPeak"]     = "0";
    el.props["showGlass"]    = "0";

    {
        const auto v = paint(bar, el);
        check(drew(v, "00c8ff"), "with no rule, the bar fills in its own colour", all(v));
        check(drew(v, "202020"), "…on its own track", all(v));
    }

    // A rule whose ACCENT is red: accent is the channel the fill listens to, the same mapping the dial
    // uses, because the fill is the part that says how much.
    {
        PanelElement b = el;
        b.props["ranges"] = ",,#ff453a,,0,value > 40";
        const auto v = paint(bar, b);
        check(drew(v, "ff453a"), "a rule that holds recolours the FILL", all(v));
        check(!drew(v, "00c8ff"), "…instead of the authored colour, not beside it", all(v));
    }

    // …and a rule that does NOT hold changes nothing.
    {
        PanelElement b = el;
        b.props["ranges"] = ",,#ff453a,,0,value > 90";
        const auto v = paint(bar, b);
        check(drew(v, "00c8ff") && !drew(v, "ff453a"), "a rule that does not hold leaves it alone", all(v));
    }

    // The track answers to `bg`, so a band can darken the groove without touching the fill.
    {
        PanelElement b = el;
        b.props["ranges"] = "#101010,,,,0,value > 40";
        const auto v = paint(bar, b);
        check(drew(v, "101010"), "a rule's background recolours the track", all(v));
        check(drew(v, "00c8ff"), "…leaving the fill as authored", all(v));
    }

    // A blinking rule alternates: on the off phase the bar is its authored self again. Which phase we are
    // in is a shared clock, so the test asserts the pair — one of the two must be true, never neither.
    {
        PanelElement b = el;
        b.props["ranges"] = ",,#ff453a,,1,value > 40";
        const auto v = paint(bar, b);
        check(drew(v, "ff453a") != drew(v, "00c8ff"), "a blinking rule shows one colour or the other", all(v));
    }

    std::printf("[bar-range] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
