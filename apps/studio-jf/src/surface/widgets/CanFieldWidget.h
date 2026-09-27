#pragma once

// CanFieldWidget — which generic CAN receive field a sensor reads, chosen BY NAME.
//
// A CAN sensor names its field with two settings: `can_frame`, the frame's bus/ext/id as one key, and
// `can_bit`, the start bit of the field within it. Neither is a number anybody would type, and the
// pair is one choice, so this is one control over both: pick "Wideband (CAN1 0x360) · bits 16..31"
// and it writes them together. It binds `can_frame` (the Data Source) and derives `can_bit` as its
// sibling, the same way the enum picker derives the interface field that gates a pin list — they are
// a pair by construction in the schema and a page cannot sensibly bind one without the other.
//
// IT USED TO BE A SPIN BOX OVER THE FIELD'S INDEX IN THE 512-DEEP POOL. Nobody can look at 137 and say
// which field that is, and worse, the studio REPACKS the whole pool on any structural edit — so adding
// a frame above this one renumbered every later field and silently re-pointed the sensor at somebody
// else's bits, decoding a plausible number with no code raised.
//
// WHAT IT OFFERS is every field of every RECEIVE frame in the tune. A field that already names a
// channel is listed but GREYED: the frame writes that channel itself, so a sensor publishing it too
// would put two producers on one channel (P1656), and showing it greyed answers "why is my field not
// in the list" where leaving it out cannot.

#include "HostedControlWidget.h"
#include <j/core/JComboBox.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class CanFieldWidget : public HostedControlWidget {
public:
    explicit CanFieldWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "canfield") {}
    std::string elementType() const override { return "canfield"; }
    std::string paletteTitle() const override { return "CAN Field Picker"; }
    float       defaultW()     const override { return 260.f; }
    float       defaultH()     const override { return 34.f; }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);   // keeps the Data Source row (bound to can_frame)
        m.remove("displayUnit");              // holds a frame key, not a unit'd reading
        m.remove("value");
    }

    // One offer in the list: what it reads, and the two numbers picking it writes.
    struct Choice {
        std::string label;
        uint32_t    frame = 0;       // (bus << 30) | (ext << 29) | id — the firmware's frame key
        int         bit   = -1;      // the field's start bit; -1 = "(none)"
        bool        pickable = true; // a field that already names a channel is shown, greyed
    };

    // The list as it stands right now, and which entry the tune is on. Public so a headless test can
    // ask the same two questions the control does.
    std::vector<Choice> choices() const;
    int                 currentChoice(const std::vector<Choice>& cs) const;

    // The sibling that holds the start bit: the bound path with its last name swapped. Empty when the
    // binding is not a `can_frame` field, which is the only thing this widget is for.
    std::string bitPath() const;

protected:
    jf::JControl* control() override;
    void          syncControl() override;

private:
    std::unique_ptr<jf::JComboBox> m_combo;
    std::vector<Choice>            m_choices;
    std::string                    m_syncedKey;   // the list last pushed (rebuild guard)
};
