// ComboBoxWidget — supplies a live jf::JComboBox to HostedControlWidget + syncs it to the bound scalar.
// The base does the paint + input-forward; picking writes the item's VALUE to the field (echo-guarded).

#include <algorithm>
#include "ComboBoxWidget.h"

#include <cstdio>
#include "../../model/Cache.h"
#include "../../model/MetaModel.h"
#include <cmath>

std::function<std::vector<int>(const std::string&)> ComboBoxWidget::optionFilter;

jf::JControl* ComboBoxWidget::control() {
    if (!m_combo) {
        m_combo = std::make_unique<jf::JComboBox>(sceneGraph());
        m_combo->onIndexChanged.connect([this](int idx) {
            if (syncing()) return;                        // ignore our own setCurrentIndex echo
            // WRITE needs the raw config path — signalName() is a [#path] sigil EXPRESSION (fine for the
            // evalSource read, but setConfigValue wants the resolved path or it silently no-ops).
            const std::string path = writableConfigPath(bindPath());
            if (!path.empty() && idx >= 0 && idx < static_cast<int>(m_values.size()))
                Cache::instance().setConfigValue(path, m_values[idx]);
        });
    }
    return m_combo.get();
}

void ComboBoxWidget::syncControl() {
    const std::string bind = bindPath();
    // Items come from the authored list when there is one, else from the DEFINITION's own options for the
    // bound field. An enum already knows its options; making someone retype them as a prop is how a combo
    // ends up empty next to a field that has a perfectly good list in the meta. Enum value == index, which
    // is the same rule the enum picker reads them by.
    std::string key = m_items;
    std::vector<Item> items;
    if (!m_items.empty()) {
        items = parseItems();
    } else if (!bind.empty()) {
        const MetaModel* meta = Cache::instance().meta();
        const std::vector<std::string> opts = meta ? meta->enumOptions(bind) : std::vector<std::string>{};
        // The VALUE is the option's position in the definition's list, so the list is kept whole in the
        // meta — but "INVALID" is a slot the firmware uses to mean "not a pin", never something to choose.
        // Skipping it here drops it from what is OFFERED while every remaining entry keeps its real value;
        // filtering it out of the meta instead (as the importer used to) shifted all 176 entries after it.
        // A SENSOR TYPE a generic input cannot be given is not offered either, for the same reason and by
        // the same means. effective_type() accepts a stored type only where the catalog marks it
        // selectable: a generic input is read as an analog voltage through a cal curve, so choosing
        // switch, frequency or composition leaves it unconfigured and publishing nothing — a menu entry
        // that silently does the opposite of what it says. Recognised by option ID, so this costs
        // nothing on every other enum in the definition and cannot be fooled by a renamed label.
        // The CURRENT value always stays in the list: a catalogued sensor whose own type is one of
        // those three (vehicle_speed is a frequency) must still be able to show what it is.
        const std::vector<std::string> ids = meta ? meta->enumOptionIds(bind) : std::vector<std::string>{};
        const int curVal = static_cast<int>(std::lround(evalSource(bind).v));
        // ONLY ON THE SENSOR-TYPE ENUM. An option ID is unique within its SET and nowhere else, and
        // `switch` is both a sensor TYPE (not selectable — a generic input cannot be made one) and an
        // INTERFACE (how a pin is read). Matching on the id alone applied the type rule to the
        // interface list and silently removed "Switch" from it — the one interface that maps to the
        // digital-level pool, so a switch sensor could be wired to no digital input at all and the
        // dropdown gave no hint why. Every other interface it could not use was still offered.
        const bool isSensorType = meta && meta->enumSetId(bind) == "sensor_type";
        // AN INTERFACE THIS SENSOR CANNOT BE READ THROUGH is not offered either. The catalogue says
        // what each input may bind (a flex-fuel sender is [digital_freq] and nothing else); the
        // firmware has carried it as SensorDescriptor::interface_mask all along and nothing published
        // it, so the whole enum was offered for every sensor and choosing a wrong one built a pipeline
        // over a pin the sensor is not wired to. Empty = unconstrained, which is also what an older
        // ECU's meta answers — it keeps offering everything rather than nothing.
        const std::vector<std::string> allowIf = meta ? meta->allowedOptionIds(bind)
                                                      : std::vector<std::string>{};
        // AN OPTION THIS ELEMENT'S HARDWARE CANNOT DO (an output row is its pin: only an IGN pin can be a
        // coil), and AN OPTION THE TUNE RULES OUT (a cylinder this engine does not have). By index.
        const std::vector<int> allowIdx = meta ? meta->allowedOptionIndices(bind) : std::vector<int>{};
        const std::vector<int> allowNow = optionFilter ? optionFilter(bind) : std::vector<int>{};
        auto in = [](const std::vector<int>& v, int i) { return std::find(v.begin(), v.end(), i) != v.end(); };
        for (size_t i = 0; i < opts.size(); ++i) {
            if (opts[i] == "INVALID") continue;
            if (isSensorType && i < ids.size() && int(i) != curVal)
                if (const MetaModel::SensorType* st = meta->sensorType(ids[i]))
                    if (!st->selectable) continue;
            // The CURRENT value always stays, so a tune already holding an out-of-catalogue interface
            // shows what it is instead of silently reading as the first entry in the list.
            if (!allowIf.empty() && i < ids.size() && int(i) != curVal
                && std::find(allowIf.begin(), allowIf.end(), ids[i]) == allowIf.end()) continue;
            if (int(i) != curVal && !allowIdx.empty() && !in(allowIdx, int(i))) continue;
            if (int(i) != curVal && !allowNow.empty() && !in(allowNow, int(i))) continue;
            items.push_back({ opts[i], double(i) });
        }
        // THE KEY NAMES THE LIST, not its length: a cylinder list for 1-4 and one for 5-8 are both four long.
        key = "\x01" + bind + ":" + std::to_string(opts.size()) + ":";
        for (const Item& it : items) key += std::to_string(std::lround(it.value)) + ",";
    }
    // NO OPTIONS = SHOW THE NUMBER, not an empty box. A definition that describes a field's values in
    // its label instead of declaring them ("Mode (0=Open Loop, 1=Closed Loop)") gives a combo nothing to
    // offer — and so does a definition that HAS them while the connected ECU is running an older one,
    // which is the ordinary state of affairs between a schema change and a flash. An empty control says
    // the setting does not exist; the raw value at least says what it is, and can still be typed into by
    // the fallback the enum picker uses.
    if (items.empty() && !bind.empty()) {
        char v[24];
        std::snprintf(v, sizeof v, "%g", evalSource(bind).v);
        items.push_back({ v, evalSource(bind).v });
        key = "\x01raw:" + bind + ":" + v;
    }
    // (Re)push the item list only when it actually changes (setItems does not emit; guard anyway).
    if (key != m_syncedItems) {
        m_syncedItems = key;
        m_values.clear();
        std::vector<std::string> labels;
        for (const auto& it : items) { labels.push_back(it.label); m_values.push_back(it.value); }
        m_combo->setItems(std::move(labels));   // base SyncScope guards the echo
    }
    // Reflect the bound field's current value as the selection (setCurrentIndex emits -> guard the echo).
    if (bind.empty()) return;
    const double cur = evalSource(bind).v;
    int idx = -1;
    for (size_t k = 0; k < m_values.size(); ++k)
        if (std::lround(m_values[k]) == std::lround(cur)) { idx = static_cast<int>(k); break; }
    if (idx >= 0 && idx != m_combo->currentIndex()) m_combo->setCurrentIndex(idx);   // base SyncScope guards the echo
}

// Self-registration — type key, palette order, factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("combobox", ComboBoxWidget, 95);
