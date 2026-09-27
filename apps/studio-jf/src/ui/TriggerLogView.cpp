#include "TriggerLogView.h"
#include "WrapText.h"

#include <algorithm>
#include <cstdio>

using namespace jf;

namespace {

const uint8_t kBar[4]  = { 0x5a, 0xa8, 0xf0, 255 };   // blue — a measured edge
const uint8_t kGap[4]  = { 0xe0, 0x3a, 0x3a, 255 };   // red  — records the ECU's ring lost
const uint8_t kAxis[4] = { 0x50, 0x50, 0x58, 255 };
const uint8_t kOver[4] = { 0xe8, 0x9a, 0x1c, 255 };   // amber — bar exceeds the lane's scale
                                                      // (distinct from kGap: nothing is MISSING here)

// A round microsecond step giving roughly 4-8 gridlines over the tallest bar. Round in the unit the
// user is reading, not in pixels — a line at 1000 us is worth more than one at 1137.
uint32_t tickStep(uint32_t maxUs) {
    static const uint32_t steps[] = { 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000,
                                      100000, 200000, 500000, 1000000, 2000000, 5000000 };
    for (uint32_t s : steps) if (maxUs / s <= 8) return s;
    return 10000000;
}

}  // namespace

void TriggerLogView::populateRenderPrimitives(JPrimitiveBuffer& buf) {
    const JRect r = bounds();
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

    if (lanes_.empty()) {
        // Clear the hit-test map on THIS path too. Returning early left last frame's entries live,
        // so a hover after the lanes went away still indexed a layout that no longer existed.
        laneRects_.clear();
        const float mh = wraptext::height(status_, plot.width - 16.f);   // wrapped, centred as a block
        wraptext::drawCentred(buf, plot.x + 8.f, plot.y + (plot.height - mh) * 0.5f, plot.width - 16.f,
                              status_, Colors::TextSecondary);
        return;
    }

    plotRect_ = plot;
    laneRects_.clear();
    uint32_t t0 = 0, t1 = 0; size_t refBars = 0;
    const bool haveSpan = triggerlog::windowSpan(lanes_, first_, count_, t0, t1, refBars);
    const float laneH = (plot.height - kLaneGap * static_cast<float>(lanes_.size() - 1))
                      / static_cast<float>(lanes_.size());
    float y = plot.y;
    for (size_t li = 0; li < lanes_.size(); ++li) {
        laneRects_.push_back({ li, y, laneH });
        if (haveSpan) drawLane(buf, plot, lanes_[li], y, laneH, t0, t1, refBars);
        y += laneH + kLaneGap;
    }
}

// Say what the axes ARE, on the screen, every time. This view exists next to one that draws
// crank degrees, and the single most expensive mistake a reader could make here is assuming the
// horizontal axis means rotation. It does not, and it cannot.
const char* TriggerLogView::axisLegend() {
    return "bar = one edge  \xC2\xB7  height = \xC2\xB5s since the previous edge  \xC2\xB7  "
           "no angle, no rpm \xE2\x80\x94 a log cannot carry either";
}

float TriggerLogView::headerH(float w) const {
    return kHeaderH + wraptext::extra(axisLegend(), w - 2.f * kPadX - 110.f)
                    + wraptext::extra(notice_, w - 2.f * kPadX);
}

void TriggerLogView::drawHeader(JPrimitiveBuffer& buf, const JRect& r) {
    float x = r.x + kPadX;
    const float y = r.y + 10.f;
    JTextHelper::pushText(buf, x, y, "Trigger Log", Colors::TextPrimary, 200.f);
    x += 110.f;

    // The legend WRAPS beside the title, and the second line moves down by what it took.
    const float ly = y + 16.f + wraptext::draw(buf, x, y, axisLegend(), Colors::TextSecondary,
                                               r.x + r.width - kPadX - x) - JTextHelper::lineHeight();

    // The notice goes on the SECOND line, full width, and WRAPS: sharing line one with the axis
    // description truncated it mid-word ("...press Start to clear it and run agai"), and a sentence
    // longer than the pane did the same on its own line. A hover takes that line instead while it is
    // up — one line, one reader, no overprinting; it is a readout, so it stays one line rather than
    // moving the plot under the cursor as it changes.
    if (!notice_.empty() && hover_.empty())
        wraptext::draw(buf, r.x + kPadX, ly, notice_, Colors::TextPrimary, r.width - kPadX * 2.f);

    if (!hover_.empty())
        JTextHelper::pushText(buf, r.x + kPadX, ly, hover_.c_str(), Colors::TextPrimary,
                              r.width - kPadX * 2.f);
}

void TriggerLogView::drawLane(JPrimitiveBuffer& buf, const JRect& plot,
                              const triggerlog::Lane& lane, float y, float h,
                              uint32_t t0, uint32_t t1, size_t refBars) {
    buf.pushRectangle(plot.x, y, plot.width, h, Colors::Surface1, 2.f);
    JTextHelper::pushText(buf, plot.x - kLabelW, y + h * 0.5f - 6.f, lane.name.c_str(),
                          Colors::TextSecondary, kLabelW - 8.f);
    if (lane.bars.empty()) return;

    // The bars of THIS lane that fall inside the window's time span. A lane that fired nothing in
    // this stretch draws nothing, which is a truthful empty rather than an artefact of indexing.
    size_t from = 0, to = 0;
    triggerlog::barsInSpan(lane, t0, t1, from, to);
    if (to <= from) return;
    const size_t n = to - from;

    // Scale past the OUTLIERS, not to the tallest bar. Windowing already kept a distant outlier out
    // of the picture; it does not help when the outlier is IN the window, which is exactly what
    // tearing the trigger away produces. The interval spanning that silence is not a tooth spacing —
    // on a 60-2 at 1200 rpm it is 60x a pitch — and scaling to it flattens every real tooth into the
    // bottom fiftieth of the lane, hiding the teeth either side of the break, which is the one thing
    // worth looking at. See triggerlog::scaleHeight: it judges out-of-family against the window's
    // median and knows nothing about wheels.
    const uint32_t maxUs = triggerlog::scaleHeight(lane, from, to);

    // Bar WIDTH still comes from the busiest lane's bar count, so a sparse lane draws bars of the
    // same weight rather than one blob per edge stretched across the plot.
    const float step = plot.width / static_cast<float>(refBars ? refBars : n);
    const float span = static_cast<float>(t1 - t0);
    const float barW = std::max(kMinBarW, step * 0.7f);
    const float top  = y + 4.f;
    const float base = y + h - 4.f;
    const float usable = base - top;

    // Gridlines in round microseconds, so a height can be read and not merely compared. The LINES go
    // down first, behind the bars; the LABELS go on last, after them — drawn here they were painted
    // over by the comb and were unreadable across the dense lower half of every lane, which is
    // exactly where the useful ones sit.
    const uint32_t gs = tickStep(maxUs);
    for (uint32_t v = gs; v <= maxUs; v += gs) {
        const float gy = base - usable * (static_cast<float>(v) / static_cast<float>(maxUs));
        buf.pushRectangle(plot.x, gy, plot.width, 1.f, kAxis);
    }

    for (size_t i = 0; i < n; ++i) {
        const triggerlog::Interval& iv = lane.bars[from + i];
        // POSITIONED IN TIME, so lanes line up. This is what makes relating a cam to a crank
        // possible at all — the whole reason a record names its stream and snapshots the others.
        const float x  = plot.x + plot.width * (static_cast<float>(iv.tUs - t0) / span);
        // A bar past the scale is drawn full height and CAPPED, never hidden and never allowed to
        // set the scale. The hover still reports its true duration, so nothing is lost — the cap
        // says "longer than this lane's axis", which is the honest reading of a silence.
        const bool over = iv.deltaUs > maxUs;
        const float bh = over ? usable
                        : usable * (static_cast<float>(iv.deltaUs) / static_cast<float>(maxUs));
        buf.pushRectangle(x, base - bh, barW, bh, kBar);
        if (over) buf.pushRectangle(x, top, barW, std::max(2.f, usable * 0.04f), kOver);
        // A break where the ECU's ring wrapped past our cursor. Drawn full height and in red because
        // the bars either side are NOT adjacent in time, and two disconnected stretches butted
        // together read as one continuous capture — the failure worth being loud about.
        if (iv.gapBefore) buf.pushRectangle(x - step * 0.15f, top, std::max(2.f, step * 0.3f),
                                            usable, kGap);
    }

    // Labels last, over the bars, each on its own backing so it reads against a solid comb. Pinned
    // to the left edge rather than floated: a gridline value is a reference, and a reference that
    // moves with the data is one more thing to find.
    for (uint32_t v = gs; v <= maxUs; v += gs) {
        const float gy = base - usable * (static_cast<float>(v) / static_cast<float>(maxUs));
        char lbl[24];
        if (gs >= 1000) std::snprintf(lbl, sizeof(lbl), "%.1f ms", v / 1000.0);
        else            std::snprintf(lbl, sizeof(lbl), "%u \xC2\xB5s", v);
        const float w = JTextHelper::measureWidth(lbl) + 6.f;
        buf.pushRectangle(plot.x + 1.f, gy - 13.f, w, 14.f, Colors::Surface0, 2.f);
        JTextHelper::pushText(buf, plot.x + 4.f, gy - 12.f, lbl, Colors::TextSecondary, 70.f);
    }
}

void TriggerLogView::handleMouseMove(float mx, float my) {
    hover_.clear();
    for (const LaneRect& lr : laneRects_) {
        if (my < lr.y || my > lr.y + lr.h) continue;
        // Re-check against the CURRENT lanes: the layout was recorded last frame and the data may
        // have been replaced since, which is the whole reason this is an index.
        if (lr.lane >= lanes_.size()) break;
        const triggerlog::Lane& lane = lanes_[lr.lane];
        // Resolve through the same TIME span the paint used, or the readout names a different bar
        // from the one under the cursor on every lane but the busiest.
        uint32_t t0 = 0, t1 = 0; size_t refBars = 0;
        if (!triggerlog::windowSpan(lanes_, first_, count_, t0, t1, refBars)) break;
        const float frac = (mx - plotRect_.x) / plotRect_.width;
        if (frac < 0.f || frac > 1.f) break;
        const auto tAt = static_cast<uint32_t>(
            static_cast<float>(t0) + frac * static_cast<float>(t1 - t0));
        // The bar nearest that instant, so a sparse lane still answers a hover anywhere near it
        // rather than only on the exact pixel column its edge landed in.
        const auto it = std::lower_bound(lane.bars.begin(), lane.bars.end(), tAt,
                        [](const triggerlog::Interval& iv, uint32_t v) { return iv.tUs < v; });
        if (lane.bars.empty()) break;
        size_t idx = static_cast<size_t>(it - lane.bars.begin());
        if (idx >= lane.bars.size()) idx = lane.bars.size() - 1;
        if (idx > 0) {
            const uint32_t dPrev = tAt - lane.bars[idx - 1].tUs;
            const uint32_t dHere = lane.bars[idx].tUs - tAt;
            if (dPrev < dHere) --idx;
        }
        if (lane.bars[idx].tUs < t0 || lane.bars[idx].tUs > t1) break;   // nothing here in this span
        const triggerlog::Interval& iv = lane.bars[idx];
        char b[176];
        // The bar's ORDINAL and its duration. Not "tooth 5 at 50 degrees" — the log knows neither
        // which tooth this is nor that teeth are what it is looking at.
        std::snprintf(b, sizeof(b), "%s  \xC2\xB7  bar %zu  \xC2\xB7  %.3f ms since the previous edge"
                                    "  \xC2\xB7  line went %s  \xC2\xB7  t+%.1f ms%s",
                      lane.name.c_str(), idx + 1, iv.deltaUs / 1000.0,
                      iv.high ? "HIGH" : "LOW", iv.tUs / 1000.0,
                      iv.gapBefore ? "  \xC2\xB7  RECORDS MISSING BEFORE THIS BAR" : "");
        hover_ = b;
        break;
    }
    invalidate();
}
