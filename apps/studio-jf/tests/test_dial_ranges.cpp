// VALUE RANGES REACH THE DIAL.
//
// The range machinery is generic — any number of [start,end] bands, each optionally overriding a colour
// channel, first match wins — and every widget that paints through resolved*() got it for free. The dial
// did not: its colours are its OWN named props (face/fill/bezel/glow), read through the static
// elColor(), which goes straight to the element and never consults a band. So ranges could be set on a
// dial and were silently ignored — on the one widget where a red zone is the entire point.
//
// Pinned here, against the resolver the dial actually paints through:
//   - below the band, each part keeps its own authored colour
//   - inside the band, the mapped channel overrides it (face<-bg, fill<-accent, bezel<-border, glow<-fg)
//   - a channel the band leaves empty does NOT override — the authored colour still shows
//   - first matching band wins where two overlap
//   - a part with no authored colour and no band falls back to the built-in default
//
//   cmake --build build --target dial_ranges_test && ./build/dial_ranges_test

#include "../src/surface/widgets/DialWidget.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[dial-range] %-58s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  - " + detail).c_str());
    if (!ok) ++fails;
}
static std::string hex(const uint8_t* c) {
    char b[16]; std::snprintf(b, sizeof b, "%02x%02x%02x", c[0], c[1], c[2]); return b;
}

// A DialWidget that exposes the banded resolve exactly as render() calls it.
struct Probe : DialWidget {
    using DialWidget::DialWidget;
    std::string face (const PanelElement& e, const uint8_t* fb) { uint8_t o[4]; return hex(elColorBanded(e, "dialColor",  BandChannel::Bg,     fb, o)); }
    std::string fill (const PanelElement& e, const uint8_t* fb) { uint8_t o[4]; return hex(elColorBanded(e, "fillColor",  BandChannel::Accent, fb, o)); }
    std::string bezel(const PanelElement& e, const uint8_t* fb) { uint8_t o[4]; return hex(elColorBanded(e, "bezelColor", BandChannel::Border, fb, o)); }
    std::string glow (const PanelElement& e, const uint8_t* fb) { uint8_t o[4]; return hex(elColorBanded(e, "glowColor",  BandChannel::Fg,     fb, o)); }
    // Through setValue, which is what the surface calls: a rule compares the DISPLAY value, and setting
    // the raw one alone left every band asking about nought (the reason this file's assertions passed
    // for a value it had never actually been given).
    void at(double v) { setValue(v); }
};

int main() {
    jf::JSceneGraph graph;
    Probe dial(graph);
    static const uint8_t kFallback[4] = { 0x11, 0x22, 0x33, 0xff };

    PanelElement el;
    el.props["dialColor"]  = "#101010";
    el.props["fillColor"]  = "#00c8ff";
    el.props["bezelColor"] = "#505050";
    // glowColor deliberately unset: it must fall back until a band gives it one.

    // Two rules: a warning over 80 that recolours fill + face, and a red one over 90 that also takes the
    // bezel and the glow. A rule is an EXPRESSION now, not a [start,end] interval — `value` is the
    // control's own reading — so an interval is written as the comparison it always meant.
    auto rule = [](std::string bg, std::string fg, std::string accent, std::string border, std::string when) {
        CanvasWidget::Range r; r.bg = std::move(bg); r.fg = std::move(fg); r.accent = std::move(accent);
        r.border = std::move(border); r.when = std::move(when); return r;
    };
    dial.m_ranges = {
        rule("#332200", "",        "#ffaa00", "",        "value >= 80 && value < 90"),
        rule("#330000", "#ff2222", "#ff0000", "#aa0000", "value >= 90"),
    };

    dial.at(50);                     // below every band
    check(dial.face(el, kFallback)  == "101010", "below the bands the face keeps its authored colour");
    check(dial.fill(el, kFallback)  == "00c8ff", "below the bands the fill keeps its authored colour");
    check(dial.glow(el, kFallback)  == "112233", "an unset colour with no band falls back");

    dial.at(85);                     // inside the warning band
    check(dial.fill(el, kFallback)  == "ffaa00", "inside a band the fill takes the accent override");
    check(dial.face(el, kFallback)  == "332200", "inside a band the face takes the bg override");
    check(dial.bezel(el, kFallback) == "505050", "a channel the band leaves empty does not override",
          dial.bezel(el, kFallback));
    check(dial.glow(el, kFallback)  == "112233", "an empty channel does not override the fallback either");

    dial.at(95);                     // inside the red zone
    check(dial.fill(el, kFallback)  == "ff0000", "the red zone recolours the fill");
    check(dial.bezel(el, kFallback) == "aa0000", "... the bezel");
    check(dial.glow(el, kFallback)  == "ff2222", "... and the glow, which had no authored colour");

    // Overlap: first match wins, as the range contract says.
    dial.m_ranges.insert(dial.m_ranges.begin(), rule("#0000ff", "", "#0000ff", "", "value >= 0"));
    dial.at(95);
    check(dial.fill(el, kFallback) == "0000ff", "first matching band wins where two overlap");

    std::printf("[dial-range] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
