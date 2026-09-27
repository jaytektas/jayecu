#include "WizardButtonWidget.h"
#include "../../model/Cache.h"

std::function<void(std::string)> WizardButtonWidget::onOpen;

jf::JControl* WizardButtonWidget::control() {
    if (!m_btn) {
        m_btn = std::make_unique<jf::JButton>(sceneGraph(), std::string{});
        // bindPath() is the RESOLVED slot — a page addressing "whichever output is selected" hands the
        // wizard a concrete element, not a template.
        m_btn->onClicked.connect([this] { if (onOpen) onOpen(bindPath()); });
    }
    return m_btn.get();
}

void WizardButtonWidget::syncControl() {
    if (m_btn && m_btn->label() != m_labelText) m_btn->setLabel(m_labelText);
}

#include "../WidgetRegistry.h"
REGISTER_WIDGET("wizard", WizardButtonWidget, 171);
