#pragma once

// InsertValueDialog — "what value?", asked when a breakpoint is inserted.
//
// Insert used to guess: the midpoint of its neighbours, or last + (last - prev) off the end. A guess is
// fine as a starting point and wrong as an answer — on an axis reading the pedal it walked past 100 and
// invented 110, 120, and even in range it lands on a number nobody chose. So it seeds the guess and asks.
//
// The channel's Min and Max are STATED, in its own units, because the accepted range is the one thing a
// tuner cannot see anywhere else in this dialog — it was previously enforced silently, which is how a
// clamped entry could look like the studio eating a keystroke.
//
// The field is a SPIN BOX, not the reference ECU's combo. Theirs is a combo because it offers special
// channel values to pick from; we have no such thing, so a combo meant a numeric entry with no
// validation, no step and no decimals, whose drop-down held two items — or none at all when the channel
// states no range. A spin box is the same three facts wired to a control that uses them: it refuses
// out-of-range text as it is typed, steps by the channel's own precision, and shows the number at that
// precision. Give it a drop-down back the day a channel has named values worth picking.
//
// It still goes through Cache::snapBin on the way in: the box guards the RANGE, the axis owns the grid,
// and one place has to be the authority for both.

#include <j/app/JDialogWindow.h>   // window, chrome, focus + key routing, frame loop
#include <j/core/JLabel.h>
#include <j/core/JDoubleSpinBox.h>
#include <j/core/JDialogButtonBox.h>

#include "../model/Cache.h"
#include "../model/TableImage.h"

#include <cmath>
#include <functional>
#include <memory>
#include <string>

class InsertValueDialog : public jf::JDialogWindow {
public:
    static constexpr uint32_t kW = 430, kH = 190;
    static constexpr float kPad = 14.f, kGap = 8.f, kLblW = 82.f;
    // A number entry is sized for the number, not for the window. Stretched to the dialog it read as a
    // text field that happened to hold digits, and it ran under the units label sitting at the right
    // edge — "ADC" came out as "\DC". Wide enough for a full-scale value and its decimals, no wider.
    static constexpr float kFieldW = 150.f, kUnitsW = 44.f;

    // (seed, domain, onAccept) lead; hal/pos/handle tail from openModal.
    InsertValueDialog(double seed, Cache::ChannelDomain dom, std::function<void(double)> onAccept,
                      jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : JDialogWindow("Insert Value", kW, kH, hal, sx, sy, parent)
        , m_dom(std::move(dom))
        , m_onAccept(std::move(onAccept)) {
        using namespace jf;
        m_lbl = std::make_unique<JLabel>(graph(), "New Value:", kLblW);

        // Bounds, precision and step all come from the channel. Unknown states nothing, so the box takes
        // a range wide enough not to be a limit and a precision fine enough not to round anything away:
        // refusing entry there, or snapping to a made-up grid, would be inventing a rule nothing states.
        const int    dec = m_dom.digits >= 0 ? m_dom.digits : 6;
        const double lo  = m_dom.hasRange ? m_dom.lo : -1e9;
        const double hi  = m_dom.hasRange ? m_dom.hi :  1e9;
        m_value = std::make_unique<JDoubleSpinBox>(graph(), lo, hi, std::pow(10.0, -dec), dec, kFieldW);
        m_value->setValue(seed);                      // the old midpoint/extrapolation guess, as a start

        m_box = std::make_unique<JDialogButtonBox>(graph());
        m_box->addButton("Cancel", JDialogButtonBox::Role::Reject, 84.f);
        m_box->addButton("Insert", JDialogButtonBox::Role::Accept, 84.f);
        m_box->onReject.connect([this] { close(); });
        m_box->onAccept.connect([this] { _accept(); });

        add(m_lbl.get());
        add(m_value.get());
        add(m_box.get());
    }

protected:
    void layout(float w, float h) override {
        const float rowY = contentTop() + 6.f;
        m_lbl->setBounds({ kPad + 6.f, rowY + (rowH() - jf::JTextHelper::lineHeight()) * 0.5f, kLblW, rowH() });
        m_value->setBounds({ kPad + 92.f, rowY, kFieldW, rowH() });
        m_box->setBounds({ kPad, h - kPad - btnH(), w - 2.f * kPad, btnH() });
    }

    void paint(jf::JPrimitiveBuffer& buf, float w, float h) override {
        using namespace jf;
        if (!JTextHelper::hasAtlas()) return;
        const float lh = JTextHelper::lineHeight();
        const float rowY = contentTop() + 6.f;
        // The group box the fields sit in, so the window reads as a form rather than loose controls.
        buf.pushRectangle(kPad - 6.f, contentTop() - 4.f, w - 2.f * (kPad - 6.f),
                          h - contentTop() - btnH() - kPad - 4.f, Colors::Surface0, 5.f, 1.f, Colors::Border);
        // Units sit just past the field's arrows, not against the window edge.
        if (!m_dom.units.empty())
            JTextHelper::pushText(buf, kPad + 92.f + kFieldW + 10.f, rowY + (rowH() - lh) * 0.5f,
                                  m_dom.units, Colors::TextSecondary, kUnitsW);
        const float infoY = rowY + rowH() + kGap + 4.f;
        if (m_dom.hasRange) {
            const int d = std::max(0, m_dom.digits);
            JTextHelper::pushText(buf, kPad + 6.f, infoY, "Max:", Colors::TextSecondary, 40.f);
            JTextHelper::pushText(buf, kPad + 52.f, infoY, fmtBreak(m_dom.hi, d), Colors::Warning, 90.f);
            JTextHelper::pushText(buf, kPad + 6.f, infoY + lh + 4.f, "Min:", Colors::TextSecondary, 40.f);
            JTextHelper::pushText(buf, kPad + 52.f, infoY + lh + 4.f, fmtBreak(m_dom.lo, d), Colors::Warning, 90.f);
        }
        JTextHelper::pushText(buf, w * 0.5f - 10.f, infoY, "Hint: type a value, or step it",
                              Colors::TextSecondary, w * 0.5f);
        JTextHelper::pushText(buf, w * 0.5f - 10.f, infoY + lh + 4.f, "with the arrows.",
                              Colors::TextSecondary, w * 0.5f);
    }

    // Enter commits. The spin box's own field takes Return to commit its text, so the dialog looks
    // first — otherwise typing a value and pressing Enter would only re-read the field.
    bool onKey(const jf::JKeyEvent& ke) override {
        if (ke.key != jf::JKeyEvent::JKey::Return) return false;
        _accept();
        return true;
    }

private:
    // No parse, no error state: the box only ever holds a number, and only ever one inside the range.
    void _accept() {
        if (m_onAccept) m_onAccept(m_value->value());
        close();
    }

    Cache::ChannelDomain m_dom;
    std::function<void(double)> m_onAccept;
    std::unique_ptr<jf::JLabel>    m_lbl;
    std::unique_ptr<jf::JDoubleSpinBox> m_value;
    std::unique_ptr<jf::JDialogButtonBox> m_box;
};
