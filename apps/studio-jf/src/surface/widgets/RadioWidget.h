#pragma once

// RadioWidget — one button per enum option of the bound config field.
// A real CanvasWidget subclass carrying its typed properties; pixels + click input still come from the
// "radio" class render()/onControlInput(), defined in RadioWidget.cpp.

#include "../CanvasWidget.h"

class RadioWidget : public CanvasWidget {
public:
    explicit RadioWidget(jf::JSceneGraph& g) : CanvasWidget(g, "radio") {}
    std::string elementType() const override { return "radio"; }
    std::string paletteTitle() const override { return "Radio Group"; }
    float       defaultW()     const override { return 160.f; }
    float       defaultH()     const override { return 110.f; }
    bool        interactive()    const override { return true; }
    bool        handleControlInput(const jf::JRect& screen, const ControlInput& in) override;
    void        render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.add("orientation", this, &RadioWidget::m_orientation,
              jf::JPropertyMeta{ .label = "Orientation", .def = "Vertical", .order = 105,
                                 .choices = { std::string("Horizontal"), std::string("Vertical") } });
    }

private:
    std::string m_orientation = "Vertical";
};
