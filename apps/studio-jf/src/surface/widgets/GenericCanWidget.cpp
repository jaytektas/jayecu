#include "GenericCanWidget.h"
#include "../WidgetRegistry.h"
#include "../../model/MathEvaluator.h"

int GenericCanWidget::busIndex() const {
    if (m_bus.empty()) return 0;
    const std::string s = resolveTemplate(m_bus, elementContext());
    try { return std::stoi(MathEvaluator::elementKey(s)); } catch (...) {}
    try { return std::stoi(s); } catch (...) {}
    return 0;
}

REGISTER_WIDGET("genericcan", GenericCanWidget, 150);
