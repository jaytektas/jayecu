// SurfaceCamera — pure world->screen math extracted verbatim from Surface (Stage 0). No behaviour change.

#include "SurfaceCamera.h"

#include <algorithm>
#include <cmath>

SurfaceCamera::Xform SurfaceCamera::xform() const {
    Xform t;
    t.viewX = viewport.x; t.viewY = viewport.y + crumbH;
    const float availH = viewport.height - crumbH;   // vertical space left for the page after the breadcrumb
    // Scaling. Two boxes, one rule: the authored page is fitted into a box, and `fixed` only chooses
    // WHICH box. Static fits it into the page size from Preferences, so that setting scales every page in
    // the document up or down together; dynamic fits it into the viewport it is actually being shown in.
    // Aspect-preserving either way, so a page never overflows the box it was fitted to.
    //
    // A static surface used to be 1:1 regardless, which made the preference a canvas rather than a page
    // size: raising it did not enlarge anything, it only added empty margin around content that stayed
    // exactly as big. Degenerate (0) canvas or an unstated target still pins 1:1 — there is nothing to
    // scale against.
    t.scale = pageScale(fixed, canvasW, canvasH, staticScale, viewport.width, availH, minScale);
    const float xExtra = viewport.width - canvasW * t.scale, yExtra = availH - canvasH * t.scale;
    const int col = anchor % 3, row = anchor / 3;
    // Canvas SMALLER than the viewport -> letterbox by the anchor. Canvas LARGER (fixed/static) -> the anchor
    // is meaningless, so top/left-align and pan by scrollX/scrollY within [-overflow, 0].
    t.offX = (xExtra >= 0.f) ? ((col == 0) ? 0.f : (col == 1) ? xExtra * 0.5f : xExtra)
                             : std::clamp(scrollX, xExtra, 0.f);
    t.offY = (yExtra >= 0.f) ? ((row == 0) ? 0.f : (row == 1) ? yExtra * 0.5f : yExtra)
                             : std::clamp(scrollY, yExtra, 0.f);
    t.cw = canvasW; t.ch = canvasH;
    return t;
}

float SurfaceCamera::pageScale(bool fixed, float canvasW, float canvasH,
                               float staticScale, float availW, float availH, float minScale) {
    if (canvasW <= 0.f || canvasH <= 0.f) return 1.f;
    if (fixed) return staticScale > 0.f ? staticScale : 1.f;   // the interface scale; overflow scrolls
    // FIT-TO-WINDOW, WITH A FLOOR. Squeezing a page into the space available is fine until the space is
    // smaller than the page needs, and then it is not a fit, it is a thumbnail: a 940-tall page under a
    // 590-tall area comes out at 0.63 and its labels are six pixels high. Nobody asked to be shown an
    // unreadable picture of a page — they asked for it to fit — so below the floor it stops shrinking
    // and the surface scrolls instead, which is the honest answer to "there is not enough room".
    //
    // The floor is relative to the interface scale, because that is what the user said "readable" means.
    const float fit = std::min(availW / canvasW, availH / canvasH);
    return minScale > 0.f ? std::max(fit, minScale) : fit;
}

jf::JRect SurfaceCamera::pageRect(const Xform& t) {
    return jf::JRect{ t.viewX + t.offX, t.viewY + t.offY, t.cw * t.scale, t.ch * t.scale };
}

jf::JRect SurfaceCamera::toScreen(const jf::JRect& e, const Xform& t) {
    // Snap to whole pixels. At any scale != 1 the transform lands element edges on fractional coordinates,
    // and a 1px border then renders at partial coverage — one side can disappear entirely while the opposite
    // side looks solid (the "border isn't drawn round the widget" artefact). Round the EDGES rather than
    // position+size independently, so width stays consistent and neighbouring elements still abut exactly.
    const float x0 = std::round(t.viewX + t.offX + e.x * t.scale);
    const float y0 = std::round(t.viewY + t.offY + e.y * t.scale);
    const float x1 = std::round(t.viewX + t.offX + (e.x + e.width)  * t.scale);
    const float y1 = std::round(t.viewY + t.offY + (e.y + e.height) * t.scale);
    return jf::JRect{ x0, y0, x1 - x0, y1 - y0 };
}

SurfaceCamera::ScrollBars SurfaceCamera::scrollBars(const Xform& t) const {
    ScrollBars sb;
    const jf::JRect& b = viewport;
    const float kSB = scrollBarW;
    const float cw = t.cw * t.scale, chh = t.ch * t.scale;
    sb.hasV = chh > b.height + 0.5f;
    sb.hasH = cw  > b.width  + 0.5f;
    if (!sb.hasV && !sb.hasH) return sb;
    const float availH = b.height - (sb.hasH ? kSB : 0.f);
    const float availW = b.width  - (sb.hasV ? kSB : 0.f);
    if (sb.hasV) {
        sb.vTrack = { b.x + b.width - kSB, b.y, kSB, availH };
        const float thumbH   = std::max(24.f, availH * (b.height / chh));
        const float overflow = chh - b.height;
        const float pos = overflow > 0.f ? std::clamp(-scrollY, 0.f, overflow) / overflow : 0.f;
        sb.vThumb = { sb.vTrack.x + 1.f, sb.vTrack.y + pos * (availH - thumbH), kSB - 2.f, thumbH };
    }
    if (sb.hasH) {
        sb.hTrack = { b.x, b.y + b.height - kSB, availW, kSB };
        const float thumbW   = std::max(24.f, availW * (b.width / cw));
        const float overflow = cw - b.width;
        const float pos = overflow > 0.f ? std::clamp(-scrollX, 0.f, overflow) / overflow : 0.f;
        sb.hThumb = { sb.hTrack.x + pos * (availW - thumbW), sb.hTrack.y + 1.f, thumbW, kSB - 2.f };
    }
    return sb;
}

jf::JRect SurfaceCamera::screenRectOf(const Xform& t, const std::vector<LayoutChild>& children, int index) const {
    const jf::JRect own = toScreen(children[index].rect, t);   // Free-mode screen rect / managed fallback
    if (layoutMode == 0 || children.empty()) return own;       // Free fast path
    const jf::JRect page = pageRect(t);                        // managed content sits below/above the title bar
    const jf::JRect content{ page.x, page.y + titleTop, page.width, page.height - titleTop - titleBottom };
    return layoutChildRect(layoutMode, content, children, index,
                           gridColumns, focusIndex, t.scale, t.scale, own);
}

bool SurfaceCamera::rectVisible(const jf::JRect& r, const jf::JRect& v) {
    return r.x < v.x + v.width && r.x + r.width > v.x
        && r.y < v.y + v.height && r.y + r.height > v.y;
}
