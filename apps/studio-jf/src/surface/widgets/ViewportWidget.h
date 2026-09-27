#pragma once

// ViewportWidget — a container that renders another node's page in place.
// A real CanvasWidget subclass carrying its typed properties; its pixels + nested-page rendering still come
// from the "viewport" class render()/onControlInput(), defined in ViewportWidget.cpp. It re-styles the inherited border rows (title/colour/width/radius) the way a viewport presents them.

#include "../CanvasWidget.h"

#include <memory>
#include <unordered_map>
#include <vector>

class ViewportWidget : public CanvasWidget {
public:
    explicit ViewportWidget(jf::JSceneGraph& g) : CanvasWidget(g, "viewport") {}
    std::string elementType() const override { return "viewport"; }
    std::string paletteTitle() const override { return "Viewport"; }
    float       defaultW()     const override { return 400.f; }
    float       defaultH()     const override { return 300.f; }
    bool        interactive()    const override { return true; }
    // A viewport is a CONTAINER, not a control: it owns the focus of the children it mirrors and draws the ring
    // on the focused one, so the Surface must not outline the whole viewport when something inside it has the
    // keyboard.
    bool        drawsOwnFocus()  const override { return true; }
    bool        handleControlInput(const jf::JRect& screen, const ControlInput& in) override;
    bool        wantsWheel(const jf::JRect& screen, float mx, float my) override;   // its page, or a scrolling child in it
    bool        isContainer()  const override { return true; }
    // The SOURCE element id of the mirrored control under (mx,my) — and its owning page model in `outModel` —
    // so a run-mode context menu can reach a table/curve inside the viewport. 0 if nothing is hit.
    // The live widget mirroring a source element, by that element's id. A mirrored page's instances are
    // OWNED HERE, not by the surface, so a caller that needs to go deeper (into a mirrored panel's children)
    // has to ask the viewport for the widget rather than looking it up on the surface.
    CanvasWidget* mirroredWidget(int sourceElementId) const {
        const auto it = m_childByEl.find(sourceElementId);
        return it == m_childByEl.end() ? nullptr : it->second.get();
    }

    // The mirrored element under the cursor. `outRect` receives its on-screen rect, so a caller that needs
    // to go deeper (a panel's children) has the geometry to do it.
    int         mirroredElementAt(const jf::JRect& screen, float mx, float my, PanelModel*& outModel,
                                  jf::JRect* outRect = nullptr) const;
    // render() draws the background + the mirrored page (clipped); the FRAME (border + title bar) is painted in
    // drawBorder() — the base's unclipped post-content pass — so a 1px edge border isn't trimmed at the bounds.
    void        drawBorder(jf::JPrimitiveBuffer&, const jf::JRect&) const override;
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;
    void        loadContent(const PanelElement& el) override;   // reconcile the mirrored children to the node's page

    // THE PAGE ITS SURFACE IS ON. Pushed by the Surface that paints it, exactly as renderZoom is: a
    // widget has no route back to its container, and a global would be wrong the moment two surfaces
    // sit on different pages.
    void        setSurfaceNode(const std::string& n) { m_surfaceNode = n; }
    // The page this viewport shows: its own when pinned or pasted, else its surface's.
    std::string pageKey() const;
    // The page it is mirroring, and the caption it actually draws — exposed so the drawn result can be
    // tested rather than a helper that merely agrees with it.
    const PanelModel* mirroredPage() const;
    std::string       titleShown() const;
    void        commitOwnedChildren() override;                 // save-time: flush mirrored children into the node's page model

    // The mirrored child instances (keyed by SOURCE element id) — exposed for the @-sigil tree walk.
    std::vector<int> keyboardRing(bool assumeEnabled = false) override;

    std::vector<CanvasWidget*> childWidgets() override {
        std::vector<CanvasWidget*> v; v.reserve(m_childByEl.size());
        for (const auto& [id, c] : m_childByEl) if (c) v.push_back(c.get());
        return v;
    }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("value");          // a container has no single readout
        m.remove("scale");          // a viewport mirrors another control; it has no value of its own to scale
        m.remove("accentColor");    // a viewport never paints an accent
        m.remove("padding");        // the viewport ignores padding (fixed 2px content inset)
        using jf::JPropertyMeta;
        m.add("labelText", this, &ViewportWidget::m_labelText, JPropertyMeta{ .label = "Title", .order = 100 });
        m.add("borderColor",  static_cast<CanvasWidget*>(this), &CanvasWidget::borderColor,  JPropertyMeta{ .label = "Border Colour", .editor = "color", .inheritable = true, .order = 101 });
        m.add("borderWidth",  static_cast<CanvasWidget*>(this), &CanvasWidget::borderWidth,  JPropertyMeta{ .label = "Border Width", .min = 0, .max = 20, .order = 102 });
        m.add("borderRadius", static_cast<CanvasWidget*>(this), &CanvasWidget::borderRadius, JPropertyMeta{ .label = "Corner Radius", .min = 0, .max = 40, .order = 103 });
        m.add("fgColor",      static_cast<CanvasWidget*>(this), &CanvasWidget::fgColor,      JPropertyMeta{ .label = "Title Colour", .editor = "color", .inheritable = true, .order = 104 });   // re-style the inherited Text row
        m.add("fontName",     static_cast<CanvasWidget*>(this), &CanvasWidget::fontName,     JPropertyMeta{ .label = "Title Font", .editor = "font", .inheritable = true, .order = 105 });   // re-style the inherited skin font row
    }

private:
    // --- Scrolling the mirrored page ------------------------------------------------------------------
    // A viewport shows a page at 1:1 when the surface is fixed-size, and a page can be taller than the
    // viewport (an imported TunerStudio dialog usually is). Without a scroll of its own, everything past the
    // fold was simply cut off — the surface's own bar only pans the surface, which is a different thing.
    struct View {
        jf::JRect content{};                 // the mirrored page's clip box (inside the title bar)
        float sc = 1.f, ox = 0.f, oy = 0.f;  // page->screen scale and origin (origin includes the scroll)
        float pageW = 0.f, pageH = 0.f;      // the page's on-screen extent
        bool  overV = false, overH = false;  // does it overflow the content box?
        jf::JRect vTrack{}, vThumb{}, hTrack{}, hThumb{};
    };
    View viewOf(const jf::JRect& r, const PanelModel* pm) const;   // clamps the scroll; render + input share it
    bool _childWantsWheel(const PanelModel* pm, const View& view, float mx, float my);   // see CanvasWidget::wantsWheel
    // The screen rect of one mirrored child. A key/focus/blur carries no cursor, so it used to be handed the
    // VIEWPORT's rect — see the .cpp for what that did to a widget that lays itself out from it.
    jf::JRect _childRectOf(int id, const jf::JRect& r) const;
    static constexpr float kScrollBarW = 10.f;

    // Mutable: viewOf() is the ONE geometry, and the const hit-test uses it too — the clamp is view
    // state (it re-fits an offset the page or the viewport has outgrown), not a change to the document.
    mutable float m_scrollX = 0.f, m_scrollY = 0.f;   // page-space offset, <= 0, clamped to the overflow
    int   m_barDrag = 0;                      // 0 none, 1 vertical thumb, 2 horizontal thumb
    // POINTER CAPTURE — the child a press landed on, until the button comes up (0 = none). Separate from
    // m_activeChild, which is the KEYBOARD: a press captures the pointer and takes the keyboard together,
    // but a Tab moves the keyboard alone and a release ends the capture alone.
    int   m_ptrCapture = 0;
    float m_barGrab = 0.f;                    // cursor offset within the thumb when the drag began

    void reconcile(const std::string& node);   // create/drop mirrored instances to match the source page
    void refreshChildrenIfChanged(const PanelModel* pm);   // re-setData mirrored children ONLY when pm's generation moves
    std::string m_labelText;
    std::unordered_map<int, std::unique_ptr<CanvasWidget>> m_childByEl;   // OWNED, keyed by SOURCE element id
    void _setActiveChild(int id, const jf::JRect& r);   // blurs the outgoing child (commits its edit)
    std::vector<int> _childOrder(const PanelModel* pm, bool assumeEnabled = false) const;              // keyboard-eligible children, in order
    bool _focusChild(int dir, const jf::JRect& r, const PanelModel* pm);   // Tab within this viewport
    int m_activeChild = 0;   // source element id of the child that owns the keyboard in here (0 = none).
                             // Keystrokes go ONLY here: they carry no cursor position, and broadcasting them
                             // put every hosted ConfigEdit in the viewport into edit mode at once.
    uint64_t m_mirrorGen = ~0ull;              // source page generation last synced into the children (~0 = never)
    // WHICH page those children were built from. Everything cached here belongs to one page, and the
    // cache is keyed by an id that repeats across pages, so the page's identity is the only safe thing
    // to compare. "" = nothing mirrored yet.
    std::string m_mirrorNode;
    std::string m_surfaceNode;   // "" until a surface says; an unbound viewport shows this page
};
