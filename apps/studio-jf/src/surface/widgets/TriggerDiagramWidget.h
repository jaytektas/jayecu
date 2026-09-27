#pragma once

// TriggerDiagramWidget — the trigger picture, drawn from the CONFIGURATION on the ECU.
//
// The same dial and per-stream trace the designer draws, over the same WheelGeometry, from the other
// source: wheelFromConfig() reads trigger.streams[] into a Wheel and wheelGeometry() turns it into the
// geometry the painter takes. Nothing in the painter knows which side it got.
//
// WHY IT EXISTS. A stream is thirteen fields whose meanings change with `primitive`, and the numbers
// that matter most are the ones no form can show: cell[] is a list of present-tooth INDICES under GAP
// and inter-event ANGLES under SEQUENCE, `repeats` is a count you have to turn into an angular period,
// and how two streams sit relative to each other is arithmetic across all of it. The picture answers in
// a glance what the form makes you assemble in your head.
//
// FOCUS COMES FROM THE PAGE, not from a property. A viewport carrying a Data Source pushes its element
// down to everything it mirrors (the same context "[*]" bindings resolve against), so this widget shows
// all streams on the Trigger page and highlights the one whose page it is on — with no switch to set,
// and nothing to re-point when the page is copied to the other five streams. The Focus property is an
// override for the cases where the page is not about a stream.
//
// It only draws. Editing geometry by dragging is the designer's job and stays there: the designer edits
// a WORKING copy and installing is a separate, deliberate act (see TriggerEditModel) — a drag that
// wrote the live tune would be the opposite of that.

#include "../CanvasWidget.h"
#include "../../ui/TriggerDiagram.h"

class TriggerDiagramWidget : public CanvasWidget {
public:
    using CanvasWidget::CanvasWidget;

    std::string elementType()  const override { return "triggerdiagram"; }
    std::string paletteTitle() const override { return "Trigger Diagram"; }
    float       defaultW()     const override { return 620.f; }
    float       defaultH()     const override { return 420.f; }

    // Which stream to highlight: a slot index, or "" to take it from the page (see the header note).
    // "[*]" resolves like any binding, so one template page focuses whichever stream its viewport shows.
    const std::string& focus() const { return m_focus; }
    void setFocus(const std::string& f) { if (m_focus == f) return; m_focus = f; emitModified(); }

    // What to do with the streams that are NOT the focus. Ghosting is the default because phase is the
    // question a diagram answers that a form cannot, and hiding the other lanes throws it away.
    const std::string& others() const { return m_others; }
    void setOthers(const std::string& o) { if (m_others == o) return; m_others = o; emitModified(); }

protected:
    void render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;
    void loadContent(const PanelElement& el) override {
        m_focus  = el.prop("focus");
        m_others = el.prop("others");
    }
    void saveContent(PanelElement& el) const override {
        el.props["focus"]  = m_focus;
        el.props["others"] = m_others;
    }
    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        using jf::JPropertyMeta;
        m.add("focus",  this, &TriggerDiagramWidget::focus,  &TriggerDiagramWidget::setFocus,
              JPropertyMeta{ .label = "Focus stream", .editor = "expr", .order = 150 });
        m.add("others", this, &TriggerDiagramWidget::others, &TriggerDiagramWidget::setOthers,
              JPropertyMeta{ .label = "Other streams", .def = "Ghost", .order = 151,
                             .choices = { std::string("Ghost"), std::string("Show"),
                                          std::string("Hide") } });
    }

private:
    // The slot this diagram is focused on, or -1 for none. Reads the Focus property when set, else the
    // element context the host viewport pushed down — which is the page saying what it is about.
    int focusSlot() const;

    std::string m_focus, m_others;
};
