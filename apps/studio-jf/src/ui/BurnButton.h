#pragma once

// BurnButton — the toolbar's commit-to-flash button, with the ECU's own answer to "is there anything
// to commit" drawn on its face.
//
// The studio's editing model is that an edit reaches ECU RAM immediately and flash only on a deliberate
// burn, so "unburned changes" is a state the user has to be able to SEE — otherwise the only way to
// find out that an afternoon's tuning was never committed is to power-cycle and lose it. The dot on
// this button is that state: amber while the tune is dirty, green for a moment after a successful
// burn, and a dim outline when there is nothing to commit (the button is disabled then, so a click
// cannot pretend to do something).
//
// It is a jf::JButton subclass rather than a bespoke control: the toolbar's other bespoke control
// (ConnectButton) draws a chip that no standard widget could, whereas this is a button that wants a
// button's behaviour — hover, press, keyboard activation, click-cancel on release-outside — plus one
// indicator. Only the two paint hooks are overridden.

#include <j/core/JButton.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/core/Timer.h>
#include <j/graphics/VectorGraphics.h>

#include <chrono>

class BurnButton : public jf::JButton {
public:
    explicit BurnButton(jf::JSceneGraph& g, float w = 86.f, float h = 22.f)
        : jf::JButton(g, "Burn", w, h)
    {
        setEnabled(false);                       // nothing to commit until something says otherwise
        m_flash.onTick.connect([this] { m_flash.stop(); m_burned = false; invalidate(); });
    }

    // The tune's state, as reported by whoever knows it (see main.cpp: the ECU's own config_dirty
    // telemetry channel when it publishes one, the studio's pending-write flag otherwise).
    void setDirty(bool d) {
        if (d == m_dirty) return;
        m_dirty = d;
        if (d) m_burned = false;                 // a fresh edit ends the just-burned confirmation
        setEnabled(d);
        invalidate();
    }
    bool dirty() const { return m_dirty; }
    bool justBurned() const { return m_burned; }   // the green confirmation is still showing

    // A BURN IS IN FLIGHT AND HAS NOT LANDED YET. The ECU acknowledges the 'b' command the moment it
    // receives it and hands the actual write to its save task, which takes the better part of two
    // seconds to reach flash. Going green on the ack said "burned" while the tune was still only in
    // RAM — and a reset inside that window silently discards it and the ECU comes back with the
    // PREVIOUS tune (CliCommands.cpp:172 records the same trap costing a findlimits + autotune).
    // So the green now waits for the ECU's own config_dirty to clear, and this is what is shown in
    // between: still not safe to reset.
    void setBurning(bool b) {
        if (b == m_burning) return;
        m_burning = b;
        invalidate();
    }
    bool burning() const { return m_burning; }

    // Held green for a couple of seconds after a burn lands: the dot going out is not, by itself,
    // evidence that anything happened — it looks identical to a tune that was never dirty.
    void showBurned() {
        m_burned = true;
        m_flash.start(std::chrono::milliseconds(2200), jf::JTimer::JMode::SingleShot);
        invalidate();
    }

protected:
    void drawBackground(jf::JPrimitiveBuffer& buf, const jf::JRect& b, const uint8_t* fill,
                        bool focused) override
    {
        jf::JButton::drawBackground(buf, b, fill, focused);
        // A dirty tune gets an amber edge as well as the dot. The dot alone is ~7 px in a toolbar the
        // eye is not looking at; the outline is what makes it register peripherally.
        if (m_dirty && !m_burned && !m_burning) {
            jf::JVectorCanvas vg;
            vg.setAntiAlias(1.2f);
            vg.strokeRoundedRect(b.x + 0.5f, b.y + 0.5f, b.width - 1.f, b.height - 1.f,
                                 jf::JStyle::current().hint(jf::JStyleHint::ControlRadius),
                                 1.4f, jf::JPaint::solid(kAmber));
            vg.flush(buf);
        }
    }

    void drawLabel(jf::JPrimitiveBuffer& buf, const jf::JRect& b) override {
        const float r  = 3.4f;
        const float cx = b.x + 11.f;
        const float cy = b.y + b.height * 0.5f;

        jf::JVectorCanvas vg;
        vg.setAntiAlias(1.2f);
        if (m_burned)      vg.fillCircle(cx, cy, r, jf::JPaint::solid(kGreen));
        // In flight: a hollow ring in the burned colour. Distinct from both the amber "you have unburned
        // changes" and the solid green "it is on the flash" — the dot is literally not filled in yet.
        else if (m_burning) vg.strokeCircle(cx, cy, r + 0.6f, 1.6f, jf::JPaint::solid(kGreen));
        else if (m_dirty)  vg.fillCircle(cx, cy, r, jf::JPaint::solid(kAmber));
        else               vg.strokeCircle(cx, cy, r, 1.2f, jf::JPaint::solid(kIdle));   // hollow: nothing pending
        vg.flush(buf);

        // The caption centres in what is left after the dot, so it does not drift off-centre as the
        // indicator changes — the dot's column is reserved whether or not it is filled.
        if (!jf::JTextHelper::hasAtlas()) return;
        const jf::JColor t = jf::jstyle::pal().color(
            jf::JColorRole::ButtonText,
            isEnabled() ? jf::JColorGroup::Active : jf::JColorGroup::Disabled);
        const uint8_t tc[4] = { t.r, t.g, t.b, 230 };
        const float x0 = b.x + 20.f;
        jf::JTextHelper::pushTextAligned(buf, x0, b.y, b.width - (x0 - b.x) - 4.f, b.height,
                                         label(), tc, jf::JTextHelper::Align::Center, 2.f);
    }

private:
    static inline const jf::JColor kAmber = jf::rgb(0xEE, 0xAA, 0x3C);   // matches ConnectButton's amber
    static inline const jf::JColor kGreen = jf::rgb(0x46, 0xB3, 0x5A);
    static inline const jf::JColor kIdle  = jf::rgb(0x8B, 0x90, 0x99);

    bool       m_dirty  = false;
    bool       m_burning = false;   // 'b' sent, ECU's save task has not finished — see setBurning()
    bool       m_burned = false;
    jf::JTimer m_flash;
};
