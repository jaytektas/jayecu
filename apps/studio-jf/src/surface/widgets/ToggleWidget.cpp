// ToggleWidget — supplies a live jf::JToggleButton to HostedControlWidget + syncs it to the bound bool field.
// Toggling writes the raw config value (via the writable path); the label shows the on/off text.

#include "ToggleWidget.h"
#include "../../model/Cache.h"

jf::JControl* ToggleWidget::control() {
    if (!m_toggle) {
        m_toggle = std::make_unique<jf::JToggleButton>(sceneGraph(), std::string{});
        m_toggle->onToggled.connect([this](bool) {
            if (syncing()) return;                                  // ignore our own setToggled echo
            const std::string path = writableConfigPath(bindPath());
            if (path.empty()) return;                              // read-only source -> display only
            const bool cur = evalSource(bindPath()).v != 0.0;
            Cache::instance().setConfigValue(path, cur ? 0.0 : 1.0);
        });
    }
    return m_toggle.get();
}

void ToggleWidget::syncControl() {
    const std::string b = bindPath();
    bool on = !b.empty() && evalSource(b).v >= 0.5;
    if (element() && element()->prop("flipValue") == "1") on = !on;
    if (on != m_toggle->isToggled()) m_toggle->setToggled(on);   // base SyncScope guards the echo
    const std::string onT  = element() ? element()->prop("onText")  : std::string{};
    const std::string offT = element() ? element()->prop("offText") : std::string{};
    m_toggle->setLabel(on ? (onT.empty() ? "On" : onT) : (offT.empty() ? "Off" : offT));
}

// Self-registration — type key, palette order, factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("toggle", ToggleWidget, 130);
