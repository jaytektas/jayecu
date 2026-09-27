#pragma once

// LearnedActionWidget — the two ends of a learned correction, as buttons on the page that owns it.
//
// A trim exists to end up in the map it corrects. That workflow has two moves — fold it in, or throw
// it away — and both of them lived behind a right-click on the grid, in a menu of table operations
// like Smooth and Linearise. They are not table operations: Smooth is about the shape of some numbers,
// these are about what a learned correction is FOR. And offering them on every table (they no-op on a
// map that declares no base) taught people the menu was not to be believed.
//
// So they are buttons, on the LTFT page and the Bank Trim page, next to the surface they act on. The
// button names its own table, which is the other half of the point: a page button acts on the thing
// the page is about, not on whatever happens to be selected.
//
// STUDIO-SIDE, ENTIRELY — there is no firmware command for either; see model/LearnedOps.h. Both are
// operator-confirmed, and both are one undo step.

#include "HostedControlWidget.h"
#include <j/core/JButton.h>
#include <functional>
#include <memory>
#include <string>

class LearnedActionWidget : public HostedControlWidget {
public:
    explicit LearnedActionWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "learnedaction") {}
    std::string elementType() const override { return "learnedaction"; }
    std::string paletteTitle() const override { return "Learned Trim Action (apply / reset)"; }
    float       defaultW()     const override { return 150.f; }
    float       defaultH()     const override { return 26.f; }

    // Asked before anything is written. The app owns the window, so it owns the question — the same
    // arrangement Surface uses for every other confirmation here.
    static inline std::function<void(std::string title, std::string detail, std::string verb,
                                     std::function<void()> onConfirm)> onConfirmAction;

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("signalName"); m.remove("displayUnit"); m.remove("value");   // acts on a table, binds no channel
        using jf::JPropertyMeta;
        m.add("labelText", this, &LearnedActionWidget::m_labelText,
              JPropertyMeta{ .label = "Button Text", .def = "Apply to Base Table", .order = 100 });
        // WHICH TABLE, named outright. Not the selection: a page button acts on the page's own subject.
        m.add("table", this, &LearnedActionWidget::m_table,
              JPropertyMeta{ .label = "Learned Table", .editor = "expr", .order = 101 });
        m.add("action", this, &LearnedActionWidget::m_action,
              JPropertyMeta{ .label = "Action (apply | reset)", .def = "apply", .order = 102 });
    }

protected:
    jf::JControl* control() override;
    void          syncControl() override;   // label, and greyed out when there is nothing to act on

private:
    void run();

    std::unique_ptr<jf::JButton> m_btn;
    std::string m_labelText = "Apply to Base Table";
    std::string m_table;
    std::string m_action = "apply";
};
