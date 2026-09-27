#pragma once

// ComboBoxWidget — a canvas dropdown hosting a real framework jf::JComboBox (via HostedControlWidget).
// Binds a SCALAR config field (base signalName / "Data Source") and writes the selected item's VALUE
// (not an enum index) — so it fits plain scalars like engine.cylinder_count (Items "1,2,...,12") and
// value-mapped lists ("Off=0, Low=1, High=2"). The host base does paint + input-forward; this class only
// supplies the control and the value sync.

#include "HostedControlWidget.h"
#include <j/core/JComboBox.h>
#include <memory>
#include <string>
#include <vector>
#include <cstdlib>
#include <functional>

class ComboBoxWidget : public HostedControlWidget {
public:
    explicit ComboBoxWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "combobox") {}
    std::string elementType() const override { return "combobox"; }
    std::string paletteTitle() const override { return "Combo Box"; }
    float       defaultW()     const override { return 180.f; }
    float       defaultH()     const override { return 34.f; }

    void loadContent(const PanelElement& el) override { m_items = el.prop("items"); }
    void saveContent(PanelElement& el)  const override { el.props["items"] = m_items; }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);   // keeps the Data Source (bind) row
        m.add("items", this, &ComboBoxWidget::m_items,
              jf::JPropertyMeta{ .label = "Items (label=value, comma-sep)", .def = "", .order = 24 });
        m.remove("displayUnit");              // holds a discrete field value, not a unit'd reading
    }

    // WHICH OPTIONS THE TUNE ALLOWS RIGHT NOW, for a field whose sensible values depend on other settings
    // (an output row's Cylinder: the cylinders this engine has, or a bank for a bank-fired stage). Returns
    // the allowed option indices, or empty for "no opinion". Set by the app, so the widget knows no field.
    static std::function<std::vector<int>(const std::string& bind)> optionFilter;

    struct Item { std::string label; double value; };
    // "label=value" or bare "value" (label==value); comma-separated; blanks skipped.
    std::vector<Item> parseItems() const {
        std::vector<Item> out;
        const std::string& s = m_items;
        auto trim = [](std::string x) -> std::string {
            const size_t a = x.find_first_not_of(" \t");
            if (a == std::string::npos) return {};
            return x.substr(a, x.find_last_not_of(" \t") - a + 1);
        };
        size_t i = 0;
        while (i <= s.size()) {
            const size_t comma = s.find(',', i);
            std::string tok = trim(s.substr(i, comma == std::string::npos ? std::string::npos : comma - i));
            i = (comma == std::string::npos) ? s.size() + 1 : comma + 1;
            if (tok.empty()) continue;
            const size_t eq = tok.find('=');
            Item it;
            if (eq != std::string::npos) { it.label = trim(tok.substr(0, eq)); it.value = std::atof(trim(tok.substr(eq + 1)).c_str()); }
            else                         { it.label = tok;                     it.value = std::atof(tok.c_str()); }
            out.push_back(std::move(it));
        }
        return out;
    }

protected:
    jf::JControl* control() override;
    void          syncControl() override;

private:
    std::unique_ptr<jf::JComboBox> m_combo;
    std::string          m_items;
    std::string          m_syncedItems;   // last authored list pushed to the control (change detect)
    std::vector<double>  m_values;         // value per combo item, index-aligned
};
