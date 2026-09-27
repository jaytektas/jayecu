#pragma once

// SaveChangesDialog — "You have unsaved changes." shown when closing with a dirty document. Three
// choices (Save / Discard / Cancel) plus a "Don't ask again — remember my choice" checkbox that, when
// ticked, persists the chosen Save/Discard as the default (also editable in Preferences ▸ Editor).
// Result is delivered via onResult(result, remember): result 1=Save, 2=Discard, 0=Cancel.

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/core/JTitleBar.h>   // the ONE canonical styled title bar — no custom chrome
#include <j/core/JButton.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/FocusManager.h>
#include <j/core/JCheckBox.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include "WrapText.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

class SaveChangesDialog {
public:
    static constexpr uint32_t kW = 420, kH = 168;
    static constexpr float kBtnW = 92.f;
    static float kBtnH() { return jf::JStyle::current().buttonHeight; }   // HEIGHT from JStyle (single source of truth)
    static float hdrH() { return jf::JStyle::current().titleBarHeight; }
    // kH is the PLACEMENT height; the window is built at heightFor(message) — the message WRAPS, and the
    // checkbox and buttons sit below however many lines it took.
    static uint32_t heightFor(const std::string& msg) {
        const float need = hdrH() + 16.f + wraptext::height(msg, static_cast<float>(kW) - 32.f)
                         + 12.f + 26.f + kBtnH() + 12.f;
        return std::max(kH, static_cast<uint32_t>(std::ceil(need)));
    }

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    SaveChangesDialog(std::string message, std::function<void(int, bool)> onResult,
                      jf::JGpuHal& hal, int screenX, int screenY, NativeWinHandleType parent)
        : m_msg(std::move(message)), m_onResult(std::move(onResult)), m_h(heightFor(m_msg))
        , m_window(std::make_unique<PlatformWinType>("Unsaved changes", kW, m_h, screenX, screenY,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, m_h)) {
        using namespace jf;
        m_remember = std::make_unique<JCheckBox>(m_graph, "Don't ask again (remember my choice)");
        // Standard footer by ROLE: Discard is Destructive so the box keeps it left of the pair (the
        // Qt/GNOME shape for a save-changes prompt), Cancel is Reject (Escape) and Save is Accept (Return).
        // The box owns order, placement, hover, click-on-release and focus.
        m_box = std::make_unique<jf::JDialogButtonBox>(m_graph);
        m_discard = m_box->addButton("Discard", jf::JDialogButtonBox::Role::Destructive, kBtnW);
        m_box->addButton("Cancel",  jf::JDialogButtonBox::Role::Reject, kBtnW);
        m_box->addButton("Save",    jf::JDialogButtonBox::Role::Accept, kBtnW);
        m_discard->onClicked.connect([this] { finish(2); });
        m_box->onReject.connect([this] { finish(0); });
        m_box->onAccept.connect([this] { finish(1); });
        m_window->grabKeyboardFocus();
    }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) { finish(0); return false; }   // closing the prompt = Cancel

        const float W = static_cast<float>(kW), H = static_cast<float>(m_h);
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease();

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            _refreshFocusRoots();
            if (jf::jRouteKey(ke, m_focus)) continue;          // focused control, then Tab traversal
            if (m_box->handleKeyEvent(ke)) { if (m_done) return false; continue; }   // Return=Save, Escape=Cancel
        }

        m_graph.setHostWindow(m_window->screenX(), m_window->screenY(), static_cast<std::uintptr_t>(m_window->rawWindowId()));

        const float pad = 16.f;
        m_remember->setBounds({ pad, H - kBtnH() - 12.f - 26.f, W - 2 * pad, 20.f });
        const float by = H - kBtnH() - 12.f;
        m_box->setBounds({ pad, by, W - 2 * pad, kBtnH() });

        _refreshFocusRoots();
        if (pressed) jf::jRouteMouse(mx, my, m_focus);                        // click-to-focus
        if (!m_focusSeeded) { m_focusSeeded = true; m_focus.focusFirst(); }   // keyboard works on open
        m_remember->handleMouseMove(mx, my);
        m_box->handleMouseMove(mx, my);                                       // buttons DO get hover now
        if (pressed)  { m_remember->handleMousePress(mx, my);   m_box->handleMousePress(mx, my); }
        if (released) { m_remember->handleMouseRelease(mx, my); m_box->handleMouseRelease(mx, my); }
        if (m_done) return false;

        buf.clear();
        buf.pushRectangle(0.f, 0.f, W, H, Colors::DialogBg, 8.f, 1.f, Colors::Border);
        jf::JTitleBar::draw(buf, 0.f, 0.f, W, hdrH(), "Unsaved changes", jf::JStyle::current().cornerRadius, 0, 14.f);
        if (JTextHelper::hasAtlas())
            wraptext::draw(buf, 16.f, hdrH() + 16.f, m_msg, Colors::TextSecondary, W - 32.f);
        m_remember->populateRenderPrimitives(buf);
        m_box->populateRenderPrimitives(buf);

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    void finish(int result) {
        if (m_done) return;
        m_done = true;
        if (m_onResult) m_onResult(result, m_remember && m_remember->isChecked());
    }

    std::string m_msg;
    std::function<void(int, bool)> m_onResult;
    uint32_t m_h;                                  // the real height: the wrapped message's (heightFor)
    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{ 0 };
    jf::JSceneGraph  m_graph;
    std::unique_ptr<jf::JCheckBox> m_remember;
    std::unique_ptr<jf::JDialogButtonBox> m_box;                 // the standard footer
    jf::JButton*                          m_discard{nullptr};    // owned by the box
    jf::JFocusManager                     m_focus;               // this dialog's keyboard focus
    bool                                  m_focusSeeded{false};

    // This dialog's focus tree: the remember checkbox and the footer buttons.
    void _refreshFocusRoots() {
        std::vector<jf::JWidget*> roots;
        if (m_remember) roots.push_back(m_remember.get());
        if (m_box)      roots.push_back(m_box.get());
        m_focus.setFocusRoots(std::move(roots));
    }
    bool m_done{ false };
};
