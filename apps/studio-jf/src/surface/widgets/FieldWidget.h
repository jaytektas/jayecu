#pragma once

// FieldWidget — a labelled data readout: the bound value in a boxed cell. A real CanvasWidget subclass carrying its typed properties; pixels still come from the
// "field" class render()/onControlInput(), defined in FieldWidget.cpp.

#include "../CanvasWidget.h"

class FieldWidget : public CanvasWidget {
public:
    explicit FieldWidget(jf::JSceneGraph& g) : CanvasWidget(g, "field") {}
    std::string elementType() const override { return "field"; }
    std::string paletteTitle() const override { return "Field"; }
    float       defaultW()     const override { return 200.f; }
    float       defaultH()     const override { return 40.f; }
    double      sigilValue(const std::string& prop, const PanelElement& el, const Cache& cache) const override;
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);   // keeps the binding + value readout rows
        // Blank = the displayed unit's own precision (see ValueWidget: a "%.1f" default becomes an
        // explicit format the moment the page is saved, and then overrides the unit forever).
        m.add("format", this, &FieldWidget::m_format, jf::JPropertyMeta{ .label = "Number Format", .def = "", .order = 24 });
        // A readout in a row reads best hard against its right edge (the default, unchanged); one placed on
        // a gauge face has to sit under the needle, centred.
        m.add("align", this, &FieldWidget::m_align, jf::JPropertyMeta{ .label = "Align", .editor = "enum", .order = 25,
              .choices = { "Right", "Centre", "Left" } });
        // Same row as the Value cell's, because both readouts print through the one fmtVal: Display Unit
        // picks which unit, this says whether to print it.
        m.add("showUnit", this, &FieldWidget::m_showUnit, jf::JPropertyMeta{ .label = "Show Unit", .def = "1", .order = 26 });
    }

private:
    std::string m_format = "%.1f";
    int         m_align = 0;             // 0 right (as it always was), 1 centre, 2 left
    bool        m_showUnit = true;       // false → the number without its unit (prop "showUnit" == "0")
};
