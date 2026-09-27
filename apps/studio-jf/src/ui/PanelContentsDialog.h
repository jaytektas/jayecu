#pragma once

// PanelContentsDialog — authors a panel control's nested children: a scrollable list of rows (control-type
// combo, X/Y/W/H spins, a data-source combo, Remove), Add, and OK/Cancel. OK serialises the children to the
// JSON array the panel render consumes ("children" prop). A WM-managed modal (combos/spins are click-driven).
// This is the composable authoring path for panels; in-place drag-into-panel is a follow-up.

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/FocusManager.h>
#include <j/core/JTextHelper.h>
#include <j/core/JTitleBar.h>   // the ONE canonical styled title bar — no custom chrome
#include <j/core/JButton.h>
#include <j/core/JComboBox.h>
#include <j/core/JContainer.h>
#include <j/core/JDoubleSpinBox.h>
#include <j/core/JLabel.h>
#include <j/core/JScrollArea.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>
#include <j/config/Json.h>
#include <j/core/Uuid.h>          // stable per-child UUID (jf::makeUuid) — each child's identity

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class PanelContentsDialog {
public:
    static constexpr uint32_t kW = 620, kH = 420;
    static constexpr float kBtnW = 92.f, kBtnH = 22.f;
    static float hdrH() { return jf::JStyle::current().titleBarHeight; }
    static float rowH() { return jf::JStyle::current().controlHeight + 4.f; }

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    PanelContentsDialog(std::string initial, std::vector<std::string> types, std::vector<std::string> channels,
                        std::function<void(std::string)> onApply,
                        jf::JGpuHal& hal, int screenX, int screenY, NativeWinHandleType parent)
        : m_types(std::move(types)), m_channels(std::move(channels)), m_onApply(std::move(onApply))
        , m_window(std::make_unique<PlatformWinType>("Panel Contents", kW, kH, screenX, screenY,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        using namespace jf;
        parse(initial);
        m_scroll = std::make_unique<JScrollArea>(m_graph);
        m_btnAdd    = std::make_unique<JButton>(m_graph, "+ Add control", 130.f);
        m_btnCancel = std::make_unique<JButton>(m_graph, "Cancel", kBtnW);
        m_btnOk     = std::make_unique<JButton>(m_graph, "OK", kBtnW);
        m_btnAdd->onClicked.connect([this] { m_kids.push_back({ std::string(), m_types.empty() ? "value" : m_types.front(), 10, 10, 80, 40, "" }); m_rebuild = true; });
        m_btnCancel->onClicked.connect([this] { m_done = true; });
        m_btnOk->onClicked.connect([this] { applyAndClose(); });
        m_window->setResizable(true);
        m_window->setResizeTopInset(hdrH());
        m_window->setMinSize(500, 300);
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

        const float btnY = H - kBtnH - 12.f;
        const float okX = W - kBtnW - 12.f, cancelX = okX - kBtnW - 8.f;
        m_btnAdd->setBounds({ 12.f, btnY, 130.f, kBtnH });
        m_btnCancel->setBounds({ cancelX, btnY, kBtnW, kBtnH });
        m_btnOk->setBounds({ okX, btnY, kBtnW, kBtnH });
        m_scroll->setBounds({ 8.f, hdrH() + 6.f, W - 16.f, btnY - hdrH() - 16.f });
        m_scroll->handleMouseMove(mx, my);
        if (pressed) { m_scroll->handleMousePress(mx, my); m_btnAdd->handleMousePress(mx, my); m_btnCancel->handleMousePress(mx, my); m_btnOk->handleMousePress(mx, my); }
        if (released) { m_scroll->handleMouseRelease(mx, my); m_btnAdd->handleMouseRelease(mx, my); m_btnCancel->handleMouseRelease(mx, my); m_btnOk->handleMouseRelease(mx, my); }
        if (const float wheel = m_window->consumeWheel(); wheel != 0.f) m_scroll->handleScroll(mx, my, wheel);
        if (m_done) return false;

        buf.clear();
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface0);
        jf::JTitleBar::draw(buf, 0.f, 0.f, W, hdrH(), "Panel Contents — type, x/y/w/h, source", jf::JStyle::current().cornerRadius, 0, 12.f);
        m_scroll->populateRenderPrimitives(buf);
        m_btnAdd->populateRenderPrimitives(buf);
        m_btnCancel->populateRenderPrimitives(buf);
        m_btnOk->populateRenderPrimitives(buf);
        auto frame = hal.beginFrame(m_surface); hal.drawPrimitives(buf); hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    struct Kid { std::string uid; std::string type; double x, y, w, h; std::string channel; };

    void parse(const std::string& s) {
        if (auto j = jf::JJson::tryParse(s); j && j->isArray())
            for (const auto& e : j->arr()) {
                Kid k; k.uid = e["uid"].str(); k.type = e["type"].str(); k.x = e["x"].number(); k.y = e["y"].number();
                k.w = e["w"].number(80); k.h = e["h"].number(40); k.channel = e["props"]["signalName"].str();
                if (!k.type.empty()) m_kids.push_back(std::move(k));
            }
    }

    void rebuild() {
        using namespace jf;
        m_scroll->clearChildren();   // destroys the previously-owned rows (which own their children)
        const float W = static_cast<float>(kW) - 24.f;
        for (size_t i = 0; i < m_kids.size(); ++i) {
            const int idx = static_cast<int>(i);
            auto rowU = std::make_unique<JContainer>(m_graph);
            JContainer* row = rowU.get();
            row->setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::JRow)->setGap(5.f);   // Flex is Column by default
            row->setBounds({ 0.f, 0.f, W, rowH() });
            JComboBox* type = row->add(std::make_unique<JComboBox>(m_graph, m_types, 130.f));   // row owns each child
            for (size_t k = 0; k < m_types.size(); ++k) if (m_types[k] == m_kids[i].type) { type->setCurrentIndex(static_cast<int>(k)); break; }
            type->onIndexChanged.connect([this, idx](int j) { if (j >= 0 && j < static_cast<int>(m_types.size())) m_kids[idx].type = m_types[j]; });
            row->add(numBox(m_kids[i].x, [this, idx](double v) { m_kids[idx].x = v; }));
            row->add(numBox(m_kids[i].y, [this, idx](double v) { m_kids[idx].y = v; }));
            row->add(numBox(m_kids[i].w, [this, idx](double v) { m_kids[idx].w = v; }));
            row->add(numBox(m_kids[i].h, [this, idx](double v) { m_kids[idx].h = v; }));
            JComboBox* chan = row->add(std::make_unique<JComboBox>(m_graph, m_channels, 150.f));
            for (size_t k = 0; k < m_channels.size(); ++k) if (m_channels[k] == m_kids[i].channel) { chan->setCurrentIndex(static_cast<int>(k)); break; }
            chan->onIndexChanged.connect([this, idx](int j) { if (j >= 0 && j < static_cast<int>(m_channels.size())) m_kids[idx].channel = m_channels[j]; });
            JButton* rm = row->add(std::make_unique<JButton>(m_graph, "X", 26.f));
            rm->onClicked.connect([this, idx] { if (idx >= 0 && idx < static_cast<int>(m_kids.size())) { m_kids.erase(m_kids.begin() + idx); m_rebuild = true; } });
            m_scroll->addChildWidget(std::move(rowU));   // scroll owns the row
        }
        if (m_kids.empty())
        {   // the empty-state note WRAPS, and is as tall as its lines
            auto note = std::make_unique<JLabel>(m_graph, "Empty panel — Add a control below.", W);
            note->setWordWrap(true);
            note->setBounds({ 0.f, 0.f, W, note->heightFor(W) });
            m_scroll->addChildWidget(std::move(note));
        }
    }
    std::unique_ptr<jf::JDoubleSpinBox> numBox(double v, std::function<void(double)> cb) {
        auto s = std::make_unique<jf::JDoubleSpinBox>(m_graph, -100000.0, 100000.0, 1.0, 0, 62.f);
        s->setValue(v); s->onValueChanged.connect(std::move(cb));
        return s;
    }

    void applyAndClose() {
        jf::JJson arr = jf::JJson::array();
        for (const Kid& k : m_kids) {
            jf::JJson e = jf::JJson::object();
            e["uid"] = k.uid.empty() ? jf::makeUuid() : k.uid;   // stable identity → @-addressable
            e["type"] = k.type; e["x"] = k.x; e["y"] = k.y; e["w"] = k.w; e["h"] = k.h;
            jf::JJson props = jf::JJson::object(); if (!k.channel.empty()) props["signalName"] = k.channel; e["props"] = props;
            arr.push(e);
        }
        if (m_onApply) m_onApply(arr.dump());
        m_done = true;
    }

    std::vector<std::string> m_types, m_channels;
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
    std::vector<Kid> m_kids;
    uint32_t m_lastW{ kW }, m_lastH{ kH };
    float    m_ax{ 0 }, m_ay{ 0 };
    bool     m_done{ false }, m_drag{ false }, m_rebuild{ false };
};
