#pragma once

// SettingSelectorWidget — a preset dropdown that writes a set of config constants and auto-selects the
// option matching live config. It HOSTS a framework jf::JComboBox (via HostedControlWidget) rather than
// drawing a box and a triangle of its own: it is a combo box, so it should be the app's combo box — one
// that follows the scheme, the corner radius, the hover and focus treatment, and the popup behaviour
// every other dropdown in the app has. The hand-drawn one had its border, radius and arrow as literals,
// so it drifted from the theme the moment either changed.
//
// The preset list is a structured property authored through its own editor (PresetEditorDialog).

#include "HostedControlWidget.h"
#include <j/core/JComboBox.h>
#include <memory>
#include <string>
#include <vector>

class SettingSelectorWidget : public HostedControlWidget {
public:
    explicit SettingSelectorWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "settingselector") {}
    std::string elementType() const override { return "settingselector"; }
    std::string paletteTitle() const override { return "Setting Selector"; }
    float       defaultW()     const override { return 200.f; }
    float       defaultH()     const override { return 34.f; }
    double      sigilValue(const std::string& prop, const PanelElement& el, const Cache& cache) const override;
    std::vector<std::string> sigilNames() const override;

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("displayUnit"); m.remove("value");   // the selection is a preset, not a unit'd number
        m.remove("presets");    // structured preset list — authored via its own editor, not a plain row
    }

protected:
    jf::JControl* control() override;
    void          syncControl() override;

private:
    std::unique_ptr<jf::JComboBox> m_combo;
    std::string                    m_spec;      // the presets prop the item list was built from
    bool                           m_built = false;   // …and whether it has been built at all
    int                            m_customIdx = -1;   // index of the trailing "Custom" row, or -1
};
