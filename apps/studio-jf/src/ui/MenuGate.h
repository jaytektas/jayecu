#pragma once

// MenuGate — the studio offers only what the ECU in front of it can do.
//
// Two answers per item, the usual desktop split:
//   show   — does this apply to this KIND of ECU at all? A knock scope on an imported rusEFI
//            definition, a reset on a TunerStudio link: not offered, not even greyed.
//   enable — can it be done RIGHT NOW? Reset with no connection, Save Tune with no tune open:
//            offered, greyed, because connecting or opening one would make it work.
//
// Rules are keyed by the item's LABEL within its menu rather than by pointer, because some menus are
// rebuilt when their state changes (Logging's Start/Stop Recording) and a pointer taken once would dangle.
// apply() is cheap -- a few dozen predicates -- and runs on the housekeeping tick, so no event has to be
// remembered: whatever changed (a connect, a meta load, a project closing), the next tick says so.

#include <j/core/JWidget.h>
#include <j/core/MenuSystem.h>

#include <functional>
#include <string>
#include <vector>

struct Gate {
    bool show   = true;
    bool enable = true;
};

class MenuGate {
public:
    using Rule = std::function<Gate()>;

    void item(jf::JMenu& menu, std::string label, Rule rule) {
        m_items.push_back({ &menu, std::move(label), std::move(rule) });
    }
    void widget(jf::JWidget& w, Rule rule) { m_widgets.push_back({ &w, std::move(rule) }); }

    void apply() const {
        for (const auto& r : m_items) {
            const Gate gt = r.rule();
            for (const auto& it : r.menu->items())
                if (auto* mi = dynamic_cast<jf::JMenuItem*>(it.get()); mi && mi->label() == r.label) {
                    mi->setVisible(gt.show);
                    mi->setEnabled(gt.show && gt.enable);
                }
        }
        for (const auto& r : m_widgets) {
            const Gate gt = r.rule();
            r.w->setVisible(gt.show);
            r.w->setEnabled(gt.show && gt.enable);
        }
    }

private:
    struct ItemRule   { jf::JMenu* menu; std::string label; Rule rule; };
    struct WidgetRule { jf::JWidget* w; Rule rule; };
    std::vector<ItemRule>   m_items;
    std::vector<WidgetRule> m_widgets;
};
