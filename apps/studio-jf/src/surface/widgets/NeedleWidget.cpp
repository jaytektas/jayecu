// NeedleWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "NeedleWidget.h"
#include "../../model/Cache.h"
#include <j/graphics/VectorGraphics.h>
#include "../../model/ImageCache.h"   // Needle Image Override — path -> texture, decoded once

void NeedleWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache&) {
    const PanelElement& el = *m_el;   // pure gauge, no value number; base drawBackground painted the card
    auto numD = [&](const char* k, double d) { const std::string s = el.prop(k); if (s.empty()) return d; try { return std::stod(s); } catch (...) { return d; } };
    const std::string bind = bindPath();
    const double val = dispV(el, evalSource(bind)), lo = dispV(el, Raw{numOrExpr(el.prop("minValue"), 0)}), hi = dispV(el, Raw{numOrExpr(el.prop("maxValue"), 100)});
    double frac = (hi > lo) ? std::clamp((val - lo) / (hi - lo), 0.0, 1.0) : 0.0;
    if (el.prop("inverted") == "1") frac = 1.0 - frac;
    const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f;
    const float s = std::min(r.width, r.height) / 1000.f;   // virtual -> screen
    if (s <= 0.f) return;
    constexpr float kPi = 3.14159265358979f;
    const double a0 = numD("startAngle", 225), a1 = numD("endAngle", -45);
    const float A = static_cast<float>((a0 + (a1 - a0) * frac) * kPi / 180.0);
    const float ca = std::cos(A), sa = std::sin(A);
    auto P = [&](float lx, float ly) -> jf::JVectorCanvas::JVec2 {
        return { cx + s * (sa * lx - ca * ly), cy + s * (ca * lx + sa * ly) };
    };
    const float nLen  = 450.f * static_cast<float>(numD("needleLength", 0.9));
    const float tLen  = 450.f * static_cast<float>(numD("tailLength", 0.15));
    const float halfW = static_cast<float>(numD("needleWidth", 30)) * 0.5f;
    const float hRad  = 500.f * static_cast<float>(numD("hubRadius", 0.05));
    uint8_t nb[4], hb[4];
    static const uint8_t kNeedle[4] = {220, 50, 50, 255}, kHub[4] = {60, 60, 60, 255};
    const uint8_t* nc = elColor(el, "needleColor", kNeedle, nb);
    const uint8_t* hc = elColor(el, "hubColor", kHub, hb);
    const uint8_t nd[4] = { static_cast<uint8_t>(nc[0] * 2 / 3), static_cast<uint8_t>(nc[1] * 2 / 3),
                            static_cast<uint8_t>(nc[2] * 2 / 3), 255 };
    jf::JVectorCanvas vg; vg.setAntiAlias(1.2f);

    // NEEDLE IMAGE OVERRIDE. The property existed and nothing read it -- a row in the inspector that
    // looked live and did nothing. Given a loadable file, the drawn needle IS that image, rotated to the
    // same angle the vector one would take, about the pivot the image declares.
    //
    // The pivot is in IMAGE space (0..1 across the file), because that is the only frame the artist has:
    // the point on the artwork that sits on the hub. 0.5,0.5 is the middle. Everything else follows --
    // the quad is placed so the pivot lands on the gauge centre, and rotated about it.
    //
    // Height is scaled to needleLength so image and vector needles agree on how far the pointer reaches,
    // and width follows the file's own aspect so nothing is stretched. A file that will not load leaves
    // tex null and the vector needle draws instead, so a bad path degrades to the default rather than to
    // an empty gauge with no clue why.
    const ImageCache::Entry& img = ImageCache::instance().get(el.prop("imagePath"));
    if (img.tex != jf::kNullTexture && img.w > 0 && img.h > 0) {
        const float px = static_cast<float>(numD("pivotX", 0.5));
        const float py = static_cast<float>(numD("pivotY", 0.5));
        const float ih = (nLen + tLen) * s;                        // reach, in screen units
        const float iw = ih * static_cast<float>(img.w) / static_cast<float>(img.h);
        // Quad corners relative to the pivot, then rotated by the same (ca, sa) the vector needle uses.
        // -py puts the pivot on the gauge centre: image +y is DOWN, which is the tail direction here.
        const float x0 = -px * iw, x1 = (1.f - px) * iw;
        const float y0 = -py * ih, y1 = (1.f - py) * ih;
        auto Q = [&](float lx, float ly) -> std::pair<float,float> {
            return { cx + (sa * lx - ca * ly), cy + (ca * lx + sa * ly) };
        };
        const auto [ax, ay] = Q(x0, y0);  const auto [bx, by] = Q(x1, y0);
        const auto [dx, dy] = Q(x1, y1);  const auto [ex, ey] = Q(x0, y1);
        static const uint8_t kWhite[4] = { 255, 255, 255, 255 };   // modulate: leave the artwork's colours
        std::vector<jf::JRenderVertex> verts = {
            { { ax, ay }, { 0.f, 0.f }, { kWhite[0], kWhite[1], kWhite[2], kWhite[3] } },
            { { bx, by }, { 1.f, 0.f }, { kWhite[0], kWhite[1], kWhite[2], kWhite[3] } },
            { { dx, dy }, { 1.f, 1.f }, { kWhite[0], kWhite[1], kWhite[2], kWhite[3] } },
            { { ax, ay }, { 0.f, 0.f }, { kWhite[0], kWhite[1], kWhite[2], kWhite[3] } },
            { { dx, dy }, { 1.f, 1.f }, { kWhite[0], kWhite[1], kWhite[2], kWhite[3] } },
            { { ex, ey }, { 0.f, 1.f }, { kWhite[0], kWhite[1], kWhite[2], kWhite[3] } },
        };
        buf.pushGeometry(std::move(verts), img.tex);
    } else {
        vg.fillConvex({ P(-halfW, tLen), P(halfW, tLen), P(0.f, -nLen) }, jf::JPaint::solid(jc(nc)));
        vg.fillConvex({ P(-halfW * 0.8f, tLen * 0.8f), P(halfW * 0.8f, tLen * 0.8f), P(0.f, tLen * 1.2f) }, jf::JPaint::solid(jc(nd)));
    }
    if (hRad * s > 0.5f) vg.fillCircle(cx, cy, hRad * s, jf::JPaint::solid(jc(hc)));
    // == "1", not != "0". The declared default is m_showGlass = FALSE, and an absent prop is "" -- which
    // != "0" reads as ON. So a freshly dropped widget drew glass that its own property said was off, and
    // it vanished the moment anything committed the props (a resize), which reads as the resize breaking
    // it rather than the drop having lied. An absent prop must mean whatever the member declares.
    if (el.prop("showGlass") == "1") {
        const float gr = 450.f * s;
        vg.fillCircle(cx, cy, gr, jf::JPaint::linear(cx, cy - gr, jf::rgba(255, 255, 255, 40), cx, cy + gr, jf::rgba(255, 255, 255, 20)));
    }
    vg.flush(buf);
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("needle", NeedleWidget, 30);
