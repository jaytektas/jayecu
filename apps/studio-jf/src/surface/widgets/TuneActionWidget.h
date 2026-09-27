#pragma once

// TuneActionWidget — the footer controls on every dialog: [<] [>] [Burn].
//
// They are what makes a dialog a safe place to experiment. An edit reaches the ECU's RAM the moment it is
// made, but flash is a separate commit: until Burn, a reset takes the tune back. The arrows walk THIS
// PAGE's edits — not a global history, which would undo something on another page that the user cannot
// even see — and Burn commits everything pending, greying itself out again once there is nothing to commit.
//
// One widget, three actions, because they share every bit of their behaviour but the verb: a hosted
// jf::JButton whose enabled state is recomputed each frame from the thing it acts on.

#include "HostedControlWidget.h"
#include <j/core/JButton.h>
#include <memory>
#include <string>

class TuneActionWidget : public HostedControlWidget {
public:
    explicit TuneActionWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "tuneaction") {}
    std::string elementType() const override { return "tuneaction"; }
    std::string paletteTitle() const override { return "Tune Action (undo / redo / burn)"; }
    float       defaultW()     const override { return 34.f; }
    float       defaultH()     const override { return 24.f; }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("signalName"); m.remove("displayUnit"); m.remove("value");   // acts on the tune, binds no channel
        using jf::JPropertyMeta;
        m.add("action", this, &TuneActionWidget::m_action,
              JPropertyMeta{ .label = "Action", .def = "burn", .order = 100 });   // undo | redo | burn
        m.add("scope",  this, &TuneActionWidget::m_scope,
              JPropertyMeta{ .label = "Page (undo/redo scope)", .order = 101 });
        m.add("labelText", this, &TuneActionWidget::m_labelText,
              JPropertyMeta{ .label = "Button Text", .order = 102 });
    }

protected:
    jf::JControl* control() override;
    void          syncControl() override;   // label once, enabled state every frame

private:
    void run();
    // The page whose history the arrows walk: the one stamped in at import, else whatever page the user
    // is on — a hand-placed button with no scope still acts on what is in front of it.
    std::string scope() const;

    std::unique_ptr<jf::JButton> m_btn;
    std::string m_action = "burn";
    std::string m_scope, m_labelText;
    bool m_lastEnabled = false;
};
