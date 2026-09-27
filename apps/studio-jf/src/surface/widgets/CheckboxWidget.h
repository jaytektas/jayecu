#pragma once

// CheckboxWidget — a bound on/off tick box, hosting a real framework jf::JCheckBox (via HostedControlWidget).
// The base does paint + input-forward; this class supplies the control and syncs it to the bound bool field.
// flipValue inverts the DISPLAYED state only (the written value still toggles the raw field).

#include "HostedControlWidget.h"
#include <j/core/JCheckBox.h>
#include <memory>
#include <string>
#include <vector>

class CheckboxWidget : public HostedControlWidget {
public:
    explicit CheckboxWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "checkbox") {}
    std::string elementType() const override { return "checkbox"; }
    std::string paletteTitle() const override { return "Checkbox"; }
    float       defaultW()     const override { return 150.f; }
    float       defaultH()     const override { return 32.f; }
    double      sigilValue(const std::string& prop, const PanelElement& el, const Cache& cache) const override;
    std::vector<std::string> sigilNames() const override;

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("displayUnit"); m.remove("value");   // on/off box: no unit, no numeric readout row
        m.add("flipValue", this, &CheckboxWidget::m_flipValue, jf::JPropertyMeta{ .label = "Flip Value", .order = 110 });
    }

protected:
    jf::JControl* control() override;
    void          syncControl() override;

private:
    std::unique_ptr<jf::JCheckBox> m_check;
    bool                           m_flipValue = false;
};
