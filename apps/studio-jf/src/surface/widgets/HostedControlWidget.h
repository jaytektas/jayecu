#pragma once

// HostedControlWidget — the base for canvas widgets that HOST a live framework jf::JControl (checkbox,
// combo, slider, button, …) instead of bespoke self-rendered cells. It owns the two halves of the drive
// split that belong to the host (the framework owns FOCUS, KEYBOARD and any popup): it PAINTS the control
// each frame and FORWARDS the run-mode mouse to it. Subclasses just supply the control and the value sync.
//
//   - Paint: setBounds to our camera-transformed rect (the Surface already put it on bounds()) + the
//     control's own populateRenderPrimitives, clipped to our box. Nothing else paints the control.
//   - Input: forward run-mode press/move/release/scroll/key to the control's handlers (the press opens a
//     combo's dropdown, drags a slider, clicks a button, or begins a spin-box edit; keys type into an entry).
//     Only called in run mode, so in EDIT mode the control is inert and the Surface's select/drag/resize
//     works unchanged.
//   - Focus: the control is forced NoFocus so it can't steal edit-mode keyboard focus (Delete/nudge). Click
//     controls don't need it; and a text-entry control's editing state is driven by handleMousePress (begin
//     edit) + the forwarded key events, NOT by isFocused() — so requestFocus() is a harmless no-op (the
//     framework FocusManager ignores a NoFocus widget) and keys still reach it via the Surface's activeControl_.
//
// Subclass contract:
//   - control()      : return the owned jf::JControl (lazy-construct it + connect its change signal on first
//                      call; the write must early-out on syncing()).
//   - syncControl()  : push the bound field's current value into the control each frame. The BASE holds the
//                      echo guard for the whole call (SyncScope below), so NOTHING done here can write the
//                      tune — value, range, item list, decimals, anything. Subclasses cannot clear the flag.
//
// Why the base owns it: a control's setters emit their change signal whenever they alter the value, and that
// includes changes the user never made. JDoubleSpinBox::setRange() calls setValue(m_value) to re-clamp, so
// configuring the range of a freshly-constructed spin box (placeholder value -1e12) clamped to the field's
// minimum and emitted it as if the user had typed it — writing the field's min to the ECU on the panel's
// FIRST PAINT, with no interaction at all. That was guardable per-statement and was missed for setRange
// while every setValue was wrapped. Scoping the guard to the whole call makes the mistake unrepresentable.

#include "../CanvasWidget.h"
#include <j/core/JControl.h>
#include <j/core/JStyle.h>   // JPaletteScope — local (per-paint) override of the global JStyle defaults

class HostedControlWidget : public CanvasWidget {
public:
    using CanvasWidget::CanvasWidget;
    bool interactive() const override { return true; }
    bool drawsOwnFocus() const override { return true; }   // the hosted control paints its own focus ring

    // The hosted control IS this widget's content, painted through the base render() hook. That routes it
    // through the ONE paint template (element scope, tooltip, background, error wash, border, content clip)
    // exactly like a self-drawn widget — no second paint path to keep in step, and every base decoration
    // (notably a value rule's colours) applies to hosted controls for free. `content` is the element rect
    // minus padding, and the base has already clipped to the widget rect.
    void render(jf::JPrimitiveBuffer& buf, const jf::JRect& content, const Cache&) override {
        jf::JControl* c = control();
        if (!c) return;
        // The control is INSIDE this widget — subclasses build it lazily, so parent it here. The edge carries
        // effective visibility and the canvas's scan exclusion down to it; without one, a control on a hidden
        // element stayed flagged visible and the framework's hover scan kept finding it at its last bounds.
        if (c->parentWidget() != this) addChild(c);
        // ALWAYS NoFocus: the canvas is its own focus domain (the Surface tracks activeControl_ and drives
        // Focus/Blur), so a hosted control cannot be in the window's focus chain — StrongFocus made the
        // framework clear its focus one frame after every click. NoFocus also no-ops requestFocus().
        c->setFocusPolicy(jf::JFocusPolicy::NoFocus);
        { SyncScope guard(*this); syncControl(); }       // nothing syncControl() does may write the tune
        c->setBounds(content);                           // window-absolute -> hit-test + popup anchor correct
        // A FILL BEHIND THE CONTROL CANNOT SHOW -- it paints opaque over it -- so anything that recolours
        // this widget has to reach the control's OWN palette instead. A rule sets Base from its bg and
        // Text from its fg; either may be empty, and an unset channel inherits exactly as it does
        // everywhere else. The control renders its value ON the result: full colour, legible.
        jf::JPalette pal;
        if (rulePalette(activeRange(), pal)) {
            jf::JPaletteScope scope(pal);
            c->populateRenderPrimitives(buf);              // control resolves its roles against the scope
        } else {
            c->populateRenderPrimitives(buf);              // the control paints itself
        }
    }

    // The palette a range rule asks for, or false when it recolours nothing. Separated from the paint so
    // a test can ask the same question the render does.
    //
    // A ROLE IS NOT A COLOUR SLOT: a control paints from the role that names its PART, and the parts
    // differ by control. Base is the field surface — a line edit, a spin box, a combo — and setting only
    // that left a command button exactly the colour it already was, because jstyle::buttonFill resolves
    // Button (normal), ToolTipBase (hovered) and Highlight (pressed) and never looks at Base. So a
    // status colour on a button silently did nothing, which is the one place a red/green rule is most
    // often wanted.
    //
    // All three button fills take the rule's background, not just the resting one: a colour that
    // vanishes the moment the pointer crosses the control reads as a glitch rather than as feedback.
    // Hover and press still show through the border and the focus ring. ButtonText follows the rule's
    // foreground for the same reason Text does — though note JButton draws its NORMAL caption from the
    // theme's ControlText, not from the palette, so today only the disabled caption follows it.
    static bool rulePalette(const Range* rule, jf::JPalette& pal) {
        uint8_t rb[4], rf[4];
        const bool hasBg = rule && skin::parseHex(rule->bg, rb);
        const bool hasFg = rule && skin::parseHex(rule->fg, rf);
        if (!hasBg && !hasFg) return false;
        pal = jf::JStyle::current().palette();
        if (hasBg) {
            const jf::JColor c2 = jf::JColor::fromArray(rb);
            pal.setRole(jf::JColorRole::Base, c2, c2);          // fields: line edit, spin box, combo
            pal.setRole(jf::JColorRole::Button, c2, c2);        // buttons: the resting fill…
            pal.setRole(jf::JColorRole::ToolTipBase, c2, c2);   // …hovered…
            pal.setRole(jf::JColorRole::Highlight, c2, c2);     // …and pressed
        }
        if (hasFg) {
            const jf::JColor c2 = jf::JColor::fromArray(rf);
            pal.setRole(jf::JColorRole::Text, c2, c2);
            pal.setRole(jf::JColorRole::ButtonText, c2, c2);
        }
        return true;
    }

    // Test access: the hosted control, and ONE sync pass — the half of the paint that has no pixels in
    // it. A headless test can then assert what the control shows, which is what the user reads.
    jf::JControl* controlForTest() { return control(); }
    void          syncForTest()    { if (control()) { SyncScope guard(*this); syncControl(); } }

    bool handleControlInput(const jf::JRect&, const ControlInput& in) override {
        jf::JControl* c = control();                     // run mode only (Surface gates this)
        if (!c) return false;
        switch (in.kind) {
            // A press both focuses and acts: the Surface has just made this element activeControl_, so mark the
            // control focused (that is what paints its focus ring) before handing it the click.
            case ControlInput::Kind::Press:   c->setFocused(true);
                                              c->handleMousePress(in.mx, in.my);   return true;
            case ControlInput::Kind::Move:    c->handleMouseMove(in.mx, in.my);    return true;
            case ControlInput::Kind::Release: c->handleMouseRelease(in.mx, in.my); return true;
            case ControlInput::Kind::Scroll:  return c->handleScroll(in.mx, in.my, in.wheel);
            case ControlInput::Kind::Key: {
                if (!in.key) return false;
                const bool took = c->handleKeyEvent(*in.key);
                JLOGC("surface.key", jf::JLogLevel::Debug) << "  hosted " << elementType()
                    << ": handleKeyEvent -> " << took << " (focused=" << c->isFocused() << ")";
                return took;
            }
            // Focus moved to another control: commit any edit, drop the caret, drop the ring. The framework
            // cannot do this for us — a hosted control is not in the window's focus chain, so no real blur ever
            // reaches it.
            case ControlInput::Kind::Blur:    c->endEdit(); c->setFocused(false);      return true;
            // Keyboard focus arrived without a click (Tab): show the ring, and begin editing with the value
            // selected if this control edits text. A control that does not (a checkbox, a button) just gains the
            // ring and its Space/Return activation.
            case ControlInput::Kind::Focus:   c->setFocused(true); c->beginEdit();     return true;
            default:                          return false;
        }
    }

protected:
    virtual jf::JControl* control() = 0;   // owned control (lazy-construct + wire signal); typed in subclass
    virtual void          syncControl() {} // push bound value into the control (the base guards the call)

    // True while the base is pushing values INTO the control. Every change-signal handler that writes the
    // tune must early-out on this. Read-only to subclasses by design — only SyncScope may set it, so no
    // subclass can clear it part-way through a sync and let a programmatic set escape as a user edit.
    bool syncing() const { return m_syncing; }

private:
    // RAII echo guard around syncControl(). Saves/restores rather than assigning false, so it nests safely.
    struct SyncScope {
        HostedControlWidget& w;
        bool                 prev;
        explicit SyncScope(HostedControlWidget& host) : w(host), prev(host.m_syncing) { w.m_syncing = true; }
        ~SyncScope() { w.m_syncing = prev; }
    };

    bool m_syncing = false;                // suppress the control's change signal during our programmatic set
};
