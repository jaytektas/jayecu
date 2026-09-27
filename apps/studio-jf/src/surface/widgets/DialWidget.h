#pragma once

// DialWidget — a circular/arc dial gauge (type "dial"): segmented arc fill over a scale, with bezel/glass/
// glow/peak and a centre cap. A real CanvasWidget subclass carrying its
// typed properties; pixels still come from the "dial" class render()/onControlInput(), defined in DialWidget.cpp.

#include "SkinableWidget.h"

class DialWidget : public SkinableWidget {
public:
    explicit DialWidget(jf::JSceneGraph& g) : SkinableWidget(g, "dial") {}
    std::string elementType() const override { return "dial"; }
    std::string paletteTitle() const override { return "Dial Gauge"; }
    float       defaultW()     const override { return 150.f; }
    float       defaultH()     const override { return 150.f; }
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;
    bool        paintsZones()  const override { return true; }   // the dial IS the gauge face

    void collectProperties(jf::JPropertyModel& m) override {
        SkinableWidget::collectProperties(m);
        using jf::JPropertyMeta;
        // THE RING PAINTED IN THE DIAL COLOUR, WHICH USED TO BE COMPULSORY. It is the backing a value fill
        // runs over, so a fill gauge wants it — but it is drawn across the WHOLE ring at full opacity, and a
        // dial carrying a scale and a needle instead has that scale drawn on top of a solid disc of one
        // colour, which is just a solid disc. Optional, defaulting on, so every dial that wanted it keeps it.
        m.add("showDialBand",  this, &DialWidget::m_showDialBand,  JPropertyMeta{ .label = "Show Dial Band",  .def = "1",     .order = 59 });
        m.add("showFill",      this, &DialWidget::m_showFill,      JPropertyMeta{ .label = "Show Value Fill", .def = "1",     .order = 60 });
        m.add("segmentedFill", this, &DialWidget::m_segmentedFill, JPropertyMeta{ .label = "Segmented Fill",      .order = 61 });
        m.add("showPeak",      this, &DialWidget::m_showPeak,      JPropertyMeta{ .label = "Show Peak Indicator", .order = 62 });
        m.add("peakHoldTime",  this, &DialWidget::m_peakHoldTime,  JPropertyMeta{ .label = "Peak Hold Time (ms)", .min = 0, .max = 10000, .order = 63 });
        m.add("dialColor",  this, &DialWidget::m_dialColor,  JPropertyMeta{ .label = "Dial Colour", .editor = "color", .order = 110 });
        m.add("fillColor",  this, &DialWidget::m_fillColor,  JPropertyMeta{ .label = "Fill Colour", .editor = "color", .order = 111 });
        m.add("peakColor",  this, &DialWidget::m_peakColor,  JPropertyMeta{ .label = "Peak Colour", .editor = "color", .order = 112 });
        m.add("bezelWidth", this, &DialWidget::m_bezelWidth, JPropertyMeta{ .label = "Bezel Width", .min = 0, .max = 0.5, .step = 0.005, .decimals = 3, .order = 113 });
        m.add("bezelColor", this, &DialWidget::m_bezelColor, JPropertyMeta{ .label = "Bezel Colour", .editor = "color", .order = 114 });
        m.add("showGlass",  this, &DialWidget::m_showGlass,  JPropertyMeta{ .label = "Show Glass Effect", .order = 115 });
        m.add("glowRadius", this, &DialWidget::m_glowRadius, JPropertyMeta{ .label = "Glow Radius", .min = 0, .max = 1, .step = 0.01, .decimals = 2, .order = 116 });
        m.add("glowColor",  this, &DialWidget::m_glowColor,  JPropertyMeta{ .label = "Glow Colour", .editor = "color", .order = 117 });
        m.add("innerRadius",     this, &DialWidget::m_innerRadius,     JPropertyMeta{ .label = "Inner Radius", .def = "0.8", .min = 0, .max = 1, .step = 0.05, .decimals = 2, .order = 150 });
        m.add("startAngle",      this, &DialWidget::m_startAngle,      JPropertyMeta{ .label = "Start Angle", .def = "225", .min = -360, .max = 360, .order = 151 });
        m.add("endAngle",        this, &DialWidget::m_endAngle,        JPropertyMeta{ .label = "End Angle", .def = "-45", .min = -360, .max = 360, .order = 152 });
        m.add("segmentCount",    this, &DialWidget::m_segmentCount,    JPropertyMeta{ .label = "Segment Count", .min = 0, .max = 100, .order = 153 });
        m.add("segmentSpacing",  this, &DialWidget::m_segmentSpacing,  JPropertyMeta{ .label = "Segment Spacing", .min = 0, .max = 20, .order = 154 });
        m.add("centerCapRadius", this, &DialWidget::m_centerCapRadius, JPropertyMeta{ .label = "Center Cap Radius", .def = "0.05", .min = 0, .max = 0.5, .step = 0.005, .decimals = 3, .order = 155 });
        m.add("centerCapColor",  this, &DialWidget::m_centerCapColor,  JPropertyMeta{ .label = "Centre Cap Colour", .editor = "color", .order = 156 });
        // How much of the ring the zones (edited below the properties, beside the value rules) take,
        // anchored at the OUTER edge. 1 = the whole ring, with the value fill painting over them.
        m.add("zoneWidth", this, &DialWidget::m_zoneWidth, JPropertyMeta{ .label = "Zone Band Width", .def = "1", .min = 0, .max = 1, .step = 0.05, .decimals = 2, .order = 157 });
    }

private:
    bool        m_showDialBand = true;
    bool        m_showFill = true, m_segmentedFill = false, m_showPeak = false;
    double      m_peakHoldTime = 0;
    std::string m_dialColor, m_fillColor, m_peakColor;
    double      m_bezelWidth = 0;
    std::string m_bezelColor;
    bool        m_showGlass = false;
    double      m_glowRadius = 0;
    std::string m_glowColor;
    double      m_innerRadius = 0.8, m_startAngle = 225, m_endAngle = -45, m_segmentCount = 0, m_segmentSpacing = 0;
    double      m_centerCapRadius = 0.05, m_zoneWidth = 1.0;
    std::string m_centerCapColor;
};
