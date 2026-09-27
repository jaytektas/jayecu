#pragma once

// PinWireWidget — one board pin's outside-world connection, drawn rather than spelled.
//
// "IGN2 - CN2-25, wire W" is a sentence about a wire that makes you translate it twice: which
// connector is CN2, and what colour is W. Both answers are already in the meta and both are COLOURS,
// which is what you are actually looking for with the loom in your hand. So the connector is a badge
// filled in its own shell colour and the wire is a drawn bar in its insulation colours — the same two
// things the assign widget draws, on the page for a pin that cannot be assigned because the page IS
// the pin.
//
// Display only, and deliberately: it names a fixed fact about the board. It takes a resource name
// ("IGN2", "LS7", "AV2") rather than a bound field, because an output row has no field to bind — the
// row is the pin.
#include "../CanvasWidget.h"

class PinWireWidget : public CanvasWidget {
public:
    explicit PinWireWidget(jf::JSceneGraph& g) : CanvasWidget(g, "pinwire") {}
    std::string elementType() const override { return "pinwire"; }
    std::string paletteTitle() const override { return "Pin Wiring"; }
    float       defaultW()     const override { return 300.f; }
    float       defaultH()     const override { return 26.f; }
    bool        interactive()  const override { return false; }
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;

    // The typed member, from the serialized element. collectProperties only declares the inspector
    // row — it does not hydrate, and a widget that relies on it alone comes up with an empty resource
    // and draws nothing at all, which on a page reads as a widget that failed rather than one that was
    // never told what pin it is on.
    void loadContent(const PanelElement& el) override { m_resource = el.prop("resource"); }
    void saveContent(PanelElement& el) const override { el.props["resource"] = m_resource; }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.add("resource", this, &PinWireWidget::m_resource,
              jf::JPropertyMeta{ .label = "Board Resource (IGN2, LS7, AV2)", .def = "", .order = 50 });
    }

private:
    std::string m_resource;
};
