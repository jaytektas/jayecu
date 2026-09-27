#include "EngineCycleView.h"
#include "WrapText.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jf;
using enginecycle::Signal;
using enginecycle::Span;
using enginecycle::Trace;

namespace {

// Lane fill by signal kind, so coils and injectors are separable at a glance without a legend.
const uint8_t* laneColour(Signal s) {
    static const uint8_t coil[4]  = { 0xe0, 0x9a, 0x2a, 255 };   // amber — dwell/spark
    static const uint8_t inj[4]   = { 0x3f, 0xb9, 0x50, 255 };   // green — injector open
    static const uint8_t trig[4]  = { 0x5a, 0xa8, 0xf0, 255 };   // blue  — REAL trigger teeth
    static const uint8_t virt[4]  = { 0x7a, 0x6f, 0xd0, 255 };   // violet — the PLL's own grid
    static const uint8_t other[4] = { 0x9a, 0x9a, 0xa2, 255 };
    switch (s) {
        case Signal::Coil:     return coil;
        case Signal::Injector: return inj;
        // Deliberately a different colour from the real teeth. Same blue would invite reading the
        // two combs as one signal, when the interesting thing is precisely where they disagree.
        case Signal::Virtual:  return virt;
        case Signal::Crank:
        case Signal::Cam:      return trig;
        default:               return other;
    }
}

// Axis tick spacing: keep roughly 8-16 labelled ticks whatever the cycle span, on round numbers a
// tuner thinks in (90 deg on a four-stroke, 45 on a two-stroke, 90 on a rotary's 1080).
double tickStep(double span) {
    if (span <= 360.0)  return 45.0;
    if (span <= 720.0)  return 90.0;
    return 90.0;
}

} // namespace

// WHERE EVERYTHING GOES, decided before anything is drawn. The lane rectangles and the plot rect are
// what a hover resolves against, and they used to be produced as a SIDE EFFECT of painting — below an
// early return taken when there is no font atlas. Anything that skipped the paint therefore left the
// view unhittable, and a headless test could not reach the hover path at all. Position is geometry;
// it does not depend on whether text can be rendered.
void EngineCycleView::layOut() {
    const JRect r = bounds();
    lanes_.clear();
    plotRect_ = JRect{};
    const float plotX = r.x + kPadX + kLabelW;
    const float hh    = headerH(r.width);
    const float plotY = r.y + hh;
    const float plotW = r.width - (kPadX * 2.f) - kLabelW;
    const float plotH = r.height - hh - kPadY;
    if (plotW <= 20.f || plotH <= 20.f || cycle_.empty()) return;
    plotRect_ = JRect{ plotX, plotY, plotW, plotH };
    float y = plotY + kPadY;
    for (const Trace& t : cycle_.traces()) {
        if (y + kLaneH > plotY + plotH) break;      // out of room; the view scrolls in a later pass
        lanes_.push_back(LaneRect{ &t, y, kLaneH });
        y += kLaneH + kLaneGap;
    }
}

void EngineCycleView::populateRenderPrimitives(JPrimitiveBuffer& buf) {
    const JRect r = bounds();
    layOut();                                  // before the atlas guard: hovering must work regardless
    buf.pushRectangle(r.x, r.y, r.width, r.height, Colors::Surface0);
    if (!JTextHelper::hasAtlas()) return;

    drawHeader(buf, r);

    const float plotX = r.x + kPadX + kLabelW;
    const float hh    = headerH(r.width);
    const float plotY = r.y + hh;
    const float plotW = r.width - (kPadX * 2.f) - kLabelW;
    const float plotH = r.height - hh - kPadY;
    if (plotW <= 20.f || plotH <= 20.f) return;
    const JRect plot{ plotX, plotY, plotW, plotH };

    drawGrid(buf, plot);

    if (cycle_.empty()) {   // the status may be a capture's error: wrapped, centred as a block
        const float mh = wraptext::height(status_, plot.width - 16.f);
        wraptext::drawCentred(buf, plot.x + 8.f, plot.y + (plot.height - mh) * 0.5f, plot.width - 16.f,
                              status_, Colors::TextSecondary);
        return;
    }

    // Draw the lanes layOut() already decided, so what is hit-tested and what is painted cannot drift.
    buf.pushClip(r.x, plotY, r.width, plotH);
    for (const LaneRect& l : lanes_) drawTrace(buf, plot, *l.trace, l.y);
    buf.popClip();

    drawReadout(buf, r);   // last: it goes over everything, and outside the plot clip
}

// The hover box. Drawn by the view rather than left to the framework tooltip — see the header for
// why. Placed beside the cursor and flipped back inside the widget at the right and bottom edges, so
// reading the last coil in the cycle does not push the numbers off the panel.
void EngineCycleView::drawReadout(JPrimitiveBuffer& buf, const JRect& r) const {
    if (hoverText_.empty() || !JTextHelper::hasAtlas()) return;

    // Split once, measure the widest line: a two- or three-line readout in a box sized to its longest
    // line, not to a guess.
    std::vector<std::string> lines;
    for (size_t i = 0, j; i <= hoverText_.size(); i = j + 1) {
        j = hoverText_.find('\n', i);
        if (j == std::string::npos) j = hoverText_.size();
        lines.emplace_back(hoverText_.substr(i, j - i));
    }
    const float lh = JTextHelper::lineHeight();
    float textW = 0.f;
    for (const std::string& l : lines) textW = std::max(textW, JTextHelper::measureWidth(l));

    const float padX = 8.f, padY = 6.f;
    const float boxW = textW + padX * 2.f;
    const float boxH = lh * static_cast<float>(lines.size()) + padY * 2.f;
    float bx = hoverX_ + 14.f;                       // clear of the cursor itself
    float by = hoverY_ + 16.f;
    if (bx + boxW > r.x + r.width)  bx = hoverX_ - boxW - 10.f;
    if (by + boxH > r.y + r.height) by = hoverY_ - boxH - 10.f;
    bx = std::max(bx, r.x + 2.f);
    by = std::max(by, r.y + 2.f);

    buf.pushRectangle(bx, by, boxW, boxH, Colors::Surface2, 5.f, 1.f, Colors::Border);
    float ty = by + padY;
    for (size_t i = 0; i < lines.size(); ++i) {
        // First line is the channel's name — the identity, so it gets the primary weight; the numbers
        // under it are secondary.
        JTextHelper::pushText(buf, bx + padX, ty, lines[i],
                              i == 0 ? Colors::TextPrimary : Colors::TextSecondary, textW);
        ty += lh;
    }
}

float EngineCycleView::headerH(float w) const {
    const float tw = w - 2.f * kPadX;
    return kHeaderH + wraptext::height(notice_, tw) + wraptext::height(cycle_.note(), tw);
}

void EngineCycleView::drawHeader(JPrimitiveBuffer& buf, const JRect& r) {
    const float lh = JTextHelper::lineHeight();
    char caption[128];

    // Clip the header to our own bounds. Without this a long note (a rotary span, a truncated
    // capture, a single-anchor reconstruction) simply kept drawing past the right edge and over
    // whatever dock sat beside us — text belonging to this panel appearing inside another one.
    buf.pushClip(r.x, r.y, r.width, headerH(r.width));

    // The span is part of the reading: 1080 deg tells you it is a rotary as surely as the trace names.
    std::snprintf(caption, sizeof(caption), "Engine Cycle  \xE2\x80\x94  %.0f\xC2\xB0",
                  cycle_.cycleAngle());
    JTextHelper::pushText(buf, r.x + kPadX, r.y + kPadY, caption, Colors::TextPrimary, r.width);
    float x = r.x + kPadX + JTextHelper::measureWidth(caption) + 18.f;


    if (cycle_.rpm() > 0.0) {
        char rpm[48];
        std::snprintf(rpm, sizeof(rpm), "%.0f rpm", cycle_.rpm());
        JTextHelper::pushText(buf, x, r.y + kPadY, rpm, Colors::TextSecondary, r.width);
        x += JTextHelper::measureWidth(rpm) + 18.f;
    }

    // Say which kind of angle this is. Our own ECU reports the angle it scheduled against; a
    // converted source had to infer it from TDC markers and the RPM between them, which smears under
    // acceleration — exactly when someone is most likely to be looking. Presenting the two
    // identically would be the lie.
    static const uint8_t warn[4] = { 0xd2, 0x9a, 0x2a, 255 };
    if (cycle_.isReconstructed()) {
        // A filled dot, not an open one: U+25CB is absent from the font atlas and drew as "?",
        // which reads as an error rather than a marker. The AMBER carries "reconstructed".
        const char* note = "\xE2\x97\x8F reconstructed";
        JTextHelper::pushText(buf, x, r.y + kPadY, note, warn, r.width);
        x += JTextHelper::measureWidth(note) + 18.f;
    } else if (!cycle_.empty()) {
        static const uint8_t ok[4] = { 0x3f, 0xb9, 0x50, 255 };
        const char* note = "\xE2\x97\x8F measured";
        JTextHelper::pushText(buf, x, r.y + kPadY, note, ok, r.width);
        x += JTextHelper::measureWidth(note) + 18.f;
    }

    // The notice comes FIRST under the caption and in the reconstruction colour, because it says the
    // frames below are not current — which changes how everything else on this screen should be read.
    // Then the capture's own caveats — a crank-folded span, a truncated buffer, an rpm-scaled
    // reconstruction — which change how the picture must be read. Both are sentences, so each WRAPS
    // on lines of its own and headerH() moves the plot down by them; on the caption row they were
    // cut off at whatever room the row had left.
    float ny = r.y + kPadY + lh;
    ny += wraptext::draw(buf, r.x + kPadX, ny, notice_, Colors::Warning, r.width - 2.f * kPadX);
    wraptext::draw(buf, r.x + kPadX, ny, cycle_.note(), warn, r.width - 2.f * kPadX);

    buf.popClip();
    (void)lh;
}

void EngineCycleView::drawGrid(JPrimitiveBuffer& buf, const JRect& plot) {
    const double span = cycle_.cycleAngle();
    const double step = tickStep(span);

    for (double a = 0.0; a <= span + 0.001; a += step) {
        const float x = xOf(plot, a);
        const bool major = (std::fmod(a, span / 2.0) < 0.001) || a <= 0.001;
        buf.pushRectangle(x, plot.y, 1.f, plot.height,
                          major ? Colors::Border : Colors::Surface2);
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "%.0f", a);
        JTextHelper::pushText(buf, x + 3.f, plot.y - JTextHelper::lineHeight() - 2.f,
                              lbl, Colors::TextSecondary, 60.f);
    }
    // Close the right edge so the cycle reads as a bounded thing rather than a cut-off strip.
    buf.pushRectangle(plot.x + plot.width, plot.y, 1.f, plot.height, Colors::Border);
}

void EngineCycleView::drawTrace(JPrimitiveBuffer& buf, const JRect& plot,
                                const Trace& t, float y) {
    // Label gutter. Rotary identity when we have it — "R1F2 Lead" says more than "Coil 3" on an
    // engine where the coil serves three faces.
    char label[64];
    if (t.rotor >= 0 && t.face >= 0) {
        std::snprintf(label, sizeof(label), "R%dF%d %s", t.rotor, t.face,
                      t.trailing ? "Trail" : "Lead");
    } else {
        std::snprintf(label, sizeof(label), "%s", t.label.c_str());
    }
    JTextHelper::pushText(buf, plot.x - kLabelW, y + (kLaneH - JTextHelper::lineHeight()) * 0.5f,
                          label, Colors::TextSecondary, kLabelW - 6.f);

    // Lane background, so an idle trace is still visibly a trace.
    buf.pushRectangle(plot.x, y, plot.width, kLaneH, Colors::Surface1, 2.f);

    const uint8_t* col = laneColour(t.signal);

    // An EVENT trace is drawn as a tick per edge, not as spans. A tooth is an instant: it has no
    // "off" edge to close against, so asking for its high spans yields one bar across the whole
    // cycle — which is exactly what a trigger lane looked like before this split. The teeth ARE the
    // reference the output lanes are read against, so they have to read as a comb.
    if (enginecycle::isEventSignal(t.signal)) {
        for (const enginecycle::Edge& e : t.edges) {
            const float x = xOf(plot, e.angle);
            buf.pushRectangle(x, y + 3.f, 2.f, kLaneH - 6.f, col);
        }
        return;
    }

    for (const Span& s : cycle_.spans(t)) {
        const float x0 = xOf(plot, s.from);
        if (!s.wrapped) {
            const float x1 = xOf(plot, s.to);
            buf.pushRectangle(x0, y + 3.f, std::max(1.f, x1 - x0), kLaneH - 6.f, col, 2.f);
        } else {
            // A span crossing the cycle origin is ONE event drawn as two pieces — the tail at the
            // right edge and the head at the left. Drawing it as a single rect from `from` to `to`
            // would render backwards, or vanish.
            const float xEnd = plot.x + plot.width;
            buf.pushRectangle(x0, y + 3.f, std::max(1.f, xEnd - x0), kLaneH - 6.f, col, 2.f);
            const float x1 = xOf(plot, s.to);
            buf.pushRectangle(plot.x, y + 3.f, std::max(1.f, x1 - plot.x), kLaneH - 6.f, col, 2.f);
        }
    }
}

// ---------------------------------------------------------------------------
// handleMouseMove — read out the bar under the cursor.
// ---------------------------------------------------------------------------
// ONE BOX, NOT TWO. This used to set the framework tooltip to the same text as well, on the theory
// that a still cursor may as well have both. It gets both: the view's box appears immediately and
// the tooltip fades in on top of it a moment later, at its own slightly different offset, and the
// two outlines overlapping read as a single box gone 3-D. Whichever draws the readout has to be the
// only one that draws it, and it is this one, because a positional readout cannot wait out a dwell.
// The tooltip is cleared rather than left alone so nothing stale can surface behind the box.
//
// Repainting only when the TEXT changes keeps a sweep across a wide bar from invalidating every frame
// for a box that would redraw identically; the position is refreshed regardless so the box tracks.
void EngineCycleView::setReadout(const std::string& text, float ax, float ay) {
    const bool changed = (text != hoverText_);
    hoverText_ = text;
    hoverX_ = ax; hoverY_ = ay;
    setTooltip({});
    if (changed || !text.empty()) invalidate();
}

void EngineCycleView::handleMouseMove(float mx, float my) {
    // MOUSE COORDINATES ARE ALREADY ABSOLUTE. JWidget::isPointInside compares the very same mx/my
    // against the node's absolute boundingBox, so that is the space the framework dispatches in —
    // and the lane rectangles were recorded in it too, because they came from the layout rect the
    // primitives were drawn with. This used to add bounds() on top, which offset every hit test by
    // the widget's own origin: the computed point landed outside plotRect_ for any real cursor
    // position, so the lane search never matched and the readout was cleared on every move. The
    // angles, dwell, spark and millisecond conversion were all correct and could never be seen —
    // including through setTooltip(), which is why the framework tooltip never appeared either.
    const float ax = mx, ay = my;
    if (cycle_.empty() || lanes_.empty() || plotRect_.width <= 0.f) { setReadout({}, ax, ay); return; }

    const LaneRect* hit = nullptr;
    for (const LaneRect& l : lanes_)
        if (ay >= l.y && ay < l.y + l.h) { hit = &l; break; }
    if (!hit || ax < plotRect_.x || ax > plotRect_.x + plotRect_.width) { setReadout({}, ax, ay); return; }

    const double span = cycle_.cycleAngle() > 0.0 ? cycle_.cycleAngle() : 720.0;
    const double deg  = (ax - plotRect_.x) / plotRect_.width * span;
    const double mspd = msPerDegree();
    const Trace& t    = *hit->trace;

    char buf[256];
    if (enginecycle::isEventSignal(t.signal)) {
        // A tooth is an instant, so the useful readout is WHICH tooth and the gap to the one before
        // — that gap is how a missing tooth shows itself numerically rather than as a visual guess.
        const enginecycle::Edge* nearest = nullptr;
        double best = 1e9;
        size_t idx = 0, hitIdx = 0;
        for (const enginecycle::Edge& e : t.edges) {
            const double d = std::fabs(e.angle - deg);
            if (d < best) { best = d; nearest = &e; hitIdx = idx; }
            ++idx;
        }
        // Only claim a tooth the cursor is actually near; a few degrees at this scale is a few pixels.
        if (!nearest || best > span * 0.01) { setReadout({}, ax, ay); return; }
        const double prev = (hitIdx > 0) ? t.edges[hitIdx - 1].angle : nearest->angle;
        const double gap  = (hitIdx > 0) ? (nearest->angle - prev) : 0.0;
        if (gap > 0.0)
            std::snprintf(buf, sizeof(buf), "%s  tooth %zu of %zu\n%.1f\xC2\xB0   gap %.1f\xC2\xB0",
                          t.label.c_str(), hitIdx + 1, t.edges.size(), nearest->angle, gap);
        else
            std::snprintf(buf, sizeof(buf), "%s  tooth %zu of %zu\n%.1f\xC2\xB0",
                          t.label.c_str(), hitIdx + 1, t.edges.size(), nearest->angle);
        setReadout(buf, ax, ay);
        return;
    }

    // A LEVEL lane: find the span under the cursor. Wrap-aware, because a dwell that crosses the
    // cycle origin is one span drawn as two pieces and must read as one.
    for (const Span& s : cycle_.spans(t)) {
        const bool inside = s.wrapped ? (deg >= s.from || deg <= s.to)
                                      : (deg >= s.from && deg <= s.to);
        if (!inside) continue;
        const double len = cycle_.spanLength(s);
        // Name the edges for what they ARE. On a coil the falling edge is the SPARK and the span is
        // the dwell; calling them "start/end" would make the reader translate every time.
        const bool coil = (t.signal == Signal::Coil);
        if (mspd > 0.0)
            std::snprintf(buf, sizeof(buf), "%s\n%s %.1f\xC2\xB0   %s %.1f\xC2\xB0\n%s %.1f\xC2\xB0 = %.2f ms",
                          t.label.c_str(),
                          coil ? "dwell from" : "open",  s.from,
                          coil ? "spark"      : "close", s.to,
                          coil ? "dwell"      : "width", len, len * mspd);
        else
            std::snprintf(buf, sizeof(buf), "%s\n%s %.1f\xC2\xB0   %s %.1f\xC2\xB0\n%s %.1f\xC2\xB0",
                          t.label.c_str(),
                          coil ? "dwell from" : "open",  s.from,
                          coil ? "spark"      : "close", s.to,
                          coil ? "dwell"      : "width", len);
        setReadout(buf, ax, ay);
        return;
    }
    setReadout({}, ax, ay);
}
