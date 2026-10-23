#include "AutotuneView.h"
#include "WrapText.h"

#include <j/config/Settings.h>

#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/graphics/VectorGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace jf;

namespace {

constexpr float kPad     = 6.f;
constexpr float kAxisW   = 54.f;    // the y-bin label column
constexpr float kAxisH   = 20.f;    // the x-bin label row
constexpr float kTitleH  = 18.f;
constexpr float kCoverH  = 3.f;     // the coverage sliver along the bottom of a cell

struct Rgba { uint8_t v[4]; };

Rgba mix(const uint8_t a[4], const uint8_t b[4], double t) {
    t = std::clamp(t, 0.0, 1.0);
    Rgba o{};
    for (int i = 0; i < 4; ++i)
        o.v[i] = static_cast<uint8_t>(a[i] + (b[i] - a[i]) * t);
    return o;
}

// A DIVERGING scale, because the quantity diverges. A change of zero is not "the bottom of a range",
// it is the middle — the cell is right — so it must read as the background does, and the two
// directions must be told apart at a glance rather than by their number's sign.
Rgba changeColour(double pct, double full) {
    static const uint8_t neutral[4] = { 52, 52, 56, 255 };
    static const uint8_t add[4]     = { 210, 70, 60, 255 };    // more fuel
    static const uint8_t take[4]    = { 60, 120, 210, 255 };   // less fuel
    const double t = full > 0.0 ? std::min(1.0, std::fabs(pct) / full) : 0.0;
    return mix(neutral, pct >= 0.0 ? add : take, t);
}

// A plain sequential ramp for a VE surface, the same cold-to-hot reading a fuel map is normally shown
// with, so Base and Proposed can be flicked between and compared.
Rgba heatColour(double v, double lo, double hi) {
    static const uint8_t cold[4] = { 46, 62, 110, 255 };
    static const uint8_t mid[4]  = { 60, 140, 110, 255 };
    static const uint8_t hot[4]  = { 200, 90, 60, 255 };
    if (!(hi > lo)) return Rgba{ { mid[0], mid[1], mid[2], mid[3] } };
    const double t = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
    return t < 0.5 ? mix(cold, mid, t * 2.0) : mix(mid, hot, (t - 0.5) * 2.0);
}

}  // namespace

AutotuneView::AutotuneView(JSceneGraph& g) : JWidget(g, "AutotuneView") {
    // The same three views, by the same names, as a table's Display ▸ View.
    static const char* const kViews[3] = { "2D Grid", "3D Surface", "Slice" };
    for (int i = 0; i < 3; ++i) {
        viewItems_[i] = menu_.add(g, kViews[i]);
        viewItems_[i]->setCheckable(true);
        viewItems_[i]->onTriggered.connect([this, i] { setLayout(static_cast<Layout>(i)); });
    }
    setContextMenu(&menu_);
    const int saved = JSettings::instance().get<int>("autotune.view", 0);
    layout_ = static_cast<Layout>(saved >= 0 && saved <= 2 ? saved : 0);
}

void AutotuneView::setLayout(Layout l) {
    layout_ = l;
    JSettings::instance().set("autotune.view", JVariant(static_cast<int>(l)));
    invalidate();
}

void AutotuneView::prepareContextMenu(float, float) {
    for (int i = 0; i < 3; ++i) viewItems_[i]->setChecked(static_cast<int>(layout_) == i);
}

// What one cell shows in the current mode. `height` is the same quantity on a 0..1 scale (−1..1 for the
// two signed modes, so a cell that took fuel away sinks below the floor in 3D rather than sitting on it).
struct AutotuneView::Cell {
    Rgba   fill{};
    char   label[24] = "";
    double height = 0.0;
};

namespace {
template <class Scales>
Scales scalesOf(const autotune::Engine& e, AutotuneView::Mode mode) {
    Scales s;
    const int rows = e.rows(), cols = e.cols();
    // The value range Base/Proposed are shaded against — taken over BOTH so the two modes share one
    // scale. Shading each against its own range would make an unchanged map and a rewritten one look
    // exactly alike, which is the one comparison this view exists to support.
    double lo = 1e300, hi = -1e300;
    if (e.hasBase())
        for (int rr = 0; rr < rows; ++rr)
            for (int cc = 0; cc < cols; ++cc)
                for (double v : { e.base(cc, rr), e.proposed(cc, rr) }) {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
    if (lo <= hi) { s.lo = lo; s.hi = hi; }
    s.fullW = autotune::fullWeightOf(e.settings().resistance);
    // THE COLOUR SCALE IS THE RUN'S OWN, not the authority limit. Shading a 6 % change against a 50 %
    // ceiling paints it almost the same grey as an untouched cell — the map looked empty on a bench run
    // that had in fact found exactly what it was set up to find. So the strongest change in the
    // proposal is full colour and everything else is read against it, with a floor so that a map which
    // is nearly right does not get its rounding errors painted like a discovery.
    s.authority = std::max(2.0, e.stats().maxChange);
    // The session has its own scale for the same reason: the biggest move so far is full colour.
    if (mode == AutotuneView::Mode::Session)
        for (int rr = 0; rr < rows; ++rr)
            for (int cc = 0; cc < cols; ++cc) s.sessionMax = std::max(s.sessionMax, std::fabs(e.sessionPct(cc, rr)));
    return s;
}
}  // namespace

AutotuneView::Cell AutotuneView::cellOf(int cc, int sr) const {
    Cell c;
    const Scales& s = scales_;
    const double span = s.hi > s.lo ? s.hi - s.lo : 1.0;
    switch (mode_) {
        case Mode::Base:
            c.fill = heatColour(eng_->base(cc, sr), s.lo, s.hi);
            c.height = (eng_->base(cc, sr) - s.lo) / span;
            std::snprintf(c.label, sizeof c.label, "%.1f", eng_->base(cc, sr));
            break;
        case Mode::Proposed:
            c.fill = heatColour(eng_->proposed(cc, sr), s.lo, s.hi);
            c.height = (eng_->proposed(cc, sr) - s.lo) / span;
            std::snprintf(c.label, sizeof c.label, "%.1f", eng_->proposed(cc, sr));
            break;
        case Mode::Weight: {
            static const uint8_t none[4] = { 34, 34, 38, 255 }, some[4] = { 70, 160, 110, 255 };
            const double w = eng_->weight(cc, sr);
            const double t = s.fullW > 0.0 ? w / (s.fullW * 2.0) : 0.0;
            c.fill = mix(none, some, t);
            c.height = std::clamp(t, 0.0, 1.0);
            if (w > 0.0) std::snprintf(c.label, sizeof c.label, "%.1f", w);
            break;
        }
        case Mode::Change: {
            const double pct = eng_->changePct(cc, sr);
            c.fill = changeColour(pct, s.authority);
            c.height = std::clamp(pct / s.authority, -1.0, 1.0);
            if (pct != 0.0) std::snprintf(c.label, sizeof c.label, "%+.1f", pct);
            break;
        }
        case Mode::Session: {
            // Every cell the session moved, and by how much in all — rounding noise below a
            // twentieth of a percent is left blank, as an untouched cell is.
            double sp = eng_->sessionPct(cc, sr);
            if (std::fabs(sp) < 0.05) sp = 0.0;
            c.fill = changeColour(sp, s.sessionMax);
            c.height = std::clamp(sp / s.sessionMax, -1.0, 1.0);
            if (sp != 0.0) std::snprintf(c.label, sizeof c.label, "%+.1f", sp);
            break;
        }
    }
    return c;
}

void AutotuneView::populateRenderPrimitives(JPrimitiveBuffer& buf) {
    const JRect r = bounds();
    buf.pushRectangle(r.x, r.y, r.width, r.height, Colors::Surface0);
    if (!JTextHelper::hasAtlas()) return;

    const float lh = JTextHelper::lineHeight();
    const auto text = [&](float x, float y, const std::string& s, const uint8_t* c, float maxW) {
        JTextHelper::pushText(buf, x, y, s, c, maxW);
    };

    const int cols = eng_ ? eng_->cols() : 0;
    const int rows = eng_ ? eng_->rows() : 0;
    if (!eng_ || cols < 1 || rows < 1 || !notice_.empty()) {
        const std::string msg = notice_.empty() ? "No table to tune" : notice_;
        const float mh = wraptext::height(msg, r.width - 16.f);   // wrapped, centred as a block
        wraptext::drawCentred(buf, r.x + 8.f, r.y + (r.height - mh) * 0.5f, r.width - 16.f, msg,
                              Colors::TextSecondary);
        return;
    }

    // ---- the title line: what is being shown, and what its axes are ---------------------------------
    const char* modeName = mode_ == Mode::Base     ? "Base"
                         : mode_ == Mode::Proposed ? "Proposed"
                         : mode_ == Mode::Weight   ? "Records per cell"
                         : mode_ == Mode::Session  ? "Session %  (since Start / Reset)"
                                                   : "Change %";
    std::string title = modeName;
    if (!yLabel_.empty() || !xLabel_.empty()) title += "    " + yLabel_ + " x " + xLabel_;
    if (layout_ == Layout::Surface) title += "    drag to rotate, wheel for height";
    text(r.x + kPad, r.y + 2.f, title, Colors::TextSecondary, r.width - kPad * 2.f);

    scales_ = scalesOf<Scales>(*eng_, mode_);

    const JRect body{ r.x, r.y + kTitleH, r.width, r.height - kTitleH };
    if (layout_ == Layout::Surface) { drawSurface(buf, body); return; }
    if (layout_ == Layout::Slice)   { drawSlice(buf, body);   return; }

    const float gx = r.x + kPad + kAxisW;
    const float gy = r.y + kTitleH;
    const float gw = r.width - kPad * 2.f - kAxisW;
    const float gh = r.height - kTitleH - kAxisH - kPad;
    if (gw < 40.f || gh < 30.f) return;
    const float cw = gw / static_cast<float>(cols);
    const float ch = gh / static_cast<float>(rows);
    gx_ = gx; gy_ = gy; cw_ = cw; ch_ = ch;
    const double fullW = scales_.fullW;

    const std::vector<double>& yb = eng_->yBins();
    const std::vector<double>& xb = eng_->xBins();

    for (int rr = 0; rr < rows; ++rr) {
        // Load ASCENDING UP the screen, the way every fuel map is drawn and every tuner reads one:
        // row 0 of storage is the bottom of the picture.
        const int   sr = rows - 1 - rr;
        const float y  = gy + rr * ch;
        for (int cc = 0; cc < cols; ++cc) {
            const float x = gx + cc * cw;
            const double w = eng_->weight(cc, sr);
            const Cell cell = cellOf(cc, sr);
            buf.pushRectangle(x, y, cw - 1.f, ch - 1.f, cell.fill.v);

            // COVERAGE, in every mode. A cell that has met the evidence bar fills the sliver; one that
            // has seen a single record shows a stub, and the difference between the two claims is
            // visible without switching mode or reading a number.
            if (w > 0.0 && fullW > 0.0) {
                static const uint8_t cover[4] = { 235, 235, 240, 200 };
                const float frac = static_cast<float>(std::min(1.0, w / fullW));
                buf.pushRectangle(x, y + ch - 1.f - kCoverH, (cw - 2.f) * frac, kCoverH, cover);
            }

            if (cell.label[0]) {
                // Light text on a dark fill and dark on a light one, from the fill's own luminance —
                // a diverging scale runs through both and a fixed colour is unreadable at one end.
                const Rgba& fill = cell.fill;
                const int lum = (std::max({ fill.v[0], fill.v[1], fill.v[2] })
                               + std::min({ fill.v[0], fill.v[1], fill.v[2] })) / 2;
                static const uint8_t dark[4] = { 12, 12, 14, 255 }, light[4] = { 240, 240, 245, 255 };
                const float tw = JTextHelper::measureWidth(cell.label);
                text(x + std::max(1.f, (cw - tw) * 0.5f), y + (ch - lh) * 0.5f, cell.label,
                     lum > 140 ? dark : light, cw - 2.f);
            }
        }
        // The row's own breakpoint.
        char yb_s[24];
        std::snprintf(yb_s, sizeof yb_s, "%.0f", yb[static_cast<size_t>(sr)]);
        text(r.x + kPad, y + (ch - lh) * 0.5f, yb_s, Colors::TextSecondary, kAxisW - 4.f);
    }

    // ---- the live cursor: where the last accepted record LANDED ------------------------------------
    // Not where the engine is. The gas just read was made some time ago, and this trailing the
    // operating point during a pull is the delay lookback visibly doing its job.
    if (eng_->lastCol() >= 0 && eng_->lastRow() >= 0) {
        const float cx = gx + eng_->lastCol() * cw;
        const float cy = gy + (rows - 1 - eng_->lastRow()) * ch;
        static const uint8_t hi_c[4] = { 255, 255, 255, 255 };
        buf.pushRectangle(cx, cy, cw - 1.f, ch - 1.f, Colors::Transparent, 0.f, 2.f, hi_c);
    }
    // The row the slice view will show, when one has been picked.
    if (sliceRow_ >= 0 && sliceRow_ < rows) {
        static const uint8_t sel[4] = { 255, 235, 60, 160 };
        buf.pushRectangle(gx, gy + (rows - 1 - sliceRow_) * ch, gw - 1.f, ch - 1.f, Colors::Transparent, 0.f, 1.f, sel);
    }

    // ---- the column breakpoints --------------------------------------------------------------------
    for (int cc = 0; cc < cols; ++cc) {
        char xb_s[24];
        std::snprintf(xb_s, sizeof xb_s, "%.0f", xb[static_cast<size_t>(cc)]);
        const float tw = JTextHelper::measureWidth(xb_s);
        text(gx + cc * cw + std::max(0.f, (cw - tw) * 0.5f), gy + gh + 3.f, xb_s,
             Colors::TextSecondary, cw - 1.f);
    }
}

// ---- 3D: the same cells as a surface, in the same colours ---------------------------------------------
// Storage row 0 (lowest load) is nearest the viewer. Signed modes (Change, Session) rise and sink about
// a floor at zero, so "took fuel away" reads as a dip, not a short hill.
void AutotuneView::drawSurface(JPrimitiveBuffer& buf, const JRect& r) {
    const int cols = eng_->cols(), rows = eng_->rows();
    const float lh = JTextHelper::lineHeight();
    if (cols < 2 || rows < 2) {
        JTextHelper::pushText(buf, r.x + 8.f, r.y + r.height * 0.5f, "3D needs a 2-axis table", Colors::TextSecondary, r.width);
        return;
    }
    const bool signedMode = mode_ == Mode::Change || mode_ == Mode::Session;
    const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f + r.height * (signedMode ? 0.04f : 0.16f);
    const float sxy = std::min(r.width, r.height) * 0.82f / static_cast<float>(std::max(cols, rows));
    const float hPix = r.height * (signedMode ? 0.22f : 0.38f) * hScale_;
    const float yaw = yaw_ * 3.14159265f / 180.f, pitch = pitch_ * 3.14159265f / 180.f;
    const float cyaw = std::cos(yaw), syaw = std::sin(yaw), spitch = std::sin(pitch);
    using V2 = JVectorCanvas::JVec2;
    // Screen row: storage row flipped so low load is at the front.
    auto proj = [&](float col, float srow, float h, V2& o, float& depth) {
        const float X = col - (cols - 1) * 0.5f, Y = (rows - 1 - srow) - (rows - 1) * 0.5f;
        const float rx = X * cyaw - Y * syaw, ry = X * syaw + Y * cyaw;
        o.x = cx + rx * sxy; o.y = cy + ry * sxy * spitch - h * hPix; depth = ry;
    };

    std::vector<Cell> cell(static_cast<size_t>(rows) * cols);
    for (int rr = 0; rr < rows; ++rr)
        for (int cc = 0; cc < cols; ++cc) cell[static_cast<size_t>(rr) * cols + cc] = cellOf(cc, rr);
    auto at = [&](int c, int rr) -> const Cell& { return cell[static_cast<size_t>(rr) * cols + c]; };

    JVectorCanvas vg; vg.setAntiAlias(1.2f);
    {   // the floor grid
        V2 p0, p1; float d;
        const JPaint gc = JPaint::solid(rgba(58, 58, 70, 255));
        for (int cc = 0; cc < cols; cc += 2) { proj((float)cc, 0.f, 0.f, p0, d); proj((float)cc, (float)rows - 1, 0.f, p1, d); vg.strokePolyline({ p0, p1 }, 0.8f, gc, false); }
        for (int rr = 0; rr < rows; rr += 2) { proj(0.f, (float)rr, 0.f, p0, d); proj((float)cols - 1, (float)rr, 0.f, p1, d); vg.strokePolyline({ p0, p1 }, 0.8f, gc, false); }
    }
    struct Q { float depth; V2 p[4]; uint8_t col[4]; int outline; };
    std::vector<Q> quads; quads.reserve(static_cast<size_t>(rows - 1) * (cols - 1));
    const float lx = 0.35f, ly = -0.45f, lz = 0.82f, hModel = std::max(rows, cols) * 0.45f * hScale_;
    const int lc = eng_->lastCol(), lr = eng_->lastRow();
    for (int rr = 0; rr < rows - 1; ++rr)
        for (int cc = 0; cc < cols - 1; ++cc) {
            const float z00 = (float)at(cc, rr).height,     z10 = (float)at(cc + 1, rr).height;
            const float z11 = (float)at(cc + 1, rr + 1).height, z01 = (float)at(cc, rr + 1).height;
            Q q; float dep = 0.f, dt;
            proj((float)cc, (float)rr, z00, q.p[0], dt); dep += dt;
            proj((float)cc + 1, (float)rr, z10, q.p[1], dt); dep += dt;
            proj((float)cc + 1, (float)rr + 1, z11, q.p[2], dt); dep += dt;
            proj((float)cc, (float)rr + 1, z01, q.p[3], dt); dep += dt;
            q.depth = dep * 0.25f;
            // Shade the quad's colour (the mean of its four corners') by a fixed light, so the shape
            // reads without changing what the colour means.
            const float uz = (z10 - z00) * hModel, vz = (z01 - z00) * hModel;
            const float nl = std::sqrt(uz * uz + vz * vz + 1.f) + 1e-6f;
            const float diff = std::clamp((-uz * lx + vz * ly + lz) / nl, 0.f, 1.f), bright = 0.6f + 0.4f * diff;
            for (int i = 0; i < 3; ++i) {
                const float m = (at(cc, rr).fill.v[i] + at(cc + 1, rr).fill.v[i]
                               + at(cc + 1, rr + 1).fill.v[i] + at(cc, rr + 1).fill.v[i]) * 0.25f;
                q.col[i] = static_cast<uint8_t>(std::clamp(m * bright * 1.15f, 0.f, 255.f));
            }
            q.col[3] = 255;
            q.outline = 0;
            if (sliceRow_ >= 0 && (rr == sliceRow_ || rr + 1 == sliceRow_)) q.outline = 1;
            if (lc >= 0 && lr >= 0 && (rr == lr || rr + 1 == lr) && (cc == lc || cc + 1 == lc)) q.outline = 2;
            quads.push_back(q);
        }
    std::sort(quads.begin(), quads.end(), [](const Q& a, const Q& b) { return a.depth < b.depth; });
    for (const Q& q : quads) {
        vg.fillConvex({ q.p[0], q.p[1], q.p[2], q.p[3] }, JPaint::solid(rgba(q.col[0], q.col[1], q.col[2], 255)));
        const std::vector<V2> ol{ q.p[0], q.p[1], q.p[2], q.p[3] };
        if (q.outline == 2)      vg.strokePolyline(ol, 2.0f, JPaint::solid(rgba(255, 255, 255, 255)), true);
        else if (q.outline == 1) vg.strokePolyline(ol, 1.0f, JPaint::solid(rgba(255, 235, 60, 160)), true);
        else                     vg.strokePolyline(ol, 0.5f, JPaint::solid(rgba(0, 0, 0, 60)), true);
    }
    vg.flush(buf);

    const int rstep = std::max(1, rows / 8), cstep = std::max(1, cols / 8);
    V2 pp; float dd; char lb[24];
    for (int rr = 0; rr < rows; rr += rstep) {
        proj(-0.6f, (float)rr, 0.f, pp, dd);
        std::snprintf(lb, sizeof lb, "%.0f", eng_->yBins()[static_cast<size_t>(rr)]);
        JTextHelper::pushText(buf, pp.x - 28.f, pp.y - lh * 0.5f, lb, Colors::TextSecondary, 40.f);
    }
    for (int cc = 0; cc < cols; cc += cstep) {
        proj((float)cc, -0.8f, 0.f, pp, dd);
        std::snprintf(lb, sizeof lb, "%.0f", eng_->xBins()[static_cast<size_t>(cc)]);
        JTextHelper::pushText(buf, pp.x - 10.f, pp.y, lb, Colors::TextSecondary, 40.f);
    }
}

// ---- Slice: one load row as bars across the x axis -----------------------------------------------------
// The row is the one picked (a click in the grid, or the wheel here), else wherever the last record
// landed. In Base and Proposed both are drawn as lines over the bars, so the row's before and after
// can be read against each other directly.
void AutotuneView::drawSlice(JPrimitiveBuffer& buf, const JRect& r) {
    const int cols = eng_->cols(), rows = eng_->rows();
    const float lh = JTextHelper::lineHeight();
    const int sr = sliceRow_ >= 0 ? std::min(sliceRow_, rows - 1)
                 : eng_->lastRow() >= 0 ? eng_->lastRow() : rows / 2;
    // Clear of the caption by half a line, so the top gridline's value (drawn centred on it) does not
    // run into it.
    const float plotX = r.x + kPad + kAxisW, plotY = r.y + lh * 2.f + 4.f;
    const float plotW = r.width - kPad * 2.f - kAxisW, plotH = r.height - lh * 3.f - kAxisH;
    if (plotW < 40.f || plotH < 30.f) return;
    const float bw = plotW / static_cast<float>(cols);
    const bool signedMode = mode_ == Mode::Change || mode_ == Mode::Session;
    // Heights are cellOf's: 0..1 from the bottom, or −1..1 about the middle for the signed modes.
    auto yOf = [&](double h) {
        return signedMode ? plotY + plotH * 0.5f - static_cast<float>(h) * plotH * 0.5f
                          : plotY + plotH - static_cast<float>(std::clamp(h, 0.0, 1.0)) * plotH;
    };

    const Scales& s = scales_;
    // Gridlines and their values, in the mode's own units.
    for (int i = 0; i <= 4; ++i) {
        const float yy = plotY + plotH * i / 4.f;
        static const uint8_t gl[4] = { 60, 60, 70, 255 };
        buf.pushRectangle(plotX, yy, plotW, 1.f, gl);
        const double f = 1.0 - i / 4.0;   // 1 at the top
        double v = 0.0;
        switch (mode_) {
            case Mode::Base: case Mode::Proposed: v = s.lo + (s.hi - s.lo) * f; break;
            case Mode::Weight:  v = s.fullW * 2.0 * f; break;
            case Mode::Change:  v = s.authority * (f * 2.0 - 1.0); break;
            case Mode::Session: v = s.sessionMax * (f * 2.0 - 1.0); break;
        }
        char vb[24];
        std::snprintf(vb, sizeof vb, signedMode ? "%+.1f" : "%.1f", v);
        JTextHelper::pushText(buf, r.x + kPad, yy - lh * 0.5f, vb, Colors::TextSecondary, kAxisW - 4.f);
    }

    const float base0 = yOf(0.0);
    for (int cc = 0; cc < cols; ++cc) {
        const Cell c = cellOf(cc, sr);
        const float y = yOf(c.height);
        Rgba fill = c.fill; fill.v[3] = 200;
        buf.pushRectangle(plotX + cc * bw + 1.f, std::min(y, base0), bw - 2.f, std::max(1.f, std::fabs(base0 - y)), fill.v);
        if (cc == eng_->lastCol() && sr == eng_->lastRow()) {
            static const uint8_t hi_c[4] = { 255, 255, 255, 255 };
            buf.pushRectangle(plotX + cc * bw, plotY, bw, plotH, Colors::Transparent, 0.f, 1.5f, hi_c);
        }
    }
    if ((mode_ == Mode::Base || mode_ == Mode::Proposed) && cols >= 2) {
        const double span = s.hi > s.lo ? s.hi - s.lo : 1.0;
        std::vector<JVectorCanvas::JVec2> bl, pl;
        for (int cc = 0; cc < cols; ++cc) {
            const float x = plotX + cc * bw + bw * 0.5f;
            bl.push_back({ x, yOf((eng_->base(cc, sr) - s.lo) / span) });
            pl.push_back({ x, yOf((eng_->proposed(cc, sr) - s.lo) / span) });
        }
        JVectorCanvas vg; vg.setAntiAlias(1.2f);
        vg.strokePolyline(bl, 1.6f, JPaint::solid(rgba(200, 200, 210, 255)), false);
        vg.strokePolyline(pl, 1.8f, JPaint::solid(rgba(120, 220, 120, 255)), false);
        vg.flush(buf);
    }

    const int cstep = std::max(1, cols / 12);
    for (int cc = 0; cc < cols; cc += cstep) {
        char xb_s[24];
        std::snprintf(xb_s, sizeof xb_s, "%.0f", eng_->xBins()[static_cast<size_t>(cc)]);
        JTextHelper::pushText(buf, plotX + cc * bw, plotY + plotH + 3.f, xb_s, Colors::TextSecondary, bw * cstep);
    }
    char cap[96];
    std::snprintf(cap, sizeof cap, "Row %.0f %s%s    wheel to change row",
                  eng_->yBins()[static_cast<size_t>(sr)], yLabel_.c_str(),
                  (mode_ == Mode::Base || mode_ == Mode::Proposed) ? "    grey = base, green = proposed" : "");
    JTextHelper::pushText(buf, r.x + kPad, r.y + 2.f, cap, Colors::TextPrimary, r.width - kPad * 2.f);
}

// ---- input --------------------------------------------------------------------------------------------
void AutotuneView::handleMousePress(float mx, float my) {
    pressed_ = true; dragged_ = false;
    pressX_ = lastX_ = mx; pressY_ = lastY_ = my;
}

void AutotuneView::handleMouseMove(float mx, float my) {
    if (!pressed_ || layout_ != Layout::Surface) return;
    if (std::fabs(mx - pressX_) + std::fabs(my - pressY_) > 4.f) dragged_ = true;
    if (!dragged_) return;
    yaw_ -= (mx - lastX_) * 0.5f;
    pitch_ = std::clamp(pitch_ + (my - lastY_) * 0.4f, 5.f, 85.f);
    lastX_ = mx; lastY_ = my;
    invalidate();
}

void AutotuneView::handleMouseRelease(float mx, float my) {
    const bool click = pressed_ && !dragged_;
    pressed_ = false;
    // A click on a grid cell picks its row for the slice (again to clear it, back to following).
    if (!click || layout_ != Layout::Grid || !eng_ || cw_ <= 0.f || ch_ <= 0.f) return;
    const int cc = static_cast<int>((mx - gx_) / cw_), rr = static_cast<int>((my - gy_) / ch_);
    if (mx < gx_ || my < gy_ || cc >= eng_->cols() || rr >= eng_->rows()) return;
    const int sr = eng_->rows() - 1 - rr;
    sliceRow_ = sliceRow_ == sr ? -1 : sr;
    invalidate();
}

bool AutotuneView::handleScroll(float, float, float wheel) {
    if (!eng_ || eng_->rows() < 1) return false;
    if (layout_ == Layout::Surface) {
        hScale_ = std::clamp(hScale_ * (wheel > 0 ? 1.12f : 1.f / 1.12f), 0.2f, 4.f);
        invalidate();
        return true;
    }
    if (layout_ == Layout::Slice) {
        const int cur = sliceRow_ >= 0 ? sliceRow_ : eng_->lastRow() >= 0 ? eng_->lastRow() : eng_->rows() / 2;
        sliceRow_ = std::clamp(cur + (wheel > 0 ? 1 : -1), 0, eng_->rows() - 1);
        invalidate();
        return true;
    }
    return false;
}
