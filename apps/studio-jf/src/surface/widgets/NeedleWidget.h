#pragma once

// NeedleWidget — a rotating-needle gauge (type "needle") over a scale, optional needle image + hub/glow.
// A real CanvasWidget subclass carrying its typed properties; pixels
// still come from the "needle" class render()/onControlInput(), defined in NeedleWidget.cpp.

#include "SkinableWidget.h"

class NeedleWidget : public SkinableWidget {
public:
    explicit NeedleWidget(jf::JSceneGraph& g) : SkinableWidget(g, "needle") {}
    std::string elementType() const override { return "needle"; }
    std::string paletteTitle() const override { return "Needle Gauge"; }
    float       defaultW()     const override { return 150.f; }
    float       defaultH()     const override { return 150.f; }
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;

    void collectProperties(jf::JPropertyModel& m) override {
        SkinableWidget::collectProperties(m);
        using jf::JPropertyMeta;
        m.add("needleColor", this, &NeedleWidget::m_needleColor, JPropertyMeta{ .label = "Needle Colour", .editor = "color", .order = 110 });
        m.add("hubColor",    this, &NeedleWidget::m_hubColor,    JPropertyMeta{ .label = "Hub Colour",    .editor = "color", .order = 111 });
        m.add("glowRadius",  this, &NeedleWidget::m_glowRadius,  JPropertyMeta{ .label = "Glow Radius", .min = 0, .max = 1, .step = 0.01, .decimals = 2, .order = 112 });
        m.add("glowColor",   this, &NeedleWidget::m_glowColor,   JPropertyMeta{ .label = "Glow Colour", .editor = "color", .order = 113 });
        m.add("showGlass",   this, &NeedleWidget::m_showGlass,   JPropertyMeta{ .label = "Show Glass Effect", .order = 114 });
        m.add("imagePath",   this, &NeedleWidget::m_imagePath,   JPropertyMeta{ .label = "Needle Image Override", .order = 115 });
        m.add("startAngle",  this, &NeedleWidget::m_startAngle,  JPropertyMeta{ .label = "Start Angle", .def = "225", .min = -360, .max = 360, .order = 150 });
        m.add("endAngle",    this, &NeedleWidget::m_endAngle,    JPropertyMeta{ .label = "End Angle", .def = "-45", .min = -360, .max = 360, .order = 151 });
        m.add("needleWidth",  this, &NeedleWidget::m_needleWidth,  JPropertyMeta{ .label = "Needle Width", .def = "30", .min = 1, .max = 200, .decimals = 1, .order = 152 });
        m.add("needleLength", this, &NeedleWidget::m_needleLength, JPropertyMeta{ .label = "Needle Length", .def = "0.9", .min = 0, .max = 1, .step = 0.05, .decimals = 2, .order = 153 });
        m.add("tailLength",   this, &NeedleWidget::m_tailLength,   JPropertyMeta{ .label = "Tail Length", .def = "0.15", .min = 0, .max = 1, .step = 0.05, .decimals = 2, .order = 154 });
        m.add("hubRadius",    this, &NeedleWidget::m_hubRadius,    JPropertyMeta{ .label = "Hub Radius", .def = "0.05", .min = 0, .max = 0.5, .step = 0.01, .decimals = 2, .order = 155 });
        m.add("pivotX",       this, &NeedleWidget::m_pivotX,       JPropertyMeta{ .label = "Image Pivot X", .def = "0.5", .min = 0, .max = 1, .step = 0.01, .decimals = 2, .order = 156 });
        m.add("pivotY",       this, &NeedleWidget::m_pivotY,       JPropertyMeta{ .label = "Image Pivot Y", .def = "0.5", .min = 0, .max = 1, .step = 0.01, .decimals = 2, .order = 157 });
    }

private:
    std::string m_needleColor, m_hubColor;
    double      m_glowRadius = 0;
    std::string m_glowColor;
    bool        m_showGlass = false;
    std::string m_imagePath;
    double      m_startAngle = 225, m_endAngle = -45;   // match the render fallbacks + the .def above
    double      m_needleWidth = 30, m_needleLength = 0.9, m_tailLength = 0.15, m_hubRadius = 0.05, m_pivotX = 0.5, m_pivotY = 0.5;
};
