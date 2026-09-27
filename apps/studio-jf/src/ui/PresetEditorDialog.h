#pragma once

// PresetEditorDialog — authors a settingselector's preset options: a scrollable list of rows, each a Label
// field and a "path=value, path=value" pairs field, plus Add/Remove and OK/Cancel.
//
// A PRESET IS RECORDED, NOT TYPED. The pairs are config paths and RAW values, so writing one by hand means
// knowing both the path and the field's scale — 1550 for 155.0 deg. Two per-row buttons remove that: "+ Path"
// picks a field through the app's own source picker and records its CURRENT value, and "Capture" refreshes
// every value in the row from live config. So the way to build a preset is to set the ECU up the way you want
// it, then press Capture. The text field stays as the way to read and hand-edit what you have. OK serialises the options
// to the compact string and hands it back via onApply. A WM-managed modal; unlike the other dialogs this one
// has text fields, so it routes keystrokes to the focused JLineEdit (set via each field's onClicked).

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/FocusManager.h>
#include <j/core/JTextHelper.h>
#include <j/core/JTitleBar.h>   // the ONE canonical styled title bar — no custom chrome
#include <j/core/JButton.h>
#include <j/core/JContainer.h>
#include <j/core/JLabel.h>
#include <j/core/JLineEdit.h>
#include <j/core/JScrollArea.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include "../model/PresetOptions.h"
#include "../model/ExprCompiler.h"
#include "../model/MetaModel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class PresetEditorDialog {
public:
    static constexpr uint32_t kW = 760, kH = 440;
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

    // pickPath opens the app's source picker (a nested modal) and calls back with the chosen path.
    using PickFn = std::function<void(std::string current, std::function<void(std::string)> onPick)>;
    PresetEditorDialog(std::string initial, std::function<void(std::string)> onApply, PickFn pickPath,
                       jf::JGpuHal& hal, int screenX, int screenY, NativeWinHandleType parent)
        : m_onApply(std::move(onApply)), m_pickPath(std::move(pickPath))
        , m_window(std::make_unique<PlatformWinType>("Preset Options", kW, kH, screenX, screenY,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        using namespace jf;
        m_options = PresetOptions::fromCompact(initial).options;
        m_scroll = std::make_unique<JScrollArea>(m_graph);
        m_btnAdd    = std::make_unique<JButton>(m_graph, "+ Add preset", 130.f);
        m_btnCancel = std::make_unique<JButton>(m_graph, "Cancel", kBtnW);
        m_btnOk     = std::make_unique<JButton>(m_graph, "OK", kBtnW);
        m_btnAdd->onClicked.connect([this] { syncFromWidgets(); m_options.push_back({ "Preset", {} }); m_rebuild = true; });
        m_btnCancel->onClicked.connect([this] { m_done = true; });
        m_btnOk->onClicked.connect([this] { applyAndClose(); });
        m_window->setResizable(true);
        m_window->setResizeTopInset(hdrH());
        m_window->setMinSize(440, 300);
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

        // Keyboard: Escape closes; every other key goes to the focused text field.
        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            if (ke.key == JKeyEvent::JKey::Escape) return false;
            _refreshFocusRoots();
            if (jf::jRouteKey(ke, m_focus)) continue;   // framework focus + Tab; the old per-dialog
            if (m_focused) m_focused->handleKeyEvent(ke);   // m_focused pointer stays as the fallback
        }

        const float btnY = H - kBtnH() - 12.f;
        const float okX = W - kBtnW - 12.f, cancelX = okX - kBtnW - 8.f;
        m_btnAdd->setBounds({ 12.f, btnY, 130.f, kBtnH() });
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
        jf::JTitleBar::draw(buf, 0.f, 0.f, W, hdrH(), "Preset Options — Label + \"path=value, path=value\"", jf::JStyle::current().cornerRadius, 0, 12.f);
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
    struct Row { jf::JLineEdit* label = nullptr; jf::JLineEdit* pairs = nullptr; };

    void focusEdit(jf::JLineEdit* e) {
        for (const Row& r : m_rows) { if (r.label) r.label->setFocused(r.label == e); if (r.pairs) r.pairs->setFocused(r.pairs == e); }
        m_focused = e;
    }

    void syncFromWidgets() {   // pull the live field text back into the option model
        for (size_t i = 0; i < m_rows.size() && i < m_options.size(); ++i) {
            m_options[i].label = m_rows[i].label->text();
            m_options[i].pairs = PresetOptions::pairsFromText(m_rows[i].pairs->text());
        }
    }

    void rebuild() {
        using namespace jf;
        m_focused = nullptr;
        m_scroll->clearChildren();   // destroys the previously-owned rows (which own their children)
        m_rows.clear();
        const float W = static_cast<float>(kW) - 24.f;
        for (size_t i = 0; i < m_options.size(); ++i) {
            const int idx = static_cast<int>(i);
            auto rowU = std::make_unique<JContainer>(m_graph);
            JContainer* row = rowU.get();
            // ROW, and it has to say so: Flex defaults to Column (SceneGraph.h:139), which stacked the
            // pairs field and the Remove button 34 and 68px BELOW a 28px row — clipped away by the
            // container's own clip. The list looked like labels with no fields and no way to delete one.
            row->setLayoutMode(JLayoutMode::Flex)->setDirection(JFlexDirection::JRow)->setGap(6.f);
            row->setBounds({ 0.f, 0.f, W, rowH() });
            JLineEdit* label = row->add(std::make_unique<JLineEdit>(m_graph, "Label", 140.f));   // row owns each child
            label->setText(m_options[i].label);
            label->onClicked.connect([this, label] { focusEdit(label); });
            JLineEdit* pairs = row->add(std::make_unique<JLineEdit>(m_graph, "path=value, path=value", 300.f));
            pairs->setText(PresetOptions::pairsToText(m_options[i].pairs));
            pairs->onClicked.connect([this, pairs] { focusEdit(pairs); });
            // + Path — choose a config field instead of remembering its path, and record what it reads NOW.
            JButton* addp = row->add(std::make_unique<JButton>(m_graph, "+ Path", 70.f));
            addp->onClicked.connect([this, idx] {
                if (!m_pickPath) return;
                syncFromWidgets();                       // the picker is a nested modal; keep the typed text
                m_pickPath({}, [this, idx](std::string path) {
                    if (path.empty() || idx < 0 || idx >= static_cast<int>(m_options.size())) return;
                    // The tree offers rows that are NOT fields: an element row ("engine.cyl[0]") is there to
                    // be dropped on a viewport, not written. A preset can only hold something the Cache can
                    // locate, so anything else is declined rather than recorded as a pair that writes nowhere.
                    const MetaModel* meta = Cache::instance().meta();
                    if (!meta || !meta->locate(path).valid()) return;
                    // A program or a string is captured as what it IS — the source text, the string —
                    // rather than as byte 0 of its storage read as a number.
                    PresetOptions::Pair pr;
                    pr.path = path;
                    if (meta->isExpressionField(path)) {
                        pr.kind = PresetOptions::Kind::Expr;
                        const std::vector<uint8_t> blob = Cache::instance().configBlob(path);
                        pr.text = ExprCompiler::decompile(blob.data(),
                                                          static_cast<uint16_t>(blob.size()), *meta);
                    } else if (Cache::instance().widgetTypeFor(path) == "text") {
                        pr.kind = PresetOptions::Kind::Text;
                        pr.text = Cache::instance().configString(path);
                    } else {
                        pr.value = Cache::instance().configValue(path);
                    }
                    m_options[idx].pairs.push_back(std::move(pr));
                    m_rebuild = true;
                });
            });
            // Capture — re-read every value in this row from live config. Set the ECU up the way the preset
            // means, press this, and the numbers are right without anyone converting a scale by hand.
            JButton* cap = row->add(std::make_unique<JButton>(m_graph, "Capture", 78.f));
            cap->onClicked.connect([this, idx] {
                syncFromWidgets();
                if (idx < 0 || idx >= static_cast<int>(m_options.size())) return;
                const MetaModel* cmeta = Cache::instance().meta();
                for (auto& pr : m_options[idx].pairs) {
                    if (pr.kind == PresetOptions::Kind::Expr) {
                        const std::vector<uint8_t> blob = Cache::instance().configBlob(pr.path);
                        if (cmeta) pr.text = ExprCompiler::decompile(blob.data(),
                                                                     static_cast<uint16_t>(blob.size()), *cmeta);
                    } else if (pr.kind == PresetOptions::Kind::Text) {
                        pr.text = Cache::instance().configString(pr.path);
                    } else {
                        pr.value = Cache::instance().configValue(pr.path);
                    }
                }
                m_rebuild = true;
            });
            JButton* rm = row->add(std::make_unique<JButton>(m_graph, "Remove", 70.f));
            rm->onClicked.connect([this, idx] { syncFromWidgets(); if (idx >= 0 && idx < static_cast<int>(m_options.size())) { m_options.erase(m_options.begin() + idx); m_rebuild = true; } });
            m_rows.push_back({ label, pairs });
            m_scroll->addChildWidget(std::move(rowU));   // scroll owns the row
        }
        if (m_options.empty())
        {   // the empty-state note WRAPS, and is as tall as its lines
            auto note = std::make_unique<JLabel>(m_graph, "No presets yet — Add one below, name it, then use + Path to pick the fields it sets.", W);
            note->setWordWrap(true);
            note->setBounds({ 0.f, 0.f, W, note->heightFor(W) });
            m_scroll->addChildWidget(std::move(note));
        }
    }

    void applyAndClose() {
        syncFromWidgets();
        PresetOptions po; po.options = m_options;
        if (m_onApply) m_onApply(po.toCompact());
        m_done = true;
    }

    std::function<void(std::string)> m_onApply;
    PickFn m_pickPath;
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
    std::vector<Row> m_rows;
    std::vector<PresetOptions::Option> m_options;
    jf::JLineEdit*   m_focused = nullptr;
    uint32_t m_lastW{ kW }, m_lastH{ kH };
    float    m_ax{ 0 }, m_ay{ 0 };
    bool     m_done{ false }, m_drag{ false }, m_rebuild{ false };
};
