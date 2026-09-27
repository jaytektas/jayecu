// ViewportWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "ViewportWidget.h"
#include "../../ui/WrapText.h"

#include "../SurfaceCamera.h"
#include "../ContainerLayout.h"
#include "../StyleMetrics.h"
#include <j/core/JStyle.h>
#include <j/core/Log.h>
#include "../../model/Perf.h"
#include "../WidgetRegistry.h"
#include "../../model/Cache.h"
#include "../../model/MathEvaluator.h"
#include "../../model/EditorSettings.h"
#include "../PanelLibrary.h"

#include <unordered_set>

// A viewport MIRRORS another node's page (resolved live via nodeviewport::resolver()). reconcile() keeps the
// OWNED child instances (keyed by SOURCE element id) in step with that page: create instances for new source
// elements, drop stale ones — a cheap set-delta, NOT a per-frame teardown, so the mirrored widgets persist and
// carry live state. Node-tagged source elements (nested viewports) are skipped. Called from loadContent()
// (every setData) and again in render()/onControlInput() so a live change to the source model is picked up.
void ViewportWidget::reconcile(const std::string& node) {
    PERF_SCOPE("Viewport::reconcile");
    const PanelModel* pm = nodeviewport::resolver() ? nodeviewport::resolver()(node) : nullptr;
    if (!pm) { m_childByEl.clear(); m_mirrorNode.clear(); m_mirrorGen = ~0ull; return; }

    // A DIFFERENT PAGE MEANS DIFFERENT CHILDREN, and none of what is cached here survives the move.
    // The map is keyed by SOURCE ELEMENT ID, and ids are per-page starting at 1 — so page B's element 3
    // finds page A's widget, matches on type, and is reused WITHOUT setSource(): it keeps A's bounds,
    // props and bindings while claiming to be B's. That is what "scaled widgets and content from another
    // page" looks like. refreshChildrenIfChanged cannot rescue it either, because it is gated on the
    // model's generation and two different models can sit on the same number.
    //
    // The scroll goes too: it is an offset into the page that was, and applying it to the page that is
    // parks the new content off its own edge.
    if (node != m_mirrorNode) {
        m_mirrorNode = node;
        m_childByEl.clear();
        m_mirrorGen  = ~0ull;
        m_scrollX = m_scrollY = 0.f;
    }
    std::unordered_set<int> live;
    for (const PanelElement& pe : pm->elements()) {
        if (!pe.prop("node").empty()) continue;
        live.insert(pe.id);
        auto it = m_childByEl.find(pe.id);
        if (it == m_childByEl.end() || !it->second || it->second->elementType() != pe.type) {
            std::unique_ptr<CanvasWidget> inst = makeWidgetInstance(pe.type, sceneGraph());
            const bool created = static_cast<bool>(inst);
            it = m_childByEl.insert_or_assign(pe.id, std::move(inst)).first;
            if (created) {
                it->second->setSource(pe);            // hydrate the mirrored child's owned override store
                // A mirrored child IS inside this viewport, so it carries the parent edge — that is what
                // makes it disappear with us. It is still OWNED by m_childByEl (the map keys it by element
                // id and rebuilds it on reconcile), which is a separate matter from where it lives.
                addChild(it->second.get());
            }
        }
        // TEMPLATE CONTEXT: this viewport's own Data Source is the ELEMENT its page is about, pushed down
        // to everything it mirrors so a binding written as sensors.sensor[*].enabled resolves to THIS
        // sensor. That is what lets one page be drawn once and assigned to a whole group — each viewport
        // supplies its own element instead of each sensor needing its own near-identical page. Re-applied
        // on every reconcile, so re-pointing the viewport re-points its whole page.
        if (it->second) it->second->setElementContext(m_el ? m_el->prop("signalName") : std::string());
    }
    for (auto it = m_childByEl.begin(); it != m_childByEl.end(); )
        it = live.count(it->first) ? std::next(it) : m_childByEl.erase(it);
}

// Re-sync the mirrored children from the source page ONLY when that page actually changed (its generation
// moved) — not every frame. The children are the running state; they carry their own live cursor/3D/edit
// state between edits. Mirrors Surface::refreshInstancesIfChanged for the nested-page case.
void ViewportWidget::refreshChildrenIfChanged(const PanelModel* pm) {
    PERF_SCOPE("Viewport::refreshChildren");
    if (!pm || pm->generation() == m_mirrorGen) return;
    m_mirrorGen = pm->generation();
    for (const PanelElement& pe : pm->elements()) {
        if (!pe.prop("node").empty()) continue;
        auto it = m_childByEl.find(pe.id);
        if (it != m_childByEl.end() && it->second) it->second->setSource(pe);
    }
}

void ViewportWidget::loadContent(const PanelElement&) { reconcile(pageKey()); }

// Its own page when it has one, else the page its SURFACE is on. Unbound is the ordinary case and the
// reason one viewport per surface is enough to reach every page.
// The page model this viewport is mirroring, or null. One place, because every caller that resolved it
// for itself had to remember to use pageKey() rather than viewportPage() — and drawBorder did not, so
// the page's own title and border settings were dropped for every unbound viewport while the content
// beside them rendered perfectly.
const PanelModel* ViewportWidget::mirroredPage() const {
    return nodeviewport::resolver() ? nodeviewport::resolver()(pageKey()) : nullptr;
}

std::string ViewportWidget::pageKey() const {
    const std::string p = m_el ? viewportPage(*m_el) : std::string();
    return p.empty() ? m_surfaceNode : p;
}

// Save-time: flush each mirrored child's owned state back to the NODE's page model (the real editable source
// behind the mirror), so a table's owned view/etc. authored inside a viewport survives serialization. The page
// model is what PanelLibrary::toJson() writes, so committing here is what makes owned state persist.
void ViewportWidget::commitOwnedChildren() {
    if (!m_el) return;
    const std::string node = pageKey();
    PanelModel* pm = nodeviewport::resolver() ? const_cast<PanelModel*>(nodeviewport::resolver()(node)) : nullptr;
    if (!pm) return;
    for (auto& [id, child] : m_childByEl)
        if (child)
            if (PanelElement* pe = pm->get(id)) { child->commit(*pe); child->commitOwnedChildren(); }
}

// The effective viewport title: its own caption if set, else the MIRRORED PAGE's own title — so a page's
// title still shows when the page is viewed through a viewport (run mode), matching the page chrome the
// editor draws for it. Without this a page mirrored into a viewport lost its title in run mode.
static std::string viewportTitle(const PanelElement& el, const PanelModel* pm) {
    const std::string own = el.prop("labelText");
    return !own.empty() ? own : (pm ? pm->title() : std::string());
}
// A title reserves NO content height — every viewport title (its own caption AND the mirrored page's)
// OVERLAYS the content top (drawn in drawBorder), matching how the editor draws a page's own Inside title.
// So a viewport can never grow a scroll bar just to show a title. Kept as a function returning 0 so the
// content-inset call sites still read intentionally (and a future "reserve the bar" option has one home).
// The caption actually drawn: the viewport's own when it was given one, else the page's.
std::string ViewportWidget::titleShown() const {
    if (!m_el) return {};
    return viewportTitle(*m_el, mirroredPage());
}

static float viewportTitleBarH(const PanelElement&) { return 0.f; }

// The mirrored page's geometry for this frame — scale, origin (scroll included), and the scroll bars. ONE
// function, called by render() and by onControlInput(), so what you click is what you see. It also clamps the
// scroll: the page or the viewport can change size under it (a live edit, a resize), and a stale offset would
// leave the content parked off its own edge.
// THE ONE CHILD RECT — used by the render, the hit-test and the input walk alike.
//
// It was written out four times, and then only the render learned that a mirrored page can arrange its
// own children: paint placed a widget where the page's layout put it, while every hit-test still looked
// for it where the element was AUTHORED. Clicks landed on whatever used to occupy that spot, which reads
// as an invisible offset — you press one control and another answers.
//
// `lc` is the page's children as layout inputs; a caller builds it once for a walk. Free (layout 0)
// returns the plain scaled rect, which is what every one of those four copies used to do.
static jf::JRect mirroredChildRect(int pageLayout, const jf::JRect& pageBox,
                                   const std::vector<LayoutChild>& lc, int index,
                                   int gridColumns, int focusIndex,
                                   const PanelElement& pe, float sc, float ox, float oy) {
    const jf::JRect freeRect{ ox + pe.x * sc, oy + pe.y * sc, pe.w * sc, pe.h * sc };
    if (pageLayout == 0) return freeRect;
    return layoutChildRect(pageLayout, pageBox, lc, index, gridColumns, focusIndex, sc, sc, freeRect);
}

// The layout inputs for a mirrored page's children, in element order — heights resolved from the style
// where the document does not state one, exactly as the surface resolves them.
static std::vector<LayoutChild> mirroredLayoutChildren(const PanelModel* pm) {
    std::vector<LayoutChild> lc;
    if (!pm) return lc;
    lc.reserve(pm->elements().size());
    for (const auto& pe : pm->elements())
        lc.push_back({ jf::JRect{ pe.x, pe.y, pe.w, layoutHeightOf(pe.type, pe.h) }, pe.prop("region") });
    return lc;
}

ViewportWidget::View ViewportWidget::viewOf(const jf::JRect& r, const PanelModel* pm) const {
    View v;
    const PanelElement& el = *m_el;
    const float top = r.y + viewportTitleBarH(el);
    // Two boxes: `full` is everything below the title bar, `padded` insets it by 2px so a page doesn't paint
    // over the viewport's own border. The inset is COSMETIC — it must not be mistaken for overflow, which is
    // what used to happen: a 1:1 canvas the same size as its viewport is 4px bigger than the padded box, so
    // both scroll bars appeared permanently to report 4px of "overflow" that was really just the margin.
    // Scale-to-fit still fits inside the padded box (it never overflows either way, and keeps its margin);
    // a fixed 1:1 page is measured against `full` and gives up the margin only when it actually needs it.
    const jf::JRect full  { r.x, top, r.width, (r.y + r.height) - top };
    const jf::JRect padded{ full.x + 2.f, full.y + 2.f, full.width - 4.f, full.height - 4.f };
    v.content = padded;
    if (!pm || v.content.width <= 4.f || v.content.height <= 4.f) return v;

    float cw = pm->effCanvasW(), ch = pm->effCanvasH();   // its own size, else the Preferences default
    const int   cs = pm->canvasStatic();
    // REFLOW (3): the page IS this viewport. No authored size to fit or squeeze — it takes the box it is
    // given, at the interface scale, and its own layout arranges the content into it.
    const bool reflow = (cs == 3);
    const bool fixed  = reflow || (cs == 1) || (cs == 0 && EditorSettings::instance().staticSurfaceSize());
    // THE SAME RULE THE SURFACE USES — and this is the path that matters, because a tuning page is
    // reached through a node viewport, not by being a surface. Static renders the page at the size
    // Preferences asks for, so raising it enlarges everything on every page; this used to be a flat 1:1,
    // which is why changing the preference resized the frame around a page and nothing inside it.
    const float uis = jf::JStyle::uiScale();
    if (reflow) {                                   // the canvas is the box, in the page's own units
        const float k = (uis > 0.f ? uis : 1.f);
        cw = std::max(1.f, v.content.width / k);
        // Height: the box, or the CONTENT when there is more of it than the box — otherwise the page can
        // never overflow, never scrolls, and the rest of it is simply clipped off the bottom.
        ch = std::max(std::max(1.f, v.content.height / k),
                      contentHeightOf(pm->elements(), pm->layout()));
    }
    v.sc = SurfaceCamera::pageScale(fixed, cw, ch, uis, v.content.width, v.content.height,
                                    reflow ? 0.f : uis * SurfaceCamera::kMinReadableScale);
    v.pageW = cw * v.sc; v.pageH = ch * v.sc;
    if (fixed) {   // per axis — a page matching the viewport's width shouldn't pay for its height overflowing
        if (v.pageW <= full.width  + 0.5f) { v.content.x = full.x; v.content.width  = full.width;  }
        if (v.pageH <= full.height + 0.5f) { v.content.y = full.y; v.content.height = full.height; }
    }
    v.overV = v.pageH > v.content.height + 0.5f;
    v.overH = v.pageW > v.content.width  + 0.5f;

    const float maxScrollY = std::max(0.f, v.pageH - v.content.height);
    const float maxScrollX = std::max(0.f, v.pageW - v.content.width);
    m_scrollY = std::clamp(m_scrollY, -maxScrollY, 0.f);
    m_scrollX = std::clamp(m_scrollX, -maxScrollX, 0.f);

    // Overflowing: pin to the top-left and pan. Fitting: place it by the canvas's ANCHOR — its own if it
    // set one, else the global default. This used to centre unconditionally, so a small canvas in a
    // viewport sat in the middle no matter what the anchor said: the setting existed and this path, the
    // one that actually positions a hosted canvas, never read it.
    const int anchorRaw = pm->canvasAnchor();
    const int anchor = (anchorRaw >= 0) ? anchorRaw : EditorSettings::instance().surfaceAnchor();
    const int col = anchor % 3, row = anchor / 3;             // 0..8 row-major, TL..BR
    const float fx = col == 0 ? 0.f : (col == 1 ? 0.5f : 1.f);
    const float fy = row == 0 ? 0.f : (row == 1 ? 0.5f : 1.f);
    v.ox = v.overH ? v.content.x + m_scrollX : v.content.x + (v.content.width  - v.pageW) * fx;
    v.oy = v.overV ? v.content.y + m_scrollY : v.content.y + (v.content.height - v.pageH) * fy;

    if (v.overV) {
        const float availH = v.content.height - (v.overH ? kScrollBarW : 0.f);
        v.vTrack = { v.content.x + v.content.width - kScrollBarW, v.content.y, kScrollBarW, availH };
        const float thumbH = std::max(24.f, availH * (v.content.height / v.pageH));
        const float pos    = maxScrollY > 0.f ? -m_scrollY / maxScrollY : 0.f;
        v.vThumb = { v.vTrack.x + 1.f, v.vTrack.y + pos * (availH - thumbH), kScrollBarW - 2.f, thumbH };
    }
    if (v.overH) {
        const float availW = v.content.width - (v.overV ? kScrollBarW : 0.f);
        v.hTrack = { v.content.x, v.content.y + v.content.height - kScrollBarW, availW, kScrollBarW };
        const float thumbW = std::max(24.f, availW * (v.content.width / v.pageW));
        const float pos    = maxScrollX > 0.f ? -m_scrollX / maxScrollX : 0.f;
        v.hThumb = { v.hTrack.x + pos * (availW - thumbW), v.hTrack.y + 1.f, thumbW, kScrollBarW - 2.f };
    }
    return v;
}

void ViewportWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    PERF_SCOPE("Viewport::render");
    const PanelElement& el = *m_el;   // container: content fill, then the node's page children
    const int br = std::max(0, skin::num(el, "borderRadius", 6));
    const float lh = jf::JTextHelper::lineHeight();
    // Background fills here; the FRAME (border) and title bar are painted in drawBorder() — i.e. AFTER the base
    // pops the content clip — so the 1px edge frame is never clipped at the widget's own bounds.
    uint8_t bgb[4]; const uint8_t* bg = elColor(el, "bgColor", jf::Colors::Surface1, bgb);   // Background, else the default content fill
    buf.pushRectangle(r.x, r.y, r.width, r.height, bg, static_cast<float>(br));
    const std::string node = pageKey();
    const PanelModel* pm = nodeviewport::resolver() ? nodeviewport::resolver()(node) : nullptr;
    const float top = r.y + viewportTitleBarH(el);
    const jf::JRect content{ r.x + 2.f, top + 2.f, r.width - 4.f, (r.y + r.height) - top - 4.f };
    reconcile(node);   // pick up any live add/remove in the mirrored page (cheap set-delta)
    refreshChildrenIfChanged(pm);   // re-sync child data only on a real edit to the source page (not per frame)
    bool any = false;
    const View view = viewOf(r, pm);
    if (pm && content.width > 4.f && content.height > 4.f) {
        const float sc = view.sc, ox = view.ox, oy = view.oy;
        // THE MIRRORED PAGE'S OWN LAYOUT, applied here as the Surface applies it when the same page is
        // opened directly. This did not happen at all: every child was drawn at its authored rect times
        // the scale, so a page that arranges itself — a converted dialog with its footer as a South strip
        // — was mirrored as though it had no layout, and the footer landed wherever it was authored,
        // which for a reflowing page is the top-left corner. A page must not look different depending on
        // which way you reached it.
        const int pageLayout = pm->layout();
        const std::vector<LayoutChild> lc = mirroredLayoutChildren(pm);
        const jf::JRect pageBox{ ox, oy, view.pageW, view.pageH };
        int lcIndex = -1;
        buf.pushClip(content.x, content.y, content.width, content.height);
        for (const auto& pe : pm->elements()) {
            ++lcIndex;
            if (!pe.prop("node").empty()) continue;
            auto it = m_childByEl.find(pe.id);
            if (it == m_childByEl.end() || !it->second) continue;
            CanvasWidget* child = it->second.get();
            // Visibility condition (run mode only, like the surface's own loop): a child whose condition
            // evaluates false is hidden at runtime but still shown in the editor so it stays selectable.
            // Asked of the CHILD, because on a template page the condition names "[*]" and only the child
            // knows which element this viewport is showing.
            if (!s_editMode && !child->visibleNow()) continue;
            const jf::JRect cr = mirroredChildRect(pageLayout, pageBox, lc, lcIndex, pm->gridColumns(),
                                                  pm->focusIndex(), pe, sc, ox, oy);
            const std::string sig = child->bindPath();   // the child already carries this viewport's sensor
            child->setValue(sig.empty() ? 0.0 : MathEvaluator::instance().evaluate(sig));
            child->setCache(&c);
            child->setRenderZoom(sc);
            child->setBounds(cr);
            // Mirror the Surface's disabled treatment so a mirrored child fades on a false enableCondition
            // here too, not only when the page is opened directly.
            // Disabled CASCADES, as it does through a panel: the Surface deliberately does NOT wash a
            // container, on the understanding that its children carry the state down and fade themselves.
            // A viewport never did, so a viewport with a false enableCondition greyed nothing whatsoever
            // and stayed fully editable — the condition read as ignored outright.
            const bool selfOff = !child->enabledNow();
            child->setRenderDisabled(!s_editMode && (renderDisabled() || selfOff));
            child->populateRenderPrimitives(buf);
            // ...and the VEIL, which is the only thing that makes "disabled" visible for every widget that
            // does not fade itself. Setting the flag alone was enough for a label (it dims its own caption)
            // and for nothing else: a mirrored spin box, checkbox or table went inert on a false condition
            // while still looking live and editable — the one failure worse than not honouring the condition
            // at all. Same rule as the Surface and the panel: a container is not washed, its children are.
            if (child->renderDisabled() && pe.type != "label" && pe.type != "panel" && pe.type != "viewport")
                disabledWash(buf, cr);
            // Keyboard focus inside a viewport belongs to the CHILD, never to the container: the Surface sees
            // this whole viewport as one control, so without drawing it here the ring landed around the entire
            // viewport when a single field inside it was focused. Children that paint their own focus (the
            // hosted framework controls) do it themselves.
            // focusRingEnabled: the same question the Surface asks before drawing its own ring, and the
            // panel asks before drawing one for its child. Without it a widget whose "Focus Ring" is set
            // to Off — or a table when the global table.focusRing is Off — kept its ring when mirrored.
            if (!s_editMode && pe.id == m_activeChild && !child->drawsOwnFocus() && focusRingEnabled(pe)) {
                static const uint8_t clear[4] = { 0, 0, 0, 0 };
                buf.pushRectangle(cr.x - 2.f, cr.y - 2.f, cr.width + 4.f, cr.height + 4.f, clear, 3.f, 2.f,
                                  jf::Colors::Accent);
            }
            any = true;
        }
        buf.popClip();
    }
    if (!any && jf::JTextHelper::hasAtlas()) {
        const char* msg = node.empty() ? "(no node)" : (pm ? "(empty page)" : "(unresolved node)");
        wraptext::drawCentred(buf, content.x + 4.f, content.y + (content.height - wraptext::height(msg, content.width - 8.f)) * 0.5f,
                              content.width - 8.f, msg, jf::Colors::TextSecondary);   // wrapped, centred as a block
    }
    // The bars, over the content and OUTSIDE the clip, so a page taller than its viewport says so — and can
    // be dragged there. Same look as the surface's own bars.
    static const uint8_t track[4] = { 30, 30, 34, 200 };
    if (view.overV) {
        buf.pushRectangle(view.vTrack.x, view.vTrack.y, view.vTrack.width, view.vTrack.height, track, 2.f);
        buf.pushRectangle(view.vThumb.x, view.vThumb.y, view.vThumb.width, view.vThumb.height,
                          jf::Colors::Surface3, 3.f);
    }
    if (view.overH) {
        buf.pushRectangle(view.hTrack.x, view.hTrack.y, view.hTrack.width, view.hTrack.height, track, 2.f);
        buf.pushRectangle(view.hThumb.x, view.hThumb.y, view.hThumb.width, view.hThumb.height,
                          jf::Colors::Surface3, 3.f);
    }
}

void ViewportWidget::drawBorder(jf::JPrimitiveBuffer& buf, const jf::JRect& r) const {
    const PanelElement& el = *m_el;
    // pageKey(), NOT viewportPage(el): the latter is empty for an unbound viewport, so pm came back null
    // and the page's own title — and its border settings — were silently dropped. The title is drawn
    // here, in the post-content pass, which is why it was the one thing that stayed missing.
    const PanelModel* pm = mirroredPage();
    // Unclipped frame pass — the base calls this AFTER popping the content clip, so a 1px edge border is not
    // trimmed at the widget bounds. Border: the viewport's OWN if it has one, else the mirrored PAGE's border
    // — so a page's frame shows through a borderless viewport, as it does when the page is drilled into
    // (Surface::drawFrame). (Border style dash/dot isn't reproduced here — solid width/colour/radius only.)
    int bw = resolvedBorderWidth(), brad = resolvedBorderRadius();
    uint8_t bcb[4];
    const uint8_t* bc = elColor(el, "borderColor", jf::Colors::Border, bcb);   // Border colour, else global
    if (bw <= 0 && pm && pm->borderWidth() > 0) {                              // fall back to the mirrored page's
        bw = pm->borderWidth(); brad = pm->borderRadius();
        bc = skin::parseHex(pm->borderColor(), bcb) ? bcb : jf::Colors::Border;
    }
    if (bw > 0) paintBorder(buf, r, bw, brad, bc);

    // Title (own caption, else the mirrored page's) OVERLAYS the content top — reserves no height, so it
    // never adds a scroll bar. drawn at r.y over whatever content is below.
    const std::string title = titleShown();
    if (!title.empty() && jf::JTextHelper::hasAtlas()) {
        const int br = std::max(0, skin::num(el, "borderRadius", 6));
        const float lh = jf::JTextHelper::lineHeight();
        const float titlePx = fontSpecPx(el.prop("fontName"), lh);                 // Title Font size ("family|size|b|i")
        const float barH = titlePx + 6.f;
        uint8_t tcb[4]; const uint8_t* tc = elColor(el, "fgColor", jf::Colors::TextPrimary, tcb);   // Title Colour, else global
        buf.pushRectangle(r.x, r.y, r.width, barH, jf::Colors::Surface2, static_cast<float>(br));
        jf::JTextHelper::pushTextScaled(buf, r.x + 8.f, r.y + 3.f, title, tc,
                                        lh > 0.f ? titlePx / lh : 1.f, 0.f, fontSpecFace(el.prop("fontName")));
    }
}

int ViewportWidget::mirroredElementAt(const jf::JRect& r, float mx, float my, PanelModel*& outModel,
                                     jf::JRect* outRect) const {
    outModel = nullptr;
    const PanelElement& el = *m_el;
    const std::string node = pageKey();
    const PanelModel* pm = nodeviewport::resolver() ? nodeviewport::resolver()(node) : nullptr;
    if (!pm) return 0;
    float top = r.y + viewportTitleBarH(el);   // same content inset as render() (own caption only)
    const jf::JRect content{ r.x + 2.f, top + 2.f, r.width - 4.f, (r.y + r.height) - top - 4.f };
    if (content.width <= 4.f || content.height <= 4.f) return 0;
    const View view = viewOf(r, pm);
    const float sc = view.sc, ox = view.ox, oy = view.oy;
    const auto& es = pm->elements();
    const int pageLayout = pm->layout();
    const std::vector<LayoutChild> lc = mirroredLayoutChildren(pm);
    const jf::JRect pageBox{ ox, oy, view.pageW, view.pageH };
    for (size_t i = es.size(); i-- > 0; ) {
        const auto& pe = es[i];
        if (!pe.prop("node").empty()) continue;
        const jf::JRect cr = mirroredChildRect(pageLayout, pageBox, lc, int(i), pm->gridColumns(),
                                               pm->focusIndex(), pe, sc, ox, oy);
        if (mx >= cr.x && mx < cr.x + cr.width && my >= cr.y && my < cr.y + cr.height) {
            outModel = const_cast<PanelModel*>(pm);   // the node page is the real editable source behind the mirror
            if (outRect) *outRect = cr;
            return pe.id;
        }
    }
    return 0;
}

// Move the keyboard owner inside this viewport, telling the OUTGOING child it lost it. Two ConfigEdits in the
// same viewport are one element as far as the Surface is concerned (activeControl_ is the VIEWPORT), so the
// Surface's blur never fires when the user clicks from one to the other -- without this, the first one stayed in
// edit mode with its caret up and its typed text uncommitted.
// The screen rect of ONE mirrored child — the same geometry the dispatch loop hands a click.
//
// A key, a focus and a blur carry no cursor, so nothing hit-tests and they used to be delivered with the
// VIEWPORT's rect. Any widget that derives its layout from the rect it is given then computed one geometry
// for a click and another for a keystroke: a 15-row strip handed the whole page believed 32 rows fitted,
// concluded there was nothing to scroll, and walked its selection off the bottom of a box it thought was
// bigger than it is — the arrow keys appeared to do nothing at all.
jf::JRect ViewportWidget::_childRectOf(int id, const jf::JRect& r) const {
    const std::string node = pageKey();
    const PanelModel* pm = nodeviewport::resolver() ? nodeviewport::resolver()(node) : nullptr;
    if (!pm) return r;
    const View view = viewOf(r, pm);
    // Through the SAME rect the render and the hit-test use. This one placed a control at its authored
    // position while the page's layout had moved it, so a keyboard focus ring, a blur and every follow-up
    // input after the first click were all aimed at where the widget was not.
    const std::vector<LayoutChild> lc = mirroredLayoutChildren(pm);
    const jf::JRect pageBox{ view.ox, view.oy, view.pageW, view.pageH };
    const auto& es = pm->elements();
    for (size_t i = 0; i < es.size(); ++i)
        if (es[i].id == id)
            return mirroredChildRect(pm->layout(), pageBox, lc, int(i), pm->gridColumns(),
                                     pm->focusIndex(), es[i], view.sc, view.ox, view.oy);
    return r;
}

void ViewportWidget::_setActiveChild(int id, const jf::JRect& r) {
    if (m_activeChild == id) return;
    const int prev = m_activeChild;
    m_activeChild = id;
    if (!prev) return;
    auto it = m_childByEl.find(prev);
    if (it == m_childByEl.end() || !it->second) return;
    ControlInput blur; blur.kind = ControlInput::Kind::Blur;
    it->second->setCache(m_cache);
    it->second->onControlInput(_childRectOf(prev, r), blur);
}

// The mirrored children that can take the keyboard, in the order the page READS — not the order it was
// authored in (see CanvasWidget::readingOrder).
std::vector<int> ViewportWidget::_childOrder(const PanelModel* pm, bool assumeEnabled) const {
    std::vector<TabSlot> slots;
    if (!pm) return {};
    for (const auto& pe : pm->elements()) {
        if (!pe.prop("node").empty()) continue;
        // A child HIDDEN by its condition is not on the page, so it cannot be in the ring. The Surface has
        // always filtered its own ring this way (runTarget_); the viewport never did, so Tab handed the
        // keyboard to controls that were not drawn — focus simply vanished for a press or two.
        auto it = m_childByEl.find(pe.id);
        if (it != m_childByEl.end() && it->second && !it->second->visibleNow()) continue;
        // A child greyed by its enableCondition is out of the tab ring as well as out of the mouse's reach —
        // asked through acceptsInput so this site cannot drift from the rule the other two use. Tabbing onto
        // one would have handed it a Focus, which for a hosted control BEGINS ITS EDIT: the caret would sit
        // blinking in a field that refuses every key.
        if (it != m_childByEl.end() && it->second && it->second->interactive() &&
            (assumeEnabled || it->second->acceptsInput(ControlInput::Kind::Focus)))
            slots.push_back({ pe.id, (float)pe.x, (float)pe.y, (float)pe.h });
    }
    return CanvasWidget::readingOrder(std::move(slots));
}

// Move the keyboard to the next/previous child. A viewport is a NESTED focus domain: the Surface sees it as one
// control, so Tab has to walk the children in here, and when it runs off either end it returns false to hand the
// key back -- the Surface then moves to the next top-level control. It does NOT wrap, or the keyboard could
// never leave the viewport.
// Asked from outside (the page audit): the ring this viewport would walk over the page it mirrors.
std::vector<int> ViewportWidget::keyboardRing(bool assumeEnabled) {
    const PanelModel* pm = nodeviewport::resolver() ? nodeviewport::resolver()(pageKey()) : nullptr;
    return pm ? _childOrder(pm, assumeEnabled) : std::vector<int>{};
}

bool ViewportWidget::_focusChild(int dir, const jf::JRect& r, const PanelModel* pm) {
    const auto ids = _childOrder(pm);
    if (ids.empty()) return false;
    int cur = -1;
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == m_activeChild) { cur = (int)i; break; }
    const int next = (cur < 0) ? (dir > 0 ? 0 : (int)ids.size() - 1) : cur + dir;
    if (next < 0 || next >= (int)ids.size()) { _setActiveChild(0, r); return false; }
    _setActiveChild(ids[next], r);
    auto it = m_childByEl.find(ids[next]);
    if (it == m_childByEl.end() || !it->second) return false;
    ControlInput f; f.kind = ControlInput::Kind::Focus; f.focused = true;   // tabbed into it -> begin its edit
    it->second->setCache(m_cache);
    it->second->onControlInput(_childRectOf(ids[next], r), f);
    return true;
}

bool ViewportWidget::handleControlInput(const jf::JRect& r, const ControlInput& in) {
    const PanelElement& el = *m_el;
    const std::string node = pageKey();
    const PanelModel* pm = nodeviewport::resolver() ? nodeviewport::resolver()(node) : nullptr;
    if (!pm) return false;
    reconcile(node);
    refreshChildrenIfChanged(pm);   // children carry their own runtime state; re-sync only on a real source edit
    // A keystroke has no cursor position, so it goes to the child that last took focus in here -- NOT to
    // every child. Broadcasting was deliberate once (so a table with an armed inline op could receive its
    // typed operand) and was safe only while children ignored keys they had no focus for. A hosted
    // JDoubleSpinBox accepts digits unconditionally, so a broadcast put EVERY ConfigEdit in the viewport
    // into edit mode and each committed to its own config path: typing 98 into max_tps_pct also wrote 98
    // into min_tps_pct, straight through to the ECU. The armed-table case still works, because arming it
    // requires interacting with it, which is what makes it the active child.
    // Losing keyboard focus: the child that owns it must hear about it (a hosted ConfigEdit commits its text
    // and drops its caret), and nothing in here owns the keyboard afterwards.
    if (in.kind == ControlInput::Kind::Blur) {
        const bool had = m_activeChild != 0;
        _setActiveChild(0, r);
        m_ptrCapture = 0;      // focus left without a release ever arriving; do not hold a stale grab
        return had;
    }
    // Keyboard focus arrived without a click (Tab into the viewport): hand it to the first child.
    if (in.kind == ControlInput::Kind::Focus) return _focusChild(1, r, pm);

    if (in.kind == ControlInput::Kind::Key) {
        JLOGC("surface.key", jf::JLogLevel::Debug) << "  viewport: activeChild=" << m_activeChild
            << " children=" << m_childByEl.size() << " (this=" << static_cast<const void*>(this) << ")";
        if (m_activeChild) {
            auto it = m_childByEl.find(m_activeChild);
            if (it == m_childByEl.end() || !it->second) m_activeChild = 0;
            else {
                it->second->setCache(m_cache);
                ControlInput sub = in; sub.focused = true;
                if (it->second->acceptsInput(in.kind) &&
                    it->second->onControlInput(_childRectOf(m_activeChild, r), sub)) return true;
            }
        }
        // The active child did not want it. A Tab moves the keyboard WITHIN this viewport; anything else bubbles.
        // Same portable rule as everywhere else (X11 BackTab vs Windows/macOS Tab+shift).
        if (in.key && in.key->pressed && !in.key->ctrl && !in.key->alt && jf::jIsTabNav(*in.key))
            return _focusChild(jf::jTabNavDir(*in.key), r, pm);
        return false;
    }
    const View view = viewOf(r, pm);
    const jf::JRect content = view.content;
    if (content.width <= 4.f || content.height <= 4.f) return false;
    auto inRect = [](const jf::JRect& q, float x, float y) {
        return x >= q.x && x < q.x + q.width && y >= q.y && y < q.y + q.height;
    };
    // A thumb DRAG, before anything else: the bars sit over the content, so a child under one must not take
    // the press instead. Release ends the drag wherever the cursor is.
    if (in.kind == ControlInput::Kind::Release) { const bool had = m_barDrag != 0; m_barDrag = 0; if (had) return true; }

    // POINTER CAPTURE — the gesture belongs to whoever took the press, wherever the cursor goes.
    //
    // Without this, pointer events were routed by POSITION on every frame, and a child only heard from the
    // viewport while the cursor was inside its rect. Press a button and slide off it and the button was
    // simply abandoned mid-gesture: no move to spring it back out of its pressed look, and no release at
    // all — so it sat highlighted for ever, still armed, and the click could not be cancelled the way it
    // can in every other toolkit. (Cancelling by releasing outside is exactly what JControl implements;
    // it just never got the events to implement it with.) Everything else already worked this way — the
    // Surface routes to activeControl_, JAppWindow captures the centre and the dock a press landed on,
    // the scroll thumbs above capture their drag — and this is the one level that did not.
    if (m_ptrCapture && (in.kind == ControlInput::Kind::Move || in.kind == ControlInput::Kind::Release)) {
        const int id = m_ptrCapture;
        if (in.kind == ControlInput::Kind::Release) m_ptrCapture = 0;   // the button coming up ends it
        auto it = m_childByEl.find(id);
        if (it == m_childByEl.end() || !it->second) return false;
        ControlInput sub = in; sub.focused = true;
        it->second->setCache(m_cache);
        return it->second->acceptsInput(in.kind) && it->second->onControlInput(_childRectOf(id, r), sub);
    }
    if (m_barDrag && in.kind == ControlInput::Kind::Move) {
        if (m_barDrag == 1 && view.overV) {
            const float travel = view.vTrack.height - view.vThumb.height;
            const float over   = view.pageH - content.height;
            if (travel > 0.f) m_scrollY = -std::clamp((in.my - m_barGrab - view.vTrack.y) / travel, 0.f, 1.f) * over;
        } else if (m_barDrag == 2 && view.overH) {
            const float travel = view.hTrack.width - view.hThumb.width;
            const float over   = view.pageW - content.width;
            if (travel > 0.f) m_scrollX = -std::clamp((in.mx - m_barGrab - view.hTrack.x) / travel, 0.f, 1.f) * over;
        }
        return true;
    }
    if (in.kind == ControlInput::Kind::Press) {
        if (view.overV && inRect(view.vThumb, in.mx, in.my)) { m_barDrag = 1; m_barGrab = in.my - view.vThumb.y; return true; }
        if (view.overH && inRect(view.hThumb, in.mx, in.my)) { m_barDrag = 2; m_barGrab = in.mx - view.hThumb.x; return true; }
    }
    // The WHEEL pans the page whenever it overflows. A child only gets the wheel if it is the one that has
    // focus in here — hovering a spin box must not swallow the scroll (or quietly re-tune the field) — or
    // if it is itself a SCROLLING VIEW under the cursor, which is what a wheel is for. Without that second
    // case the page always won, and a strip with more rows than it can show could be scrolled by dragging
    // its bar and by no other means. See CanvasWidget::wantsWheel.
    if (in.kind == ControlInput::Kind::Scroll && (view.overV || view.overH) &&
        !_childWantsWheel(pm, view, in.mx, in.my)) {
        const float step = 48.f;
        if (view.overV) m_scrollY += in.wheel * step;
        else            m_scrollX += in.wheel * step;
        return true;
    }
    const float sc = view.sc, ox = view.ox, oy = view.oy;
    const auto& es = pm->elements();
    const int pageLayout = pm->layout();
    const std::vector<LayoutChild> lc = mirroredLayoutChildren(pm);
    const jf::JRect pageBox{ ox, oy, view.pageW, view.pageH };
    for (size_t i = es.size(); i-- > 0; ) {
        const auto& pe = es[i];
        if (!pe.prop("node").empty()) continue;
        const jf::JRect cr = mirroredChildRect(pageLayout, pageBox, lc, int(i), pm->gridColumns(),
                                               pm->focusIndex(), pe, sc, ox, oy);
        if (in.mx >= cr.x && in.mx < cr.x + cr.width && in.my >= cr.y && in.my < cr.y + cr.height) {
            auto it = m_childByEl.find(pe.id);
            if (it == m_childByEl.end() || !it->second) return false;
            if (!it->second->visibleNow()) continue;   // hidden: not drawn, so not a click target either
            if (in.kind == ControlInput::Kind::Press) {
                JLOGC("surface.key", jf::JLogLevel::Debug) << "  viewport: press \xE2\x86\x92 activeChild=" << pe.id
                    << " type=" << pe.type << " (this=" << static_cast<const void*>(this) << ")";
                _setActiveChild(pe.id, r);   // this child now owns the keyboard
            }
            ControlInput sub = in; sub.focused = true;
            it->second->setCache(m_cache);
            // Greyed means INERT — the same gate the Surface and the panel put on every delivery. Without it
            // a mirrored control disabled by its condition still took the press and wrote the tune, which is
            // the UI lying about what it will do.
            const bool took = it->second->acceptsInput(in.kind) && it->second->onControlInput(cr, sub);
            // A press that was actually TAKEN captures the pointer. An inert child captures nothing —
            // otherwise a greyed control would swallow the rest of the gesture on its way to no effect.
            if (in.kind == ControlInput::Kind::Press && took) m_ptrCapture = pe.id;
            return took;
        }
    }
    if (in.kind == ControlInput::Kind::Press) _setActiveChild(0, r);   // pressed empty space -> nobody owns the keyboard
    return false;
}

// A viewport is a container as well: a page that overflows scrolls, and so does a scrolling child in it.
bool ViewportWidget::wantsWheel(const jf::JRect& r, float mx, float my) {
    if (!m_el) return false;
    const std::string node = pageKey();
    const PanelModel* pm = nodeviewport::resolver() ? nodeviewport::resolver()(node) : nullptr;
    if (!pm) return false;
    const View view = viewOf(r, pm);
    return view.overV || view.overH || _childWantsWheel(pm, view, mx, my);
}

// Is the mirrored child under the cursor a scrolling view? Same geometry the dispatch loop uses, so the
// widget that answers is the widget the wheel would be delivered to.
bool ViewportWidget::_childWantsWheel(const PanelModel* pm, const View& view, float mx, float my) {
    if (!pm) return false;
    const float sc = view.sc, ox = view.ox, oy = view.oy;
    const auto& es = pm->elements();
    const int pageLayout = pm->layout();
    const std::vector<LayoutChild> lc = mirroredLayoutChildren(pm);
    const jf::JRect pageBox{ ox, oy, view.pageW, view.pageH };
    for (size_t i = es.size(); i-- > 0; ) {
        const auto& pe = es[i];
        if (!pe.prop("node").empty()) continue;
        const jf::JRect cr = mirroredChildRect(pageLayout, pageBox, lc, int(i), pm->gridColumns(),
                                               pm->focusIndex(), pe, sc, ox, oy);
        if (mx < cr.x || mx >= cr.x + cr.width || my < cr.y || my >= cr.y + cr.height) continue;
        auto it = m_childByEl.find(pe.id);
        if (it == m_childByEl.end() || !it->second) return false;
        if (!it->second->visibleNow()) continue;
        it->second->setCache(m_cache);
        return it->second->acceptsInput(ControlInput::Kind::Scroll) &&
               it->second->wantsWheel(cr, mx, my);
    }
    return false;
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("viewport", ViewportWidget, 210);
