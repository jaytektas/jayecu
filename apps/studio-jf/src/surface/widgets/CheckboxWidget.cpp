// CheckboxWidget — supplies a live jf::JCheckBox to HostedControlWidget + syncs it to the bound bool field.
// The base does paint + input-forward; clicking toggles the raw config value (via the writable path).

#include "CheckboxWidget.h"
#include "../../model/Cache.h"
#include <cmath>

jf::JControl* CheckboxWidget::control() {
    if (!m_check) {
        m_check = std::make_unique<jf::JCheckBox>(sceneGraph(), std::string{});
        m_check->onStateChanged.connect([this](bool) {
            if (syncing()) return;                                  // ignore our own setChecked echo
            const std::string path = writableConfigPath(bindPath());   // read-only source -> no path -> display only
            if (path.empty()) return;
            const bool cur = evalSource(bindPath()).v != 0.0;
            Cache::instance().setConfigValue(path, cur ? 0.0 : 1.0);      // toggle the RAW field
        });
    }
    return m_check.get();
}

void CheckboxWidget::syncControl() {
    const std::string b = bindPath();
    bool on = !b.empty() && evalSource(b).v != 0.0;
    if (element() && element()->prop("flipValue") == "1") on = !on;      // flip is display-only
    if (on != m_check->isChecked()) m_check->setChecked(on);   // base SyncScope guards the echo
}

double CheckboxWidget::sigilValue(const std::string& prop, const PanelElement& el, const Cache&) const {
    if (prop == "checked") { bool on = evalSource(bindPath()).v != 0.0;
                             if (el.prop("flipValue") == "1") on = !on; return on ? 1.0 : 0.0; }
    if (prop == "value")   return evalSource(bindPath()).v;
    return std::nan("");
}

std::vector<std::string> CheckboxWidget::sigilNames() const { return { "checked" }; }

// Self-registration — type key, palette order, factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("checkbox", CheckboxWidget, 40);
