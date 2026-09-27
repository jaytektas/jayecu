#pragma once

// SurfaceCamera — the pure world->screen transform for a tuning surface, the ONE source of truth every
// hit-test, render, drag and managed-layout mapping goes through. The Surface gathers the live inputs
// (its on-screen viewport, the effective canvas size, fixed-vs-fit + letterbox anchor, the in-scope
// breadcrumb inset, the pan, and the managed-layout params) into one of these; ALL positioning math lives
// here with no Surface / model / EditorSettings access, so it is unit-testable and cannot drift between the
// paint path and the input path. The math is byte-for-byte what used to live on Surface (Stage 0 of the
// container migration is a pure extraction).

#include <j/core/SceneGraph.h>     // jf::JRect
#include "ContainerLayout.h"        // LayoutChild, layoutChildRect (managed modes)

#include <vector>

struct SurfaceCamera {
    // --- inputs (the Surface fills these from its live state each use) ---
    jf::JRect viewport{};                 // the surface's on-screen bounds (getBoundingBox)
    float     crumbH   = 0.f;             // top strip reserved for the in-scope breadcrumb (0 = none)
    float     canvasW  = 0.f, canvasH = 0.f;   // the page's AUTHORED size — what its widgets were laid out to
    // HOW BIG THE INTERFACE IS, when `fixed` is set — the same Interface scale that sizes every control
    // and every glyph in the app (JStyle::uiScale). A page's widget positions are authored virtual units,
    // which that scale does NOT reach: scale the fonts alone and a page's text outgrows the boxes it was
    // laid out in. So a static surface renders its authored page at the interface scale, and one number
    // moves the whole application together — chrome, dialogs, docks and pages.
    //
    // This replaced a SECOND scale, "static surface size", which divided the page by a canvas size from
    // Preferences. Two scale controls that had to agree, and no way to tell which one was fighting you.
    //
    // 0 = not stated, which is 1:1 — what a caller with no interface scale to offer should leave alone.
    float     staticScale = 0.f;
    // The floor a SQUEEZE may not go below (0 = none). Set only where the user asked for fit-to-window,
    // so a deliberate scale-down elsewhere is left alone.
    float     minScale = 0.f;
    bool      fixed    = false;           // true = render at targetW/H (the preference); false = fit the viewport
    int       anchor   = 4;               // letterbox anchor 0..8 (row-major); used only when the canvas is smaller
    float     scrollX  = 0.f, scrollY = 0.f;   // pan (only meaningful when the canvas overflows)
    int       layoutMode  = 0;            // 0 = Free; 1..6 = a managed container layout
    int       gridColumns = 0, focusIndex = -1;   // managed-layout params (Grid / Index Card)
    float     titleTop = 0.f, titleBottom = 0.f;  // content inset reserved for the title bar (managed modes)
    float     scrollBarW = 10.f;          // scrollbar track/thumb thickness (was Surface::kSB)

    // The derived affine: scale + letterbox/pan offset + view origin + canvas size.
    struct Xform { float scale = 1.f, offX = 0.f, offY = 0.f, viewX = 0.f, viewY = 0.f, cw = 0.f, ch = 0.f; };
    Xform xform() const;

    // THE SCALING RULE, IN ONE PLACE. A page is always fitted into a box; `fixed` only chooses which box.
    // Static fits it into the size Preferences asks for, so that one setting scales every page in the
    // document together; dynamic fits it into the space actually available. Aspect-preserving both ways.
    //
    // Shared, because a page is shown two ways — as a surface, and mirrored inside a node viewport — and
    // the rule was written out twice. It drifted: the viewport is what a tuning page is actually drawn
    // through, so a preference that scaled only the surface changed the frame around the page and nothing
    // on it. A page too big for its area SCROLLS; it is never shrunk to fit, because shrinking to fit is
    // how a fuel map became unreadable on a smaller window.
    // HOW FAR A FIT MAY SHRINK A PAGE before it stops being a page and becomes a picture of one. 0.8 of
    // the interface scale still reads; below that a squeeze is a thumbnail. Opt-in via `minScale`,
    // because not every fit is a user asking to see a page: the difference report fits two pages into
    // half a window on purpose and must not be floored.
    static constexpr float kMinReadableScale = 0.8f;

    static float pageScale(bool fixed, float canvasW, float canvasH,
                           float staticScale, float availW, float availH, float minScale = 0.f);

    static jf::JRect pageRect(const Xform& t);                        // the effective (scaled) canvas rect
    static jf::JRect toScreen(const jf::JRect& elem, const Xform& t); // an element's world rect -> screen

    // Scroll-bar geometry, present only when the effective canvas overflows the viewport.
    struct ScrollBars { bool hasV = false, hasH = false; jf::JRect vTrack, vThumb, hTrack, hThumb; };
    ScrollBars scrollBars(const Xform& t) const;

    // One element's on-screen rect honouring the Layout mode. Free (layoutMode 0) returns its own toScreen
    // rect; the managed modes route every child through the shared layoutChildRect within the title-inset
    // content rect. children = every element's RAW rect + region in order; index selects the one to place.
    jf::JRect screenRectOf(const Xform& t, const std::vector<LayoutChild>& children, int index) const;

    // Whether an element's screen rect is at all visible within a viewport — the SurfaceCanvas paint/focus
    // cull decision (setVisible(false) when off-viewport removes a child from paint AND framework input).
    static bool rectVisible(const jf::JRect& r, const jf::JRect& viewport);
};
