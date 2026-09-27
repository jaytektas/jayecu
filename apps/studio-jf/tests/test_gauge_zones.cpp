// SCALE ZONES REACH THE GAUGE FACE — asserted against what the render actually painted.
//
// A gauge is three widgets stacked: the DIAL paints the face, the SCALE paints the ticks and numbers, the
// needle points. A zone belongs to the first two, and this covers both — a redline whose arc and whose
// numbers disagree is a broken gauge, so they are pinned in one place.
//
// A zone is not a value rule: it is a fixed span of the SCALE, painted into the face whatever the reading
// is, which is how a tacho has a red 6500-8000 with the engine stopped. Getting it "on the dial" is not
// enough — it has to land on the RIGHT ARC, or the redline sits somewhere the numbers beside it don't
// agree with, which is worse than no redline at all.
//
// So this asks the primitive buffer where the coloured triangles ended up, and converts each one back to
// the angle and radius it was drawn at:
//   - the red zone occupies exactly the arc between the angles for 6500 and 8000
//   - the orange one sits beside it, not overlapping
//   - both paint with the value fill switched OFF (dial + scale + needle stacked: the modern look)
//   - zoneWidth keeps the band on the OUTER edge of the ring and leaves the rest to the fill
//   - a segmented dial paints its zone too
//
//   cmake --build build --target gauge_zones_test && ./build/gauge_zones_test

#include "../src/surface/widgets/DialWidget.h"
#include "../src/surface/widgets/ScaleWidget.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"

#include <j/core/SceneGraph.h>
#include <j/graphics/RenderPrimitive.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[gauge-zone] %-60s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  - " + detail).c_str());
    if (!ok) ++fails;
}
static std::string hex(const uint8_t* c) {
    char b[16]; std::snprintf(b, sizeof b, "%02x%02x%02x", c[0], c[1], c[2]); return b;
}

// The geometry the dial painted, as (colour, angle°, radius) about the widget's centre. The dial draws
// through the vector canvas, which flushes ONE geometry command of coloured triangles — so every part it
// drew is in here, and where each one is can simply be measured.
struct Dot { std::string color; double deg, r; };
static constexpr float kW = 200.f, kH = 200.f;

static std::vector<Dot> paint(CanvasWidget& d, const PanelElement& el) {
    d.setSource(el);
    d.setValue(0.0);                       // the reading is irrelevant to a zone — that is the point
    jf::JPrimitiveBuffer buf;
    d.render(buf, jf::JRect{ 0.f, 0.f, kW, kH }, Cache::instance());
    std::vector<Dot> out;
    const double cx = kW * 0.5, cy = kH * 0.5;
    for (const auto& c : buf.getCommands()) {
        if (c.kind != jf::JPrimitiveBuffer::JDrawCommand::JKind::Geometry) continue;
        for (const jf::JRenderVertex& v : c.geom.verts) {
            const double dx = v.position[0] - cx, dy = v.position[1] - cy;
            out.push_back({ hex(v.color), std::atan2(-dy, dx) * 180.0 / 3.14159265358979, std::hypot(dx, dy) });
        }
    }
    return out;
}

// The angular span a colour was painted over, and how far out it sits. A zone that was never drawn comes
// back empty, which is the failure worth telling apart from one drawn in the wrong place.
struct Span { bool any = false; double lo = 1e9, hi = -1e9, rMin = 1e9, rMax = -1e9; };
static Span spanOf(const std::vector<Dot>& dots, const std::string& color) {
    Span s;
    for (const Dot& d : dots) {
        if (d.color != color || d.r < 1.0) continue;   // r<1: the centre vertex of a fan says nothing
        s.any = true;
        s.lo = std::min(s.lo, d.deg); s.hi = std::max(s.hi, d.deg);
        s.rMin = std::min(s.rMin, d.r); s.rMax = std::max(s.rMax, d.r);
    }
    return s;
}
static std::string fmt(const Span& s) {
    char b[96];
    std::snprintf(b, sizeof b, "%.1f°..%.1f°  r %.1f..%.1f", s.lo, s.hi, s.rMin, s.rMax);
    return b;
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

int main() {
    jf::JSceneGraph graph;
    DialWidget dial(graph);

    // A 0-8000 tacho over the usual 225°..-45° sweep, drawn with nothing but its track and its zones:
    // no bezel, no glow, no glass, no cap, and the value fill OFF — the needle-less face the dial is
    // stacked under a scale and a needle to make.
    PanelElement el;
    el.id = 1; el.type = "dial";
    el.props["signalName"]      = "0";
    el.props["minValue"]        = "0";
    el.props["maxValue"]        = "8000";
    el.props["startAngle"]      = "225";
    el.props["endAngle"]        = "-45";
    el.props["innerRadius"]     = "0.8";
    el.props["dialColor"]       = "#202020";
    el.props["fillColor"]       = "#00c8ff";
    el.props["showFill"]        = "0";
    el.props["showPeak"]        = "0";
    el.props["showGlass"]       = "0";
    el.props["bezelWidth"]      = "0";
    el.props["glowRadius"]      = "0";
    el.props["centerCapRadius"] = "0";
    el.props["zones"]           = "5500,6500,#ff8800;6500,8000,#ff0000";

    // Where each zone's edges SHOULD be: the same 0..1 fraction of the span the fill uses, mapped onto
    // the sweep. 6500/8000 = 0.8125 of 270° from 225 = 5.6°, and the top of the scale is the sweep's end.
    auto angleAt = [](double v) { return 225.0 + (v / 8000.0) * (-45.0 - 225.0); };
    const double redHi = angleAt(6500), redLo = angleAt(8000);      // 5.6° .. -45°
    const double amberHi = angleAt(5500), amberLo = angleAt(6500);  // 39.4° .. 5.6°

    std::vector<Dot> dots = paint(dial, el);
    Span red = spanOf(dots, "ff0000"), amber = spanOf(dots, "ff8800");

    check(red.any, "the red zone is painted with the value fill switched off");
    check(amber.any, "and so is the warning zone beside it");
    // 1.2px of anti-alias at the ring's inner radius is ~1°, so a degree or two of slop is the geometry,
    // not a misplaced band.
    check(red.any && near(red.hi, redHi, 2.0) && near(red.lo, redLo, 2.0),
          "the red zone spans exactly 6500..8000 on the sweep", fmt(red));
    check(amber.any && near(amber.hi, amberHi, 2.0) && near(amber.lo, amberLo, 2.0),
          "the warning zone spans exactly 5500..6500", fmt(amber));
    check(red.any && amber.any && red.hi <= amber.lo + 2.0,
          "the two meet at 6500 rather than overlapping", fmt(red) + " | " + fmt(amber));

    // Default zoneWidth: the band is the whole ring, inner radius 0.8 of R (= 0.9 * half = 90px).
    check(red.any && near(red.rMin, 72.0, 2.5) && near(red.rMax, 90.0, 2.5),
          "and by default it fills the ring's whole thickness", fmt(red));

    // A zone reaching past the top of the scale is CLIPPED to the dial, not wrapped round it.
    el.props["zones"] = "6500,12000,#ff0000";
    Span over = spanOf(paint(dial, el), "ff0000");
    check(over.any && near(over.hi, redHi, 2.0) && near(over.lo, -45.0, 2.0),
          "a zone past the end of the scale is clipped to the sweep", fmt(over));

    // zoneWidth < 1: the zones keep the OUTER sliver and the value fill gets the rest, so a face can show
    // a permanent coloured rim over a moving arc instead of choosing between them.
    el.props["zones"]     = "0,8000,#ff0000";
    el.props["zoneWidth"] = "0.5";
    el.props["showFill"]  = "1";
    el.props["signalName"] = "8000";                 // full sweep, so the fill is everywhere the zone is
    dial.setValue(8000);
    Span rim = spanOf(paint(dial, el), "ff0000"), fill = spanOf(paint(dial, el), "00c8ff");
    check(rim.any && rim.rMin > 79.0, "a half-width zone sits on the ring's OUTER half", fmt(rim));
    check(fill.any && fill.rMax < 83.0, "... and the value fill keeps the inner half to itself", fmt(fill));

    // A segmented dial gets its zones too — same span, drawn per bar.
    el.props["zoneWidth"]      = "1";
    el.props["showFill"]       = "0";
    el.props["segmentCount"]   = "20";
    el.props["segmentSpacing"] = "2";
    el.props["zones"]          = "6500,8000,#ff0000";
    Span seg = spanOf(paint(dial, el), "ff0000");
    check(seg.any, "a segmented dial paints its zone as well");
    check(seg.any && seg.lo > -47.0 && seg.hi < redHi + 3.0,
          "... over the bars the zone covers, and no further", fmt(seg));

    // ---- AND THE SCALE STACKED OVER IT ---------------------------------------------------------
    // The dial paints the arc; the scale is what puts the NUMBERS and the ticks in red beside it. Same
    // zone, same sweep, so the two agree about where 6500 is — which is the whole reason this is one test.
    ScaleWidget scale(graph);
    PanelElement se;
    se.id = 2; se.type = "scale";
    se.props["minValue"]   = "0";
    se.props["maxValue"]   = "8000";
    se.props["startAngle"] = "225";
    se.props["endAngle"]   = "-45";
    se.props["scaleMode"]  = "Circular";
    se.props["autoTicks"]  = "0";
    se.props["majorTicks"] = "9";          // one every 1000 rpm, so 6500 falls between two of them
    se.props["minorTicks"] = "4";
    se.props["tickColor"]  = "#ffffff";
    se.props["minorTickColor"] = "#ffffff";
    se.props["showLabels"] = "0";          // a headless test has no font atlas; the ticks carry the proof
    se.props["zones"]      = "6500,8000,#ff0000";

    std::vector<Dot> sd = paint(scale, se);
    Span sRed = spanOf(sd, "ff0000");
    check(sRed.any, "the scale draws its ticks in the zone's colour");
    check(sRed.any && sRed.lo > -47.0 && sRed.hi < redHi + 2.0,
          "... only the ticks from 6500 up, none below it", fmt(sRed));
    // The ticks below the redline keep their own colour. Counted rather than measured as a span: the
    // sweep crosses 180°, where atan2 wraps, so the unzoned ticks have no single min/max to compare —
    // what matters is that NONE of them is inside the red arc, and that there are plenty outside it.
    int whiteInRed = 0, whiteOutside = 0;
    for (const Dot& d : sd) {
        if (d.color != "ffffff" || d.r < 1.0) continue;
        (d.deg >= redLo + 2.0 && d.deg <= redHi - 2.0) ? ++whiteInRed : ++whiteOutside;
    }
    check(whiteInRed == 0, "... and no tick inside the redline escaped it", std::to_string(whiteInRed));
    check(whiteOutside > 0, "... while the ticks below it keep their own colour", std::to_string(whiteOutside));

    // Ticks and labels are separately switchable, so a face can have red numbers over plain ticks.
    se.props["zoneTicks"] = "0";
    check(!spanOf(paint(scale, se), "ff0000").any, "with zone ticks off, the ticks stay their own colour");

    std::printf("[gauge-zone] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
