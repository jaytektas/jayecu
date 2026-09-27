#include "LearnedActionWidget.h"

#include "../../model/Cache.h"
#include "../../model/LearnedOps.h"

jf::JControl* LearnedActionWidget::control() {
    if (!m_btn) {
        m_btn = std::make_unique<jf::JButton>(sceneGraph(), std::string{});
        m_btn->onClicked.connect([this] { run(); });
    }
    return m_btn.get();
}

// GREYED OUT WHEN THERE IS NOTHING TO DO, recomputed each frame from the table itself — an all-zero
// trim has nothing to apply and nothing to forget, and a button that is always live means pressing it
// tells you nothing about whether it did anything.
void LearnedActionWidget::syncControl() {
    const PanelElement* el = element();
    const std::string path   = el ? el->prop("table")  : std::string{};
    const std::string action = el ? el->prop("action") : std::string{};
    const std::string label  = el ? el->prop("labelText") : std::string{};
    m_btn->setLabel(label.empty() ? (action == "reset" ? "Reset Learned Values" : "Apply to Base Table")
                                  : label);
    const learned::Plan p = (action == "reset") ? learned::planReset(path)
                                                : learned::planApplyToBase(path);
    m_btn->setEnabled(p.possible && p.cells > 0);
}

void LearnedActionWidget::run() {
    const PanelElement* el = element();
    if (!el) return;
    const std::string path   = el->prop("table");
    const std::string action = el->prop("action");
    const bool reset = (action == "reset");
    const learned::Plan p = reset ? learned::planReset(path) : learned::planApplyToBase(path);
    if (!p.possible || p.cells == 0) return;

    auto go = [path, reset] {
        if (reset) learned::resetToZero(path);
        else       learned::applyToBase(path);
    };
    const std::string title = reset ? ("Reset " + p.trimName + "?")
                                    : ("Apply to " + p.baseName + "?");
    const std::string verb  = reset ? "Reset" : "Apply to Base Table";
    if (onConfirmAction) onConfirmAction(title, p.detail, verb, go);
    else                 go();   // no host to ask (headless/tests): the caller asked for it
}

// Self-registration — type key, palette order, factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("learnedaction", LearnedActionWidget, 71);
