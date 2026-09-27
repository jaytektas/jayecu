// CLICKING IN AN OPEN CAPTION PUTS THE CARET WHERE YOU CLICKED.
//
// The report: opening a label for editing and then clicking part-way along the text drops the caret at the
// START of the caption instead of under the pointer.
//
// The click path converts a screen x to a byte index through LabelWidget::caretAtX, which measures against
// the caption origin the RENDER published (tx/scale). So this drives the real thing — render, open, render,
// click — and asks where the caret landed, printing the geometry it was computed from.
//
//   cmake --build build --target caption_caret_test && ./build/caption_caret_test

#include "model/Cache.h"
#include "model/MetaModel.h"
#include "surface/PanelModel.h"
#include "surface/Surface.h"
#include "surface/widgets/LabelWidget.h"

#include <j/core/SceneGraph.h>
#include "surface/SurfaceCamera.h"   // the same transform the surface uses, so a test can aim a click
#include <j/graphics/FontEngine.h>


#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[caret] %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());
    // A REAL ATLAS, baked on the CPU. Without one the caption's render bails before it publishes the
    // origin the hit-test measures from — which is not the app's situation, and a test that ran without
    // one would be testing the wrong code path.
    jf::JFontEngine fe;
    if (!fe.loadFromFile(TEST_FONT)) { std::fprintf(stderr, "cannot load %s\n", TEST_FONT); return 77; }
    jf::JTextHelper::setAtlas(fe.buildAtlas(14.f));

    jf::JSceneGraph graph;

    PanelModel page;
    page.setCanvasSize(900.f, 600.f);
    page.setCanvasStatic(1);                      // 1:1, so a canvas coordinate IS a screen coordinate
    const std::string caption = "Throttle body A enabled";
    const int id = page.add("label", 40.f, 40.f, 300.f, 30.f,
                            { { "labelText", caption }, { "align", "Left" } });

    Surface s(graph, c, &page);
    s.setBounds({ 0.f, 0.f, 900.f, 600.f });
    s.setMode(Surface::Mode::Edit);
    // PINNED TO 1:1, so this test says what it means. The page canvas and the surface bounds are
    // deliberately the same size here; fitting one to the other is exactly scale 1, whatever the
    // user's "static surface size" preference happens to be. Static is no longer 1:1 — it renders a
    // page AT the preference size — so without this a hit-test lands wherever that preference put it.
    s.setFitToView(true);
    jf::JPrimitiveBuffer buf;
    s.populateRenderPrimitives(buf);

    const PanelElement& e = *page.get(id);
    const float cx = e.x + e.w * 0.5f, cy = e.y + e.h * 0.5f;
    s.handleMousePress(cx, cy); s.handleMouseRelease(cx, cy); s.handleMousePress(cx, cy);
    auto& ed = LabelWidget::labelEdits()[page.get(id)->uid];
    ck(ed.active, "the caption is open for editing");

    s.populateRenderPrimitives(buf);              // the frame that publishes tx/ty/scale for the hit-test
    std::printf("[caret] geometry: tx=%.2f ty=%.2f scale=%.3f lineH=%.2f  element x=%.0f w=%.0f\n",
                ed.tx, ed.ty, ed.scale, ed.lineH, e.x, e.w);
    if (!jf::JTextHelper::hasAtlas()) { std::printf("[caret] NO FONT ATLAS — cannot measure text\n"); return 77; }

    // Aim at the exact screen x of a known byte boundary, the same measure the render uses.
    const size_t want = 9;                        // "Throttle " | "body A enabled"
    const float wantX = ed.tx + jf::JTextHelper::measureWidthScaled(caption.substr(0, want), ed.scale);
    const float capW  = jf::JTextHelper::measureWidthScaled(caption, ed.scale);
    std::printf("[caret] caption spans x %.2f..%.2f; clicking at %.2f (byte %zu)\n",
                ed.tx, ed.tx + capW, wantX, want);

    s.handleMousePress(wantX, cy);
    const size_t got = LabelWidget::labelEdits()[page.get(id)->uid].core.caret();
    std::printf("[caret] caret landed at byte %zu -> %s|%s\n", got,
                caption.substr(0, got).c_str(), caption.substr(got).c_str());
    ck(got == want, "a click part-way along places the caret there", "got " + std::to_string(got));

    // ...and the far end, which is the other thing a start-biased hit-test gets wrong.
    s.handleMouseRelease(wantX, cy);
    const float endX = ed.tx + capW - 1.f;
    s.handleMousePress(endX, cy);
    const size_t gotEnd = LabelWidget::labelEdits()[page.get(id)->uid].core.caret();
    ck(gotEnd == caption.size(), "a click at the end places the caret at the end",
       "got " + std::to_string(gotEnd) + " of " + std::to_string(caption.size()));

    s.handleMouseRelease(endX, cy);
    s.handleKeyEvent([]{ jf::JKeyEvent e; e.pressed = true; e.key = jf::JKeyEvent::JKey::Escape; return e; }());

    // ---- THE CASES A REAL PAGE IS IN -------------------------------------------------------------
    // The one above is the easy geometry: a static canvas, so a canvas coordinate IS a screen coordinate.
    // A real surface is scaled and centred in its dock, and real captions sit inside panels and viewports.
    // Each of those puts the caption's rect and the mouse in DIFFERENT spaces if anything along the way
    // forgets a transform, and the reported symptom — the caret pinned to byte 0 — is what a hit-test
    // measuring a screen x against a canvas-local origin looks like when the origin is to the RIGHT of
    // the click.
    auto caretAfterClickAt = [&](Surface& sf, PanelModel& pm, int elid, float frac) {
        const PanelElement& el = *(sf.model() ? sf.model() : &pm)->get(elid);
        jf::JPrimitiveBuffer b;
        sf.populateRenderPrimitives(b);
        // Open it: two presses at the element's centre, through the same transform the surface uses.
        const auto p = sf.toScreen(el, sf.xform());
        const float mx = p.x + p.width * 0.5f, my = p.y + p.height * 0.5f;
        sf.handleMousePress(mx, my); sf.handleMouseRelease(mx, my); sf.handleMousePress(mx, my);
        sf.populateRenderPrimitives(b);
        auto& e2 = LabelWidget::labelEdits()[el.uid];
        if (!e2.active) return std::string("NOT OPEN");
        const std::string txt = e2.core.text();
        const float w = jf::JTextHelper::measureWidthScaled(txt, e2.scale);
        const float clickX = e2.tx + w * frac;
        sf.handleMousePress(clickX, my);
        const size_t got = LabelWidget::labelEdits()[el.uid].core.caret();
        char note[256];
        std::snprintf(note, sizeof note,
                      "rect x=%.1f w=%.1f | tx=%.1f scale=%.2f | click %.1f | caret %zu/%zu",
                      p.x, p.width, e2.tx, e2.scale, clickX, got, txt.size());
        return std::string(note);
    };

    {   // A SCALED, CENTRED CANVAS — the default: the page is fitted into the dock, not drawn 1:1.
        PanelModel pm;
        pm.setCanvasSize(1280.f, 720.f);
        pm.setCanvasStatic(2);                    // scale-to-fit, centred
        const int lid = pm.add("label", 100.f, 100.f, 300.f, 30.f,
                               { { "labelText", caption }, { "align", "Left" } });
        Surface sf(graph, c, &pm);
        sf.setBounds({ 250.f, 60.f, 900.f, 500.f });   // a dock-sized viewport, offset by the tree pane
        sf.setMode(Surface::Mode::Edit);
        const std::string note = caretAfterClickAt(sf, pm, lid, 0.5f);
        std::printf("[caret] scaled+centred canvas: %s\n", note.c_str());
        ck(note.find("caret 0/") == std::string::npos,
           "a click mid-caption on a SCALED canvas is not pinned to byte 0", note);
    }

    {   // INSIDE A PANEL, edited in place. A page of any size is built out of panels, and a caption in one
        // is edited by drilling in — which swaps the surface onto the panel's own child canvas, with its
        // own size and a breadcrumb strip above it.
        PanelModel pm;
        pm.setCanvasSize(1280.f, 720.f);
        pm.setCanvasStatic(2);
        const std::string kids =
            "[{\"id\":1,\"uid\":\"aaaaaaaa-0000-0000-0000-000000000001\",\"type\":\"label\","
            "\"x\":20,\"y\":30,\"w\":300,\"h\":30,\"props\":{\"labelText\":\"" + caption + "\",\"align\":\"Left\"}}]";
        const int pid = pm.add("panel", 60.f, 60.f, 500.f, 400.f, { { "labelText", "Feed back" }, { "children", kids } });
        Surface sf(graph, c, &pm);
        sf.setBounds({ 250.f, 60.f, 900.f, 500.f });
        sf.setMode(Surface::Mode::Edit);
        jf::JPrimitiveBuffer b; sf.populateRenderPrimitives(b);
        sf.enterPanel(pid);
        const int lid = sf.model()->elements().front().id;
        const std::string note = caretAfterClickAt(sf, pm, lid, 0.5f);
        std::printf("[caret] inside an entered PANEL: %s\n", note.c_str());
        ck(note.find("caret 0/") == std::string::npos && note.find("NOT OPEN") == std::string::npos,
           "a click mid-caption inside a panel is not pinned to byte 0", note);
        while (sf.inScope()) sf.exitScope();
    }

    {   // A DOUBLE-CLICK INSIDE AN OPEN CAPTION SELECTS A WORD. The press handler tries the "double-click a
        // label opens its caption" rule FIRST, and that rule does not ask whether the caption it would open
        // is the one already open — so a double-click inside an open caption re-entered the edit and
        // selected everything, which is what a click that "jumps to the start/end of the text" is.
        PanelModel pm;
        pm.setCanvasSize(900.f, 600.f);
        pm.setCanvasStatic(1);
        const int lid = pm.add("label", 40.f, 40.f, 300.f, 30.f,
                               { { "labelText", caption }, { "align", "Left" } });
        Surface sf(graph, c, &pm);
        sf.setBounds({ 0.f, 0.f, 900.f, 600.f });
        sf.setMode(Surface::Mode::Edit);
        jf::JPrimitiveBuffer b; sf.populateRenderPrimitives(b);
        const PanelElement& le = *pm.get(lid);
        const float my2 = le.y + le.h * 0.5f, cx2 = le.x + le.w * 0.5f;
        sf.handleMousePress(cx2, my2); sf.handleMouseRelease(cx2, my2); sf.handleMousePress(cx2, my2);
        sf.handleMouseRelease(cx2, my2);
        sf.populateRenderPrimitives(b);
        auto& e3 = LabelWidget::labelEdits()[pm.get(lid)->uid];
        // Two presses in the same place, inside the double-click window, over the word "body".
        const float wordX = e3.tx + jf::JTextHelper::measureWidthScaled(caption.substr(0, 11), e3.scale);
        sf.handleMousePress(wordX, my2); sf.handleMouseRelease(wordX, my2);
        sf.handleMousePress(wordX, my2);
        const auto& e4 = LabelWidget::labelEdits()[pm.get(lid)->uid];
        const size_t lo = e4.core.selectionStart(), hi = e4.core.selectionEnd();
        char note[160];
        std::snprintf(note, sizeof note, "selection %zu..%zu of %zu = \"%s\"", lo, hi, caption.size(),
                      caption.substr(lo, hi - lo).c_str());
        std::printf("[caret] double-click in an open caption: %s\n", note);
        ck(!(lo == 0 && hi == caption.size()), "a double-click inside an open caption does not re-select ALL", note);
        ck(caption.substr(lo, hi - lo) == "body", "...it selects the word under the pointer", note);
    }

    std::printf("[caret] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
