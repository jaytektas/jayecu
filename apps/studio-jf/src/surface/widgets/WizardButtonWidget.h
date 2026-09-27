#pragma once

// WizardButtonWidget — the button that opens a slot's personality wizard.
//
// A page can carry a template PICKER (a settingselector writing a set of values), and that is the right
// control when a template is a set of numbers. An output slot is not: it is two compiled conditions, a
// handful of timings, a fail direction, four parameters whose meaning changes with the template, and a
// pin somebody has to wire. That wants a conversation, not a dropdown — so the button hands off to a
// dialog, the way the wiring row hands off to the connection dialog.
//
// The widget itself is deliberately thin: it knows its slot (its Data Source is the slot's own path)
// and nothing else. Everything the wizard does lives in the dialog and in OutputTemplate.

#include "HostedControlWidget.h"
#include <j/core/JButton.h>
#include <functional>
#include <memory>
#include <string>

class WizardButtonWidget : public HostedControlWidget {
public:
    explicit WizardButtonWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "wizard") {}
    std::string elementType() const override { return "wizard"; }
    std::string paletteTitle() const override { return "Wizard Button"; }
    float       defaultW()     const override { return 150.f; }
    float       defaultH()     const override { return 28.f; }

    // Wired by the app (main.cpp) to open the modal — the widget layer never owns a window.
    static std::function<void(std::string bind)> onOpen;

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("value");
        using jf::JPropertyMeta;
        m.add("labelText", this, &WizardButtonWidget::m_labelText,
              JPropertyMeta{ .label = "Button Text", .def = "Set up\xE2\x80\xA6", .order = 100 });
    }

protected:
    jf::JControl* control() override;
    void          syncControl() override;

private:
    std::string m_labelText = "Set up\xE2\x80\xA6";
    std::unique_ptr<jf::JButton> m_btn;
};
