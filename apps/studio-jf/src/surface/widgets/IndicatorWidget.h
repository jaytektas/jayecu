#pragma once

// IndicatorWidget — a bound on/off lamp: fills its whole rect with the state colour and centres the state
// title. A real CanvasWidget subclass carrying its typed
// properties; pixels still come from the "indicator" class render()/onControlInput(), defined in IndicatorWidget.cpp.

#include "../CanvasWidget.h"

class IndicatorWidget : public CanvasWidget {
public:
    explicit IndicatorWidget(jf::JSceneGraph& g) : CanvasWidget(g, "indicator") {}
    std::string elementType() const override { return "indicator"; }
    std::string paletteTitle() const override { return "Indicator"; }
    float       defaultW()     const override { return 120.f; }
    float       defaultH()     const override { return 90.f; }
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;   // defined in IndicatorWidget.cpp

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("displayUnit"); m.remove("value");   // a lamp has no unit / numeric readout
        using jf::JPropertyMeta;
        m.add("onTitle",  this, &IndicatorWidget::m_onTitle,  JPropertyMeta{ .label = "On Title", .def = "ON",       .order = 100 });
        m.add("offTitle", this, &IndicatorWidget::m_offTitle, JPropertyMeta{ .label = "Off Title", .def = "OFF",      .order = 101 });
        m.add("onBg",     this, &IndicatorWidget::m_onBg,     JPropertyMeta{ .label = "On Background", .def = "#2ea043",  .editor = "color", .order = 102 });
        m.add("onFg",     this, &IndicatorWidget::m_onFg,     JPropertyMeta{ .label = "On Text", .def = "#000000",        .editor = "color", .order = 103 });
        // NO DEFAULT FOR THE UNLIT STATE. A `.def` here is not a suggestion, it is a value on the element,
        // so elColor found "#333333" before it ever reached the render's fallback — a dark grey chip with
        // white text, correct on the dark scheme and a hole punched in the light one. Left unset, the
        // render takes the scheme's own surface and the lamp is quiet on either. The LIT colours keep
        // their defaults: those say something, and they say it on any scheme.
        m.add("offBg",    this, &IndicatorWidget::m_offBg,    JPropertyMeta{ .label = "Off Background", .editor = "color", .order = 104 });
        m.add("offFg",    this, &IndicatorWidget::m_offFg,    JPropertyMeta{ .label = "Off Text",       .editor = "color", .order = 105 });
    }

private:
    std::string m_onTitle  = "ON";
    std::string m_offTitle = "OFF";
    std::string m_onBg, m_onFg, m_offBg, m_offFg;   // "" = the descriptor's built-in default colour
};
