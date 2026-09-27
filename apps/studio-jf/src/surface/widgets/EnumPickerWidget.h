#pragma once

// EnumPickerWidget — a dropdown of the bound config field's enum options; the options come from the meta
// model, not authored props.
//
// IT HOSTS A REAL jf::JComboBox. It used to draw one: its render() copied JComboBox's geometry by hand —
// field fill, border, arrow well, chevron — and then diverged from it, which is what a hand-drawn copy of
// a framework control always does. One value chosen from a list IS a combo box, so it is one, and every
// behaviour the framework gives a combo (hover, focus ring, keyboard, the dropdown, disabled entries)
// arrives with it instead of being reimplemented here.
//
// …OR A JPickerField, for the two kinds whose list is a searchable DIALOG rather than a drop-down: a
// signal selector offers the whole channel catalogue, a table selector 167 maps. Those used to host the
// same combo, with the press intercepted above it to open the dialog — which made the control lie twice.
// A chevron promises a list that drops down under it, and the one affordance a combo has was spoken for,
// so there was nowhere to say "nothing chosen" — an ✕ bolted on never even received the click, because
// the interception happened first.
//
// So the kind picks the control: a real drop-down is a JComboBox, a searchable one is a JPickerField
// (value, an ellipsis well, and an ✕). The binding decides which, and a binding does not change kind
// under a live element — only an edit-mode rebind can, which tears the old control down.

#include "HostedControlWidget.h"
#include <j/core/JComboBox.h>
#include <j/core/JPickerField.h>
#include <memory>
#include <vector>

class EnumPickerWidget : public HostedControlWidget {
public:
    // The physical pins already claimed by OTHER enabled siblings, mapped to the sensor holding each —
    // keyed by board pin NAME, which is the true physical identity (AV1 and DIG1 are both pool index 0;
    // DIG3-as-frequency and DIG3-as-switch are one pin in two PickerSets). Public so it can be tested
    // without a window: what it answers decides which pins you are allowed to pick.
    // Every pin held ANYWHERE in the config, pin name -> what holds it. Pool-wide, not
    // sibling-scoped: DIG1-8 carry four capabilities at once, so a sensor and a trigger stream
    // can claim one physical pin from two different arrays.
    struct PinClaim {
        std::string holder;             // "Per-Cylinder Map 4" — what to say beside the option
        std::string path;               // ...and the config field that holds it, so a caller can release it
        bool        shareable = false;  // may the field being edited name it TOO? (a coil, an injector)
    };
    static std::unordered_map<std::string, PinClaim> claimedPoolPins(const std::string& myPath);

    explicit EnumPickerWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "enum") {}
    std::string elementType() const override { return "enum"; }
    std::string paletteTitle() const override { return "Enum Picker"; }
    float       defaultW()     const override { return 180.f; }
    float       defaultH()     const override { return 34.f; }
    double      sigilValue(const std::string& prop, const PanelElement& el, const Cache& cache) const override;
    std::vector<std::string> sigilNames() const override;

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("displayUnit"); m.remove("value");   // the picker holds an enum index, not a unit'd number
    }

protected:
    // A subclass (WiringWidget) that renders the same bound value in a different skin passes its own type
    // key through, rather than being stamped "enum".
    EnumPickerWidget(jf::JSceneGraph& g, const char* type) : HostedControlWidget(g, type) {}

    jf::JControl* control() override;
    void          syncControl() override;

    // The list this field offers right now: what to show, which entries may be chosen, and the config
    // value each one writes. One answer, so the combo's items and a pick's meaning cannot disagree.
    struct Options {
        std::vector<std::string> labels;
        std::vector<uint8_t>     enabled;   // parallel to labels; 0 = shown greyed
        std::vector<int>         values;    // what to write for each label (empty = the index itself)
        int                      current = -1;
        bool                     searchable = false;   // a signal / table field: a dialog, not a drop-down
    };
    Options optionsNow() const;

    // ONE OF THESE, never both: the binding's kind decides which, and control() tears the other down if
    // a rebind changes it. Holding both alive would leave the unused one parented at its last bounds,
    // which the framework's hover scan still finds.
    std::unique_ptr<jf::JComboBox>    m_combo;
    std::unique_ptr<jf::JPickerField> m_picker;
    std::vector<std::string>          m_shown;   // the item list the combo holds (rebuild guard)

    // Does this binding open a searchable dialog rather than drop a list down?
    bool wantsPicker() const;
    // Open it. Called from the picker field's onOpenRequested, and nowhere else.
    void openPicker();
    // Unset the binding (-1): no input assigned, no table named. Both are answers the firmware reads.
    void clearBinding();

    // Resolve the bound field to the label the picker would show for its current value — a signal name,
    // a board-pin option, a plain enum option, or "(none)". `optsOut` (if given) receives the plain-enum
    // option list. Shared by render() and subclasses so a re-skinned picker shows exactly the same text.
    std::string resolveLabel(const Cache& c, std::vector<std::string>* optsOut = nullptr) const;
};
