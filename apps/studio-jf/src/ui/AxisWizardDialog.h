#pragma once

// AxisWizardDialog — lay out a whole axis from start, end and increment.
//
// Building an axis one bin at a time is the clunkiest thing in the axis editor: Insert, type, Insert,
// type, twenty times, and Linearise only helps once the ends are already right and the count already
// matches. Say "0 to 100 every 5" and the axis is what you asked for — including its SIZE, which is
// the part Linearise cannot touch.
//
// The dialog only collects the three numbers and hands them over. Sizing the axis, resampling the cells
// under it and holding every bin to what the channel can read all belong to the axis, not to a form.
//
// All three are SPIN BOXES for the same reason the Insert field is: the axis already states a range and
// a precision, so a control that uses them refuses an impossible start as it is typed and steps by the
// channel's own resolution. What remains for the dialog to check is the one thing a per-field validator
// cannot see — that the end is past the start.

#include <j/app/JDialogWindow.h>   // window, chrome, focus + key routing, frame loop
#include <j/core/JLabel.h>
#include <j/core/JDoubleSpinBox.h>
#include <j/core/JDialogButtonBox.h>

#include "../model/Cache.h"
#include "../model/TableImage.h"
#include "WrapText.h"

#include <functional>
#include <memory>
#include <string>

class AxisWizardDialog : public jf::JDialogWindow {
public:
    static constexpr uint32_t kW = 400, kH = 250;
    static constexpr float kPad = 14.f, kGap = 10.f, kLblW = 132.f;
    static constexpr float kFieldW = 150.f, kUnitsW = 44.f;   // sized for the number — see InsertValueDialog

    // (start, end, increment, domain, onApply) lead; hal/pos/handle tail from openModal.
    AxisWizardDialog(double start, double end, double inc, Cache::ChannelDomain dom,
                     std::function<void(double, double, double)> onApply,
                     jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : JDialogWindow("Axis Wizard", kW, kH, hal, sx, sy, parent)
        , m_dom(std::move(dom))
        , m_onApply(std::move(onApply)) {
        using namespace jf;
        const int    d    = m_dom.digits >= 0 ? m_dom.digits : 6;
        const double step = std::pow(10.0, -d);
        const double lo   = m_dom.hasRange ? m_dom.lo : -1e9;
        const double hi   = m_dom.hasRange ? m_dom.hi :  1e9;
        const char*  names[3] = { "Axis Start Value:", "Axis End Value:", "Axis Increment Value:" };
        const double seeds[3] = { start, end, inc };
        for (int i = 0; i < 3; ++i) {
            m_lbl[i] = std::make_unique<JLabel>(graph(), names[i], kLblW);
            // Start and end live in the axis's own domain. An INCREMENT is a distance in it, so it runs
            // from one step (zero would never terminate) to the width of the domain.
            const bool isStep = (i == 2);
            m_fld[i] = std::make_unique<JDoubleSpinBox>(graph(),
                           isStep ? step : lo,
                           isStep ? std::max(step, hi - lo) : hi,
                           step, d, kFieldW);
            m_fld[i]->setValue(seeds[i]);                  // seeded from the axis as it stands
            add(m_lbl[i].get());
            add(m_fld[i].get());
        }
        m_box = std::make_unique<JDialogButtonBox>(graph());
        m_box->addButton("Cancel", JDialogButtonBox::Role::Reject, 84.f);
        m_box->addButton("OK", JDialogButtonBox::Role::Accept, 84.f);
        m_box->onReject.connect([this] { close(); });
        m_box->onAccept.connect([this] { _accept(); });
        add(m_box.get());
    }

protected:
    void layout(float w, float h) override {
        float y = contentTop() + 6.f;
        for (int i = 0; i < 3; ++i) {
            m_lbl[i]->setBounds({ kPad + 6.f, y + (rowH() - jf::JTextHelper::lineHeight()) * 0.5f, kLblW, rowH() });
            m_fld[i]->setBounds({ kPad + kLblW + 12.f, y, kFieldW, rowH() });
            y += rowH() + kGap;
        }
        m_box->setBounds({ kPad, h - kPad - btnH(), w - 2.f * kPad, btnH() });
    }

    void paint(jf::JPrimitiveBuffer& buf, float w, float h) override {
        using namespace jf;
        if (!JTextHelper::hasAtlas()) return;
        const float lh = JTextHelper::lineHeight();
        float y = contentTop() + 6.f;
        if (!m_dom.units.empty())
            for (int i = 0; i < 3; ++i, y += rowH() + kGap)
                JTextHelper::pushText(buf, kPad + kLblW + 12.f + kFieldW + 10.f, y + (rowH() - lh) * 0.5f,
                                      m_dom.units, Colors::TextSecondary, kUnitsW);
        if (m_bad)
        {   // wrapped, and raised above the buttons by as many lines as it takes
            static const std::string kMsg = "The end value has to be above the start value.";
            const float mw = w - 2.f * kPad - 6.f;
            wraptext::draw(buf, kPad + 6.f, h - kPad - btnH() - wraptext::height(kMsg, mw) - 6.f,
                           kMsg, Colors::Danger, mw);
        }
    }

private:
    void _accept() {
        const double start = m_fld[0]->value(), end = m_fld[1]->value(), step = m_fld[2]->value();
        // The boxes guarantee three numbers, each in range, and a positive step. The ORDER of two of
        // them is the one thing no single field can know about.
        if (end <= start) { m_bad = true; return; }
        m_bad = false;
        if (m_onApply) m_onApply(start, end, step);
        close();
    }

    Cache::ChannelDomain m_dom;
    std::function<void(double, double, double)> m_onApply;
    std::unique_ptr<jf::JLabel>    m_lbl[3];
    std::unique_ptr<jf::JDoubleSpinBox> m_fld[3];
    std::unique_ptr<jf::JDialogButtonBox> m_box;
    bool m_bad{false};
};
