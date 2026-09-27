#pragma once

#include <j/core/GenesisComponents.h>     // JControl, JSceneGraph, JPrimitiveBuffer
#include <j/graphics/VectorGraphics.h>    // JVectorCanvas, JPaint, JColor, rgb
#include <j/core/Timer.h>                 // JTimer (spinner animation)

#include <chrono>
#include <cmath>

// ConnectButton — the ECU connection indicator: an IC chip inside a status ring, coloured by state.
// Grey when idle, an animated amber spinner while connecting + downloading the tune, green once the
// tune is in and all is well, red on error/drop. A bespoke studio control that draws itself with the
// vector canvas; it lives in the toolbar. JControl fires onClicked on press — the app toggles the link.
class ConnectButton : public jf::JControl {
public:
    enum class State { Idle, Connecting, Connected, Error };

    explicit ConnectButton(jf::JSceneGraph& g, float sz = 30.f)
        : jf::JControl(g, "ConnectButton")
    {
        m_sz = sz;
        auto& l = m_graph.getLayout(m_nodeId);
        l.boundingBox.width  = sz;
        l.boundingBox.height = sz;
        // The spinner: while connecting, advance the arc each tick and repaint. The tick marshals to
        // the UI thread and arms a redraw, so the ring animates even while the tune is downloading.
        m_spin.onTick.connect([this] {
            m_angle += 0.41888f;   // 24° per tick
            if (m_angle > 6.2831853f) m_angle -= 6.2831853f;
            invalidate();
        });
    }

    void setConnState(State s) {
        if (s == m_state) return;
        m_state = s;
        if (s == State::Connecting) m_spin.start(std::chrono::milliseconds(55), jf::JTimer::JMode::Repeating);
        else                        m_spin.stop();
        invalidate();
    }
    State connState() const { return m_state; }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override {
        // The connection status icon. W is the icon size (m_sz, e.g. 32) centred in the toolbar slot.
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float W  = std::min(m_sz, std::min(b.width, b.height));
        if (W < 6.f) return;
        const float cx = b.x + b.width  * 0.5f;
        const float cy = b.y + b.height * 0.5f;

        const jf::JColor col  = stateColor();                                  // ring: state colour
        const uint8_t* tp = jf::JStyle::current().TextPrimary;                 // chip = theme "WindowText"
        const jf::JColor chip = jf::rgb(tp[0], tp[1], tp[2]);
        const jf::JColor dot  = jf::rgb(uint8_t(tp[0] * 0.4545f), uint8_t(tp[1] * 0.4545f), uint8_t(tp[2] * 0.4545f)); // chip.darker(220)

        jf::JVectorCanvas vg;
        vg.setAntiAlias(1.2f);

        // --- status ring (colour = state, motion = spin) ---
        const float R  = W * 0.40f;
        const float rw = std::max(1.5f, W * 0.09f);
        if (m_state == State::Connecting)                                      // rotating 3/4 (270°) arc
            vg.strokeArc(cx, cy, R, m_angle, m_angle + 4.712389f, rw, jf::JPaint::solid(col));
        else if (m_state == State::Error)                                      // ring with a fault break
            vg.strokeArc(cx, cy, R, 0.5235988f, 0.5235988f + 5.235988f, rw, jf::JPaint::solid(col));
        else
            vg.strokeCircle(cx, cy, R, rw, jf::JPaint::solid(col));            // full ring

        // --- ECU chip (filled, light) ---
        const float bs = W * 0.30f;                                           // body side
        vg.fillRoundedRect(cx - bs * 0.5f, cy - bs * 0.5f, bs, bs, W * 0.04f, jf::JPaint::solid(chip));

        // pins: 3 per side (left/right)
        const float pw = std::max(1.0f, W * 0.05f);
        const float pl = W * 0.06f;
        for (int i = -1; i <= 1; ++i) {
            const float o = static_cast<float>(i) * bs * 0.32f;
            vg.drawLine(cx - bs * 0.5f - pl, cy + o, cx - bs * 0.5f, cy + o, pw, jf::JPaint::solid(chip));
            vg.drawLine(cx + bs * 0.5f, cy + o, cx + bs * 0.5f + pl, cy + o, pw, jf::JPaint::solid(chip));
        }

        // pin-1 marker (darker)
        vg.fillCircle(cx - bs * 0.5f + bs * 0.22f, cy - bs * 0.5f + bs * 0.22f, W * 0.03f, jf::JPaint::solid(dot));

        vg.flush(buf);
    }

private:
    jf::JColor stateColor() const {
        switch (m_state) {
            case State::Connecting: return jf::rgb(0xEE, 0xAA, 0x3C);  // amber
            case State::Connected:  return jf::rgb(0x46, 0xB3, 0x5A);  // green
            case State::Error:      return jf::rgb(0xE0, 0x52, 0x4F);  // red
            default:                return jf::rgb(0x8B, 0x90, 0x99);  // grey (idle)
        }
    }

    State      m_state{State::Idle};
    float      m_angle{0.f};
    float      m_sz{30.f};      // intended icon size; the icon draws at this, not the toolbar slot height
    jf::JTimer m_spin;
};
