#include "AutotuneView.h"
#include "WrapText.h"

#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/graphics/RenderPrimitive.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

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
        (void)lh;
        return;
    }

    // ---- the title line: what is being shown, and what its axes are ---------------------------------
    const char* modeName = mode_ == Mode::Base     ? "Base VE"
                         : mode_ == Mode::Proposed ? "Proposed VE"
                         : mode_ == Mode::Weight   ? "Records per cell"
                                                   : "Change %";
    std::string title = modeName;
    if (!yLabel_.empty() || !xLabel_.empty()) title += "    " + yLabel_ + " x " + xLabel_;
    text(r.x + kPad, r.y + 2.f, title, Colors::TextSecondary, r.width - kPad * 2.f);

    const float gx = r.x + kPad + kAxisW;
    const float gy = r.y + kTitleH;
    const float gw = r.width - kPad * 2.f - kAxisW;
    const float gh = r.height - kTitleH - kAxisH - kPad;
    if (gw < 40.f || gh < 30.f) return;
    const float cw = gw / static_cast<float>(cols);
    const float ch = gh / static_cast<float>(rows);

    // The value range Base/Proposed are shaded against — taken over BOTH so the two modes share one
    // scale. Shading each against its own range would make an unchanged map and a rewritten one look
    // exactly alike, which is the one comparison this view exists to support.
    double lo = 1e300, hi = -1e300;
    if (eng_->hasBase())
        for (int rr = 0; rr < rows; ++rr)
            for (int cc = 0; cc < cols; ++cc)
                for (double v : { eng_->base(cc, rr), eng_->proposed(cc, rr) }) {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
    const double fullW = autotune::fullWeightOf(eng_->settings().resistance);
    // THE COLOUR SCALE IS THE RUN'S OWN, not the authority limit. Shading a 6 % change against a 50 %
    // ceiling paints it almost the same grey as an untouched cell — the map looked empty on a bench run
    // that had in fact found exactly what it was set up to find. So the strongest change in the
    // proposal is full colour and everything else is read against it, with a floor so that a map which
    // is nearly right does not get its rounding errors painted like a discovery.
    const double authority = std::max(2.0, eng_->stats().maxChange);

    const std::vector<double>& xb = eng_->xBins();
    const std::vector<double>& yb = eng_->yBins();

    for (int rr = 0; rr < rows; ++rr) {
        // Load ASCENDING UP the screen, the way every fuel map is drawn and every tuner reads one:
        // row 0 of storage is the bottom of the picture.
        const int   sr = rows - 1 - rr;
        const float y  = gy + rr * ch;
        for (int cc = 0; cc < cols; ++cc) {
            const float x = gx + cc * cw;
            const double pct = eng_->changePct(cc, sr);
            const double w   = eng_->weight(cc, sr);

            Rgba fill{};
            char vb[24] = "";
            switch (mode_) {
                case Mode::Base:
                    fill = heatColour(eng_->base(cc, sr), lo, hi);
                    std::snprintf(vb, sizeof vb, "%.1f", eng_->base(cc, sr));
                    break;
                case Mode::Proposed:
                    fill = heatColour(eng_->proposed(cc, sr), lo, hi);
                    std::snprintf(vb, sizeof vb, "%.1f", eng_->proposed(cc, sr));
                    break;
                case Mode::Weight: {
                    static const uint8_t none[4] = { 34, 34, 38, 255 }, some[4] = { 70, 160, 110, 255 };
                    fill = mix(none, some, fullW > 0.0 ? w / (fullW * 2.0) : 0.0);
                    if (w > 0.0) std::snprintf(vb, sizeof vb, "%.1f", w);
                    break;
                }
                case Mode::Change:
                    fill = changeColour(pct, authority);
                    if (pct != 0.0) std::snprintf(vb, sizeof vb, "%+.1f", pct);
                    break;
            }
            buf.pushRectangle(x, y, cw - 1.f, ch - 1.f, fill.v);

            // COVERAGE, in every mode. A cell that has met the evidence bar fills the sliver; one that
            // has seen a single record shows a stub, and the difference between the two claims is
            // visible without switching mode or reading a number.
            if (w > 0.0 && fullW > 0.0) {
                static const uint8_t cover[4] = { 235, 235, 240, 200 };
                const float frac = static_cast<float>(std::min(1.0, w / fullW));
                buf.pushRectangle(x, y + ch - 1.f - kCoverH, (cw - 2.f) * frac, kCoverH, cover);
            }

            if (vb[0]) {
                // Light text on a dark fill and dark on a light one, from the fill's own luminance —
                // a diverging scale runs through both and a fixed colour is unreadable at one end.
                const int lum = (std::max({ fill.v[0], fill.v[1], fill.v[2] })
                               + std::min({ fill.v[0], fill.v[1], fill.v[2] })) / 2;
                static const uint8_t dark[4] = { 12, 12, 14, 255 }, light[4] = { 240, 240, 245, 255 };
                const float tw = JTextHelper::measureWidth(vb);
                text(x + std::max(1.f, (cw - tw) * 0.5f), y + (ch - lh) * 0.5f, vb,
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

    // ---- the column breakpoints --------------------------------------------------------------------
    for (int cc = 0; cc < cols; ++cc) {
        char xb_s[24];
        std::snprintf(xb_s, sizeof xb_s, "%.0f", xb[static_cast<size_t>(cc)]);
        const float tw = JTextHelper::measureWidth(xb_s);
        text(gx + cc * cw + std::max(0.f, (cw - tw) * 0.5f), gy + gh + 3.f, xb_s,
             Colors::TextSecondary, cw - 1.f);
    }
}
