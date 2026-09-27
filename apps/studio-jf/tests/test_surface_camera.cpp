// Golden-value test for SurfaceCamera — pins the world->screen math extracted verbatim from Surface (Stage 0
// of the container migration). Every expected value below is hand-computed from the transform formula, so any
// future drift in the extracted math trips here.
//   cmake --build build --target surface_camera_test && ./build/surface_camera_test

#include "SurfaceCamera.h"
#include "ContainerLayout.h"

#include <vector>

#include <cmath>
#include <cstdio>

static int g_fail = 0;
static void chk(const char* what, float got, float want, float eps = 0.02f) {
    if (std::fabs(got - want) > eps) { std::printf("FAIL %-14s got %.4f want %.4f\n", what, got, want); ++g_fail; }
}
static void chkRect(const char* what, const jf::JRect& r, float x, float y, float w, float h) {
    char b[64];
    std::snprintf(b, sizeof b, "%s.x", what); chk(b, r.x, x);
    std::snprintf(b, sizeof b, "%s.y", what); chk(b, r.y, y);
    std::snprintf(b, sizeof b, "%s.w", what); chk(b, r.width, w);
    std::snprintf(b, sizeof b, "%s.h", what); chk(b, r.height, h);
}

int main() {
    // A — scale-to-fit, square fit, centred anchor, no scroll: scale 0.5, no offset.
    {
        SurfaceCamera c; c.viewport = {10, 20, 400, 240}; c.canvasW = 800; c.canvasH = 480; c.anchor = 4;
        const auto t = c.xform();
        chk("A.scale", t.scale, 0.5f);
        chk("A.offX", t.offX, 0.f);
        chk("A.offY", t.offY, 0.f);
        chkRect("A.toScreen", SurfaceCamera::toScreen({100, 50, 40, 20}, t), 60, 45, 20, 10);
        chkRect("A.page", SurfaceCamera::pageRect(t), 10, 20, 400, 240);
        // Element rects land on WHOLE pixels: at scale 0.5 an odd-numbered element edge would otherwise
        // fall on a .5 boundary, where a 1px border renders at partial coverage and one side drops out.
        // Edges are rounded (not position+size separately), so the width follows from the snapped edges.
        chkRect("A.snap.odd", SurfaceCamera::toScreen({101, 51, 41, 21}, t), 61, 46, 20, 10);
        // Two elements sharing a world edge still abut exactly on screen: A's right edge (60+21) is B's
        // left (81). Rounding position and size independently would leave a 1px seam or overlap here.
        chkRect("A.snap.abutL", SurfaceCamera::toScreen({100, 50, 41, 20}, t), 60, 45, 21, 10);
        chkRect("A.snap.abutR", SurfaceCamera::toScreen({141, 50, 41, 20}, t), 81, 45, 20, 10);
    }
    // B — scale-to-fit, height-limited (0.4), centred: horizontal letterbox offset 40.
    {
        SurfaceCamera c; c.viewport = {10, 20, 400, 240}; c.canvasW = 800; c.canvasH = 600; c.anchor = 4;
        const auto t = c.xform();
        chk("B.scale", t.scale, 0.4f);
        chk("B.offX", t.offX, 40.f);
        chk("B.offY", t.offY, 0.f);
        chkRect("B.page", SurfaceCamera::pageRect(t), 50, 20, 320, 240);
    }
    // C — fixed 1:1, top-left anchor: scale 1, no offset.
    {
        SurfaceCamera c; c.viewport = {10, 20, 400, 240}; c.canvasW = 200; c.canvasH = 100; c.fixed = true; c.anchor = 0;
        const auto t = c.xform();
        chk("C.scale", t.scale, 1.f);
        chkRect("C.toScreen", SurfaceCamera::toScreen({50, 30, 20, 10}, t), 60, 50, 20, 10);
    }
    // D — fixed, canvas overflows the viewport, panned -> scroll bars appear and clamp.
    {
        SurfaceCamera c; c.viewport = {10, 20, 400, 240}; c.canvasW = 800; c.canvasH = 600;
        c.fixed = true; c.anchor = 4; c.scrollX = -100; c.scrollY = -50; c.scrollBarW = 10;
        const auto t = c.xform();
        chk("D.scale", t.scale, 1.f);
        chk("D.offX", t.offX, -100.f);   // clamp(-100, -400, 0)
        chk("D.offY", t.offY, -50.f);    // clamp(-50, -360, 0)
        chkRect("D.toScreen", SurfaceCamera::toScreen({10, 10, 5, 5}, t), -80, -20, 5, 5);
        const auto sb = c.scrollBars(t);
        if (!sb.hasV || !sb.hasH) { std::printf("FAIL D.bars both present\n"); ++g_fail; }
        chkRect("D.vThumb", sb.vThumb, 401, 39.17f, 8, 92);
        chkRect("D.hThumb", sb.hThumb, 58.75f, 251, 195, 8);
    }
    // E — pan clamp: scrolling past the overflow is clamped to the edge, not beyond.
    {
        SurfaceCamera c; c.viewport = {0, 0, 400, 240}; c.canvasW = 800; c.canvasH = 240;
        c.fixed = true; c.anchor = 4; c.scrollX = -9999;   // way past the -400 overflow floor
        const auto t = c.xform();
        chk("E.offX", t.offX, -400.f);   // clamp(-9999, -400, 0)
    }

    // Cull decision (SurfaceCanvas off-viewport culling): visible iff the rects overlap.
    {
        const jf::JRect vp{0, 0, 400, 240};
        auto vis = [&](jf::JRect r){ return SurfaceCamera::rectVisible(r, vp); };
        if (!vis({10, 10, 20, 20}))    { std::printf("FAIL cull.inside\n");        ++g_fail; }   // fully inside
        if (!vis({-5, -5, 20, 20}))    { std::printf("FAIL cull.straddle\n");      ++g_fail; }   // straddles the edge
        if ( vis({400, 0, 20, 20}))    { std::printf("FAIL cull.right-out\n");     ++g_fail; }   // just past the right edge
        if ( vis({-20, 0, 20, 20}))    { std::printf("FAIL cull.left-out\n");      ++g_fail; }   // fully left
        if ( vis({0, 240, 20, 20}))    { std::printf("FAIL cull.below-out\n");     ++g_fail; }   // fully below
        if (!vis({399, 239, 5, 5}))    { std::printf("FAIL cull.corner-in\n");     ++g_fail; }   // 1px corner in
    }

    // Border layout — Centre is what the STRIPS leave, whatever order they were declared in.
    //
    // TunerStudio writes its border dialogs West, Centre, East (a fan page: config | status | config), and
    // carving in declaration order handed Centre everything not yet taken — the East column's area included,
    // so the two were drawn on top of each other.
    {
        const jf::JRect page{0, 0, 900, 600};
        const std::vector<LayoutChild> kids = {
            { {0, 0, 300, 600}, "West" },
            { {0, 0, 300, 600}, "" },          // Centre, declared BEFORE the East strip
            { {0, 0, 300, 600}, "East" },
            { {0, 0, 900,  60}, "South" },
        };
        auto at = [&](int i){ return layoutChildRect(3, page, kids, i, 2, 0, 1.f, 1.f, jf::JRect{}); };
        chkRect("border.W", at(0), 0.f,   0.f, 300.f, 600.f);
        chkRect("border.E", at(2), 600.f, 0.f, 300.f, 600.f);
        chkRect("border.S", at(3), 300.f, 540.f, 300.f, 60.f);   // between W and E, along the bottom
        chkRect("border.C", at(1), 300.f, 0.f, 300.f, 540.f);    // <- the rest: not over E, not over S
    }

    // Wrap layout — a FLOW whose row breaks come from the container's width, not from the document. This is
    // what lets a strip of status lamps take however many rows the definition's count needs, and re-flow when
    // the panel is resized, instead of carrying a row/column count baked in when it was imported.
    {
        const jf::JRect page{0, 0, 300, 200};
        const std::vector<LayoutChild> kids(5, LayoutChild{ {0, 0, 100, 20}, "" });
        auto at = [&](int i){ return layoutChildRect(7, page, kids, i, 2, 0, 1.f, 1.f, jf::JRect{}); };
        chkRect("wrap.0", at(0), 0.f,   0.f,  100.f, 20.f);
        chkRect("wrap.2", at(2), 200.f, 0.f,  100.f, 20.f);   // fills the row
        chkRect("wrap.3", at(3), 0.f,   20.f, 100.f, 20.f);   // wraps to the next
        chkRect("wrap.4", at(4), 100.f, 20.f, 100.f, 20.f);

        const jf::JRect narrow{0, 0, 200, 200};               // same children, less width -> more rows
        auto atN = [&](int i){ return layoutChildRect(7, narrow, kids, i, 2, 0, 1.f, 1.f, jf::JRect{}); };
        chkRect("wrapN.2", atN(2), 0.f,   20.f, 100.f, 20.f);
        chkRect("wrapN.4", atN(4), 0.f,   40.f, 100.f, 20.f);
    }

    // ---- A STATIC SURFACE RENDERS THE PAGE AT THE INTERFACE SCALE -----------------------------------
    // One number scales the whole application — chrome, dialogs, docks and pages — because a page's
    // widget positions are authored units that the font scale cannot reach: scale the glyphs alone and a
    // page's text outgrows the boxes it was laid out in. A page too big for its area SCROLLS. It is never
    // shrunk to fit, which is how a fuel map became unreadable in a smaller window.
    {
        SurfaceCamera c; c.viewport = {0, 0, 1000, 800};
        c.canvasW = 800; c.canvasH = 600;                 // the page as authored
        c.fixed = true; c.anchor = 0;

        c.staticScale = 2.f;   chk("static.grow",   c.xform().scale, 2.f);
        c.staticScale = 0.5f;  chk("static.shrink", c.xform().scale, 0.5f);
        c.staticScale = 1.f;   chk("static.same",   c.xform().scale, 1.f);

        // NOT THE VIEWPORT. A static surface renders the same whatever window it is in — that is the
        // whole difference between it and fit-to-window, and the reason it can overflow into scroll bars
        // rather than shrinking its content away.
        c.staticScale = 2.f;
        const float wide = c.xform().scale;
        c.viewport = {0, 0, 300, 200};
        chk("static.viewport-independent", c.xform().scale, wide);

        // An unstated scale is 1:1, so a caller with no interface scale to offer is unaffected.
        c.staticScale = 0.f;
        chk("static.unstated", c.xform().scale, 1.f);

        // Fit-to-window fits the VIEWPORT, and by default is not floored — a caller that asks to fit
        // two pages into half a window (the difference report) means it.
        c.fixed = false; c.staticScale = 1.f; c.minScale = 0.f;
        c.viewport = {0, 0, 400, 600};
        chk("dynamic.fits-viewport", c.xform().scale, 0.5f);   // min(400/800, 600/600)

        // WITH A FLOOR, a squeeze stops before it becomes a thumbnail. This is the case that made a
        // rusEFI dialog render its labels six pixels high: 808x940 under 590 of height fits at 0.63.
        // Below the floor the surface scrolls instead. Remove the max() and this goes red.
        c.canvasW = 808; c.canvasH = 940;
        c.viewport = {0, 0, 1458, 590};
        c.minScale = SurfaceCamera::kMinReadableScale;
        chk("dynamic.floor", c.xform().scale, SurfaceCamera::kMinReadableScale);

        // A page that genuinely fits is untouched by the floor.
        c.canvasW = 400; c.canvasH = 200;
        c.viewport = {0, 0, 800, 400};
        chk("dynamic.no-floor-when-it-fits", c.xform().scale, 2.f);
        c.minScale = 0.f;
    }

    if (g_fail) { std::printf("SurfaceCamera: %d check(s) FAILED\\n", g_fail); return 1; }
    std::printf("SurfaceCamera: all golden values pass\n");
    return 0;
}
