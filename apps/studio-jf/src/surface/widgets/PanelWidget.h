#pragma once

// PanelWidget — a container that frames + titles a group of child elements.
// A real CanvasWidget subclass carrying its typed properties; its pixels + child layout still
// come from the "panel" class render()/onControlInput(), defined in PanelWidget.cpp. It re-styles the inherited border rows (title/colour/radius) the way a panel presents them.

#include "../CanvasWidget.h"
#include "../ContainerLayout.h"   // LayoutChild — the child geometry a blur has to be addressed with

#include <memory>
#include <vector>

class PanelWidget : public CanvasWidget {
public:
    explicit PanelWidget(jf::JSceneGraph& g) : CanvasWidget(g, "panel") {}
    std::string elementType() const override { return "panel"; }
    std::string paletteTitle() const override { return "Panel"; }
    float       defaultW()     const override { return 320.f; }
    float       defaultH()     const override { return 240.f; }
    bool        interactive()    const override { return true; }
    bool        drawsOwnFocus()  const override { return true; }   // a container, not a control (see ViewportWidget)
    bool        handleControlInput(const jf::JRect& screen, const ControlInput& in) override;
    bool        wantsWheel(const jf::JRect& screen, float mx, float my) override;   // answers for the child under the cursor
    bool        isContainer()  const override { return true; }
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;
    void        loadContent(const PanelElement& el) override;   // (re)build the OWNED child instances on a spec change

    // The persistent child instances this panel owns — exposed so the @-sigil tree walk (findWidget /
    // collectSigilTokens) can descend into nested widgets.
    // The CHILD under (mx,my), by the same geometry render() and onControlInput() use, recursing into a
    // nested panel. A menu that resolves "what is under the cursor" stops at a top-level element otherwise —
    // and on an imported page every control is a panel's child, so it stopped at the dialog.
    const PanelElement* childElementAt(const jf::JRect& r, float mx, float my) const;

    std::vector<int> keyboardRing(bool assumeEnabled = false) override;

    std::vector<CanvasWidget*> childWidgets() override {
        std::vector<CanvasWidget*> v; v.reserve(m_children.size());
        for (const auto& c : m_children) if (c) v.push_back(c.get());
        return v;
    }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("value");   // a container has no single readout
        using jf::JPropertyMeta;
        m.add("labelText", this, &PanelWidget::m_labelText, JPropertyMeta{ .label = "Title", .order = 100 });
        // Re-style the inherited skin rows as a panel presents them (bound to the base members).
        m.add("borderColor",  static_cast<CanvasWidget*>(this), &CanvasWidget::borderColor,  JPropertyMeta{ .label = "Border Colour", .editor = "color", .inheritable = true, .order = 101 });
        m.add("borderRadius", static_cast<CanvasWidget*>(this), &CanvasWidget::borderRadius, JPropertyMeta{ .label = "Corner Radius", .min = 0, .max = 40, .order = 102 });
        // Managed child layout — the panel arranges its OWNED children by these, through the shared
        // layoutChildRect() (the same 7 modes the canvas uses). Stored as the option INDEX (kind 'e').
        m.add("layoutMode",  this, &PanelWidget::m_layoutMode,  JPropertyMeta{ .label = "Layout", .editor = "enum", .order = 103,
              .choices = { std::string("Free"), std::string("Y Axis"), std::string("X Axis"), std::string("Border"),
                           std::string("Card"), std::string("Index Card"), std::string("Grid"), std::string("Wrap") } });
        m.add("gridColumns", this, &PanelWidget::m_gridColumns, JPropertyMeta{ .label = "Grid Columns", .min = 1, .max = 12, .order = 104 });
        m.add("focusIndex",  this, &PanelWidget::m_focusIndex,  JPropertyMeta{ .label = "Focus Index",  .min = 0, .order = 105 });
        // WRAP ONLY: the gutter a flow leaves between its children. It belongs to the container because it
        // is a rule about arrangement — a child that carries its own margin is a child whose width is not
        // its width, and every geometry check downstream then has to know which is meant.
        m.add("layoutGap",   this, &PanelWidget::m_layoutGap,   JPropertyMeta{ .label = "Layout Gap", .min = 0, .max = 200, .order = 106 });
        m.add("helpText",    this, &PanelWidget::m_helpText,    JPropertyMeta{ .label = "Help Text", .order = 107 });   // shown via the ? badge (run mode)
    }

private:
    std::string m_labelText;
    std::string m_helpText;   // optional help; a "?" badge in the title bar shows it via the message dialog (run mode)
    int         m_layoutMode = 0, m_gridColumns = 2, m_focusIndex = 0, m_layoutGap = 0;
    // WHERE THE CHILDREN ARE: the content box, the content scale and the layout list. One derivation, used
    // by the paint, the hit-test, the blur walk and the wheel question — see _childBasis in the .cpp.
    // z  — the uniform fit scale a FREE panel draws its coordinates at (min of the two ratios).
    // zx/zy — the per-axis ratios a MANAGED layout is given instead; see _childBasis for why.
    struct ChildBasis { jf::JRect content{}; float z = 1.f, zx = 1.f, zy = 1.f; std::vector<LayoutChild> lc; };
    ChildBasis _childBasis(const jf::JRect& r) const;
    jf::JRect  _childRect(const ChildBasis& b, size_t i) const;

    // Move the keyboard to one child, blurring the one that had it (see the .cpp for why the Surface
    // cannot do this itself for controls inside a panel).
    void _blurChildExcept(int keep, const ChildBasis& basis);
    // Tab WITHIN this panel — the nested focus domain a container has to walk itself, exactly as
    // ViewportWidget::_focusChild walks its mirrored children.
    std::vector<int> _childOrder(const ChildBasis& basis, bool assumeEnabled = false);
    bool             _focusChild(int dir, const ChildBasis& basis);

    int         m_activeChild = 0;   // child id that last took a press — the only one a wheel may reach   // managed-layout params (‖ the canvas's)
    std::vector<std::unique_ptr<CanvasWidget>> m_children;   // OWNED, persistent child instances (‖ m_childData)
    std::vector<PanelElement>                  m_childData;  // each child's authored element (rect/type/props)
    std::string                                m_childrenSpec;   // the "children" JSON the tree was built from
};
