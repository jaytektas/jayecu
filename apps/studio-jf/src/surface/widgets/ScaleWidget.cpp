// ScaleWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "ScaleWidget.h"
#include "../../model/Cache.h"
#include "../../model/RangeBands.h"   // ColorRules::Zone — the spans a tick can fall inside
#include <cstdio>
#include <cmath>
#include <vector>
#include <utility>
#include <j/graphics/VectorGraphics.h>

// Nice-number tick interval for the auto-tick scale.
static double niceInterval(double range, int targetDivisions) {
    if (range == 0 || targetDivisions <= 1) return 1.0;
    const double delta = range / (targetDivisions - 1);
    const double exponent = std::floor(std::log10(delta));
    const double fraction = delta / std::pow(10.0, exponent);
    const double nf = (fraction < 1.5) ? 1.0 : (fraction < 3.0) ? 2.0 : (fraction < 7.0) ? 5.0 : 10.0;
    const double plain = nf * std::pow(10.0, exponent);

    // A ROUND INTERVAL THAT ALSO DIVIDES THE RANGE, when one exists at a sensible density. The interval
    // above is round but need not fit: 2000 into a 0..9000 tacho leaves the last major at 8000 and the
    // ring stopping 200 short of its own end. 1000 is just as round, divides nine times, and closes the
    // ring — which is what asking for 10 major ticks rather than 6 used to stumble onto by accident.
    //
    // Only from the same family of round numbers (1, 2, 5 x 10^n) and only within a factor of about two
    // of the density asked for, so "target 6" cannot quietly become sixty. Nothing qualifying: keep the
    // plain pick, because a round interval that misses the end still beats an unround one that hits it.
    const double want = static_cast<double>(targetDivisions - 1);          // intervals, not ticks
    double best = plain, bestErr = -1.0;
    for (int de = -1; de <= 1; ++de)
        for (const double f : { 1.0, 2.0, 5.0 }) {
            const double iv = f * std::pow(10.0, exponent + de);
            if (iv <= 0.0) continue;
            const double n = range / iv;                                    // how many fit
            if (std::fabs(n - std::round(n)) > 1e-9) continue;              // ...not a whole number of them
            if (n < want * 0.5 || n > want * 2.0) continue;                 // too coarse or too dense
            const double err = std::fabs(n - want);
            if (bestErr < 0.0 || err < bestErr) { bestErr = err; best = iv; }
        }
    return best;
}

void ScaleWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache&) {
    // Faithful ScaleWidget port (1000x1000 virtual space); base drawBackground already painted the card.
    const PanelElement& el = *m_el;
    auto numD = [&](const char* k, double d) { const std::string s = el.prop(k); if (s.empty()) return d; try { return std::stod(s); } catch (...) { return d; } };
    const double lo = dispV(el, Raw{numOrExpr(el.prop("minValue"), 0)}), hi = dispV(el, Raw{numOrExpr(el.prop("maxValue"), 100)}), range = hi - lo;
    if (range == 0) return;
    const float sx = r.width / 1000.f, sy = r.height / 1000.f, sMin = std::min(sx, sy);
    auto SX = [&](double x) { return r.x + static_cast<float>(x) * sx; };
    auto SY = [&](double y) { return r.y + static_cast<float>(y) * sy; };
    constexpr double kPi = 3.14159265358979;
    const bool circular = el.prop("scaleMode") != "Linear";
    const bool autoT = el.prop("autoTicks") != "0", inv = el.prop("inverted") == "1";
    const int minorPer = std::max(0, static_cast<int>(numD("minorTicks", 4)));
    // THE TICKS ARE VALUES, AND THERE IS ONE GRID OF THEM. Both modes below fill this list with
    // (value, is-major) and everything after works from it — which is the whole fix for what nice
    // numbers used to do.
    //
    // It used to run TWO grids at once: `interval` said "a major every 2000", while the loop laid
    // (majors-1)x(minors+1)+1 ticks spread EVENLY across the range and then rounded only the majors onto
    // the nice interval. On a 0..9000 tacho asking for 6 majors that is 21 ticks 450 apart, with the
    // majors dragged from 2250 to 2000, 4500 to 4000, 6750 to 6000 — off their own minors, which stayed
    // where they were — and the last one, 9000, rounded UP to 10000 and drawn a ninth of a turn past the
    // end of the arc. A scale reading past its own maximum, on ticks that no longer lined up.
    //
    // So the majors are placed ON the nice interval and the minors evenly BETWEEN them, one grid from one
    // step, and nothing is rounded afterwards. The grid stops at the last tick that fits: with a range
    // that is not a whole number of intervals the arc simply ends a little past its final tick, which is
    // what "round tick values" costs and is not the same as inventing one that is out of range.
    std::vector<std::pair<double, bool>> ticks;
    {
        const double absR = std::abs(range);
        const int    per  = minorPer + 1;                       // ticks per major, majors included
        if (autoT) {
            const double interval = niceInterval(absR, std::max(2, static_cast<int>(numD("targetMajorTicks", 6))));
            if (interval <= 0) return;
            const double step = interval / per;
            const int    n    = static_cast<int>(std::floor((absR + absR * 1e-9) / step));
            for (int i = 0; i <= n; ++i)
                ticks.push_back({ lo + (i * step / absR) * range, i % per == 0 });
        } else {
            const int majorCount = std::max(1, static_cast<int>(numD("majorTicks", 11)));
            const int total      = (majorCount - 1) * per + 1;
            for (int i = 0; i < total; ++i)
                ticks.push_back({ lo + (i / static_cast<double>(total - 1)) * range, i % per == 0 });
        }
    }
    if (ticks.size() < 2) return;
    const float majLen = static_cast<float>(numD("majorTickLength", 50)), minLen = static_cast<float>(numD("minorTickLength", 30));
    const float tw = static_cast<float>(numD("tickWidth", 4)), lblOff = static_cast<float>(numD("labelOffset", 80)), lblFs = static_cast<float>(numD("labelFontSize", 40));
    const bool showLabels = el.prop("showLabels") != "0";
    const std::string fmt = el.prop("format").empty() ? "%.0f" : el.prop("format");
    uint8_t mtb[4], ntb[4], lcb[4];
    // Unstyled ticks and labels follow the SCHEME. They were hard-coded white, which is right on a dark dial
    // and invisible on a light one — a gauge on the light theme had ticks you could not see and numbers you
    // could not read at all.
    const uint8_t* mc = elColor(el, "tickColor", jf::Colors::TextPrimary, mtb);
    const uint8_t* nc = elColor(el, "minorTickColor", jf::Colors::TextSecondary, ntb);
    const uint8_t* lc = elColor(el, "labelColor", jf::Colors::TextPrimary, lcb);
    const float lh0 = jf::JTextHelper::lineHeight();
    const float lblScale = (jf::JTextHelper::hasAtlas() && lh0 > 0.f) ? (lblFs * sMin) / lh0 : 1.f;
    // ZONES REACH THE TICKS. The dial under this scale paints the red arc; the scale is what puts the
    // NUMBERS in red beside it, which is half of what makes a redline read as one. Bounds arrive in the
    // authored space and are converted here, exactly as minValue/maxValue are, so a zone stays put when
    // the display unit changes. Ticks and labels are separately switchable: a face wanting red numbers
    // over plain white ticks is a real style, and so is the reverse.
    std::vector<ColorRules::Zone> zones = ColorRules::zonesFromCompact(el.prop("zones"));
    for (ColorRules::Zone& z : zones) {
        // numOrExpr first: a bound may be an expression, and it is resolved to a number here so that
        // zoneAt() below — and everything else that compares against a tick value — sees numbers.
        z.start = dispV(el, Raw{numOrExpr(z.startExpr, z.start)});
        z.end   = dispV(el, Raw{numOrExpr(z.endExpr,   z.end)});
        z.startExpr.clear(); z.endExpr.clear();           // resolved: nothing downstream re-evaluates
        if (z.end < z.start) std::swap(z.start, z.end);   // a converted span can come back reversed
    }
    const bool zoneTicks = el.prop("zoneTicks") != "0", zoneLabels = el.prop("zoneLabels") != "0";
    const double sd = numD("startAngle", 225), ed = numD("endAngle", -45);
    const bool vert = el.prop("orientation") == "Vertical";
    jf::JVectorCanvas vg; vg.setAntiAlias(1.2f);
    char lbuf[32];
    for (const auto& [val, isMaj] : ticks) {
        // Where the value sits along the arc. An inverted scale reverses the GEOMETRY, not the numbers.
        const double g = (val - lo) / range;
        const double ratio = inv ? 1.0 - g : g;
        const double len = isMaj ? majLen : minLen;
        uint8_t zbuf[4];
        const uint8_t* zc = nullptr;
        if (const ColorRules::Zone* z = ColorRules::zoneAt(zones, val))
            if (skin::parseHex(z->color, zbuf)) zc = zbuf;
        const uint8_t* col = (zc && zoneTicks) ? zc : (isMaj ? mc : nc);
        const uint8_t* lcol = (zc && zoneLabels) ? zc : lc;
        const float lw = (isMaj ? tw : tw * 0.7f) * sMin;
        if (circular) {
            const double angle = sd + ratio * (ed - sd), rad = angle * kPi / 180.0;
            const double cosA = std::cos(rad), sinA = -std::sin(rad), R = 500.0;
            vg.strokePolyline({ {SX(500 + R * cosA), SY(500 + R * sinA)}, {SX(500 + (R - len) * cosA), SY(500 + (R - len) * sinA)} }, lw, jf::JPaint::solid(jc(col)));
            if (isMaj && showLabels && jf::JTextHelper::hasAtlas()) {
                std::snprintf(lbuf, sizeof(lbuf), fmt.c_str(), val);
                const double lr = R - len - lblOff;
                const float twd = jf::JTextHelper::measureWidthScaled(lbuf, lblScale), lhh = lh0 * lblScale;
                jf::JTextHelper::pushTextScaled(buf, SX(500 + lr * cosA) - twd * 0.5f, SY(500 + lr * sinA) - lhh * 0.5f, lbuf, lcol, lblScale);
            }
        } else {
            const double margin = 50.0, len0 = 1000.0 - 2 * margin, pos = margin + ratio * len0;
            if (!vert) {
                vg.strokePolyline({ {SX(pos), SY(500)}, {SX(pos), SY(500 - len)} }, lw, jf::JPaint::solid(jc(col)));
                if (isMaj && showLabels && jf::JTextHelper::hasAtlas()) {
                    std::snprintf(lbuf, sizeof(lbuf), fmt.c_str(), val);
                    const float twd = jf::JTextHelper::measureWidthScaled(lbuf, lblScale), lhh = lh0 * lblScale;
                    jf::JTextHelper::pushTextScaled(buf, SX(pos) - twd * 0.5f, SY(500 - len - lblOff) - lhh * 0.5f, lbuf, lcol, lblScale);
                }
            } else {
                const double vPos = 1000.0 - pos;
                vg.strokePolyline({ {SX(500), SY(vPos)}, {SX(500 + len), SY(vPos)} }, lw, jf::JPaint::solid(jc(col)));
                if (isMaj && showLabels && jf::JTextHelper::hasAtlas()) {
                    std::snprintf(lbuf, sizeof(lbuf), fmt.c_str(), val);
                    const float lhh = lh0 * lblScale;
                    jf::JTextHelper::pushTextScaled(buf, SX(500 + len + lblOff), SY(vPos) - lhh * 0.5f, lbuf, lcol, lblScale);
                }
            }
        }
    }
    vg.flush(buf);
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("scale", ScaleWidget, 80);
