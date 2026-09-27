// ScriptWidget — see the header. Builds the editor, and registers the type.

#include "ScriptWidget.h"
#include "../Surface.h"
#include "../WidgetRegistry.h"

jf::JControl* ScriptWidget::control() {
    if (!m_edit) {
        m_edit = std::make_unique<LuaScriptEditor>(sceneGraph());
        // APPLY HAS TO SAY WHAT HAPPENED. It sends a whole script on a button press rather than a value
        // per keystroke, so "did that land?" is a real question — and it is the same question when the
        // answer is no (a definition with no script field refuses rather than silently dropping the edit).
        m_edit->onStatus = [](const std::string& m) { if (Surface::onWidgetStatus) Surface::onWidgetStatus(m); };
    }
    return m_edit.get();
}

REGISTER_WIDGET("script", ScriptWidget, 117);
