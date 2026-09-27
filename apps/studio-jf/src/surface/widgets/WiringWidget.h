#pragma once

// WiringWidget — a "fancy skinned enum picker" row for assigning a board resource (pin) to a field.
// It derives from EnumPickerWidget so it resolves the SAME bound value (a signal / board-pin / enum
// field via its Data Source), but renders it as a wiring row — Assign / Clear buttons, the resolved
// resource name, a drawn wire in its colour, and the colour code + connector pin. DISPLAY ONLY for
// now: no input (the base's inline picker is suppressed); clicking Assign will open the Select
// Connection dialog later, and the wire colour / connector pin come from connectors→meta later —
// until then they read from authored props.

#include "EnumPickerWidget.h"
#include <j/core/JDoubleSpinBox.h>
#include <functional>
#include <memory>
#include <string>

class WiringWidget : public EnumPickerWidget {
public:
    explicit WiringWidget(jf::JSceneGraph& g) : EnumPickerWidget(g, "wiring") {}
    std::string elementType() const override { return "wiring"; }
    std::string paletteTitle() const override { return "Wiring Selector"; }
    float       defaultW()     const override { return 320.f; }
    // Tall enough for the threshold row a switch on an analog pin grows (see below). A plain wiring
    // row simply leaves the bottom band empty — better than a widget that silently hides its own
    // fields on a page whose elements were laid out before they existed.
    float       defaultH()     const override { return 80.f; }

    // THE TWO TRIP POINTS, for a switch read through an analog pin.
    //
    // A switch has no calibration curve — the firmware decodes it to 0/1 against two voltages with the
    // band between them holding the state (Stages.h, decode_switch_thresh). Those two numbers are the
    // first two breakpoints of the slot's cal axis: already stored, already in the raw units the
    // acquire produces, and already ascending, which is what fixes [0] as OFF and [1] as ON.
    //
    // They belong HERE rather than in a calibration section because they describe the CONNECTION —
    // what voltage this wire must reach for the ECU to call the switch closed — which is where the
    // wire colour and the connector pin already live. Shown only when the slot really is a switch on
    // an analog interface; on anything else this widget is the wiring row it always was.
    static constexpr float kThreshH = 34.f;

    // Test access: whether the trip-point row is showing (0 or 2 boxes), and what a box holds in the
    // STORED units — a headless test can then ask the same two questions a user does.
    int    threshBoxCountForTest() const { return (m_on && m_off) ? 2 : 0; }
    double threshRawForTest(bool on) const;

    // Assign opens the Select Connection dialog (app-wired in main.cpp); Clear unassigns the field. The
    // base enum picker's inline menu stays suppressed — the whole point is that assignment goes through
    // the dialog, not a dropdown.
    static std::function<void(std::string bind)> onAssign;
    bool interactive() const override { return true; }
    bool handleControlInput(const jf::JRect& r, const ControlInput& in) override;

    void render(jf::JPrimitiveBuffer& buf, const jf::JRect& r, const Cache& c) override;

    void collectProperties(jf::JPropertyModel& m) override {
        EnumPickerWidget::collectProperties(m);   // keeps the Data Source field + index sigil
        using jf::JPropertyMeta;
        m.add("resourceName", this, &WiringWidget::m_name,      JPropertyMeta{ .label = "Resource (fallback when unbound)", .def = "", .order = 50 });
        m.add("wireColor",    this, &WiringWidget::m_wireColor, JPropertyMeta{ .label = "Wire Colour (code, e.g. Lg/Br)",  .def = "Lg/Br", .order = 51 });
        m.add("connPin",      this, &WiringWidget::m_connPin,   JPropertyMeta{ .label = "Connector Pin", .def = "D10", .order = 52 });
    }

private:
    // Is the bound slot a switch read through an analog pin? Resolves the axis storage when it is.
    // Everything about the threshold row is gated on this one question.
    struct Thresh {
        bool        show   = false;
        int         offset = 0;       // cal_raw[0] — the OFF point; [1] is one stride further on
        int         stride = 0;
        std::string datatype;
        std::string unit, unitLabel;  // the AnalogRaw display unit (counts / mV / V), the user's choice
        double      perCount = 1.0;   // what one ADC count is worth in that unit
    };
    Thresh _thresh(const Cache& c) const;
    void   _syncBoxes(const Thresh& t);

    std::unique_ptr<jf::JDoubleSpinBox> m_on, m_off;
    jf::JControl* m_active  = nullptr;   // which box the keyboard is going to
    bool          m_syncing = false;     // suppress the echo while pushing a value in
    // How to write a box's value back. Held on the widget rather than captured in the change handler:
    // the handler outlives any one paint, and the Thresh it would have captured does not.
    std::string   m_dtype;
    double        m_perCount = 1.0;
    std::string m_name;                     // fallback label shown when there is no Data Source
    std::string m_wireColor = "Lg/Br";
    std::string m_connPin   = "D10";
};
