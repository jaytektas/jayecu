#pragma once

// ConfigEditWidget — an editable config-scalar field, hosting a real framework jf::JDoubleSpinBox (via
// HostedControlWidget) instead of a bespoke inline editor. Clicking the value area begins a text edit seeded
// with the current value (select-all), the up/down arrows + wheel + Up/Down keys nudge, and Return commits.
// The spin box operates in DISPLAY units; edits are converted back to source units (srcV) before the config
// write. decimals/step come from the "Number Format" precision (falling back to the schema's digits), and the
// range from the schema min/max mapped through the display conversion.
//
// Convergence note: this replaces the old custom render()/onControlInput cell (whose empty-buffer editor
// showed a lone "_"). The framework spin box is left-aligned and always shows step arrows, so the old
// valueJustify / valueMinWidth props no longer apply and are dropped.

#include "HostedControlWidget.h"
#include <j/core/JDoubleSpinBox.h>
#include <memory>
#include <string>

class ConfigEditWidget : public HostedControlWidget {
public:
    explicit ConfigEditWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "configedit") {}
    std::string elementType() const override { return "configedit"; }
    std::string paletteTitle() const override { return "Config Edit"; }
    float       defaultW()     const override { return 180.f; }
    float       defaultH()     const override { return 36.f; }

    // Read-only gates all input; wheel nudges (the framework spin box only takes the wheel when focused, and
    // the hosted control is NoFocus, so we drive the nudge here). Everything else defers to the base.
    bool handleControlInput(const jf::JRect& screen, const ControlInput& in) override;

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        using jf::JPropertyMeta;
        m.add("decimals", this, &ConfigEditWidget::m_decimals, JPropertyMeta{ .label = "Decimals", .def = "-1",
                                                                              .min = jf::JVariant(-1), .max = jf::JVariant(6), .order = 24 });
        m.add("readOnly", this, &ConfigEditWidget::m_readOnly, JPropertyMeta{ .label = "Read Only", .order = 105 });
    }

protected:
    // Load our typed member from the serialized element (CanvasElement::load parity). Without this the
    // property editor's value never reached the widget: it wrote "decimals" onto the element while
    // m_decimals stayed at its constructed -1, so an authored precision was silently ignored.
    void loadContent(const PanelElement& el) override;

    jf::JControl* control() override;
    void          syncControl() override;

public:
    int decimalsForTest() const { return decimals(); }   // the precision a cell will format with (headless)

protected:

    // The hosted control paints itself; this adds the out-of-range mark over it (see the .cpp).
    void          render(jf::JPrimitiveBuffer& buf, const jf::JRect& content, const Cache& cache) override;

private:
    int decimals() const;   // m_decimals when >= 0, else the schema's digits for the bound field
    void _adoptLegacyFormat(const PanelElement& el);   // migrate an old printf "format" prop

    std::unique_ptr<jf::JDoubleSpinBox> m_spin;
    int         m_decimals = -1;   // -1 = auto (schema digits)
    bool        m_readOnly = false;
    // Does the tune hold a value this control's Min/Max would not accept? Set each sync, marked in the
    // paint. It is a fact about the tune, not an error: a limit that depends on another setting can be
    // narrowed after the value was stored.
    bool        m_outOfRange = false;
};
