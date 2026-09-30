#include "KnockScopeView.h"
#include "WrapText.h"

#include <j/core/JTextHelper.h>
#include <j/core/JStyle.h>

#include <algorithm>
#include <cstdio>

using namespace jf;

namespace {
constexpr float kPadX = 10.f, kPadY = 10.f, kHeaderH = 46.f, kAxisW = 44.f, kAxisH = 18.f;

const uint8_t* verdictColour(uint8_t v) {
    static const uint8_t clean[4] = { 0x3f, 0xb9, 0x50, 255 };   // green
    static const uint8_t boot[4]  = { 0x9a, 0x9a, 0xa2, 255 };   // grey — listening, not judging
    static const uint8_t knock[4] = { 0xe0, 0x9a, 0x2a, 255 };   // amber
    static const uint8_t preig[4] = { 0xd6, 0x40, 0x40, 255 };   // red
    switch (v) {
        case KnockShot::Knocking:    return knock;
        case KnockShot::PreIgnition: return preig;
        case KnockShot::Bootstrapped:return boot;
        default:                     return clean;
    }
}
const char* verdictName(uint8_t v) {
    switch (v) {
        case KnockShot::Knocking:     return "KNOCK";
        case KnockShot::PreIgnition:  return "PRE-IGNITION";
        case KnockShot::Bootstrapped: return "learning (cell has no floor yet)";
        default:                      return "clean";
    }
}
} // namespace

void KnockScopeView::populateRenderPrimitives(JPrimitiveBuffer& buf) {
    const JRect r = bounds();
    buf.pushRectangle(r.x, r.y, r.width, r.height, Colors::Surface0);
    if (!JTextHelper::hasAtlas()) return;

    const float lh = JTextHelper::lineHeight();

    // ---- Header. The EVENT leads when there has been one: it is the thing worth reading, and the
    // live line underneath is what says the scope is running rather than stuck.
    // Each line WRAPS in a narrow pane, and the plot starts below however many lines that made.
    char line[208];
    const float tw = r.width - 2.f * kPadX;
    float hy = r.y + 4.f;
    const bool haveEvent = event_.valid && event_.seq;
    if (haveEvent) {
        std::snprintf(line, sizeof(line), "LAST EVENT   cyl %u   %s   %+.1f dB over floor%s",
                      unsigned(event_.cyl + 1), verdictName(event_.verdict), event_.over,
                      event_.preFrac >= 0.0f ? "" : "");
        hy += wraptext::draw(buf, r.x + kPadX, hy, line, verdictColour(event_.verdict), tw);
        if (event_.preFrac >= 0.0f) {
            std::snprintf(line, sizeof(line), "pre-spark energy %.0f%%", event_.preFrac * 100.0f);   // under "cyl"
            const float ix = JTextHelper::measureWidth("LAST EVENT   ");
            hy += wraptext::draw(buf, r.x + kPadX + ix, hy, line, Colors::TextSecondary, tw - ix);
        } else hy += lh;
    } else {
        hy += wraptext::draw(buf, r.x + kPadX, hy, "no knock or pre-ignition yet", Colors::TextSecondary, tw) + lh;
    }
    if (live_.valid && live_.seq) {
        std::snprintf(line, sizeof(line),
                      "live  cyl %u  %s   %.1f dB   floor %.1f   over %+.1f   thr %.1f   spark %.0f BTDC   #%u",
                      unsigned(live_.cyl + 1), verdictName(live_.verdict), live_.db, live_.floorDb,
                      live_.over, live_.threshold, live_.sparkDeg, unsigned(live_.seq));
        hy += wraptext::draw(buf, r.x + kPadX, hy, line, Colors::TextSecondary, tw);
    }
    const float headerH = std::max(kHeaderH, hy - r.y + 4.f);

    // TWO LINES UNDER THE PLOT: the angle ticks and the axis title. kAxisH was one line's room, so the
    // title was drawn below the pane's bottom edge and clipped away.
    const float axisH = std::max(kAxisH, 2.f * lh + 6.f);
    const JRect plot{ r.x + kPadX + kAxisW, r.y + headerH,
                      r.width - kPadX * 2.f - kAxisW, r.height - headerH - kPadY - axisH };
    if (plot.width <= 20.f || plot.height <= 20.f) return;
    buf.pushRectangle(plot.x, plot.y, plot.width, plot.height, Colors::Surface1, 2.f);

    // ---- Nothing to draw: say which nothing ----------------------------------------------------
    const char* note = !notice_.empty() ? notice_.c_str()
                     : (!live_.valid    ? "no classifier on this ECU"
                     : (!live_.seq      ? "nothing classified yet" : nullptr));
    if (note) {
        const float nh = wraptext::height(note, plot.width - 16.f);   // wrapped, centred as a block
        wraptext::drawCentred(buf, plot.x + 8.f, plot.y + (plot.height - nh) * 0.5f, plot.width - 16.f,
                              note, Colors::TextSecondary);
        return;
    }
    if (live_.buckets.empty() && event_.buckets.empty()) return;

    // ---- Vertical scale. Anchored on the FLOOR and the THRESHOLD, not on the data --------------
    // Autoscaling to the buckets alone would make every capture look the same: a quiet cycle and a
    // violent one would both fill the plot. The floor and the bar it has to clear are what give a
    // level meaning, so they set the range and the data is drawn against them.
    const KnockShot& ref = (live_.valid && live_.seq) ? live_ : event_;
    float lo = ref.floorDb - 6.0f, hi = ref.floorDb + ref.threshold + 6.0f;
    for (const KnockShot* s2 : { &live_, &event_ })
        for (float db : s2->buckets) { lo = std::min(lo, db - 2.0f); hi = std::max(hi, db + 2.0f); }
    if (hi - lo < 6.0f) hi = lo + 6.0f;
    auto yOf = [&](float db) { return plot.y + plot.height * (1.0f - (db - lo) / (hi - lo)); };

    // ---- The floor and the threshold: the two levels that decide the verdict --------------------
    const float yFloor = yOf(ref.floorDb);
    const float yThr   = yOf(ref.floorDb + ref.threshold);
    buf.pushRectangle(plot.x, yFloor, plot.width, 1.f, Colors::TextSecondary, 0.f);
    buf.pushRectangle(plot.x, yThr,   plot.width, 1.f, verdictColour(KnockShot::Knocking), 0.f);
    JTextHelper::pushText(buf, r.x + kPadX, yFloor - lh * 0.5f, "floor", Colors::TextSecondary, kAxisW);
    JTextHelper::pushText(buf, r.x + kPadX, yThr   - lh * 0.5f, "thr",   verdictColour(KnockShot::Knocking), kAxisW);

    // ---- The EVENT, as a held ghost: a thin marker per bucket, behind the live bars -------------
    if (haveEvent && !event_.buckets.empty()) {
        const size_t ne = event_.buckets.size();
        const float bwe = plot.width / static_cast<float>(ne);
        uint8_t ghost[4] = { verdictColour(event_.verdict)[0], verdictColour(event_.verdict)[1],
                             verdictColour(event_.verdict)[2], 90 };
        for (size_t i = 0; i < ne; ++i)
            buf.pushRectangle(plot.x + bwe * static_cast<float>(i) + 0.5f, yOf(event_.buckets[i]),
                              std::max(1.0f, bwe - 1.f), 2.f, ghost, 0.f);
    }

    // ---- The LIVE trace, solid ------------------------------------------------------------------
    const size_t n = live_.buckets.size();
    if (n) {
        const float bw = plot.width / static_cast<float>(n);
        const float thrDb = ref.floorDb + ref.threshold;
        for (size_t i = 0; i < n; ++i) {
            const float db = live_.buckets[i];
            const float y  = yOf(db);
            const float h  = std::max(1.0f, (plot.y + plot.height) - y);
            // Over the bar is tinted by verdict, under it muted: "this fired, and here is which part
            // of the window did it", without the reader comparing numbers to a line by eye.
            const uint8_t* c = (db > thrDb) ? verdictColour(live_.verdict) : Colors::TextSecondary;
            buf.pushRectangle(plot.x + bw * static_cast<float>(i) + 0.5f, y, std::max(1.0f, bw - 1.f), h, c, 0.f);
        }
    }

    // ---- The SPARK: which side of it the energy sits on IS the question -------------------------
    if (ref.hasPhase()) {
        const float span = ref.stepDeg * static_cast<float>(ref.buckets.size());
        const float sparkAngle = -ref.sparkDeg;                    // advance is BTDC; the profile is ATDC-positive (shown BTDC)
        const float t = (sparkAngle - ref.startDeg) / span;
        if (t >= 0.0f && t <= 1.0f) {
            const float x = plot.x + plot.width * t;
            buf.pushRectangle(x, plot.y, 1.f, plot.height, verdictColour(KnockShot::PreIgnition), 0.f);
            JTextHelper::pushText(buf, x + 3.f, plot.y + 2.f, "spark",
                                  verdictColour(KnockShot::PreIgnition), 60.f);
        } else {
            // The window does not reach the spark. Worth saying loudly: pre-ignition detection is
            // then structurally blind, and an empty pre-spark region looks identical to a clean one.
            wraptext::draw(buf, plot.x + 4.f, plot.y + 2.f,
                           "spark outside this window - pre-ignition cannot be seen",
                           verdictColour(KnockShot::PreIgnition), plot.width - 8.f);   // wraps over the plot
        }

        // Angle axis, in the units the window is configured in.
        char lab[32];
        for (int k = 0; k <= 4; ++k) {
            const float frac = static_cast<float>(k) / 4.0f;
            const float deg  = ref.startDeg + span * frac;
            const float x    = plot.x + plot.width * frac;
            std::snprintf(lab, sizeof(lab), "%+.0f", -deg + 0.0f);   // shown BTDC: + advanced, - retarded
            // Centred on its mark by its MEASURED width, and kept inside the pane: the last label sits on
            // the plot's right edge, and a fixed 10 px lead put half of "+50" past it.
            const float w  = JTextHelper::measureWidth(lab);
            const float lx = std::clamp(x - w * 0.5f, r.x + kPadX, r.x + r.width - kPadX - w);
            JTextHelper::pushText(buf, lx, plot.y + plot.height + 3.f, lab, Colors::TextSecondary, w + 2.f);
        }
        const char* title = "crank deg BTDC  (+ advanced, - retarded)";
        const float tw2   = JTextHelper::measureWidth(title);
        JTextHelper::pushText(buf, plot.x + (plot.width - tw2) * 0.5f, plot.y + plot.height + 3.f + lh,
                              title, Colors::TextSecondary, tw2 + 2.f);
    } else {
        wraptext::draw(buf, plot.x + 4.f, plot.y + 2.f,
                       "no phase stamp - bucket angles unknown", Colors::TextSecondary, plot.width - 8.f);
    }

    // dB gridline labels at the two anchors plus the top.
    char lab[24];
    std::snprintf(lab, sizeof(lab), "%.0f", hi);
    JTextHelper::pushText(buf, r.x + kPadX, plot.y, lab, Colors::TextSecondary, kAxisW);
}
