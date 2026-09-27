#pragma once

// ScaleWidget — a tick ruler (circular or linear) with major/minor ticks + value labels (faithful to the
// original ScaleWidget). A real CanvasWidget subclass carrying its typed properties; pixels still come from
// the "scale" class render()/onControlInput(), defined in ScaleWidget.cpp.

#include "SkinableWidget.h"

class ScaleWidget : public SkinableWidget {
public:
    explicit ScaleWidget(jf::JSceneGraph& g) : SkinableWidget(g, "scale") {}
    std::string elementType() const override { return "scale"; }
    std::string paletteTitle() const override { return "Scale"; }
    float       defaultW()     const override { return 160.f; }
    float       defaultH()     const override { return 160.f; }
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;
    bool        paintsZones()  const override { return true; }   // a zone colours the ticks + numbers in it

    void collectProperties(jf::JPropertyModel& m) override {
        SkinableWidget::collectProperties(m);
        m.remove("value");   // a ruler shows no single readout
        using jf::JPropertyMeta;
        m.add("scaleMode",        this, &ScaleWidget::m_scaleMode,       JPropertyMeta{ .label = "Scale Mode", .def = "Circular", .order = 54, .choices = { std::string("Circular"), std::string("Linear") } });
        m.add("autoTicks",        this, &ScaleWidget::m_autoTicks,       JPropertyMeta{ .label = "Nice Numbers", .def = "1", .order = 55 });
        m.add("targetMajorTicks", this, &ScaleWidget::m_targetMajorTicks,JPropertyMeta{ .label = "Target Major Ticks", .def = "6", .min = 2, .max = 20, .order = 56 });
        m.add("majorTicks",       this, &ScaleWidget::m_majorTicks,      JPropertyMeta{ .label = "Major Ticks", .def = "11", .min = 1, .max = 100, .order = 57 });
        m.add("minorTicks",       this, &ScaleWidget::m_minorTicks,      JPropertyMeta{ .label = "Minor Ticks", .def = "4", .min = 0, .max = 20, .order = 58 });
        m.add("tickColor",        this, &ScaleWidget::m_tickColor,       JPropertyMeta{ .label = "Major Tick Colour", .editor = "color", .order = 110 });
        m.add("minorTickColor",   this, &ScaleWidget::m_minorTickColor,  JPropertyMeta{ .label = "Minor Tick Colour", .editor = "color", .order = 111 });
        m.add("showLabels",       this, &ScaleWidget::m_showLabels,      JPropertyMeta{ .label = "Show Labels", .def = "1", .order = 112 });
        m.add("labelColor",       this, &ScaleWidget::m_labelColor,      JPropertyMeta{ .label = "Label Colour", .editor = "color", .order = 113 });
        m.add("labelFontSize",    this, &ScaleWidget::m_labelFontSize,   JPropertyMeta{ .label = "Label Font Size", .def = "40", .min = 1, .max = 200, .order = 114 });
        m.add("majorTickLength",  this, &ScaleWidget::m_majorTickLength, JPropertyMeta{ .label = "Major Tick Length", .def = "50", .min = 0, .max = 500, .decimals = 1, .order = 150 });
        m.add("minorTickLength",  this, &ScaleWidget::m_minorTickLength, JPropertyMeta{ .label = "Minor Tick Length", .def = "30", .min = 0, .max = 500, .decimals = 1, .order = 151 });
        m.add("tickWidth",        this, &ScaleWidget::m_tickWidth,       JPropertyMeta{ .label = "Tick Width", .def = "4", .min = 0.1, .max = 20, .step = 0.1, .decimals = 1, .order = 152 });
        m.add("labelOffset",      this, &ScaleWidget::m_labelOffset,     JPropertyMeta{ .label = "Label Offset/Radius", .def = "80", .min = -1000, .max = 1000, .decimals = 1, .order = 153 });
        m.add("startAngle",       this, &ScaleWidget::m_startAngle,      JPropertyMeta{ .label = "Start Angle", .def = "225", .min = -360, .max = 360, .order = 154 });
        m.add("endAngle",         this, &ScaleWidget::m_endAngle,        JPropertyMeta{ .label = "End Angle", .def = "-45", .min = -360, .max = 360, .order = 155 });
        // Which parts of the scale a zone takes over (the zones themselves are edited below the properties,
        // beside the value rules). Both on by default: red ticks AND red numbers is the tacho everyone means.
        m.add("zoneTicks",        this, &ScaleWidget::m_zoneTicks,      JPropertyMeta{ .label = "Zones Colour Ticks", .def = "1", .order = 157 });
        m.add("zoneLabels",       this, &ScaleWidget::m_zoneLabels,     JPropertyMeta{ .label = "Zones Colour Labels", .def = "1", .order = 158 });
        m.add("orientation",      this, &ScaleWidget::m_orientation,     JPropertyMeta{ .label = "Linear Orientation", .def = "Horizontal", .order = 156, .choices = { std::string("Horizontal"), std::string("Vertical") } });
    }

private:
    std::string m_scaleMode = "Circular";
    bool        m_autoTicks = true;
    double      m_targetMajorTicks = 6, m_majorTicks = 11, m_minorTicks = 4;
    std::string m_tickColor, m_minorTickColor;
    bool        m_showLabels = true;
    std::string m_labelColor;
    double      m_labelFontSize = 40;
    double      m_majorTickLength = 50, m_minorTickLength = 30, m_tickWidth = 4, m_labelOffset = 80;
    double      m_startAngle = 225, m_endAngle = -45;   // match the render fallbacks + the .def above
    bool        m_zoneTicks = true, m_zoneLabels = true;
    std::string m_orientation = "Horizontal";
};
