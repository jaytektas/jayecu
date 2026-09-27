#pragma once

// ChannelPropsDialog — the channels of ONE control, and what each of them looks like everywhere.
//
// Reached from a live control's own context menu ("Properties…"), so it is about the channels in front of
// you rather than the whole catalogue. One row per channel: the unit it is read in, the scale it is
// plotted against, and the two bands that turn it amber and then red.
//
// The thresholds are the CHANNEL's, not this control's — set oil pressure's warning here and the watch
// list, the gauge and the trace all obey it. That is the point of putting them in one place, and it is
// why the dialog says so at the bottom rather than leaving it to be discovered.
//
// Blank means UNSET, and unset is not zero: a channel with no warning band paints nothing amber. Clearing
// a field is how you take a threshold off again.

#include <j/core/JStyle.h>
#include <j/core/JTitleBar.h>
#include <j/core/JCloseButton.h>
#include <j/core/JTextHelper.h>
#include <j/core/JLineEdit.h>
#include <j/core/JComboBox.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/JScrollBar.h>
#include <j/core/FocusManager.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#include "../model/ChannelPrefs.h"
#include "../model/UnitManager.h"   // the pretty label for a unit id ("C" -> "°C")

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "WrapText.h"

class ChannelPropsDialog {
public:
    // One row's fixed facts: the channel and how to name it. Units offered come from the caller (the meta
    // knows which units a channel can be read in); an empty list leaves the row's unit as plain text.
    struct Row {
        std::string channel, label, unit;
        std::vector<std::string> units;    // choices for this channel, first = its own
    };

    static constexpr int   kCols = 7;   // Unit, Min, Max, Warn Low, Warn High, Alarm Low, Alarm High
    // JAppWindow::openModal centres by these, so they have to be constants known without a row count.
    // Width IS constant (the columns are fixed); the height here is the nominal one it centres against,
    // while the window itself is built to the list it actually holds.
    static constexpr uint32_t kW = static_cast<uint32_t>(2 * 12 + 250 + 7 * (96 + 8));
    static constexpr uint32_t kH = 460;
    static constexpr float kPad = 12.f, kRowH = 30.f, kNameW = 250.f, kColW = 96.f, kGap = 8.f;
    // The note under the list WRAPS, and its height is the wrapped text's — the list gives up the room.
    static constexpr const char* kNote = "Warning and alarm bands belong to the CHANNEL \xE2\x80\x94 every gauge, list and "
                                         "trace reading it uses them. Blank = not set.";
    static float kDescH() { return 26.f + wraptext::extra(kNote, static_cast<float>(kW) - 2.f * kPad); }
    static float kBtnH()   { return jf::JStyle::current().buttonHeight; }
    static float kHeader() { return jf::JStyle::current().titleBarHeight; }

    // THE WINDOW IS ITS CONTENT. A fixed 820x520 was narrower than the columns it had to hold (the last
    // one, Alarm High, was simply off the right edge) and taller than any list that fits on screen — and a
    // borderless modal has no frame to drag, so "make it bigger" is not something the user can do. Both
    // numbers come from what is actually being laid out instead.
    static uint32_t _widthFor() { return kW; }
    static uint32_t _heightFor(size_t rows) {
        const float chrome = kHeader() + kPad + 22.f      // title bar + the column captions
                           + kDescH() + kBtnH() + 2.f * kPad + 12.f;   // note + footer
        const float want = chrome + static_cast<float>(rows) * kRowH;
        // Capped, because a watch list can hold thirty channels and a window taller than the screen helps
        // nobody — past the cap the list scrolls, which it already knows how to do.
        return static_cast<uint32_t>(std::clamp(want, chrome + 3.f * kRowH, 860.f));
    }
    jf::JRect _closeRect() const { return jf::JCloseButton::rectFor({0.f, 0.f, static_cast<float>(m_w), kHeader()}); }
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

    ChannelPropsDialog(std::string title, std::vector<Row> rows, std::function<void()> onAccept,
                       jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : m_title(std::move(title)), m_rows(std::move(rows)), m_onAccept(std::move(onAccept))
        , m_w(_widthFor()), m_h(_heightFor(m_rows.size()))
        , m_window(std::make_unique<PlatformWinType>("Properties", m_w, m_h, sx, sy, jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), m_w, m_h)) {
        using namespace jf;
        for (const Row& r : m_rows) {
            Edit e;
            const ChannelPref* p = ChannelPrefs::instance().find(r.channel);
            if (!r.units.empty()) {
                std::vector<std::string> labels;
                for (const std::string& u : r.units) labels.push_back(UnitManager::instance().unitLabel(u));
                e.unit = std::make_unique<JComboBox>(m_graph, labels);
                const std::string want = (p && !p->unit.empty()) ? p->unit : r.unit;
                for (size_t i = 0; i < r.units.size(); ++i) if (r.units[i] == want) e.unit->setCurrentIndex(static_cast<int>(i));
            }
            auto num = [&](double v) {
                auto le = std::make_unique<JLineEdit>(m_graph, "");
                if (ChannelPref::has(v)) le->setText(_fmt(v));
                return le;
            };
            e.min    = num(p ? p->min : NAN);
            e.max    = num(p ? p->max : NAN);
            e.warnLo = num(p ? p->warnLo : NAN);
            e.warnHi = num(p ? p->warnHi : NAN);
            e.alarmLo = num(p ? p->alarmLo : NAN);
            e.alarmHi = num(p ? p->alarmHi : NAN);
            m_edits.push_back(std::move(e));
        }
        m_box = std::make_unique<JDialogButtonBox>(m_graph);
        m_box->addButton("Cancel", JDialogButtonBox::Role::Reject, 84.f);
        m_box->addButton("OK",     JDialogButtonBox::Role::Accept, 84.f);
        m_box->onReject.connect([this] { m_done = true; });
        m_box->onAccept.connect([this] { _accept(); m_done = true; });
    }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease(),
                   held = m_window->isLeftButtonDown();

        const JRect closeR = _closeRect();
        if (pressed && _inRect(closeR, mx, my)) return false;
        const bool inTitle = (my >= 0.f && my < kHeader() && mx < closeR.x);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            _refreshFocusRoots();
            if (jRouteKey(ke, m_focus)) continue;
            if (m_box->handleKeyEvent(ke)) { if (m_done) return false; continue; }
        }
        // A long list scrolls rather than growing the window off the screen.
        if (const float wheel = m_window->consumeWheel(); wheel != 0.f) {
            const float maxScroll = std::max(0.f, static_cast<float>(m_rows.size()) * kRowH - _listH());
            m_scroll = std::clamp(m_scroll - wheel * kRowH, 0.f, maxScroll);
        }

        // WHERE THIS WINDOW IS, EVERY FRAME. A popup opened from a control in here (the unit combo's
        // dropdown) anchors to its scene graph's host window; unset, it falls back to the MAIN window and
        // the dialog-relative coordinates land it in the top-left corner of the app. Declared per frame
        // because the dialog can be dragged while its combo is open.
        m_graph.setHostWindow(m_window->screenX(), m_window->screenY(),
                              static_cast<std::uintptr_t>(m_window->rawWindowId()));

        _layout();
        _refreshFocusRoots();
        if (pressed) jRouteMouse(mx, my, m_focus);
        if (!m_focusSeeded) { m_focusSeeded = true; m_focus.focusFirst(); }
        for (JWidget* w : _controls()) {
            w->handleMouseMove(mx, my);
            if (pressed)  w->handleMousePress(mx, my);
            if (released) w->handleMouseRelease(mx, my);
        }
        if (m_done) return false;

        _render(buf, mx, my);
        auto frame = hal.beginFrame(m_surface); hal.drawPrimitives(buf); hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    struct Edit {
        std::unique_ptr<jf::JComboBox> unit;
        std::unique_ptr<jf::JLineEdit> min, max, warnLo, warnHi, alarmLo, alarmHi;
    };

    static std::string _fmt(double v) {
        char b[32];
        std::snprintf(b, sizeof b, (v == std::floor(v) && std::fabs(v) < 1e9) ? "%.0f" : "%g", v);
        return b;
    }
    // Blank stays UNSET. Nonsense stays unset too, rather than becoming 0 and painting the whole channel
    // amber — a threshold you cannot see is worse than one you have not set.
    static double _num(const jf::JLineEdit* le) {
        if (!le) return NAN;
        const std::string t = le->text();
        if (t.empty()) return NAN;
        try { return std::stod(t); } catch (...) { return NAN; }
    }

    float _listY() const { return kHeader() + kPad + 22.f; }
    float _listH() const { return static_cast<float>(m_h) - _listY() - (kBtnH() + 2.f * kPad) - kDescH(); }

    std::vector<jf::JWidget*> _controls() {
        std::vector<jf::JWidget*> v;
        for (auto& e : m_edits) {
            if (e.unit) v.push_back(e.unit.get());
            for (jf::JLineEdit* le : { e.min.get(), e.max.get(), e.warnLo.get(), e.warnHi.get(),
                                       e.alarmLo.get(), e.alarmHi.get() }) v.push_back(le);
        }
        v.push_back(m_box.get());
        return v;
    }
    void _refreshFocusRoots() { m_focus.setFocusRoots(_controls()); }

    void _accept() {
        for (size_t i = 0; i < m_rows.size(); ++i) {
            ChannelPref p;
            const Edit& e = m_edits[i];
            if (e.unit && e.unit->currentIndex() > 0) p.unit = m_rows[i].units[static_cast<size_t>(e.unit->currentIndex())];
            p.min = _num(e.min.get());       p.max = _num(e.max.get());
            p.warnLo = _num(e.warnLo.get()); p.warnHi = _num(e.warnHi.get());
            p.alarmLo = _num(e.alarmLo.get()); p.alarmHi = _num(e.alarmHi.get());
            ChannelPrefs::instance().set(m_rows[i].channel, p);
        }
        if (m_onAccept) m_onAccept();
    }

    // Column x positions, left to right: name, unit, min, max, warn lo, warn hi, alarm lo, alarm hi.
    float _colX(int c) const { return kPad + kNameW + static_cast<float>(c) * (kColW + kGap); }

    void _layout() {
        const float y0 = _listY();
        for (size_t i = 0; i < m_edits.size(); ++i) {
            const float ry = y0 + static_cast<float>(i) * kRowH - m_scroll;
            Edit& e = m_edits[i];
            const bool vis = (ry >= y0 - kRowH) && (ry <= y0 + _listH());
            auto place = [&](jf::JWidget* w, int col) {
                if (!w) return;
                w->setVisible(vis);
                if (vis) w->setBounds({ _colX(col), ry + 2.f, kColW, kRowH - 6.f });
            };
            place(e.unit.get(), 0);
            place(e.min.get(), 1);    place(e.max.get(), 2);
            place(e.warnLo.get(), 3); place(e.warnHi.get(), 4);
            place(e.alarmLo.get(), 5); place(e.alarmHi.get(), 6);
        }
        m_box->setBounds({ kPad, static_cast<float>(m_h) - kPad - kBtnH(),
                           static_cast<float>(m_w) - 2.f * kPad, kBtnH() });
    }

    void _render(jf::JPrimitiveBuffer& buf, float mx, float my) {
        using namespace jf;
        buf.clear();
        const float W = static_cast<float>(m_w), H = static_cast<float>(m_h);
        const float r = JStyle::current().cornerRadius, lh = JTextHelper::lineHeight();
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface1, r, 1.f, Colors::Border);
        JTitleBar::draw(buf, 0.f, 0.f, W, kHeader(), m_title, r, 1, 0.f, _closeRect().width + 14.f);
        const JRect cr = _closeRect();
        JCloseButton::draw(buf, cr, _inRect(cr, mx, my));

        const char* heads[] = { "Unit", "Min", "Max", "Warn Low", "Warn High", "Alarm Low", "Alarm High" };
        const float hy = _listY() - 20.f;
        if (JTextHelper::hasAtlas()) {
            JTextHelper::pushText(buf, kPad, hy, "Channel", Colors::TextSecondary, kNameW);
            for (int c = 0; c < 7; ++c)
                JTextHelper::pushText(buf, _colX(c), hy, heads[c], Colors::TextSecondary, kColW);
            const float y0 = _listY();
            buf.pushClip(0.f, y0, W, _listH());
            for (size_t i = 0; i < m_rows.size(); ++i) {
                const float ry = y0 + static_cast<float>(i) * kRowH - m_scroll;
                if (ry < y0 - kRowH || ry > y0 + _listH()) continue;
                if (i % 2) buf.pushRectangle(kPad, ry, W - 2.f * kPad, kRowH - 2.f, Colors::Surface2);
                JTextHelper::pushText(buf, kPad + 4.f, ry + (kRowH - lh) * 0.5f, m_rows[i].label,
                                      Colors::TextPrimary, kNameW - 8.f);
            }
            buf.popClip();
            // Say whose thresholds these are: the surprise is not that a threshold exists, it is that it
            // followed you to another gauge.
            wraptext::draw(buf, kPad, H - kBtnH() - 2.f * kPad - kDescH() + 6.f, kNote,
                           Colors::TextSecondary, W - 2.f * kPad);
        }
        for (JWidget* w : _controls()) w->populateRenderPrimitives(buf);
    }

    std::string          m_title;
    std::vector<Row>     m_rows;
    std::function<void()> m_onAccept;
    const uint32_t       m_w, m_h;      // sized to the columns and the row count — see _widthFor
    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId     m_surface{0};

    jf::JSceneGraph      m_graph;
    std::vector<Edit>    m_edits;
    std::unique_ptr<jf::JDialogButtonBox> m_box;
    jf::JFocusManager    m_focus;
    bool  m_focusSeeded = false, m_done = false, m_drag = false;
    float m_ax = 0.f, m_ay = 0.f, m_scroll = 0.f;
};
