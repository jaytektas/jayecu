// LiveGraphWidget — a scrolling multi-line time-series plot. The
// per-line channel list is a structured property authored through its own editor (deferred with the setting
// selector's preset editor). A real CanvasWidget subclass carrying its scale properties; pixels still come
// from the "livegraph" class render()/onControlInput(), defined in LiveGraphWidget.cpp.
#pragma once

#include "SkinableWidget.h"

#include <algorithm>
#include <string>
#include <vector>

class LiveGraphWidget : public SkinableWidget {
public:
    explicit LiveGraphWidget(jf::JSceneGraph& g) : SkinableWidget(g, "livegraph") {}
    std::string elementType() const override { return "livegraph"; }
    std::string paletteTitle() const override { return "Live Graph"; }
    float       defaultW()     const override { return 260.f; }
    float       defaultH()     const override { return 150.f; }
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;

    // THE LEGEND IS A CONTROL. Each entry carries an eye: click it and that trace stops being drawn while
    // the others keep their colours and their ranges. Reading one line out of six is the commonest thing
    // anyone does to a trace view, and the alternative — editing the channel list and putting it back —
    // loses the history you were looking at.
    bool        interactive() const override { return true; }
    bool        handleControlInput(const jf::JRect& screen, const ControlInput& in) override;

    void collectProperties(jf::JPropertyModel& m) override {
        SkinableWidget::collectProperties(m);
        using jf::JPropertyMeta;
    }

    // Where the legend entries are, given the box — shared by the paint and the hit-test so an eye is
    // clickable exactly where it is drawn. Entry i occupies [x, x+width) of the legend row.
    struct LegendHit { int line = -1; float x = 0.f, w = 0.f; };
    static float legendHeight(const jf::JRect& r) { return std::min(18.f, r.height * 0.18f); }
    // The eye is the swatch: a filled square when shown, a hollow one when hidden.
    static int   legendEntryAt(const jf::JRect& r, const std::vector<std::string>& labels, float mx, float my);

private:
};
