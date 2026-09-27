#pragma once

// SkinableWidget — the mid-tier between CanvasWidget and the gauges. The gauge/scale/graph
// family (Linear, Dial, Needle, Scale, LiveGraph) shares ONE "scale" block — value range + printf format +
// inversion — instead of re-declaring those four rows in every widget, plus the peak-hold helper the bar and
// dial gauges use. Widgets derive from this instead of CanvasWidget and chain collectProperties() through it.

#include "../CanvasWidget.h"

#include <chrono>
#include <string>
#include <unordered_map>

class SkinableWidget : public CanvasWidget {
public:
    explicit SkinableWidget(jf::JSceneGraph& g, const std::string& name) : CanvasWidget(g, name) {}

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        using jf::JPropertyMeta;
        m.add("minValue", this, &SkinableWidget::m_minValue, JPropertyMeta{ .label = "Scale Min", .def = "0",   .editor = "expr", .order = 50 });
        m.add("maxValue", this, &SkinableWidget::m_maxValue, JPropertyMeta{ .label = "Scale Max", .def = "100", .editor = "expr", .order = 51 });
        m.add("format",   this, &SkinableWidget::m_format,   JPropertyMeta{ .label = "Label Format (e.g. %.1f)", .def = "%.0f", .order = 52 });
        m.add("inverted", this, &SkinableWidget::m_inverted, JPropertyMeta{ .label = "Invert Scale", .order = 53 });
    }

protected:
    // Peak-hold: the highest value seen within the last holdMs (the bar/dial "peak indicator"), keyed by element.
    static double peakOf(const std::string& elId, double val, double holdMs) {
        struct Peak { double v; std::chrono::steady_clock::time_point t; };
        // Keyed by UID — see TableWidget::tableCursors: an id is unique per model, this map is global.
        static std::unordered_map<std::string, Peak> s_peaks;
        const auto now = std::chrono::steady_clock::now();
        auto& p = s_peaks.try_emplace(elId, Peak{ val, now }).first->second;
        if (val >= p.v) { p.v = val; p.t = now; }
        else if (holdMs > 0 && std::chrono::duration<double, std::milli>(now - p.t).count() > holdMs) { p.v = val; p.t = now; }
        return p.v;
    }

    // Min/max are expression fields (editor "expr"): a plain number ("0"/"100") OR a live binding, since the
    // range often tracks a config/channel. render() reads the prop through numOrExpr(), which parses a number
    // when it is one and evaluates it as source otherwise. The "100" default MATCHES render()'s empty-prop
    // fallback (dial/scale/linear/needle all use `numOrExpr(el.prop("maxValue"), 100)`); when they disagreed,
    // an edit — which re-writes every field — froze maxValue=0 in and collapsed the gauge to a zero range.
    std::string m_minValue = "0", m_maxValue = "100";
    std::string m_format = "%.0f";
    bool        m_inverted = false;
};
