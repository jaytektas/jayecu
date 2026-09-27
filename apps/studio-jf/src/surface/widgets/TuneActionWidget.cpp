// TuneActionWidget — see the header. Supplies a jf::JButton to HostedControlWidget and recomputes its
// enabled state each frame: an arrow with nothing to step to, or a Burn with nothing to commit, is
// greyed out rather than a click that quietly does nothing.

#include "TuneActionWidget.h"
#include "../../model/Cache.h"
#include <j/core/Log.h>

jf::JControl* TuneActionWidget::control() {
    if (!m_btn) {
        m_btn = std::make_unique<jf::JButton>(sceneGraph(), std::string{});
        m_btn->onClicked.connect([this] { run(); });
    }
    return m_btn.get();
}

std::string TuneActionWidget::scope() const {
    const std::string s = element() ? element()->prop("scope") : std::string{};
    return s.empty() ? Cache::instance().editScope() : s;
}

void TuneActionWidget::syncControl() {
    const PanelElement* el = element();
    const std::string act = el ? el->prop("action") : std::string("burn");
    std::string label = el ? el->prop("labelText") : std::string{};
    if (label.empty())
        label = act == "undo" ? "<" : act == "redo" ? ">" : "Burn";   // the atlas has no ‹ ›
    m_btn->setLabel(label);

    Cache& c = Cache::instance();
    const std::string sc = scope();
    // Burn is offered only when there is something to commit AND something to commit it to: offline, the
    // tune lives in a file that Save writes, and there is no flash in the picture at all.
    const bool on = act == "undo" ? c.canUndoScope(sc)
                  : act == "redo" ? c.canRedoScope(sc)
                                  : (tuneburn::pending() && tuneburn::pending()());
    if (on != m_lastEnabled) {
        m_lastEnabled = on;
        JLOGC("surface.tuneaction", jf::JLogLevel::Debug) << act << " on page \"" << sc << "\" -> "
            << (on ? "ENABLED" : "disabled");
    }
    m_btn->setEnabled(on);
}

void TuneActionWidget::run() {
    const PanelElement* el = element();
    const std::string act = el ? el->prop("action") : std::string("burn");
    Cache& c = Cache::instance();
    if (act == "undo")      { c.undoScope(scope()); return; }
    if (act == "redo")      { c.redoScope(scope()); return; }
    // Burn. Flush anything still sitting in the debounced write queue FIRST — burning before the last
    // edit has left the studio commits the value before it, which is the one bug this button must not have.
    c.flushWrites();
    if (tuneburn::burn()) tuneburn::burn()();
}

// Self-registration — type key, palette order, factory.
#include "../WidgetRegistry.h"
REGISTER_WIDGET("tuneaction", TuneActionWidget, 71);
