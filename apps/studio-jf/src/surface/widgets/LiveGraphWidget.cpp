// LiveGraphWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "LiveGraphWidget.h"
#include "../../model/Cache.h"
#include "../Surface.h"   // Surface::onModified — hiding a trace is a run-mode edit, and it has to persist
#include <j/graphics/VectorGraphics.h>
#include "../../model/LineGraphModel.h"
#include <deque>

void LiveGraphWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    const PanelElement& el = *m_el;   // base drawBackground already painted the card
    // Keyed by UID (an element id is unique per model; this map is global) — see TableWidget::tableCursors.
    static std::unordered_map<std::string, std::vector<std::deque<float>>> histM;   // per-element, per-line history
    uint8_t fgb[4]; const uint8_t* fg = fgOf(el, fgb);
    auto numD = [&](const char* k, double d) { const std::string s = el.prop(k); if (s.empty()) return d; try { return std::stod(s); } catch (...) { return d; } };
    const int maxPts = std::max(2, static_cast<int>(numD("maxPoints", 300)));
    struct L { std::string channel; double min, max; bool aMin, aMax; bool hidden; uint8_t col[4]; };
    std::vector<L> lines;
    const LineGraphModel lm = LineGraphModel::fromCompact(el.prop("lines"));
    if (!lm.empty()) {
        for (size_t i = 0; i < lm.lines.size(); ++i) {
            L l; l.channel = lm.lines[i].channel; l.min = lm.lines[i].min; l.max = lm.lines[i].max;
            l.aMin = lm.lines[i].autoMin; l.aMax = lm.lines[i].autoMax; l.hidden = lm.lines[i].hidden;
            LineGraphModel::colorFor(static_cast<int>(i), l.col);
            lines.push_back(std::move(l));
        }
    } else {
        uint8_t lcb[4]; const uint8_t* lc = elColor(el, "lineColor", jf::Colors::Accent, lcb);
        L l; l.channel = el.prop("signalName"); l.min = numD("minValue", 0); l.max = numD("maxValue", 100);
        l.aMin = el.prop("autoMin") == "1"; l.aMax = el.prop("autoMax") == "1"; l.hidden = false;
        l.col[0] = lc[0]; l.col[1] = lc[1]; l.col[2] = lc[2]; l.col[3] = lc[3];
        lines.push_back(std::move(l));
    }
    std::vector<std::deque<float>>& H = histM[el.uid];
    H.resize(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        std::deque<float>& q = H[i];
        if (lines[i].channel.empty()) q.clear();
        else { q.push_back(static_cast<float>(c.value(lines[i].channel))); while (static_cast<int>(q.size()) > maxPts) q.pop_front(); }
    }
    const float legendH = legendHeight(r);
    if (el.prop("showLegend") != "0" && jf::JTextHelper::hasAtlas()) {
        float lx = r.x + 4.f; const float sw = legendH * 0.5f;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string lbl = lines[i].channel.empty() ? "(unbound)" : lines[i].channel;
            const float sy = r.y + (legendH - sw) * 0.5f + 1.f;
            // THE EYE. Filled = drawn, hollow = hidden: the same swatch either way, so the entry keeps its
            // colour and its place and the legend still reads as a key to the plot.
            if (lines[i].hidden) {
                uint8_t dim[4] = { lines[i].col[0], lines[i].col[1], lines[i].col[2], 70 };
                buf.pushRectangle(lx, sy, sw, sw, dim, 0.f, 1.f, lines[i].col);
            } else {
                buf.pushRectangle(lx, sy, sw, sw, lines[i].col);
            }
            uint8_t txt[4] = { lines[i].col[0], lines[i].col[1], lines[i].col[2],
                               static_cast<uint8_t>(lines[i].hidden ? 110 : 255) };
            jf::JTextHelper::pushText(buf, lx + sw + 4.f, r.y + (legendH - jf::JTextHelper::lineHeight()) * 0.5f, lbl, txt, r.width);
            lx += sw + 6.f + jf::JTextHelper::measureWidth(lbl) + 10.f;
            if (lx > r.x + r.width - 20.f) break;
        }
    }
    const float px = r.x + 2.f, py = r.y + legendH + 2.f, pw = r.width - 4.f, ph = r.height - legendH - 4.f;
    if (pw <= 1.f || ph <= 1.f) return;
    jf::JVectorCanvas vg; vg.setAntiAlias(1.2f);
    if (el.prop("showGrid") != "0")
        for (int i = 1; i < 4; ++i) { const float gy = py + ph * i / 4.f; vg.drawLine(px, gy, px + pw, gy, 1.f, jf::JPaint::solid(jf::rgba(fg[0], fg[1], fg[2], 40))); }
    const float stepX = pw / static_cast<float>(maxPts - 1);
    for (size_t li = 0; li < lines.size(); ++li) {
        if (lines[li].hidden) continue;          // still recorded above, just not drawn
        std::deque<float>& q = H[li];
        if (q.size() < 2) continue;
        double lo = lines[li].min, hi = lines[li].max;
        if (lines[li].aMin || lines[li].aMax) {
            float mn = q.front(), mx = q.front(); for (float v : q) { mn = std::min(mn, v); mx = std::max(mx, v); }
            if (lines[li].aMin) lo = mn;
            if (lines[li].aMax) hi = mx;
        }
        if (!(hi > lo)) hi = lo + 1.0;
        const double span = hi - lo; const int n = static_cast<int>(q.size());
        std::vector<jf::JVectorCanvas::JVec2> pts; pts.reserve(n);
        for (int j = 0; j < n; ++j) {
            const float x = (px + pw) - (n - 1 - j) * stepX;
            const double t = std::clamp((static_cast<double>(q[j]) - lo) / span, 0.0, 1.0);
            pts.push_back({ x, (py + ph) - static_cast<float>(t) * ph });
        }
        vg.strokePolyline(pts, 1.5f, jf::JPaint::solid(jc(lines[li].col)));
    }
    vg.flush(buf);
}

// Which legend entry is under the pointer? Measured the same way the legend is drawn — swatch, gap, label,
// gap — because an eye you can see but not hit is worse than no eye at all.
int LiveGraphWidget::legendEntryAt(const jf::JRect& r, const std::vector<std::string>& labels, float mx, float my) {
    if (!jf::JTextHelper::hasAtlas()) return -1;
    const float legendH = legendHeight(r);
    if (my < r.y || my > r.y + legendH) return -1;
    float lx = r.x + 4.f; const float sw = legendH * 0.5f;
    for (size_t i = 0; i < labels.size(); ++i) {
        const float w = sw + 6.f + jf::JTextHelper::measureWidth(labels[i]);
        if (mx >= lx && mx < lx + w) return static_cast<int>(i);
        lx += w + 10.f;
        if (lx > r.x + r.width - 20.f) break;
    }
    return -1;
}

bool LiveGraphWidget::handleControlInput(const jf::JRect& screen, const ControlInput& in) {
    if (in.kind != ControlInput::Kind::Press || !m_el) return false;
    const PanelElement& el = *m_el;
    if (el.prop("showLegend") == "0") return false;
    LineGraphModel lm = LineGraphModel::fromCompact(el.prop("lines"));
    if (lm.empty()) return false;                 // a single-signalName graph has nothing to toggle
    std::vector<std::string> labels;
    for (const auto& l : lm.lines) labels.push_back(l.channel.empty() ? "(unbound)" : l.channel);
    const int i = legendEntryAt(screen, labels, in.mx, in.my);
    if (i < 0) return false;
    lm.lines[static_cast<size_t>(i)].hidden = !lm.lines[static_cast<size_t>(i)].hidden;
    setOwnProp("lines", lm.toCompact());
    if (Surface::onModified) Surface::onModified();   // a run-mode layout edit still has to be saved
    invalidate();
    return true;
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("livegraph", LiveGraphWidget, 160);
