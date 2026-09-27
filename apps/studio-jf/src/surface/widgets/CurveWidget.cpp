// CurveWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "CurveWidget.h"
#include "../../ui/WrapText.h"
#include "../../model/Cache.h"
#include "../../model/MathEvaluator.h"
#include "../../model/Keymap.h"
#include <j/graphics/VectorGraphics.h>
#include <cstdio>
#include <algorithm>
#include <cmath>

// The bound Y row, clamped so a 2D table always shows a VALID row even after the axis is resized down or
// the property is left at a stale value. A 1D curve (one row) always resolves to 0.
int CurveWidget::curveRow(const Cache& c, const TableImage& t, const std::string& rowExpr) const {
    const int rows = (t.valid && t.axes.size() >= 2) ? c.tiLiveN(t, 1) : 1;
    // Sigil-aware: a plain "4" evaluates to 4, an expression to its live value. The clamp then keeps it
    // valid regardless — a stale index, a resized-down axis, or a sigil that swings out of range all fall
    // back to a real row rather than reading past the table.
    const double v = rowExpr.empty() ? 0.0 : evalSource(rowExpr).v;
    const int idx = std::isfinite(v) ? static_cast<int>(std::lround(v)) : 0;   // NaN (bad expr) -> row 0
    return std::clamp(idx, 0, std::max(0, rows - 1));
}

bool CurveWidget::declRange(const std::string& s, double& lo, double& hi) {
    const size_t c = s.find(',');
    if (c == std::string::npos) return false;
    // Each end is a number, or an EXPRESSION over the tune ("ts.cltHighXaxis"): an imported definition can
    // scale an axis to another setting, and the studio's evaluator reads a binding the same way.
    auto side = [](std::string v) {
        try { size_t used = 0; const double d = std::stod(v, &used);
              while (used < v.size() && std::isspace(static_cast<unsigned char>(v[used]))) ++used;
              if (used == v.size()) return d; }
        catch (...) {}
        return MathEvaluator::instance().evaluate(v);
    };
    lo = side(s.substr(0, c)); hi = side(s.substr(c + 1));
    return hi > lo;
}

CurveWidget::Plot CurveWidget::plotOf(const jf::JRect& r, const std::vector<double>& xs,
                                      const std::vector<double>& ys,
                                      const std::string& xDecl, const std::string& yDecl) {
    Plot g;
    const float lh = jf::JTextHelper::lineHeight();
    const bool  atlas = jf::JTextHelper::hasAtlas();
    // Margins: the left holds the tick numbers, the bottom a row of ticks PLUS the X title, and the top a
    // line for the Y title. Sized from the font so nothing has to overlap anything.
    const float ml = 42.f, mr = 10.f;
    const float mt = atlas ? lh + 2.f : 8.f;
    const float mb = atlas ? lh * 2.f + 6.f : 8.f;
    g.px = r.x + ml; g.py = r.y + mt; g.pw = r.width - ml - mr; g.ph = r.height - mt - mb;
    if (g.pw <= 4.f || g.ph <= 4.f) return g;
    if (xs.size() < 2 || ys.size() != xs.size()) return g;
    g.xlo = xs.front(); g.xhi = (xs.back() > g.xlo) ? xs.back() : g.xlo + 1.0;
    double ylo = 1e300, yhi = -1e300;
    for (double v : ys) { ylo = std::min(ylo, v); yhi = std::max(yhi, v); }
    if (!(yhi > ylo)) yhi = ylo + 1.0;
    const double padY = (yhi - ylo) * 0.06;
    g.ylo = ylo - padY; g.yhi = yhi + padY;
    // A DECLARED range wins over the data's. An untuned curve is all zeros, and auto-ranging that gives a
    // flat line between -0.06 and 1.06 — technically the data, but nothing you can tune against.
    double lo = 0, hi = 0;
    if (declRange(xDecl, lo, hi)) { g.xlo = lo; g.xhi = hi; }
    if (declRange(yDecl, lo, hi)) { g.ylo = lo; g.yhi = hi; }
    g.ok = true;
    return g;
}

void CurveWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    const PanelElement& el = *m_el;   // base drawBackground already painted the card
    uint8_t fgb[4], lcb[4]; const uint8_t* fg = fgOf(el, fgb);
    const uint8_t* lc = elColor(el, "lineColor", jf::Colors::Accent, lcb);
    const std::string bind = bindPath();   // "[*]" resolves against the host viewport's sensor
    const float lh = jf::JTextHelper::lineHeight();
    auto placeholder = [&] {
        if (jf::JTextHelper::hasAtlas())
            wraptext::drawCentred(buf, r.x + 4.f, r.y + (r.height - wraptext::height("Drop a curve here", r.width - 8.f)) * 0.5f,
                                  r.width - 8.f, "Drop a curve here", jf::Colors::TextSecondary);   // wraps in a narrow box
    };
    const bool haveAtlas = jf::JTextHelper::hasAtlas();
    const Plot frame = plotOf(r, {}, {});                     // rect (label margins) for the frame
    const float px = frame.px, py = frame.py, pw = frame.pw, ph = frame.ph;
    if (pw <= 4.f || ph <= 4.f) { placeholder(); return; }

    jf::JVectorCanvas vg; vg.setAntiAlias(1.2f);
    const jf::JPaint frameP = jf::JPaint::solid(jf::rgba(fg[0], fg[1], fg[2], 130));
    const jf::JPaint gridP  = jf::JPaint::solid(jf::rgba(fg[0], fg[1], fg[2], 40));
    // Grid: the DECLARED divisions when the definition states them, else the plain quarters this always
    // drew.
    int gcols = 0, grows = 4;
    if (const std::string gs = el.prop("grid"); !gs.empty()) {
        const size_t c = gs.find(',');
        if (c != std::string::npos) { gcols = std::atoi(gs.c_str()); grows = std::atoi(gs.c_str() + c + 1); }
    }
    // A definition's "numDivisions" counts the LINES, not the gaps between them: "yAxis = 0, 8000, 9" is the
    // nine labels 0, 1000 .. 8000. Treating it as gaps put a tick every 889 rpm — a grid nobody reads off.
    if (grows > 2) --grows;
    if (gcols > 1) --gcols;
    grows = std::clamp(grows, 2, 24); gcols = std::clamp(gcols, 0, 24);
    for (int i = 1; i < grows; ++i) { const float gy = py + ph * i / float(grows); vg.drawLine(px, gy, px + pw, gy, 1.f, gridP); }
    for (int i = 1; i < gcols; ++i) { const float gx = px + pw * i / float(gcols); vg.drawLine(gx, py, gx, py + ph, 1.f, gridP); }
    vg.drawLine(px,      py,      px + pw, py,      1.f, frameP);   // top
    vg.drawLine(px,      py + ph, px + pw, py + ph, 1.f, frameP);   // bottom
    vg.drawLine(px,      py,      px,      py + ph, 1.f, frameP);   // left
    vg.drawLine(px + pw, py,      px + pw, py + ph, 1.f, frameP);   // right (was missing -> looked clipped)

    const TableImage t = c.resolveTable(bind);
    const int n = (t.valid && !t.axes.empty() && t.cellSize > 0) ? c.tiLiveN(t, 0) : 0;
    if (bind.empty() || n < 2) { vg.flush(buf); placeholder(); return; }

    const int row = curveRow(c, t, el.prop("row"));
    const std::vector<double> xs = c.tiBins(t, 0);
    std::vector<double> ys(n);
    for (int i = 0; i < n; ++i) ys[i] = c.tiCell(t, i, row, 0);
    const Plot p = plotOf(r, xs, ys, el.prop("xRange"), el.prop("yRange"));
    if (!p.ok) { vg.flush(buf); placeholder(); return; }
    const double xlo = p.xlo, xhi = p.xhi, ylo = p.ylo, yhi = p.yhi;
    const int drag = curveDragIndex(el.uid);
    const int sel  = curveSelIndex(el.uid);              // persists after release; the keyboard-nudge target
    std::vector<jf::JVectorCanvas::JVec2> pts; pts.reserve(n);
    for (int i = 0; i < n; ++i) pts.push_back({ p.ptX(xs[i]), p.ptY(ys[i]) });
    vg.strokePolyline(pts, 1.8f, jf::JPaint::solid(jc(lc)));
    for (int i = 0; i < n; ++i) {
        const bool hot = (i == drag) || (i == sel);     // dragged OR keyboard-selected -> larger, foreground
        vg.fillCircle(pts[i].x, pts[i].y, hot ? 4.5f : 3.f, jf::JPaint::solid(hot ? jc(fg) : jc(lc)));
    }
    vg.flush(buf);

    if (haveAtlas) {
        const uint8_t* tc = jf::Colors::TextSecondary;
        const double ym = std::max(std::fabs(yhi), std::fabs(ylo));
        const int dec = ym >= 100.0 ? 0 : (ym >= 10.0 ? 1 : 2);
        char b[32];
        auto yLabel = [&](double v, float cy) {
            std::snprintf(b, sizeof(b), "%.*f", dec, v);
            const float w = jf::JTextHelper::measureWidth(b);
            jf::JTextHelper::pushText(buf, std::max(r.x + 2.f, px - 5.f - w), cy, b, tc, px - r.x);
        };
        // Ticks at every GRID line, not just the ends — the grid says where the divisions are, and a division
        // with no number on it is decoration. Two numbers in the corners left you counting squares to read
        // a value off the plot.
        for (int i = 0; i <= grows; ++i) yLabel(yhi - (yhi - ylo) * i / double(grows),
                                                py + ph * i / float(grows) - lh * 0.5f + (i == 0 ? 4.f : 0.f));
        const int xd = (std::max(std::fabs(xlo), std::fabs(xhi)) >= 100.0) ? 0 : 1;
        const int xticks = gcols > 0 ? gcols : 1;
        float lastRight = -1e9f;
        for (int i = 0; i <= xticks; ++i) {
            const double v = xlo + (xhi - xlo) * i / double(xticks);
            std::snprintf(b, sizeof(b), "%.*f", xd, v);
            const float tw = jf::JTextHelper::measureWidth(b);
            float tx = px + pw * i / float(xticks) - tw * 0.5f;
            tx = std::clamp(tx, px, px + pw - tw);
            if (tx < lastRight + 6.f) continue;                  // no crowding: skip a label that would collide
            jf::JTextHelper::pushText(buf, tx, py + ph + 3.f, b, tc, pw);
            lastRight = tx + tw;
        }
        // Axis TITLES ("Coolant" / "RPM Limit"): a curve of numbers says nothing about what it plots. X sits
        // centred under the plot; Y goes at the top of its own axis, where there is room beside the labels.
        const std::string xt = el.prop("xLabel"), yt = el.prop("yLabel");
        // Titles go where the ticks are not: X below its row of numbers, Y in the top-left ABOVE the plot,
        // where the topmost tick label no longer sits (it moved onto the grid line with the others).
        if (!xt.empty() && r.y + r.height - (py + ph + 3.f + lh) > lh * 0.4f)
            jf::JTextHelper::pushText(buf, px + (pw - jf::JTextHelper::measureWidth(xt)) * 0.5f,
                                      py + ph + 3.f + lh, xt, tc, pw);
        if (!yt.empty())
            jf::JTextHelper::pushText(buf, r.x + 2.f, r.y + 1.f, yt, tc, r.width - 4.f);
    }
}

bool CurveWidget::handleControlInput(const jf::JRect& r, const ControlInput& in) {
    const PanelElement& el = *m_el;
    Cache& c = Cache::instance();
    const TableImage t = c.resolveTable(bindPath());
    if (!t.valid || t.axes.empty() || t.cellSize <= 0) return false;
    const int n = c.tiLiveN(t, 0);
    if (n < 2) return false;
    const int row = curveRow(c, t, el.prop("row"));
    std::vector<double> xs = c.tiBins(t, 0);
    std::vector<double> ys(n);
    for (int i = 0; i < n; ++i) ys[i] = c.tiCell(t, i, row, 0);
    const Plot p = plotOf(r, xs, ys, el.prop("xRange"), el.prop("yRange"));   // SAME geometry the render drew the handles with
    if (!p.ok) return false;
    const float px = p.px, py = p.py, pw = p.pw, ph = p.ph;
    const double xlo = p.xlo, xhi = p.xhi, ylo = p.ylo, yhi = p.yhi;
    auto ptAt = [&](int i) -> std::pair<float, float> { return { p.ptX(xs[i]), p.ptY(ys[i]) }; };
    if (in.kind == ControlInput::Kind::Press) {
        int best = -1; float bestD = 14.f * 14.f;
        for (int i = 0; i < n; ++i) { const auto [sx, sy] = ptAt(i); const float d = (sx - in.mx) * (sx - in.mx) + (sy - in.my) * (sy - in.my); if (d < bestD) { bestD = d; best = i; } }
        setCurveDragIndex(el.uid, best);
        if (best >= 0) {
            setCurveSelIndex(el.uid, best);   // remember it for keyboard nudges after release
            curveDragRanges()[el.uid] = { xlo, xhi, ylo, yhi };   // freeze the mapping so the write can't move it
        }
        return best >= 0;
    }
    // Keyboard value nudge on the selected point (Keymap Increase/Decrease, x10 for the "large" variants).
    // Step tracks the displayed value resolution so the nudge feels right per table (coarse for VE, fine for
    // lambda). No inline typing on a curve, so no need to suppress while editing.
    if (in.kind == ControlInput::Kind::Key && in.key) {
        double mult = 0.0;
        switch (Keymap::instance().action(*in.key)) {
            case Keymap::Action::IncreaseValue:      mult = +1.0;  break;
            case Keymap::Action::DecreaseValue:      mult = -1.0;  break;
            case Keymap::Action::IncreaseValueLarge: mult = +10.0; break;
            case Keymap::Action::DecreaseValueLarge: mult = -10.0; break;
            default: return false;
        }
        const int k = curveSelIndex(el.uid);
        if (k < 0 || k >= n) return false;
        const double ym  = std::max(std::fabs(yhi), std::fabs(ylo));
        const int    dec = ym >= 100.0 ? 0 : (ym >= 10.0 ? 1 : 2);   // same resolution the Y labels use
        const double step = std::pow(10.0, -dec);
        c.tiSetCell(t, k, row, 0, ys[k] + mult * step);
        return true;
    }
    if (in.kind == ControlInput::Kind::Move) {
        const int k = curveDragIndex(el.uid);
        if (k < 0 || k >= n) return false;
        // Read the mouse against the range FROZEN at Press, NOT the live auto-range — writing the point
        // back below re-derives the auto-range, which would move this mapping under us (see DragRange).
        double flx = xlo, fhx = xhi, fly = ylo, fhy = yhi;
        if (auto it = curveDragRanges().find(el.uid); it != curveDragRanges().end())
            { flx = it->second.xlo; fhx = it->second.xhi; fly = it->second.ylo; fhy = it->second.yhi; }
        double x = flx + (fhx - flx) * (in.mx - px) / pw;
        const double y = fly + (fhy - fly) * ((py + ph) - in.my) / ph;
        if (k > 0)     x = std::max(x, xs[k - 1]);
        if (k < n - 1) x = std::min(x, xs[k + 1]);
        xs[k] = std::clamp(x, flx, fhx);
        c.tiWriteBins(t, 0, xs);
        c.tiSetCell(t, k, row, 0, y);
        return true;
    }
    if (in.kind == ControlInput::Kind::Release) { setCurveDragIndex(el.uid, -1); curveDragRanges().erase(el.uid); return true; }
    return false;
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("curve", CurveWidget, 180);
