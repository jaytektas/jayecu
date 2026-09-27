#pragma once

// ScriptWidget — the Lua editor ON THE PAGE. The element the Lua Scripting page is built around.
//
// It hosts a LuaScriptEditor, which is the editor: all this class does is mount it on the canvas —
// HostedControlWidget already paints a hosted control and forwards the run-mode mouse and keyboard, and
// the editor already knows which config field holds the script, how big it is, when to re-read it and
// what Apply means. (There was a "Lua Script" dock mounting the same class; the page replaced it.)
//
// WHY IT IS NOT A `text` ELEMENT. The page used to carry the source in a `text` element with
// `multiline: 1`: that is a TextFieldWidget hosting a jf::JLineEdit, a SINGLE-LINE control which ignored
// the flag, so four kilobytes of Lua rendered as one unreadable band — and a text element writes the
// config on every keystroke, which for a script means feeding half-typed Lua to an ECU that live-reloads
// it. Both are properties of the control, so the fix is a different control, not a flag.
//
// It binds no channel. The script field is not something a page author points this at: the editor asks
// the loaded definition, because the answer differs per firmware.

#include "HostedControlWidget.h"
#include "../../ui/LuaScriptEditor.h"
#include <memory>

class ScriptWidget : public HostedControlWidget {
public:
    explicit ScriptWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "script") {}
    std::string elementType()  const override { return "script"; }
    std::string paletteTitle() const override { return "Script Editor"; }
    float       defaultW()     const override { return 620.f; }
    float       defaultH()     const override { return 360.f; }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("signalName"); m.remove("displayUnit"); m.remove("value");   // it finds its own field
    }

protected:
    jf::JControl* control() override;   // in the .cpp: it wires the editor's status to the app's bar
    // Nothing is pushed into the editor here — it holds an UNSENT edit and a sync that overwrote it
    // would throw away the user's typing every frame. The only per-frame question is which line the ECU
    // is failing on, and pollEcuState() is edge-triggered, so a steady state costs two channel reads.
    void syncControl() override { m_edit->pollEcuState(); }

private:
    std::unique_ptr<LuaScriptEditor> m_edit;
};
