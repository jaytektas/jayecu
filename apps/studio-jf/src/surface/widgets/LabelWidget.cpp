// LabelWidget self-registration. The widget is header-only (all inline in LabelWidget.h); this TU exists
// only to declare it to the app registry — the widget still owns its own registration.
#include "LabelWidget.h"
#include "../WidgetRegistry.h"
REGISTER_WIDGET("label", LabelWidget, 140);
