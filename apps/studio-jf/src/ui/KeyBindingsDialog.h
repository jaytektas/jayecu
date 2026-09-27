#pragma once

// KeyBindingsDialog — the table editor's "Key Bindings…" modal (opened from the run-mode table context menu).
// A scrollable form of the 16 editable table actions, each with a
// JKeySequenceEdit capture box, plus Restore Defaults / Cancel / OK. Edits a working copy; OK writes it to the
// global Keymap (which persists via JSettings), Cancel discards, Restore Defaults reloads the built-ins.
// The framework owns the window (title-bar drag, edge-resize), scroll/clip, and each control's sizing.

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/FocusManager.h>
#include <j/core/JTextHelper.h>
#include <j/core/JTitleBar.h>   // the ONE canonical styled title bar — no custom chrome
#include <j/core/JButton.h>
#include <j/core/JContainer.h>
#include <j/core/JKeySequenceEdit.h>
#include <j/core/JLabel.h>
#include <j/core/JScrollArea.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include "../model/Keymap.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class KeyBindingsDialog {
public:
    static constexpr uint32_t kW = 380, kH = 470;
    static constexpr float kBtnW = 96.f, kRestoreW = 130.f;
    static float kBtnH() { return jf::JStyle::current().buttonHeight; }   // HEIGHT from JStyle (single source of truth)
    static float hdrH() { return jf::JStyle::current().titleBarHeight; }
    static float padV() { return jf::JStyle::current().itemPadding; }
    static float gapV() { return jf::JStyle::current().spacing; }

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    KeyBindingsDialog(jf::JGpuHal& hal, int screenX, int screenY, NativeWinHandleType parent)
        : m_window(std::make_unique<PlatformWinType>("Table Key Bindings", kW, kH, screenX, screenY,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        using namespace jf;
        const auto& acts = Keymap::editableActions();

        m_form = std::make_unique<JContainer>(m_graph);
        m_form->setLayoutMode(JLayoutMode::Form)->setGap(gapV())->setPadding(JEdges{ padV(), padV(), padV(), padV() });
        for (size_t i = 0; i < acts.size(); ++i) {
            const Keymap::Action a = acts[i];
            m_work[static_cast<size_t>(a)] = Keymap::instance().binding(a);
            m_form->add(std::make_unique<JLabel>(m_graph, Keymap::label(a), 175.f));   // form owns the label
            JKeySequenceEdit* e = m_form->add(std::make_unique<JKeySequenceEdit>(m_graph, Keymap::toString(m_work[static_cast<size_t>(a)]), 140.f));   // form owns the box
            const int idx = static_cast<int>(i);
            e->onClicked.connect([this, idx] { arm(idx); });
            e->onCaptured.connect([this, a, e](const JKeyEvent& ke) {
                const Keymap::KeySpec spec = Keymap::fromEvent(ke);
                m_work[static_cast<size_t>(a)] = spec;
                e->setText(Keymap::toString(spec));
                m_capturing = -1;
            });
            m_edits.push_back(e);
        }
        const float natH = padV() * 2.f + static_cast<float>(acts.size()) * (JStyle::current().controlHeight + gapV()) + gapV();
        m_form->setBounds({ 0.f, 0.f, static_cast<float>(kW) - 20.f, natH });
        m_scroll = std::make_unique<JScrollArea>(m_graph);
        m_scroll->addChildWidget(m_form.get());

        m_btnRestore = std::make_unique<JButton>(m_graph, "Restore Defaults", kRestoreW);
        m_btnCancel  = std::make_unique<JButton>(m_graph, "Cancel", kBtnW);
        m_btnOk      = std::make_unique<JButton>(m_graph, "OK", kBtnW);
        m_btnRestore->onClicked.connect([this] { restoreDefaults(); });
        m_btnCancel->onClicked.connect([this] { m_done = true; });
        m_btnOk->onClicked.connect([this] { applyAndClose(); });

        // Set options and forget — the framework owns edge-resize (grab zones, cursors, WM handoff).
        m_window->setResizable(true);
        m_window->setResizeTopInset(hdrH());
        m_window->setMinSize(320, 300);
    }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;

        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease(), held = m_window->isLeftButtonDown();
        const float W = static_cast<float>(m_window->width()), H = static_cast<float>(m_window->height());
        if (m_window->width() != m_lastW || m_window->height() != m_lastH) {
            hal.resizeSurface(m_surface, m_window->width(), m_window->height());
            m_lastW = m_window->width(); m_lastH = m_window->height();
        }

        // Title-bar drag (edge/corner resize is framework-managed via setResizable()).
        const bool inTitle = (my >= 0.f && my < hdrH());
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        // Keyboard: while a capture box is armed, route keys to it; otherwise Escape closes the dialog.
        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            _refreshFocusRoots();
            if (m_capturing < 0 && jf::jRouteKey(ke, m_focus)) continue;   // not capturing: focus + Tab
            if (m_capturing >= 0 && m_capturing < static_cast<int>(m_edits.size())) {
                m_edits[m_capturing]->handleKeyEvent(ke);
                if (!m_edits[m_capturing]->capturing()) m_capturing = -1;
            } else if (ke.key == JKeyEvent::JKey::Escape) {
                return false;
            }
        }

        // Buttons along the bottom strip: Restore (left), Cancel + OK (right).
        const float btnY = H - kBtnH() - 12.f;
        const float okX = W - kBtnW - 12.f, cancelX = okX - kBtnW - 8.f;
        m_btnRestore->setBounds({ 12.f, btnY, kRestoreW, kBtnH() });
        m_btnCancel->setBounds({ cancelX, btnY, kBtnW, kBtnH() });
        m_btnOk->setBounds({ okX, btnY, kBtnW, kBtnH() });

        // Content scroll area (below the title bar, above the buttons). The framework owns clip + scrollbar.
        const JRect content{ 8.f, hdrH() + 6.f, W - 16.f, btnY - hdrH() - 18.f };
        m_scroll->setBounds(content);
        m_scroll->handleMouseMove(mx, my);
        if (pressed) {
            m_scroll->handleMousePress(mx, my);
            m_btnRestore->handleMousePress(mx, my);
            m_btnCancel->handleMousePress(mx, my);
            m_btnOk->handleMousePress(mx, my);
        }
        if (released) m_scroll->handleMouseRelease(mx, my);
        if (const float wheel = m_window->consumeWheel(); wheel != 0.f) m_scroll->handleScroll(mx, my, wheel);
        if (m_done) return false;   // a button may have closed us this frame

        // Render.
        buf.clear();
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface0);
        jf::JTitleBar::draw(buf, 0.f, 0.f, W, hdrH(), "Table Key Bindings", jf::JStyle::current().cornerRadius, 0, 12.f);
        m_scroll->populateRenderPrimitives(buf);
        m_btnRestore->populateRenderPrimitives(buf);
        m_btnCancel->populateRenderPrimitives(buf);
        m_btnOk->populateRenderPrimitives(buf);

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    void arm(int idx) {   // clicking a box arms it; disarm the others so only one captures at a time
        for (int i = 0; i < static_cast<int>(m_edits.size()); ++i) if (i != idx) m_edits[i]->setCapturing(false);
        m_capturing = idx;
    }
    void restoreDefaults() {
        const auto& acts = Keymap::editableActions();
        for (size_t i = 0; i < acts.size(); ++i) {
            const Keymap::Action a = acts[i];
            m_work[static_cast<size_t>(a)] = Keymap::defaultBinding(a);
            m_edits[i]->setText(Keymap::toString(m_work[static_cast<size_t>(a)]));
            m_edits[i]->setCapturing(false);
        }
        m_capturing = -1;
    }
    void applyAndClose() {
        for (Keymap::Action a : Keymap::editableActions())
            Keymap::instance().setBinding(a, m_work[static_cast<size_t>(a)]);
        m_done = true;
    }

    // This dialog's focus tree, declared because JWidget takes a scene graph rather than a parent, so
    // members are not otherwise discoverable. Clicks focus by themselves (JControl::handleMousePress calls
    // requestFocus, which routes to this manager while the dialog is open); this adds Tab traversal and
    // sends keys to the focused control.
    void _refreshFocusRoots() {
        std::vector<jf::JWidget*> roots;
        if (m_scroll) roots.push_back(m_scroll.get());
        if (m_btnOk) roots.push_back(m_btnOk.get());
        if (m_btnCancel) roots.push_back(m_btnCancel.get());
        if (m_btnRestore) roots.push_back(m_btnRestore.get());
        m_focus.setFocusRoots(std::move(roots));
    }

    jf::JFocusManager m_focus;

    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{ 0 };
    jf::JSceneGraph  m_graph;                                     // declared before the widgets so it outlives them
    std::unique_ptr<jf::JContainer>  m_form;
    std::unique_ptr<jf::JScrollArea> m_scroll;
    std::vector<jf::JKeySequenceEdit*>        m_edits;           // parallel to editableActions()
    std::unique_ptr<jf::JButton> m_btnOk, m_btnCancel, m_btnRestore;
    std::array<Keymap::KeySpec, Keymap::kCount> m_work{};
    int      m_capturing{ -1 };
    uint32_t m_lastW{ kW }, m_lastH{ kH };
    bool     m_done{ false }, m_drag{ false };
    float    m_ax{ 0 }, m_ay{ 0 };
};
