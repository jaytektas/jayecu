#pragma once

// ToggleWidget — a bound on/off switch, hosting a real framework jf::JToggleButton (via HostedControlWidget).
// The base does paint + input-forward; this class supplies the control, syncs its state to the bound bool
// field, and sets its label to the on/off text. flipValue inverts the DISPLAYED state only.
//
// Convergence note: the framework toggle themes itself, so the old per-widget onColor/offColor overrides
// are dropped (JToggleButton has no colour API) — its look now follows the app theme like every other
// control. On/off TEXT is preserved via setLabel.

#include "HostedControlWidget.h"
#include <j/core/JToggleButton.h>
#include <memory>
#include <string>

class ToggleWidget : public HostedControlWidget {
public:
    explicit ToggleWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "toggle") {}
    std::string elementType() const override { return "toggle"; }
    std::string paletteTitle() const override { return "Toggle"; }
    float       defaultW()     const override { return 130.f; }
    float       defaultH()     const override { return 56.f; }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("displayUnit"); m.remove("value");
        using jf::JPropertyMeta;
        m.add("onText",    this, &ToggleWidget::m_onText,    JPropertyMeta{ .label = "On Text",  .def = "On",  .order = 100 });
        m.add("offText",   this, &ToggleWidget::m_offText,   JPropertyMeta{ .label = "Off Text", .def = "Off", .order = 101 });
        m.add("flipValue", this, &ToggleWidget::m_flipValue, JPropertyMeta{ .label = "Flip Value", .order = 115 });
    }

protected:
    jf::JControl* control() override;
    void          syncControl() override;

private:
    std::unique_ptr<jf::JToggleButton> m_toggle;
    std::string m_onText  = "On";
    std::string m_offText = "Off";
    bool        m_flipValue = false;
};
