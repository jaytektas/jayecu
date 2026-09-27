// fonttest.cpp — minimal JFramework reproduction harness for the
// "font picker opens BEHIND its opener modal" layering question.
//
// It builds the exact nesting the studio has, with nothing else in the way:
//
//     JAppWindow (main)  →  TestModal (a WM-managed app modal, own surface)
//                              └─ button → win.openAppFontPicker()  (nested modal)
//
// So the interesting case is: a modal is on the modal stack, and a button inside
// it pushes the font picker as a CHILD modal. Where does the child stack, and can
// it take mouse/keyboard input? Drive it by hand on the real display and watch.
//
// Build (against the installed SDK, same as busctl — no CMake needed). The lib list
// mirrors studio's own link.txt:
//   g++ -std=c++20 -I$HOME/jframework-sdk/include tools/fonttest.cpp -o tools/fonttest \
//       $HOME/jframework-sdk/lib/libj_platform.a /usr/lib/x86_64-linux-gnu/libvulkan.so \
//       -lxcb -lxcb-keysyms -lxcb-sync -ldbus-1 -lrt -latspi -lglib-2.0 -lpthread
//
// Run on the user's real GPU display:
//   DISPLAY=:0 tools/fonttest

#include <j/app/JAppWindow.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/graphics/RenderPrimitive.h>

#include <functional>
#include <memory>
#include <string>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

using namespace jf;

// A deliberately tiny app modal that satisfies the JAppWindow::openModal<T> contract:
//   static kW/kH, NativeWinHandleType, ctor(args..., JGpuHal&, sx, sy, parent),
//   pollAndRender(hal, buf) -> false when done, destroySurface(hal).
// It owns its own WM-managed surface and hand-draws one "Open Font Picker" button
// plus a "Close" button — no scene graph needed for the repro.
class TestModal {
public:
    static constexpr uint32_t kW = 380, kH = 220;
    static constexpr float kHdr = 32.f;

#if defined(_WIN32)
    using PlatformWinType     = JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    // App-set hook: the button fires this. main() wires it to win.openAppFontPicker(),
    // exactly like PreferencesDialog::onPickAppFont in the studio.
    static inline std::function<void()> onOpenFont;

    TestModal(JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : m_window(std::make_unique<PlatformWinType>("Test Modal", kW, kH, sx, sy,
                                                     JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {}

    void destroySurface(JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(JGpuHal& hal, JPrimitiveBuffer& buf) {
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress();
        const bool held    = m_window->isLeftButtonDown();

        // Title-bar drag.
        const bool inTitle = (my >= 0.f && my < kHdr && mx < static_cast<float>(kW) - 40.f);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        for (const auto& ke : m_window->consumeAllKeys())
            if (ke.pressed && ke.key == JKeyEvent::JKey::Escape) return false;

        // "Open Font Picker" button.
        const float bw = 220.f, bh = 40.f, bx = (kW - bw) * 0.5f, by = 78.f;
        const bool hovBtn = (mx >= bx && mx < bx + bw && my >= by && my < by + bh);
        // No self-ungrab here: opening the picker must be mouse-interactive purely because the
        // FRAMEWORK drops the superseded window's grab in JAppWindow::setModalDialog. If the picker
        // takes clicks with nothing added on the app side, the framework fix is proven.
        if (pressed && hovBtn) { if (onOpenFont) onOpenFont(); }

        // "Close" button.
        const float cbw = 90.f, cbh = 28.f, cbx = (kW - cbw) * 0.5f, cby = kH - cbh - 16.f;
        if (pressed && mx >= cbx && mx < cbx + cbw && my >= cby && my < cby + cbh) return false;

        // Render to this modal's own surface.
        buf.clear();
        const float W = static_cast<float>(kW), H = static_cast<float>(kH);
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface0, 6.f, 1.f, Colors::Border);
        buf.pushRectangle(0.f, 0.f, W, kHdr, Colors::Surface2, 6.f);
        buf.pushRectangle(0.f, 6.f, W, kHdr - 6.f, Colors::Surface2, 0.f);
        const float lh = JTextHelper::lineHeight();
        const bool atlas = JTextHelper::hasAtlas();
        if (atlas) JTextHelper::pushText(buf, 12.f, (kHdr - lh) * 0.5f, "Test Modal", Colors::TextPrimary, W - 20.f);

        uint8_t ac[4] = { Colors::Accent[0], Colors::Accent[1], Colors::Accent[2], static_cast<uint8_t>(hovBtn ? 255 : 210) };
        buf.pushRectangle(bx, by, bw, bh, ac, 5.f);
        if (atlas) JTextHelper::pushText(buf, bx + (bw - JTextHelper::measureWidth("Open Font Picker")) * 0.5f,
                                         by + (bh - lh) * 0.5f, "Open Font Picker", Colors::TextPrimary);

        const bool hovClose = (mx >= cbx && mx < cbx + cbw && my >= cby && my < cby + cbh);
        uint8_t cbg[4] = { Colors::Surface2[0], Colors::Surface2[1], Colors::Surface2[2], static_cast<uint8_t>(hovClose ? 255 : 230) };
        buf.pushRectangle(cbx, cby, cbw, cbh, cbg, 5.f, 1.f, Colors::Border);
        if (atlas) JTextHelper::pushText(buf, cbx + (cbw - JTextHelper::measureWidth("Close")) * 0.5f,
                                         cby + (cbh - lh) * 0.5f, "Close", Colors::TextPrimary);

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    std::unique_ptr<PlatformWinType> m_window;
    GpuSurfaceId m_surface{ 0 };
    bool  m_drag{ false };
    float m_ax{ 0.f }, m_ay{ 0.f };
};

int main() {
    JGuiApplication app;
    JAppWindow win("Framework Font-Picker Test", 900, 600);
    if (!win.valid()) return -1;

    // The modal's button opens the whole-app font picker — the same path Preferences uses.
    TestModal::onOpenFont = [&win] { win.openAppFontPicker([](std::string, int) {}); };

    // A toolbar button opens the modal.
    win.toolBar().addButton("Open Modal", [&win] { win.openModal<TestModal>(); });

    win.onRender = [&win](JPrimitiveBuffer& buf) {
        if (!JTextHelper::hasAtlas()) return;
        JTextHelper::pushText(buf, 20.f, win.contentTop() + 20.f,
                              "1) Click 'Open Modal' in the toolbar.", Colors::TextPrimary, 840.f);
        JTextHelper::pushText(buf, 20.f, win.contentTop() + 44.f,
                              "2) In the modal, click 'Open Font Picker'.", Colors::TextPrimary, 840.f);
        JTextHelper::pushText(buf, 20.f, win.contentTop() + 68.f,
                              "3) Observe whether the picker stacks above the modal and takes input.",
                              Colors::TextSecondary, 840.f);
    };

    return win.run();
}
