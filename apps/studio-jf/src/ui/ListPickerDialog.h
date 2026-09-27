#pragma once

// ListPickerDialog — a reusable WM-managed pick-from-a-list modal, serving the ECU-project flow: New Tune
// (schema list + a name field), Open ECU (device list), and the tune picker (tune-name list). Title + an
// optional name field + a scrollable JListView + Cancel/OK, and a close [x] in the header's right corner.
// onAccept(selectedIndex, nameText) — the caller maps the index back to a path/id. Cloned from
// PopupSignalPicker (keys route to the name field).

#include <j/core/JStyle.h>
#include <j/core/JTitleBar.h>
#include <j/core/JCloseButton.h>
#include <j/core/JTextHelper.h>
#include <j/core/JLineEdit.h>
#include <j/core/JListView.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/FocusManager.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <functional>
#include <memory>
#include <string>
#include <vector>

class ListPickerDialog {
public:
    // A ROW THAT IS ON THE LIST BUT CANNOT BE CHOSEN. Filtering it out instead would answer a
    // different question: "we have no template for that controller", when the truth is "we have one
    // and this bus is not set up for it". The row stays, dimmed, with its reason in the text, so the
    // fix (change the bus rate) is the obvious next move rather than a guess.
    struct Row {
        std::string text;
        bool        enabled = true;
    };

    static constexpr uint32_t kW = 420, kH = 460;
    static constexpr float kBtnW = 84.f, kPad = 12.f, kRowH = 22.f;
    static float kBtnH() { return jf::JStyle::current().buttonHeight; }   // HEIGHT from JStyle (single source of truth)
    // Title-bar height is the framework's ONE canonical value (JStyle::titleBarHeight) — never hardcoded.
    static float kHeader() { return jf::JStyle::current().titleBarHeight; }
    static float kBtnY()   { return (kHeader() - kBtnH()) * 0.5f; }   // Cancel/OK centred in the header
    // Close [x] rect derived by the framework from the header (OK sits just left of it).
    static jf::JRect _closeRect() { return jf::JCloseButton::rectFor({0.f, 0.f, static_cast<float>(kW), kHeader()}); }
    static bool _inRect(const jf::JRect& r, float mx, float my) {
        return mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height;
    }

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    // The same list, with each row saying whether it can be chosen. Index numbering is unchanged —
    // onAccept still gets the row's position in the list the caller passed, disabled rows included —
    // so a caller maps the index back exactly as it did before.
    ListPickerDialog(std::string title, std::vector<Row> rows, bool wantName,
                     std::function<void(int, std::string)> onAccept,
                     jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : ListPickerDialog(std::move(title), _texts(rows), wantName, std::move(onAccept),
                           std::function<void()>{}, std::string{}, hal, sx, sy, parent) {
        std::vector<uint8_t> flags;
        flags.reserve(rows.size());
        for (const Row& r : rows) flags.push_back(r.enabled ? 1 : 0);
        m_list->setEnabledFlags(std::move(flags));
        // Opening ON a disabled row would put the highlight somewhere Return does nothing. The list
        // moves the selection to the first row that can be chosen; if none can, there is no selection
        // and the accept button says so.
        m_list->setSelectedIndex(0);
        m_list->onSelectionChanged.connect([this](int) { _syncAccept(); });
        _syncAccept();
    }

    // (title, items, wantName, onAccept) lead; hal/pos/handle tail from openModal. onAccept(index, name).
    ListPickerDialog(std::string title, std::vector<std::string> items, bool wantName,
                     std::function<void(int, std::string)> onAccept,
                     jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : ListPickerDialog(std::move(title), std::move(items), wantName, std::move(onAccept),
                           std::function<void()>{}, std::string{}, hal, sx, sy, parent) {}

    // The same, plus onDismiss: called when the dialog closes WITHOUT a choice — Cancel, Escape, the
    // close [x], the window manager. A caller whose question has consequences either way (a connection
    // held open behind the prompt, say) needs to hear about the third answer too, and silence is not it.
    ListPickerDialog(std::string title, std::vector<std::string> items, bool wantName,
                     std::function<void(int, std::string)> onAccept, std::function<void()> onDismiss,
                     std::string acceptLabel,
                     jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : m_title(std::move(title)), m_wantName(wantName), m_onAccept(std::move(onAccept))
        , m_onDismiss(std::move(onDismiss)), m_acceptLabel(std::move(acceptLabel))
        , m_window(std::make_unique<PlatformWinType>("Pick", kW, kH, sx, sy, jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        using namespace jf;
        m_list = std::make_unique<JListView>(m_graph, std::move(items));
        if (!m_list->items().empty()) m_list->setSelectedIndex(0);
        if (m_wantName) { m_name = std::make_unique<JLineEdit>(m_graph, "Tune name…"); m_name->setFocused(true); }
        // The accept label names the CONSEQUENCE. "Open" is right for a picker and wrong for a question
        // like "which tune wins" — a dialog whose two rows overwrite opposite things must not confirm with
        // a word that describes neither. The acceptLabel ctor argument lets the caller say what pressing it does.
        m_okLabel = !m_acceptLabel.empty() ? m_acceptLabel : (m_wantName ? "Create" : "Open");
        // Standard footer by ROLE. The accept label stays meaningful ("Create" / "Open") -- the box
        // standardises order, placement, Return/Escape and focus, not the wording. These used to be
        // rectangles drawn in the TITLE BAR that fired on mouse-down: a mis-click on the Pull/Push/Keep
        // prompt committed a tune overwrite with no chance to slide off and cancel.
        m_box = std::make_unique<jf::JDialogButtonBox>(m_graph);
        m_box->addButton("Cancel", jf::JDialogButtonBox::Role::Reject, kBtnW);
        m_accept = m_box->addButton(m_okLabel, jf::JDialogButtonBox::Role::Accept, kBtnW);
        m_box->onReject.connect([this] { m_done = true; });
        m_box->onAccept.connect([this] { _accept(); m_done = true; });
    }

    // Every way out that is not an accept funnels here — there are several (footer, key, [x], WM), and
    // one of them getting missed is exactly how a dismissal turns into silence.
    ~ListPickerDialog() { if (!m_accepted && m_onDismiss) m_onDismiss(); }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease(), held = m_window->isLeftButtonDown();

        const jf::JRect closeR = _closeRect();
        if (pressed && _inRect(closeR, mx, my)) return false;      // close [x] dismisses
        const bool inTitle = (my >= 0.f && my < kHeader() && mx < closeR.x);   // whole bar drags now
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            _refreshFocusRoots();
            if (jf::jRouteKey(ke, m_focus)) continue;                                  // focused control, then Tab
            if (m_box->handleKeyEvent(ke)) { if (m_done) return false; continue; }      // Return=accept, Escape=cancel
            if (m_name) m_name->handleKeyEvent(ke);
        }

        float y = kHeader() + kPad;
        if (m_name) { m_name->setBounds({ kPad, y, static_cast<float>(kW) - 2.f * kPad, kRowH }); y += kRowH + kPad; }
        // The list stops above the footer, which now sits at the bottom where a dialog's buttons belong.
        const float footerH = kBtnH() + 2.f * kPad;
        m_list->setBounds({ kPad, y, static_cast<float>(kW) - 2.f * kPad,
                            static_cast<float>(kH) - y - footerH });
        m_box->setBounds({ kPad, static_cast<float>(kH) - kPad - kBtnH(),
                           static_cast<float>(kW) - 2.f * kPad, kBtnH() });

        _refreshFocusRoots();
        if (pressed) jf::jRouteMouse(mx, my, m_focus);                        // click-to-focus
        if (!m_focusSeeded) { m_focusSeeded = true; m_focus.focusFirst(); }   // keyboard works on open
        if (m_name) m_name->handleMouseMove(mx, my);
        m_list->handleMouseMove(mx, my);
        m_box->handleMouseMove(mx, my);
        if (pressed)  { if (m_name) m_name->handleMousePress(mx, my);   m_list->handleMousePress(mx, my);   m_box->handleMousePress(mx, my); }
        if (released) { if (m_name) m_name->handleMouseRelease(mx, my); m_list->handleMouseRelease(mx, my); m_box->handleMouseRelease(mx, my); }
        if (m_done) return false;

        _render(buf, mx, my);
        auto frame = hal.beginFrame(m_surface); hal.drawPrimitives(buf); hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    void _accept() {
        const int i = m_list->selectedIndex();
        // Nothing selectable, or nothing selected: this is a dismissal, not a choice. Reporting it as
        // an accept with index -1 would make every caller repeat the same bounds check.
        if (i < 0 || !m_list->isItemEnabled(i)) return;
        m_accepted = true;
        if (m_onAccept) m_onAccept(i, m_name ? m_name->text() : std::string());
    }

    // The accept button is dead while there is nothing it could accept.
    void _syncAccept() {
        const int i = m_list->selectedIndex();
        const bool ok = (i >= 0 && m_list->isItemEnabled(i));
        if (m_accept) m_accept->setEnabled(ok);
    }

    static std::vector<std::string> _texts(const std::vector<Row>& rows) {
        std::vector<std::string> out;
        out.reserve(rows.size());
        for (const Row& r : rows) out.push_back(r.text);
        return out;
    }
    // This dialog's focus tree: the optional name field, the list, and the footer.
    void _refreshFocusRoots() {
        std::vector<jf::JWidget*> roots;
        if (m_name) roots.push_back(m_name.get());
        if (m_list) roots.push_back(m_list.get());
        if (m_box)  roots.push_back(m_box.get());
        m_focus.setFocusRoots(std::move(roots));
    }

    void _render(jf::JPrimitiveBuffer& buf, float mx, float my) {
        using namespace jf;
        buf.clear();
        const float W = static_cast<float>(kW), H = static_cast<float>(kH), lh = JTextHelper::lineHeight();
        const float r = JStyle::current().cornerRadius;
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface1, r, 1.f, Colors::Border);
        JTitleBar::draw(buf, 0.f, 0.f, W, kHeader(), m_title, r, 1, 0.f, _closeRect().width + 14.f);   // framework title bar
        if (m_box) m_box->populateRenderPrimitives(buf);   // the footer paints itself
        const JRect cr = _closeRect();
        JCloseButton::draw(buf, cr, _inRect(cr, mx, my));   // framework close control
        if (m_name) m_name->populateRenderPrimitives(buf);
        m_list->populateRenderPrimitives(buf);
    }

    std::unique_ptr<jf::JDialogButtonBox> m_box;               // the standard footer
    jf::JFocusManager                     m_focus;             // this dialog's keyboard focus
    bool                                  m_focusSeeded{false};
    bool                                  m_done{false};       // set by the footer; ends pollAndRender
    std::string m_title, m_okLabel, m_acceptLabel;
    bool        m_wantName;
    std::function<void(int, std::string)> m_onAccept;
    std::function<void()> m_onDismiss;
    jf::JButton* m_accept = nullptr;   // the footer's accept, greyed while nothing can be chosen
    bool m_accepted = false;
    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{0};
    jf::JSceneGraph  m_graph;
    std::unique_ptr<jf::JLineEdit> m_name;
    std::unique_ptr<jf::JListView> m_list;
    bool m_drag{false};
    float m_ax{0}, m_ay{0};
};
