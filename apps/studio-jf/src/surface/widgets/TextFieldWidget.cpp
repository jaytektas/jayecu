// TextFieldWidget — supplies a live jf::JLineEdit to HostedControlWidget and syncs it to the bound STRING
// config field. The base paints the control and forwards run-mode input; this class owns the round trip.

#include "TextFieldWidget.h"
#include "../../model/Cache.h"

jf::JControl* TextFieldWidget::control() {
    if (!m_edit) {
        m_edit = std::make_unique<jf::JLineEdit>(sceneGraph());
        m_edit->onTextChanged.connect([this](const std::string& t) {
            if (syncing()) return;                              // ignore our own setText echo
            if (m_readOnly) return;
            const std::string path = writableConfigPath(bindPath());
            if (path.empty()) return;
            Cache::instance().setConfigString(path, t);         // NUL-padded into the field, then queued
        });
    }
    return m_edit.get();
}

void TextFieldWidget::syncControl() {
    const std::string path = writableConfigPath(bindPath());
    if (path.empty()) return;
    const std::string cur = Cache::instance().configString(path);
    if (cur != m_edit->text()) m_edit->setText(cur);            // base SyncScope guards the echo
}

// Self-registration — type key, palette order, factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("text", TextFieldWidget, 115);
