#pragma once

// LinearWidget — a linear bar gauge (type "gauge"): filled bar over a scale, with bezel/glass/glow/peak.
// A real CanvasWidget subclass carrying its typed properties;
// pixels still come from the "gauge" class render()/onControlInput(), defined in LinearWidget.cpp.

#include "SkinableWidget.h"

class LinearWidget : public SkinableWidget {
public:
    explicit LinearWidget(jf::JSceneGraph& g) : SkinableWidget(g, "gauge") {}
    std::string elementType() const override { return "gauge"; }
    std::string paletteTitle() const override { return "Linear Gauge"; }
    float       defaultW()     const override { return 90.f; }
    float       defaultH()     const override { return 200.f; }
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;

    void collectProperties(jf::JPropertyModel& m) override {
        SkinableWidget::collectProperties(m);
        using jf::JPropertyMeta;
        // Scale block (SkinableWidget scale, shared by the gauges).
        // Fill / peak.
        m.add("showFill",      this, &LinearWidget::m_showFill,      JPropertyMeta{ .label = "Show Value Fill", .def = "1",     .order = 60 });
        m.add("segmentedFill", this, &LinearWidget::m_segmentedFill, JPropertyMeta{ .label = "Segmented Fill",      .order = 61 });
        m.add("showPeak",      this, &LinearWidget::m_showPeak,      JPropertyMeta{ .label = "Show Peak Indicator", .def = "1", .order = 62 });
        m.add("peakHoldTime",  this, &LinearWidget::m_peakHoldTime,  JPropertyMeta{ .label = "Peak Hold Time (ms)", .min = 0, .max = 10000, .order = 63 });
        // Colours / bezel / glass / glow.
        m.add("dialColor",  this, &LinearWidget::m_dialColor,  JPropertyMeta{ .label = "Bar Background", .editor = "color", .order = 110 });
        m.add("fillColor",  this, &LinearWidget::m_fillColor,  JPropertyMeta{ .label = "Fill Colour",   .editor = "color", .order = 111 });
        m.add("peakColor",  this, &LinearWidget::m_peakColor,  JPropertyMeta{ .label = "Peak Colour",   .editor = "color", .order = 112 });
        m.add("bezelWidth", this, &LinearWidget::m_bezelWidth, JPropertyMeta{ .label = "Bezel Width", .def = "0.02", .min = 0, .max = 0.5, .step = 0.005, .decimals = 3, .order = 113 });
        m.add("bezelColor", this, &LinearWidget::m_bezelColor, JPropertyMeta{ .label = "Bezel Colour", .editor = "color", .order = 114 });
        m.add("showGlass",  this, &LinearWidget::m_showGlass,  JPropertyMeta{ .label = "Show Glass Effect", .def = "1", .order = 115 });
        m.add("glowRadius", this, &LinearWidget::m_glowRadius, JPropertyMeta{ .label = "Glow Radius", .min = 0, .max = 1, .step = 0.01, .decimals = 2, .order = 116 });
        m.add("glowColor",  this, &LinearWidget::m_glowColor,  JPropertyMeta{ .label = "Glow Colour", .editor = "color", .order = 117 });
        // Shape.
        m.add("orientation",    this, &LinearWidget::m_orientation,    JPropertyMeta{ .label = "Orientation", .def = "Vertical", .order = 150, .choices = { std::string("Horizontal"), std::string("Vertical") } });
        m.add("barWidth",       this, &LinearWidget::m_barWidth,       JPropertyMeta{ .label = "Bar Thickness", .def = "0.6", .min = 0, .max = 1, .step = 0.05, .decimals = 2, .order = 151 });
        m.add("cornerRadius",   this, &LinearWidget::m_cornerRadius,   JPropertyMeta{ .label = "Corner Radius", .min = 0, .max = 500, .order = 152 });
        m.add("segmentCount",   this, &LinearWidget::m_segmentCount,   JPropertyMeta{ .label = "Segment Count", .min = 0, .max = 100, .order = 153 });
        m.add("segmentSpacing", this, &LinearWidget::m_segmentSpacing, JPropertyMeta{ .label = "Segment Spacing", .min = 0, .max = 50, .order = 154 });
    }

private:
    bool        m_showFill = true, m_segmentedFill = false, m_showPeak = true;
    double      m_peakHoldTime = 0;
    std::string m_dialColor, m_fillColor, m_peakColor;
    double      m_bezelWidth = 0.02;
    std::string m_bezelColor;
    bool        m_showGlass = true;
    double      m_glowRadius = 0;
    std::string m_glowColor;
    std::string m_orientation = "Vertical";
    double      m_barWidth = 0.6, m_cornerRadius = 0, m_segmentCount = 0, m_segmentSpacing = 0;
};
