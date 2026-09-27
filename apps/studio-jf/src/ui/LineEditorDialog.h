#pragma once

// LineEditorDialog — authors a livegraph's lines: a scrollable list of rows (channel combo, Min/Max spins,
// Auto-min/Auto-max checks, Remove), an "+ Add line" button, and OK/Cancel. OK serialises to the compact
// string and hands it back via onApply. Line colours are auto-assigned by index, so no colour picker is
// needed. A WM-managed modal; combos/spins/checks
// are click-driven so no text-field key routing is required.

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/FocusManager.h>
#include <j/core/JTextHelper.h>
#include <j/core/JTitleBar.h>   // the ONE canonical styled title bar — no custom chrome
#include <j/core/JButton.h>
#include <j/core/JCheckBox.h>
#include <j/core/JComboBox.h>
#include <j/core/JContainer.h>
#include <j/core/JDoubleSpinBox.h>
#include <j/core/JLabel.h>
#include <j/core/JScrollArea.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include "../model/LineGraphModel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class LineEditorDialog {
public:
    static constexpr uint32_t kW = 580, kH = 400;
    static constexpr float kBtnW = 92.f;
    static float kBtnH() { return jf::JStyle::current().buttonHeight; }   // HEIGHT from JStyle (single source of truth)
    static float hdrH() { return jf::JStyle::current().titleBarHeight; }
    static float rowH() { return jf::JStyle::current().controlHeight + 4.f; }

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    LineEditorDialog(std::string initial, std::vector<std::string> channels, std::function<void(std::string)> onApply,
                     jf::JGpuHal& hal, int screenX, int screenY, NativeWinHandleType parent)
        : m_channels(std::move(channels)), m_onApply(std::move(onApply))
        , m_window(std::make_unique<PlatformWinType>("Graph Lines", kW, kH, screenX, screenY,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        using namespace jf;
        m_lines = LineGraphModel::fromCompact(initial).lines;
        m_scroll = std::make_unique<JScrollArea>(m_graph);
        m_btnAdd    = std::make_unique<JButton>(m_graph, "+ Add line", 120.f);
        m_btnCancel = std::make_unique<JButton>(m_graph, "Cancel", kBtnW);
        m_btnOk     = std::make_unique<JButton>(m_graph, "OK", kBtnW);
        m_btnAdd->onClicked.connect([this] { m_lines.push_back({ m_channels.empty() ? std::string() : m_channels.front(), 0.0, 100.0, true, true }); m_rebuild = true; });
        m_btnCancel->onClicked.connect([this] { m_done = true; });
        m_btnOk->onClicked.connect([this] { applyAndClose(); });
        m_window->setResizable(true);
        m_window->setResizeTopInset(hdrH());
        m_window->setMinSize(460, 280);
        rebuild();
    }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;
        if (m_rebuild) { rebuild(); m_rebuild = false; }

        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease(), held = m_window->isLeftButtonDown();
        const float W = static_cast<float>(m_window->width()), H = static_cast<float>(m_window->height());
        if (m_window->width() != m_lastW || m_window->height() != m_lastH) {
            hal.resizeSurface(m_surface, m_window->width(), m_window->height());
            m_lastW = m_window->width(); m_lastH = m_window->height();
        }
        const bool inTitle = (my >= 0.f && my < hdrH() && mx < W - kBtnW - 12.f);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;
        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            if (ke.key == JKeyEvent::JKey::Escape) return false;
            _refreshFocusRoots();
            if (jf::jRouteKey(ke, m_focus)) continue;   // focused control, then Tab / Shift-Tab
        }

        m_graph.setHostWindow(m_window->screenX(), m_window->screenY(), static_cast<std::uintptr_t>(m_window->rawWindowId()));

        const float btnY = H - kBtnH() - 12.f;
        const float okX = W - kBtnW - 12.f, cancelX = okX - kBtnW - 8.f;
        m_btnAdd->setBounds({ 12.f, btnY, 120.f, kBtnH() });
        m_btnCancel->setBounds({ cancelX, btnY, kBtnW, kBtnH() });
        m_btnOk->setBounds({ okX, btnY, kBtnW, kBtnH() });
        const JRect content{ 8.f, hdrH() + 6.f, W - 16.f, btnY - hdrH() - 16.f };
        m_scroll->setBounds(content);
        m_scroll->handleMouseMove(mx, my);
        if (pressed) { m_scroll->handleMousePress(mx, my); m_btnAdd->handleMousePress(mx, my); m_btnCancel->handleMousePress(mx, my); m_btnOk->handleMousePress(mx, my); }
        if (released) { m_scroll->handleMouseRelease(mx, my); m_btnAdd->handleMouseRelease(mx, my); m_btnCancel->handleMouseRelease(mx, my); m_btnOk->handleMouseRelease(mx, my); }
        if (const float wheel = m_window->consumeWheel(); wheel != 0.f) m_scroll->handleScroll(mx, my, wheel);
        if (m_done) return false;

        buf.clear();
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface0);
        jf::JTitleBar::draw(buf, 0.f, 0.f, W, hdrH(), "Graph Lines — channel, min/max, auto", jf::JStyle::current().cornerRadius, 0, 12.f);
        m_scroll->populateRenderPrimitives(buf);
        m_btnAdd->populateRenderPrimitives(buf);
        m_btnCancel->populateRenderPrimitives(buf);
        m_btnOk->populateRenderPrimitives(buf);

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    void rebuild() {
        using namespace jf;
        m_scroll->clearChildren();   // destroys the previously-owned rows (which own their children)
        const float W = static_cast<float>(kW) - 24.f;
        for (size_t i = 0; i < m_lines.size(); ++i) {
            const int idx = static_cast<int>(i);
            auto rowU = std::make_unique<JContainer>(m_graph);
            JContainer* row = rowU.get();
            row->setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::JRow)->setGap(6.f);   // Flex is Column by default
            row->setBounds({ 0.f, 0.f, W, rowH() });
            JComboBox* chan = row->add(std::make_unique<JComboBox>(m_graph, m_channels, 200.f));   // row owns each child
            for (size_t k = 0; k < m_channels.size(); ++k) if (m_channels[k] == m_lines[i].channel) { chan->setCurrentIndex(static_cast<int>(k)); break; }
            chan->onIndexChanged.connect([this, idx](int j) { if (j >= 0 && j < static_cast<int>(m_channels.size())) m_lines[idx].channel = m_channels[j]; });
            JDoubleSpinBox* mn = row->add(std::make_unique<JDoubleSpinBox>(m_graph, -1e9, 1e9, 1.0, 2, 90.f));
            mn->setValue(m_lines[i].min); mn->onValueChanged.connect([this, idx](double v) { m_lines[idx].min = v; });
            JDoubleSpinBox* mx = row->add(std::make_unique<JDoubleSpinBox>(m_graph, -1e9, 1e9, 1.0, 2, 90.f));
            mx->setValue(m_lines[i].max); mx->onValueChanged.connect([this, idx](double v) { m_lines[idx].max = v; });
            JCheckBox* aMin = row->add(std::make_unique<JCheckBox>(m_graph, "A-min", 62.f));
            aMin->setChecked(m_lines[i].autoMin); aMin->onStateChanged.connect([this, idx](bool b) { m_lines[idx].autoMin = b; });
            JCheckBox* aMax = row->add(std::make_unique<JCheckBox>(m_graph, "A-max", 62.f));
            aMax->setChecked(m_lines[i].autoMax); aMax->onStateChanged.connect([this, idx](bool b) { m_lines[idx].autoMax = b; });
            JButton* rm = row->add(std::make_unique<JButton>(m_graph, "Remove", 70.f));
            rm->onClicked.connect([this, idx] { if (idx >= 0 && idx < static_cast<int>(m_lines.size())) { m_lines.erase(m_lines.begin() + idx); m_rebuild = true; } });
            m_scroll->addChildWidget(std::move(rowU));   // scroll owns the row
        }
        if (m_lines.empty())
        {   // the empty-state note WRAPS, and is as tall as its lines
            auto note = std::make_unique<JLabel>(m_graph, "No lines — Add one below. Each plots a channel over the sample window.", W);
            note->setWordWrap(true);
            note->setBounds({ 0.f, 0.f, W, note->heightFor(W) });
            m_scroll->addChildWidget(std::move(note));
        }
    }

    void applyAndClose() {
        LineGraphModel m; m.lines = m_lines;
        if (m_onApply) m_onApply(m.toCompact());
        m_done = true;
    }

    std::vector<std::string>         m_channels;
    std::function<void(std::string)> m_onApply;
    // This dialog's focus tree, declared because JWidget takes a scene graph rather than a parent, so
    // members are not otherwise discoverable. Clicks focus by themselves (JControl::handleMousePress calls
    // requestFocus, which routes to this manager while the dialog is open); this adds Tab traversal and
    // sends keys to the focused control.
    void _refreshFocusRoots() {
        std::vector<jf::JWidget*> roots;
        if (m_scroll) roots.push_back(m_scroll.get());
        if (m_btnAdd) roots.push_back(m_btnAdd.get());
        if (m_btnCancel) roots.push_back(m_btnCancel.get());
        if (m_btnOk) roots.push_back(m_btnOk.get());
        m_focus.setFocusRoots(std::move(roots));
    }

    jf::JFocusManager m_focus;

    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{ 0 };
    jf::JSceneGraph  m_graph;
    std::unique_ptr<jf::JScrollArea> m_scroll;
    std::unique_ptr<jf::JButton>     m_btnAdd, m_btnCancel, m_btnOk;
    std::vector<LineGraphModel::Line> m_lines;
    uint32_t m_lastW{ kW }, m_lastH{ kH };
    float    m_ax{ 0 }, m_ay{ 0 };
    bool     m_done{ false }, m_drag{ false }, m_rebuild{ false };
};
