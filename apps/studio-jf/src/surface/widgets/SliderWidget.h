#pragma once

// SliderWidget — a bound value slider over the channel's config range.
// A real CanvasWidget subclass carrying its typed properties; pixels + drag input still come
// from the "slider" class render()/onControlInput(), defined in SliderWidget.cpp.

#include "../CanvasWidget.h"

class SliderWidget : public CanvasWidget {
public:
    explicit SliderWidget(jf::JSceneGraph& g) : CanvasWidget(g, "slider") {}
    std::string elementType() const override { return "slider"; }
    std::string paletteTitle() const override { return "Slider"; }
    float       defaultW()     const override { return 200.f; }
    float       defaultH()     const override { return 40.f; }
    bool        interactive()    const override { return true; }
    bool        handleControlInput(const jf::JRect& screen, const ControlInput& in) override;
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.add("orientation", this, &SliderWidget::m_orientation,
              jf::JPropertyMeta{ .label = "Orientation", .def = "Horizontal", .order = 105,
                                 .choices = { std::string("Horizontal"), std::string("Vertical") } });
    }

private:
    std::string m_orientation = "Horizontal";
};
