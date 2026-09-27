#pragma once

// CommandButtonWidget — a button that fires a CLI command, hosting a real framework jf::JButton (via
// HostedControlWidget). Binds no channel; on click it builds a CLI string from the "command" field + the two
// arg fields arg0..arg3 (literal tokens or live sigil expressions) and sends it (Cache::sendCli), with an optional
// message dialog. The base does paint + input-forward; this class supplies the control + the click action.

#include "HostedControlWidget.h"
#include <j/core/JButton.h>
#include <memory>
#include <string>

class CommandButtonWidget : public HostedControlWidget {
public:
    explicit CommandButtonWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "command") {}
    std::string elementType() const override { return "command"; }
    std::string paletteTitle() const override { return "Command Button"; }
    float       defaultW()     const override { return 150.f; }
    float       defaultH()     const override { return 40.f; }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("signalName"); m.remove("displayUnit"); m.remove("value");   // fires a command, binds no channel
        using jf::JPropertyMeta;
        m.add("labelText",          this, &CommandButtonWidget::m_labelText,          JPropertyMeta{ .label = "Button Text", .def = "Command",           .order = 100 });
        m.add("command",            this, &CommandButtonWidget::m_command,            JPropertyMeta{ .label = "Command (CLI)",         .order = 101 });
        m.add("showMessageOnClick", this, &CommandButtonWidget::m_showMessageOnClick, JPropertyMeta{ .label = "Show Message On Click", .order = 102 });
        m.add("message",            this, &CommandButtonWidget::m_message,            JPropertyMeta{ .label = "Message",               .order = 103 });
        m.add("closeDialogOnClick", this, &CommandButtonWidget::m_closeDialogOnClick, JPropertyMeta{ .label = "Close Dialog On Click", .order = 104 });
        m.add("arg0",               this, &CommandButtonWidget::m_arg0,               JPropertyMeta{ .label = "Arg 1", .editor = "expr", .order = 105 });
        m.add("arg1",               this, &CommandButtonWidget::m_arg1,               JPropertyMeta{ .label = "Arg 2", .editor = "expr", .order = 106 });
    }

protected:
    jf::JControl* control() override;
    void          syncControl() override;   // keep the button label in sync with labelText

private:
    void run();   // build + send the CLI command (resolve arg sigils live)

    std::unique_ptr<jf::JButton> m_btn;
    std::string m_labelText = "Command";
    std::string m_command, m_message, m_arg0, m_arg1;
    bool        m_showMessageOnClick = false;
    bool        m_closeDialogOnClick = false;
};
