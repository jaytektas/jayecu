#pragma once

// ActionButtonWidget — a button that WORKS OUT a setting and writes it.
//
// Some settings are measurements, not opinions. Pulses per kilometre is the clearest case: it is whatever
// the tyre and the tooth count make it, nobody knows it in advance, and the way to find it is to drive at
// a known speed and divide. Asking the user to do that arithmetic and type the answer is asking them to
// be a calculator with a clipboard — and to do it again every time they change a tyre.
//
// So the button carries a list of WRITES, each "config.path = expression", separated by ';' or newlines.
// The expressions are the ordinary studio ones, so they can read live telemetry ([$shaft_hz]), other
// settings ([#vehicle_speed.calibration_speed]) and any widget on the page — which is what makes this
// general rather than a Calibrate button with the sum hard-coded inside it.
//
// A LIST, NOT ONE, BECAUSE ONE MEASUREMENT CAN CALIBRATE SEVERAL THINGS. Six speed pickups captured by
// six separate buttons are six different moments: press them a few seconds apart and the front and rear
// calibrations disagree by however much the car's speed drifted between clicks — and traction control
// reads that permanent disagreement as slip on a car doing nothing wrong. One press, one instant, every
// pickup that is turning.
//
// It writes in each target's ENGINEERING units, the same as anything else the user could have typed
// there. A write whose value is not finite is skipped — a divide by a calibration speed nobody has set
// yet is a wrong answer, not a small one — and so is a write of ZERO, because this button computes
// MEASUREMENTS and a measurement of zero is the absence of one. That is what lets one Capture serve a
// car with two pickups and a car with six: the wheels that are not turning are not written, rather than
// having their calibration erased by the click that calibrated the others.

#include "HostedControlWidget.h"
#include <j/core/JButton.h>
#include <memory>
#include <string>
#include <vector>

class ActionButtonWidget : public HostedControlWidget {
public:
    explicit ActionButtonWidget(jf::JSceneGraph& g) : HostedControlWidget(g, "action") {}
    std::string elementType() const override { return "action"; }
    std::string paletteTitle() const override { return "Action Button (compute a setting)"; }
    float       defaultW()     const override { return 90.f; }
    float       defaultH()     const override { return 25.f; }

    void collectProperties(jf::JPropertyModel& m) override {
        CanvasWidget::collectProperties(m);
        m.remove("value");            // it writes a setting; it does not display one
        using jf::JPropertyMeta;
        m.add("labelText", this, &ActionButtonWidget::m_labelText,
              JPropertyMeta{ .label = "Button Text", .def = "Calibrate", .order = 100 });
        // What this button writes: "config.path = expression", one per write, separated by ';' or a
        // newline. The paths are named exactly as a field's Data Source would name them.
        m.add("writes", this, &ActionButtonWidget::m_writes,
              JPropertyMeta{ .label = "Writes", .editor = "expr", .order = 101 });
        // Shown under the button while it is usable, so a button that computes something says what.
        m.add("hint", this, &ActionButtonWidget::m_hint,
              JPropertyMeta{ .label = "Hint", .order = 103 });
    }

protected:
    jf::JControl* control() override;
    void          syncControl() override;

private:
    void run();
    // Every write this button would actually make right now — the skipped ones are not in the list. The
    // paint uses it to grey the button (empty = nothing to do) and run() uses it to do the writing, so
    // the two can never disagree about whether a click does anything.
    struct Write { std::string path; double value; };
    std::vector<Write> pending() const;
    // `[#path]` -> its value in ENGINEERING units, spliced in before evaluation. See the definition: the
    // sigil itself resolves to the raw stored count, which is a trap in any sum that mixes scales.
    static std::string cookExpr(const std::string& expr);

    std::unique_ptr<jf::JButton> m_btn;
    std::string m_labelText = "Calibrate", m_writes, m_hint;
    bool m_lastEnabled = false;
};
