// LinearWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "LinearWidget.h"
#include "../../model/Cache.h"
#include <j/graphics/VectorGraphics.h>

void LinearWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache&) {
    const PanelElement& el = *m_el;   // base drawBackground already painted the card
    auto numD = [&](const char* k, double d) { const std::string s = el.prop(k); if (s.empty()) return d; try { return std::stod(s); } catch (...) { return d; } };
    const std::string bind = bindPath();
    const double val = dispV(el, evalSource(bind)), lo = dispV(el, Raw{numD("minValue", 0)}), hi = dispV(el, Raw{numD("maxValue", 100)});
    const double ratio = (hi > lo) ? std::clamp((val - lo) / (hi - lo), 0.0, 1.0) : 0.0;
    const bool inv = el.prop("inverted") == "1";
    const bool vert = el.prop("orientation") != "Horizontal";   // default Vertical
    const float sx = r.width / 1000.f, sy = r.height / 1000.f, sMin = std::min(sx, sy);
    const float barW = static_cast<float>(numD("barWidth", 0.6)), margin = 500.f * static_cast<float>(numD("glowRadius", 0));
    float vx, vy, vw, vh;
    if (vert) { const float w = 1000.f * barW; vx = 500.f - w * 0.5f; vy = margin; vw = w; vh = std::max(0.1f, 1000.f - 2.f * margin); }
    else      { const float h = 1000.f * barW; vx = margin; vy = 500.f - h * 0.5f; vw = std::max(0.1f, 1000.f - 2.f * margin); vh = h; }
    const float cornerV = static_cast<float>(numD("cornerRadius", 0));
    auto pushV = [&](float X, float Y, float W, float H, const uint8_t* fillC, float radV) {
        buf.pushRectangle(r.x + X * sx, r.y + Y * sy, W * sx, H * sy, fillC, radV * sMin);
    };
    // VALUE RANGES REACH THE BAR. Its colours are its OWN named props, so reading them through the plain
    // elColor() meant every band set on a bar was accepted, evaluated, matched — and ignored, on the part
    // that most wants to go red. The dial was given the band cascade for exactly this reason; the bar was
    // left behind, so an alert on one had to be faked by recolouring the card behind the other.
    //
    // Each part listens to the band channel that matches what it MEANS, the same mapping the dial uses:
    //   track <- bg      the groove the bar runs in
    //   fill  <- accent  the part that says how much, and the one an alert wants
    //   bezel <- border  its rim is a border
    //   glow  <- fg      a bar prints no number, so fg is free, and a halo is the most alert-like thing
    //                    it draws. Peak stays local: it is detail, not state, and no channel is left.
    auto band = [&](const char* key, BandChannel ch, const uint8_t* fb2, uint8_t out[4]) {
        return elColorBanded(el, key, ch, fb2, out);
    };
    uint8_t db[4], fb[4], bzb[4];
    static const uint8_t kDial[4] = {40, 40, 40, 255}, kFill[4] = {0, 200, 255, 255}, kBezel[4] = {80, 80, 80, 255};
    const uint8_t* dial = band("dialColor", BandChannel::Bg,     kDial, db);
    const uint8_t* fill = band("fillColor", BandChannel::Accent, kFill, fb);
    const bool showFill = el.prop("showFill") != "0", segFill = el.prop("segmentedFill") == "1";
    const int segs = static_cast<int>(numD("segmentCount", 0));
    const float segSpace = static_cast<float>(numD("segmentSpacing", 2));
    if (segs > 0) {
        const float totalLen = vert ? vh : vw;
        const float segLen = std::max(1.f, (totalLen - (segs - 1) * segSpace) / segs);
        const int lit = static_cast<int>(std::lround(ratio * segs));
        const float rr = std::max(0.f, std::min(cornerV, segLen * 0.5f));
        for (int i = 0; i < segs; ++i) {
            const bool on = segFill && showFill && i < lit;
            if (vert) {
                const float y = inv ? (vy + i * (segLen + segSpace)) : (vy + vh - (i + 1) * (segLen + segSpace) + segSpace);
                pushV(vx, y, vw, segLen, on ? fill : dial, rr);
            } else {
                const float x = inv ? (vx + vw - (i + 1) * (segLen + segSpace) + segSpace) : (vx + i * (segLen + segSpace));
                pushV(x, vy, segLen, vh, on ? fill : dial, rr);
            }
        }
    } else {
        pushV(vx, vy, vw, vh, dial, cornerV);
    }
    if (showFill && !segFill && hi > lo && ratio > 1e-4) {
        float fx = vx, fy = vy, fw = vw, fh = vh;
        if (vert) { const float h = vh * static_cast<float>(ratio); if (inv) fh = h; else { fy = vy + vh - h; fh = h; } }
        else      { const float w = vw * static_cast<float>(ratio); if (inv) { fx = vx + vw - w; fw = w; } else fw = w; }
        pushV(fx, fy, fw, fh, fill, cornerV);
    }
    if (el.prop("showPeak") != "0" && hi > lo) {
        const double pk = peakOf(el.uid, val, numD("peakHoldTime", 3000));
        const double pr = std::clamp((pk - lo) / (hi - lo), 0.0, 1.0);
        uint8_t pb[4]; static const uint8_t kPeak[4] = {255, 90, 90, 255};
        const uint8_t* pc = elColor(el, "peakColor", kPeak, pb);
        if (vert) { const float y = inv ? vy + vh * static_cast<float>(pr) - 2.f : vy + vh * (1.f - static_cast<float>(pr)) - 2.f;
                    pushV(vx, std::clamp(y, vy, vy + vh - 4.f), vw, 4.f, pc, 0.f); }
        else      { const float x = inv ? vx + vw * (1.f - static_cast<float>(pr)) - 2.f : vx + vw * static_cast<float>(pr) - 2.f;
                    pushV(std::clamp(x, vx, vx + vw - 4.f), vy, 4.f, vh, pc, 0.f); }
    }
    const float bx = r.x + vx * sx, by = r.y + vy * sy, bw = vw * sx, bh = vh * sy, crS = cornerV * sMin;
    jf::JVectorCanvas vg; vg.setAntiAlias(1.2f);
    { const float glowR = static_cast<float>(numD("glowRadius", 0)) * 500.f * sMin;
      if (glowR > 1.f) {
          uint8_t gb2[4]; static const uint8_t kGlow[4] = {0, 200, 255, 255};
          const uint8_t* gc = band("glowColor", BandChannel::Fg, kGlow, gb2);
          for (int i = 1; i <= 4; ++i) {
              const float t = glowR * static_cast<float>(i) / 4.f;
              vg.strokeRoundedRect(bx - t * 0.5f, by - t * 0.5f, bw + t, bh + t, crS + t * 0.5f, glowR / 4.f,
                                   jf::JPaint::solid(jf::rgba(gc[0], gc[1], gc[2], static_cast<uint8_t>(64 / i))));
          }
      } }
    const float bezelT = static_cast<float>(numD("bezelWidth", 0.02)) * 1000.f * sMin;
    if (bezelT > 0.4f) vg.strokeRoundedRect(bx, by, bw, bh, crS, bezelT,
                                           jf::JPaint::solid(jc(band("bezelColor", BandChannel::Border, kBezel, bzb))));
    if (el.prop("showGlass") != "0") {
        const jf::JPaint glass = vert ? jf::JPaint::linear(bx, by, jf::rgba(255, 255, 255, 40), bx + bw, by, jf::rgba(255, 255, 255, 8))
                                      : jf::JPaint::linear(bx, by, jf::rgba(255, 255, 255, 40), bx, by + bh, jf::rgba(255, 255, 255, 8));
        vg.fillRoundedRect(bx, by, bw, bh, crS, glass);
    }
    vg.flush(buf);
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("gauge", LinearWidget, 10);
