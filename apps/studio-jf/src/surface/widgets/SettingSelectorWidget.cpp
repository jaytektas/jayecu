// SettingSelectorWidget — supplies a live jf::JComboBox to HostedControlWidget and keeps it in step with
// the preset list and with live config. Picking an option writes all of that option's pairs.

#include "SettingSelectorWidget.h"
#include "../../model/Cache.h"
#include "../../model/PresetOptions.h"
#include <cmath>

jf::JControl* SettingSelectorWidget::control() {
    if (!m_combo) {
        m_combo = std::make_unique<jf::JComboBox>(sceneGraph());
        m_combo->onIndexChanged.connect([this](int idx) {
            if (syncing()) return;                       // our own setCurrentIndex echo, not a choice
            if (idx < 0 || idx == m_customIdx) return;   // "Custom" describes the tune; it does not set it
            const PresetOptions po = PresetOptions::fromCompact(element() ? element()->prop("presets") : std::string{});
            po.apply(idx);                               // every pair of that option, in one edit each
        });
    }
    return m_combo.get();
}

void SettingSelectorWidget::syncControl() {
    const std::string spec = element() ? element()->prop("presets") : std::string{};
    const PresetOptions po = PresetOptions::fromCompact(spec);

    // Rebuild the item list only when the presets themselves change — setItems resets the selection, and
    // doing it every frame would fight the match below. The FIRST pass always builds, whatever the spec
    // says: an empty list is a state to show ("(no presets)"), and comparing it against an empty m_spec
    // says "no change" and leaves the control with no items and nothing to display.
    if (!m_built || spec != m_spec) {
        m_built = true;
        m_spec = spec;
        std::vector<std::string> items;
        for (const auto& o : po.options) items.push_back(o.label);
        // A tune that matches no option is a real state and the control has to be able to show it — the
        // alternative is a dropdown displaying an option whose values are not the ones in the ECU.
        m_customIdx = items.empty() ? -1 : static_cast<int>(items.size());
        if (items.empty()) items.push_back("(no presets)");
        else               items.push_back("Custom (current values)");
        m_combo->setItems(std::move(items));
    }

    // Which option the ECU is actually sitting on, re-read every frame: another control writing one of
    // these fields changes the answer, and the dropdown says so without being told.
    const int match = po.empty() ? 0 : po.matchIndex();
    const int want  = (match >= 0) ? match : m_customIdx;
    if (want >= 0 && want != m_combo->currentIndex()) m_combo->setCurrentIndex(want);   // base guards the echo
}

double SettingSelectorWidget::sigilValue(const std::string& prop, const PanelElement& el, const Cache&) const {
    if (prop == "index" || prop == "value") { return evalSource(bindPath()).v; }
    return std::nan("");
}

std::vector<std::string> SettingSelectorWidget::sigilNames() const { return { "index" }; }

// Self-registration — this widget declares itself to the app (type key, palette order, factory); no central list.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("settingselector", SettingSelectorWidget, 170);
