// PanelWidget — render / run-mode input / @-sigil state, extracted from the former descriptors translation unit.
// Pure relocation: the method bodies are byte-identical; only their file + includes changed.

#include "PanelWidget.h"
#include "../../ui/WrapText.h"
#include "../StyleMetrics.h"
#include <j/core/Log.h>
#include "../../model/Perf.h"
#include "../ContainerLayout.h"
#include "../WidgetRegistry.h"
#include "../../model/Cache.h"
#include "../../model/MathEvaluator.h"
#include <j/config/Json.h>

// A panel's children are authored as a JSON array in the "children" prop. rebuildChildren() reparses it into
// persistent, OWNED child instances (m_children ‖ m_childData) — but only when the spec actually changes, so
// the child objects live for the panel's lifetime (a nested panel builds ITS children the same way, on its
// first setData → the whole subtree persists). Called from loadContent(), i.e. every setData().
void PanelWidget::loadContent(const PanelElement& el) {
    m_layoutMode  = skin::num(el, "layoutMode", 0);     // managed-layout params — refreshed every setData (BEFORE
    m_gridColumns = skin::num(el, "gridColumns", 2);    // the spec early-return, so a layout change re-lays out the
    m_focusIndex  = skin::num(el, "focusIndex", 0);     // existing children without rebuilding the subtree)
    m_layoutGap   = skin::num(el, "layoutGap", 0);      // Wrap's gutter; 0 = pack edge to edge, as before
    m_helpText    = el.prop("helpText");                 // optional ? badge help (shown via the message dialog)
    const std::string spec = el.prop("children");
    if (spec == m_childrenSpec) return;                 // unchanged — the persistent children stand
    m_childrenSpec = spec;
    m_children.clear();
    m_childData.clear();
    if (auto j = jf::JJson::tryParse(spec); j && j->isArray()) {
        int ci = 0;
        for (const auto& w : j->arr()) {
            PanelElement ce;
            ce.id = ++ci;                               // authored children carry no id — index within the panel
            ce.type = w["type"].str();
            ce.uid = w["uid"].str();                    // usually absent (then not @-addressable)
            ce.x = static_cast<float>(w["x"].number()); ce.y = static_cast<float>(w["y"].number());
            ce.w = static_cast<float>(w["w"].number(80)); ce.h = static_cast<float>(w["h"].number(40));
            for (const auto& [k, v] : w["props"].obj()) ce.props[k] = v.str();
            m_childData.push_back(std::move(ce));
        }
        for (const PanelElement& ce : m_childData) {
            std::unique_ptr<CanvasWidget> inst = makeWidgetInstance(ce.type, sceneGraph());
            if (inst) {
                inst->setData(resolveElement(ce));        // build the (possibly nested) subtree now
                // THE PARENT EDGE. A child is inside this panel, so it says so — the same edge a viewport
                // gives what it mirrors, and for more than tidiness: the framework's tooltip search
                // DESCENDS the widget tree (jTooltipHitTest), so a control with no edge is never reached.
                // Every control on a page that puts its fields in cards was therefore silent on hover,
                // while the identical control dropped straight onto the page explained itself. Ownership
                // stays with m_children — an edge is not ownership.
                addChild(inst.get());
            }
            m_children.push_back(std::move(inst));
        }
    }
}

// The scale a panel draws its OWN children at — one function, so the hit-test and the paint cannot drift.
//
// It is UNIFORM, and it is the scale that FITS: min(width ratio, height ratio). A panel's rect is not always
// a scaled copy of what it was authored at — a managed parent (X Axis, Border, Grid) hands it a SLOT, and a
// slot is usually a different shape. Scaling each axis by its own ratio then stretched the contents to fill:
// a converted TunerStudio column, authored tall enough for its rows and dropped into a full-height slot, had
// its rows spread down the column instead of packed at the top where they were laid out. More room is
// more room — it is not a bigger scale.
//
// The title band and the 2px insets are CHROME: they are drawn at a fixed size regardless of scale, so they
// come out of the authored extent before the ratio is taken. Without that, a panel's content came out
// slightly squashed, and the shorter the panel the worse it got.
static float panelContentScale(const PanelElement& el, const jf::JRect& content, bool hasTitle, float lh) {
    const float chromeH = (hasTitle ? lh + 6.f : 0.f) + 4.f;
    const float sx = content.width  / std::max(1.f, el.w - 4.f);
    const float sy = content.height / std::max(1.f, el.h - chromeH);
    return std::max(0.01f, std::min(sx, sy));
}

void PanelWidget::render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) {
    PERF_SCOPE("Panel::render");
    const PanelElement& el = *m_el;   // container: draws its own frame + title, then its OWNED persistent children
    const int br = std::max(0, skin::num(el, "borderRadius", 6));
    uint8_t bcb[4]; const uint8_t* bc = elColor(el, "borderColor", jf::Colors::Border, bcb);
    uint8_t clear[4] = {0, 0, 0, 0};
    // The frame follows the BASE's border rule, like every other widget: a panel used to draw its 1px box
    // unconditionally, so "no border" was not expressible and a panel used purely to group things (a gauge's
    // parts, say) always came with a box round it.
    if (const int bw = resolvedBorderWidth(); bw > 0)
        buf.pushRectangle(r.x, r.y, r.width, r.height, clear, static_cast<float>(br), float(bw), bc);
    const float lh = jf::JTextHelper::lineHeight();
    const float top = drawTitleBar(buf, r);   // the shared card chrome — see CanvasWidget::drawTitleBar
    if (!m_helpText.empty() && jf::JTextHelper::hasAtlas()) {   // "?" help badge (top-right); run-mode click shows the text
        const float bh = lh + 6.f, bx = r.x + r.width - bh;
        buf.pushRectangle(bx, r.y, bh, bh, jf::Colors::Surface3, static_cast<float>(br));
        jf::JTextHelper::pushText(buf, bx + (bh - jf::JTextHelper::measureWidth("?")) * 0.5f, r.y + 3.f, "?", jf::Colors::Accent, bh);
    }
    const ChildBasis basis = _childBasis(r);       // the ONE geometry — see _childBasis
    const jf::JRect content = basis.content;
    // TEXT DOES NOT SHRINK WITH THE BOX under a managed layout. This is the zoom every child draws its
    // type at, and it was the free-fit scale — so a Column narrower than it was authored told its captions
    // to render at the width ratio: 3 px of ink beside a 9 px control, on the same row. A managed layout
    // moves and resizes boxes; it does not re-scale what is drawn in them. Free still gets the fit scale,
    // because there the whole point is that the drawing keeps the shape it was authored in.
    const float z = (m_layoutMode == 0) ? basis.z : 1.f;
    const std::vector<LayoutChild>& lc = basis.lc;
    bool any = false;
    for (size_t i = 0; i < m_children.size(); ++i) {
        CanvasWidget* child = m_children[i].get();
        if (!child) continue;
        const PanelElement& ce = m_childData[i];
        // WHAT THE CHILD IS, BEFORE ASKING WHAT IT SAYS. Its element and its data have to be in place before
        // any condition on it is evaluated: a condition is an expression about THIS element.
        //
        // The context passes THROUGH a panel. A template page names its element as "[*]" and the host
        // viewport pushes the real one down to what it mirrors — but only to its OWN children, so a control
        // inside a panel got nothing and its "[*]" never resolved. A panel is a container, not a boundary.
        //
        // This used to sit BELOW the visibility test, and the test's `continue` skipped it — so a child that
        // was hidden on the first frame never received an element and was judged for ever after against an
        // unresolved "[*]", which reads as 0. On a Card panel (every child gets the full rect and the
        // conditions pick one) that is fatal: on frame one nothing has a context, "primitive == 0" is
        // 0 == 0 and the GAP card wins by accident; from then on the other two are frozen out, and a stream
        // that is not GAP shows an EMPTY CARD. It looked like a copied page had lost its bindings.
        child->setElementContext(elementContext());
        child->setData(resolveElement(ce));
        // A child's conditions are honoured HERE, exactly as the Surface and a viewport honour them for the
        // children they own — otherwise a control's "visible"/"enabled" expression worked on a page but did
        // nothing the moment that control was inside a panel. Run mode only, so the editor can still reach a
        // conditionally-hidden child. It is skipped for DRAWING only: its layout slot stays, so hiding one
        // child never reshuffles its siblings.
        if (!s_editMode) {
            if (!child->visibleNow()) continue;
        }
        // Disabled CASCADES: a disabled panel disables everything in it, which is how a whole region behind
        // one condition greys out without the container painting a slab over its own children.
        const bool selfOff = !child->enabledNow();
        child->setRenderDisabled(!s_editMode && (renderDisabled() || selfOff));
        const jf::JRect cr = _childRect(basis, i);   // Free = the scaled stored rect; managed modes arrange
        const std::string sig = child->bindPath();   // resolved, so a templated child reads its own sensor
        child->setValue(sig.empty() ? 0.0 : MathEvaluator::instance().evaluate(sig));
        child->setCache(&c);
        // The same scale the geometry used: a managed layout stretches a child's RECT, and text must not
        // stretch with it.
        child->setRenderZoom(z);
        child->setBounds(cr);
        child->populateRenderPrimitives(buf);
        // The veil for a disabled child, as the Surface draws it for a disabled top-level element. A label
        // fades its own caption, and a nested container passes the state down to ITS children instead.
        if (child->renderDisabled() && ce.type != "label" && ce.type != "panel" && ce.type != "viewport")
            disabledWash(buf, cr);
        // THE FOCUS RING FOR THE CHILD THAT HOLDS THE KEYBOARD, because nobody else will draw it.
        //
        // The Surface draws one ring per top-level element for every widget that does not paint its own —
        // and it can only ever see the PANEL, which reports drawsOwnFocus() precisely so that ring is not
        // drawn around the whole container when one control inside it is focused. That left the hosted
        // framework controls (spin boxes, fields) fine, since they paint their own, and everything that
        // relies on the Surface — a table, a curve, a 1D array — with no ring at all once it was a panel's
        // child. Same widget, same document, ringed on its own page and silent inside a panel: an injector
        // stage's tables took the keyboard and gave no sign they had it.
        //
        // ViewportWidget has drawn this for its mirrored children all along, for exactly this reason. This
        // is that block, against the panel's own geometry. focusRingEnabled honours the per-widget "Focus
        // Ring" property and the global table.focusRing setting, so a ring turned off on a page stays off
        // in a panel — the Surface asks the same question before drawing its own.
        if (!s_editMode && ce.id == m_activeChild && !child->drawsOwnFocus() && focusRingEnabled(ce)) {
            static const uint8_t clear[4] = { 0, 0, 0, 0 };
            buf.pushRectangle(cr.x - 2.f, cr.y - 2.f, cr.width + 4.f, cr.height + 4.f, clear, 3.f, 2.f,
                              jf::Colors::Accent);
        }
        any = true;
    }
    if (!any && jf::JTextHelper::hasAtlas())
        wraptext::drawCentred(buf, content.x + 4.f, content.y + (content.height - wraptext::height("(empty panel)", content.width - 8.f)) * 0.5f,
                              content.width - 8.f, "(empty panel)", jf::Colors::TextSecondary);   // wraps in a narrow panel
}

// WHERE THE CHILDREN ARE. The panel's content box, its content scale, and the layout list — the basis
// every child rect is derived from. It existed three times over (the hit-test, childElementAt, and the
// wheel question), which is three chances for the geometry a click uses to drift from the one a paint
// used. Derived once here; _childRect turns it into the rect for one child.
PanelWidget::ChildBasis PanelWidget::_childBasis(const jf::JRect& r) const {
    ChildBasis b;
    const PanelElement& el = *m_el;
    const float lh = jf::JTextHelper::lineHeight();
    const bool titled = titleBarH() > 0.f;
    const float top = r.y + titleBarH();       // the same bar the paint draws — see CanvasWidget::titleBarH
    b.content = jf::JRect{ r.x + 2.f, top + 2.f, r.width - 4.f, (r.y + r.height) - top - 4.f };
    b.z = panelContentScale(el, b.content, titled, lh);
    // …AND THAT FIT SCALE IS FOR FREE CHILDREN ONLY.
    //
    // panelContentScale is min(width ratio, height ratio) — the right answer for a box of coordinates,
    // which has to keep its shape. It is the wrong answer for a MANAGED layout, whose whole job is to
    // arrange into the box it is given: Column multiplies each row's height by the scale it is handed, so
    // a panel narrower than it was authored — the width ratio wins the min — squashed its rows vertically
    // until a form was a stack of unreadable slivers. Nothing was too tall; the rows were told to be short.
    //
    // So a managed layout gets the ratios per AXIS. Given its natural height (which Column hands it) the
    // vertical ratio is 1 and the rows keep their heights however narrow the panel gets, which is what
    // reflowing means.
    {
        const float chromeH = (titled ? lh + 6.f : 0.f) + 4.f;
        b.zx = b.content.width  / std::max(1.f, el.w - 4.f);
        b.zy = b.content.height / std::max(1.f, el.h - chromeH);
        if (m_layoutMode == 0) { b.zx = b.z; b.zy = b.z; }   // Free: one uniform scale, as before
    }
    b.lc.reserve(m_childData.size());
    // Height from the STYLE when the child does not state one (StyleMetrics.h): a control's height
    // belongs to its kind, so a panel stacks a row of fields at the same height the app draws a field.
    for (const auto& cd : m_childData)
        b.lc.push_back({ jf::JRect{ cd.x, cd.y, cd.w, layoutHeightOf(cd.type, cd.h) }, cd.prop("region") });
    return b;
}

jf::JRect PanelWidget::_childRect(const ChildBasis& b, size_t i) const {
    const PanelElement& ce = m_childData[i];
    const float ch = layoutHeightOf(ce.type, ce.h);
    const jf::JRect freeR{ b.content.x + ce.x * b.z, b.content.y + ce.y * b.z, ce.w * b.z, ch * b.z };
    return layoutChildRect(m_layoutMode, b.content, b.lc, static_cast<int>(i), m_gridColumns, m_focusIndex,
                           b.zx, b.zy, freeR, static_cast<float>(m_layoutGap));
}

const PanelElement* PanelWidget::childElementAt(const jf::JRect& r, float mx, float my) const {
    const ChildBasis b = _childBasis(r);
    if (b.content.width <= 4.f || b.content.height <= 4.f) return nullptr;
    for (size_t i = m_children.size(); i-- > 0; ) {
        const PanelElement& ce = m_childData[i];
        if (m_children[i]) { m_children[i]->setElementContext(elementContext()); m_children[i]->setData(resolveElement(ce)); }
        if (!s_editMode && m_children[i] && !m_children[i]->visibleNow()) continue;   // as the paint does
        const jf::JRect cr = _childRect(b, i);
        if (mx < cr.x || mx >= cr.x + cr.width || my < cr.y || my >= cr.y + cr.height) continue;
        if (auto* nested = dynamic_cast<PanelWidget*>(m_children[i].get()))
            if (const PanelElement* deeper = nested->childElementAt(cr, mx, my)) return deeper;
        return &ce;
    }
    return nullptr;
}

// A panel is a container: the wheel is its business only if the child under the cursor says so. Same
// layout the hit-test uses, so the widget answering is the widget the pointer is over.
bool PanelWidget::wantsWheel(const jf::JRect& r, float mx, float my) {
    if (!m_el) return false;
    const ChildBasis b = _childBasis(r);
    for (size_t i = m_children.size(); i-- > 0; ) {
        CanvasWidget* child = m_children[i].get();
        if (!child) continue;
        const PanelElement& ce = m_childData[i];
        child->setElementContext(elementContext());
        child->setData(resolveElement(ce));
        if (!s_editMode && !child->visibleNow()) continue;       // as the hit-test and the paint do
        const jf::JRect cr = _childRect(b, i);
        if (mx >= cr.x && mx < cr.x + cr.width && my >= cr.y && my < cr.y + cr.height) {
            child->setCache(m_cache);
            return child->acceptsInput(ControlInput::Kind::Scroll) && child->wantsWheel(cr, mx, my);
        }
    }
    return false;
}

bool PanelWidget::handleControlInput(const jf::JRect& r, const ControlInput& in) {
    const PanelElement& el = *m_el;   // forward to the topmost child under the cursor (its OWNED persistent instance)
    const float lh = jf::JTextHelper::lineHeight();
    if (in.kind == ControlInput::Kind::Press && !m_helpText.empty()) {   // the "?" help badge (top-right)
        const float bh = lh + 6.f, bx = r.x + r.width - bh;
        if (in.mx >= bx && in.mx < r.x + r.width && in.my >= r.y && in.my < r.y + bh) {
            if (appmsg::show()) appmsg::show()(el.prop("labelText").empty() ? std::string("Help") : el.prop("labelText"), m_helpText);
            return true;
        }
    }
    const ChildBasis basis = _childBasis(r);          // same layout as render — so hit-test == draw
    const jf::JRect content = basis.content;
    // TEXT DOES NOT SHRINK WITH THE BOX under a managed layout. This is the zoom every child draws its
    // type at, and it was the free-fit scale — so a Column narrower than it was authored told its captions
    // to render at the width ratio: 3 px of ink beside a 9 px control, on the same row. A managed layout
    // moves and resizes boxes; it does not re-scale what is drawn in them. Free still gets the fit scale,
    // because there the whole point is that the drawing keeps the shape it was authored in.
    const float z = (m_layoutMode == 0) ? basis.z : 1.f;
    const std::vector<LayoutChild>& lc = basis.lc;
    // A KEYSTROKE HAS NO CURSOR. Everything below routes by position, which is right for a mouse and
    // meaningless for a key: the Surface sends (0,0) with it, nothing hit-tests, and the child actually
    // holding the keyboard never hears a thing. That is why a spin box inside an imported dialog took the
    // mouse — clicks and the stepper arrows worked — while typing and Up/Down went nowhere at all: the key
    // reached the Surface, the Surface reached the viewport, the viewport reached THIS panel, and here it
    // stopped. Deliver it to the child the last press focused — the one _blurChildExcept nominated, and the
    // same rule ViewportWidget already applies to its mirrored children.
    auto childRectOf = [&](size_t i) { return _childRect(basis, i); };
    // Keyboard focus arrived without a click (the Surface tabbed onto this panel): hand it to the first
    // child. A PANEL IS A NESTED FOCUS DOMAIN and had no way in but the mouse — the Surface sees the whole
    // panel as one control, and on an imported page every control is a panel's child, so Tab walked a page
    // of containers and reached not one field. The viewport has always done this for its mirrored children.
    if (in.kind == ControlInput::Kind::Focus) return _focusChild(1, basis);
    if (in.kind == ControlInput::Kind::Key) {
        const bool tab = in.key && in.key->pressed && !in.key->ctrl && !in.key->alt && jf::jIsTabNav(*in.key);
        for (size_t i = 0; m_activeChild && i < m_children.size(); ++i) {
            if (m_childData[i].id != m_activeChild || !m_children[i]) continue;
            ControlInput sub = in; sub.focused = true;
            m_children[i]->setData(resolveElement(m_childData[i]));
            m_children[i]->setCache(m_cache);
            if (m_children[i]->acceptsInput(in.kind) &&
                m_children[i]->onControlInput(childRectOf(i), sub)) return true;
            break;
        }
        // The child holding the keyboard did not want it. A Tab moves to the next child in here; running off
        // either end returns false so the Surface moves to the next top-level control. It does NOT wrap, or
        // the keyboard could never leave the panel.
        if (tab) return _focusChild(jf::jTabNavDir(*in.key), basis);
        return false;
    }
    // Losing the keyboard: the child that held it must be told, or it keeps its ring, its caret and any
    // typed-but-uncommitted text after the focus has moved on.
    if (in.kind == ControlInput::Kind::Blur) {
        const bool had = m_activeChild != 0;
        _blurChildExcept(0, basis);
        return had;
    }
    for (size_t i = m_children.size(); i-- > 0; ) {
        CanvasWidget* child = m_children[i].get();
        if (!child) continue;
        const PanelElement& ce = m_childData[i];
        // Its element and its data first, for the same reason the paint does it first: the condition below
        // is an expression about THIS element, and a child asked before it has one is judged against an
        // unresolved "[*]".
        child->setElementContext(elementContext());
        child->setData(resolveElement(ce));
        // A CHILD YOU CANNOT SEE CANNOT BE CLICKED. The paint skips a child whose condition is false; the
        // hit-test did not, and under the Card layout — where EVERY child gets the full rect and the
        // condition alone decides which one is shown — that means the last child in the list took every
        // click, whatever the page was displaying. A "Cells" card showing a stream's GAP cells handed its
        // presses to the WIDTH label stacked behind it, so no row selected, no bar dragged, nothing at all
        // happened. Same rule the Surface applies to a top-level control (runTarget_).
        if (!s_editMode && !child->visibleNow()) continue;
        const jf::JRect cr = _childRect(basis, i);
        if (in.mx >= cr.x && in.mx < cr.x + cr.width && in.my >= cr.y && in.my < cr.y + cr.height) {
            // A wheel goes only to the child that was last CLICKED, for the same reason the Surface only
            // sends one to the focused element: rolling over a panel must scroll the page, not quietly
            // re-tune whichever field the pointer is resting on.
            if (in.kind == ControlInput::Kind::Scroll && ce.id != m_activeChild &&
                !child->wantsWheel(cr, in.mx, in.my)) return false;
            // A press moves the keyboard to this child and TELLS THE OUTGOING ONE it has lost it. Several
            // controls in one panel are a single element as far as the Surface is concerned — its
            // activeControl_ is the panel — so its blur never fires when you click from one to the next.
            // Without this each control kept the focus ring it drew for itself, and its caret, and any
            // typed-but-uncommitted text: click four spin boxes and all four look focused at once.
            // (ViewportWidget::_setActiveChild does the same for mirrored children, for the same reason.)
            if (in.kind == ControlInput::Kind::Press) _blurChildExcept(ce.id, basis);
            ControlInput sub = in; sub.focused = true;
            child->setCache(m_cache);
            // A child disabled by its own (or its panel's) enableCondition is inert, not just grey.
            return child->acceptsInput(in.kind) &&
                   child->onControlInput(cr, sub);   // false if display-only / didn't consume
        }
    }
    return false;
}

// Hand the keyboard to `keep` and blur whoever held it before. The outgoing child is given its own rect,
// so a control that positions anything from it (a caret, a popup) tears down against the geometry it was
// actually drawn at.
// The children that can take the keyboard, in the order the page READS. Their positions come from the
// LAID-OUT rects rather than the authored ones: under a managed layout the stored x/y is not where the child
// went, so sorting by it would tab in an order the eye cannot follow.
std::vector<int> PanelWidget::_childOrder(const ChildBasis& basis, bool assumeEnabled) {
    std::vector<TabSlot> slots;
    for (size_t i = 0; i < m_children.size() && i < m_childData.size(); ++i) {
        CanvasWidget* child = m_children[i].get();
        if (!child) continue;
        child->setElementContext(elementContext());
        child->setData(resolveElement(m_childData[i]));   // the conditions below are about THIS element
        if (!child->visibleNow() || !child->interactive()) continue;
        // Greyed by its enableCondition is out of the ring as well as out of the mouse's reach — tabbing onto
        // one hands it a Focus, which for a hosted control BEGINS ITS EDIT: a caret blinking in a field that
        // refuses every key. Asked through acceptsInput, the same way the viewport asks it.
        if (!assumeEnabled && !child->acceptsInput(ControlInput::Kind::Focus)) continue;
        const jf::JRect cr = _childRect(basis, i);
        slots.push_back({ m_childData[i].id, cr.x, cr.y, cr.height });
    }
    return CanvasWidget::readingOrder(std::move(slots));
}

// Asked from outside (the page audit): the ring this panel would walk, against the rect it was last drawn
// at — the same geometry every other question about a child is answered with.
std::vector<int> PanelWidget::keyboardRing(bool assumeEnabled) {
    const auto b = getBoundingBox();
    if (b.width <= 0.f || b.height <= 0.f) return {};
    return _childOrder(_childBasis(jf::JRect{ b.x, b.y, b.width, b.height }), assumeEnabled);
}

bool PanelWidget::_focusChild(int dir, const ChildBasis& basis) {
    const std::vector<int> ids = _childOrder(basis);
    if (ids.empty()) return false;
    int cur = -1;
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == m_activeChild) { cur = (int)i; break; }
    const int next = (cur < 0) ? (dir > 0 ? 0 : (int)ids.size() - 1) : cur + dir;
    if (next < 0 || next >= (int)ids.size()) { _blurChildExcept(0, basis); return false; }
    _blurChildExcept(ids[next], basis);
    for (size_t i = 0; i < m_children.size() && i < m_childData.size(); ++i) {
        if (m_childData[i].id != ids[next] || !m_children[i]) continue;
        ControlInput f; f.kind = ControlInput::Kind::Focus; f.focused = true;   // tabbed into it -> begin its edit
        m_children[i]->setCache(m_cache);
        m_children[i]->onControlInput(_childRect(basis, i), f);
        return true;
    }
    return false;
}

void PanelWidget::_blurChildExcept(int keep, const ChildBasis& basis) {
    const int prev = m_activeChild;
    m_activeChild = keep;
    if (!prev || prev == keep) return;
    for (size_t i = 0; i < m_children.size() && i < m_childData.size(); ++i) {
        CanvasWidget* child = m_children[i].get();
        if (!child || m_childData[i].id != prev) continue;
        const PanelElement& ce = m_childData[i];
        const jf::JRect cr = _childRect(basis, i);
        ControlInput blur; blur.kind = ControlInput::Kind::Blur;
        child->setElementContext(elementContext());   // a blur commits, so it needs the same element
        child->setData(resolveElement(ce)); child->setCache(m_cache);
        child->onControlInput(cr, blur);
        return;
    }
}

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("panel", PanelWidget, 200);
