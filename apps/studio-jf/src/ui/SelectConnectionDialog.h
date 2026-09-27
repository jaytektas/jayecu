#pragma once

// SelectConnectionDialog — the wiring widget's "Assign" target: a modal that lists the board resources
// the bound field may take (its capability-gated PickerSet), each drawn as a wiring row — resource name,
// its wire (colour code + drawn wire), a reverse-video connector-pin badge in the shell colour, and the
// holder if another element already has it. Pick one + OK writes the firmware pool value to the field.
//
// FIRST CUT: the resource list + OK/Cancel + value write. The reference's connector diagram, the search
// box, the Devices / Connection-Types filters and Show Assigned/Unassigned are follow-ups that layer onto
// the same row data (the PickerSet is already capability-scoped, so the list is correct without them).

#include <j/core/JStyle.h>
#include <j/core/JTitleBar.h>
#include <j/core/JCloseButton.h>
#include <j/core/JTextHelper.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/JLineEdit.h>
#include <j/core/JButton.h>
#include <j/core/FocusManager.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include "../model/Cache.h"
#include "../model/MetaModel.h"
#include "../surface/widgets/EnumPickerWidget.h"   // claimedPoolPins()
#include "../surface/widgets/WireColors.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

class SelectConnectionDialog {
public:
    static constexpr uint32_t kW = 760, kH = 560;
    static constexpr float kBtnW = 90.f, kPad = 12.f, kRowH = 30.f;
    static float kBtnH()   { return jf::JStyle::current().buttonHeight; }
    static float kHeader() { return jf::JStyle::current().titleBarHeight; }
    static jf::JRect _closeRect() { return jf::JCloseButton::rectFor({0.f, 0.f, float(kW), kHeader()}); }
    static bool _in(const jf::JRect& r, float mx, float my) {
        return mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height;
    }

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    SelectConnectionDialog(std::string bind, jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : m_bind(std::move(bind))
        , m_window(std::make_unique<PlatformWinType>("Select Connection", kW, kH, sx, sy,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        _buildRows();
        m_search = std::make_unique<jf::JLineEdit>(m_graph, "Search\xE2\x80\xA6");
        m_search->setFocused(true);
        static const char* kFilters[3] = { "All", "Unassigned", "Assigned" };
        for (int i = 0; i < 3; ++i) {
            m_fbtn[i] = std::make_unique<jf::JButton>(m_graph, kFilters[i]);
            m_fbtn[i]->onClicked.connect([this, i] { m_filter = i; _refilter(); });
        }
        _refilter();
        m_box = std::make_unique<jf::JDialogButtonBox>(m_graph);
        m_box->addButton("Cancel", jf::JDialogButtonBox::Role::Reject, kBtnW);
        m_box->addButton("OK",     jf::JDialogButtonBox::Role::Accept, kBtnW);
        m_box->onReject.connect([this] { m_done = true; });
        m_box->onAccept.connect([this] { _accept(); m_done = true; });
    }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress();
        const bool held    = m_window->isLeftButtonDown();

        const JRect closeR = _closeRect();
        if (pressed && _in(closeR, mx, my)) return false;
        const bool inTitle = (my >= 0.f && my < kHeader() && mx < closeR.x);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        // Layout: filter bar (search + All/Unassigned/Assigned) under the header, list below it, footer bottom.
        m_box->setBounds({ kPad, float(kH) - kPad - kBtnH(), float(kW) - 2.f * kPad, kBtnH() });
        const float fbY = kHeader() + kPad, fbH = kBtnH();
        m_search->setBounds({ kPad, fbY, float(kW) * 0.42f, fbH });
        const float bw2 = 96.f, gap = 6.f, bstart = float(kW) - kPad - 3.f * bw2 - 2.f * gap;
        for (int i = 0; i < 3; ++i) m_fbtn[i]->setBounds({ bstart + i * (bw2 + gap), fbY, bw2, fbH });
        const float listY = fbY + fbH + kPad, listH = float(kH) - listY - (kBtnH() + 2.f * kPad);
        const int rows = int(m_view.size());
        const float maxScroll = std::max(0.f, rows * kRowH - listH);

        // Wheel scroll; then keys — footer (Return/Escape) first, list nav, else the search box.
        m_scroll = std::clamp(m_scroll - m_window->consumeWheel() * kRowH * 3.f, 0.f, maxScroll);
        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            if (m_box->handleKeyEvent(ke)) { if (m_done) return false; continue; }
            else if (ke.key == JKeyEvent::JKey::Down && rows) { m_sel = std::min(m_sel + 1, rows - 1); _sync(listH); }
            else if (ke.key == JKeyEvent::JKey::Up && rows)   { m_sel = std::max(m_sel - 1, 0);        _sync(listH); }
            else m_search->handleKeyEvent(ke);
        }
        if (m_search->text() != m_lastSearch) { m_lastSearch = m_search->text(); _refilter(); }

        // Mouse: search + filter buttons + list rows (double-click accepts) + footer.
        m_search->handleMouseMove(mx, my);
        for (auto& b : m_fbtn) b->handleMouseMove(mx, my);
        m_box->handleMouseMove(mx, my);
        if (pressed) {
            m_search->handleMousePress(mx, my);
            for (auto& b : m_fbtn) b->handleMousePress(mx, my);
            if (my >= listY && my < listY + listH) {
                const int i = int((my - listY + m_scroll) / kRowH);
                if (i >= 0 && i < rows && !_blocked(m_rows[m_view[i]])) {
                    if (i == m_sel && m_lastClickI == i) { _accept(); return false; }
                    m_sel = i; m_lastClickI = i; m_selValue = m_rows[m_view[i]].value;
                }
            } else m_box->handleMousePress(mx, my);
        }
        if (m_window->consumeRelease()) {
            m_search->handleMouseRelease(mx, my);
            for (auto& b : m_fbtn) b->handleMouseRelease(mx, my);
            m_box->handleMouseRelease(mx, my);
        }
        if (m_done) return false;

        _render(buf, mx, my, listY, listH);
        auto frame = hal.beginFrame(m_surface); hal.drawPrimitives(buf); hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    struct Row { std::string label, pin, color, shell, holder, holderPath; int value = 0; bool fixed = false;
                 bool taken = false, shareable = false, isPin = true; };
    // A pin someone else holds and will not share. Shown with its holder, never committed: picking it
    // silently created the conflict the firmware then rejects (P1650), leaving the loser publishing
    // nothing. The enum picker greys exactly this case; this dialog drew the warning and wrote anyway.
    // A FIXED pin can never be picked, whoever holds it. The board declares it `assignable: false`
    // because the HAL consumes it — the battery sense on AV12 — so it is listed for recognition and
    // refused for selection. Distinct from `taken`: taken can be shared or moved, fixed cannot.
    static bool _blocked(const Row& r) { return r.fixed || (r.taken && !r.shareable); }

    // Selection is a VIEW index (into m_view, the filtered rows); track the chosen row's VALUE so the
    // highlight survives a re-filter, and scroll it into view.
    void _sync(float listH) {
        if (m_sel >= 0 && m_sel < int(m_view.size())) m_selValue = m_rows[m_view[m_sel]].value;
        const float top = m_sel * kRowH, bot = top + kRowH;
        if (top < m_scroll) m_scroll = top;
        else if (bot > m_scroll + listH) m_scroll = bot - listH;
    }

    // Rebuild the filtered view from the search text + the All/Unassigned/Assigned filter, keeping the
    // selection on the same resource value if it survives the filter.
    void _refilter() {
        std::string q = m_search ? m_search->text() : std::string();
        for (auto& c : q) c = char(std::tolower((unsigned char)c));
        m_view.clear();
        for (int i = 0; i < int(m_rows.size()); ++i) {
            const Row& r = m_rows[i];
            if (m_filter == 1 && r.taken)  continue;   // Unassigned only
            if (m_filter == 2 && !r.taken) continue;   // Assigned only
            if (!q.empty()) {
                std::string lbl = r.label;
                for (auto& c : lbl) c = char(std::tolower((unsigned char)c));
                if (lbl.find(q) == std::string::npos) continue;
            }
            m_view.push_back(i);
        }
        m_sel = 0;
        for (int k = 0; k < int(m_view.size()); ++k)
            if (m_rows[m_view[k]].value == m_selValue) { m_sel = k; break; }
        m_scroll = 0.f;
    }

    void _buildRows() {
        const MetaModel* meta = Cache::instance().meta();
        if (!meta) return;
        std::vector<MetaModel::PickerSet> sets; std::string by;
        if (!meta->fieldPicker(m_bind, by, sets) || sets.empty()) return;
        const MetaModel::PickerSet* ps = &sets.front();
        if (!by.empty()) {                                  // interface-gated: pick the set for the sibling's value
            const auto dot = m_bind.rfind('.');
            const std::string sib = (dot == std::string::npos) ? m_bind : m_bind.substr(0, dot + 1) + by;
            const int gate = int(std::lround(Cache::instance().configValue(sib)));
            ps = nullptr;
            for (const auto& s : sets)
                if (std::find(s.ifaces.begin(), s.ifaces.end(), gate) != s.ifaces.end()) { ps = &s; break; }
            if (!ps) return;
        }
        m_selValue = int(std::lround(Cache::instance().configValue(m_bind)));
        const auto claimed = EnumPickerWidget::claimedPoolPins(m_bind);
        for (size_t i = 0; i < ps->options.size(); ++i) {
            Row row;
            row.label = ps->options[i];
            row.value = (i < ps->values.size()) ? ps->values[i] : int(i);
            row.isPin = (i >= ps->pins.size()) || ps->pins[i] != 0;
            row.fixed = (i < ps->fixed.size()) && ps->fixed[i] != 0;
            if (row.fixed) row.holder = "fixed \u2014 board hardware";
            std::string pin, col;
            if (meta->wiringTrim(row.label, pin, col)) {
                row.pin = pin; row.color = col;
                const auto dash = pin.find('-');
                if (dash != std::string::npos) {
                    auto it = meta->connectors().find(pin.substr(0, dash));
                    if (it != meta->connectors().end()) row.shell = it->second.color;
                }
            }
            if (auto it = claimed.find(row.label); it != claimed.end()) {
                row.taken = true; row.holder = it->second.holder; row.shareable = it->second.shareable;
                row.holderPath = it->second.path;
            }
            m_rows.push_back(std::move(row));
        }
    }

    void _accept() {
        if (m_sel < 0 || m_sel >= int(m_view.size())) return;
        const Row& row = m_rows[m_view[m_sel]];
        if (_blocked(row)) return;      // held by someone who will not share — see _blocked()
        Cache::instance().setConfigValue(m_bind, double(row.value));
    }

    void _render(jf::JPrimitiveBuffer& buf, float mx, float my, float listY, float listH) {
        using namespace jf;
        buf.clear();
        const float W = float(kW), H = float(kH), lh = JTextHelper::lineHeight();
        const float rc = JStyle::current().cornerRadius;
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface1, rc, 1.f, Colors::Border);
        JTitleBar::draw(buf, 0.f, 0.f, W, kHeader(), "Select Connection", rc, 1, 0.f, _closeRect().width + 14.f);
        const JRect cr = _closeRect();
        JCloseButton::draw(buf, cr, _in(cr, mx, my));

        // The filter bar: search box + All/Unassigned/Assigned (the active one underlined in the accent).
        m_search->populateRenderPrimitives(buf);
        for (int i = 0; i < 3; ++i) {
            m_fbtn[i]->populateRenderPrimitives(buf);
            if (i == m_filter) {
                const JRect b = m_fbtn[i]->bounds();
                buf.pushRectangle(b.x + 4.f, b.y + b.height - 2.f, b.width - 8.f, 2.f, Colors::Accent, 0.f);
            }
        }

        const float lx = kPad, lw = W - 2.f * kPad;
        buf.pushRectangle(lx, listY, lw, listH, Colors::Surface0, 4.f, 1.f, Colors::Border);
        buf.pushClip(lx, listY, lw, listH);
        for (int k = 0; k < int(m_view.size()); ++k) {
            const float ry = listY + k * kRowH - m_scroll;
            if (ry + kRowH < listY || ry > listY + listH) continue;   // off-screen
            const Row& row = m_rows[m_view[k]];
            const float cy = ry + kRowH * 0.5f;
            static const uint8_t kSelFg[4] = { 255, 255, 255, 255 };
            if (k == m_sel) buf.pushRectangle(lx, ry, lw, kRowH, Colors::Accent, 0.f);
            const uint8_t* fg = (k == m_sel) ? kSelFg : Colors::TextPrimary;
            // resource name — dimmed when another element holds the pin and will not share it, so the
            // row reads as unavailable rather than as a choice that quietly does nothing.
            JTextHelper::pushText(buf, lx + 10.f, cy - lh * 0.5f, row.label,
                                  _blocked(row) ? Colors::TextSecondary : fg);
            // wire: a bar of insulation in its colour (no terminal stub), then the [code]
            float x = lx + 160.f;
            if (!row.color.empty()) {
                wirecol::drawWire(buf, x, cy, 34.f, row.color);
                x += 34.f + 8.f;
                JTextHelper::pushText(buf, x, cy - lh * 0.5f, "[" + row.color + "]", fg);
                x += 62.f;
            }
            // reverse-video connector-pin badge in the shell colour
            if (!row.pin.empty()) {
                const wirecol::RGBA sh = row.shell.empty() ? wirecol::RGBA{0x60,0x60,0x60,255} : wirecol::shell(row.shell);
                const std::string shown = wirecol::terminal(row.pin, row.shell);
                const float pw = JTextHelper::measureWidth(shown), pad = 5.f;
                static const wirecol::RGBA kB{0x90,0x90,0x90,255};
                buf.pushRectangle(x, cy - lh * 0.5f - 2.f, pw + pad * 2.f, lh + 4.f, sh.data(), 3.f, 1.f, kB.data());
                JTextHelper::pushText(buf, x + pad, cy - lh * 0.5f, shown, wirecol::ink(sh).data());
                x += pw + pad * 2.f + 12.f;
            }
            // holder (another element already has this pin)
            if (row.taken)
                JTextHelper::pushText(buf, lx + lw - 220.f, cy - lh * 0.5f,
                                      "\xE2\x80\x94 " + row.holder, (k == m_sel) ? fg : Colors::TextSecondary);
        }
        buf.popClip();
        if (m_view.empty() && JTextHelper::hasAtlas())
            JTextHelper::pushText(buf, lx + 12.f, listY + 12.f,
                                  m_rows.empty() ? "No connections for this field." : "No matches.",
                                  Colors::TextSecondary);
        if (m_box) m_box->populateRenderPrimitives(buf);
    }

    std::string m_bind;
    std::vector<Row> m_rows;      // all resources for the field
    std::vector<int> m_view;      // indices into m_rows passing the search + filter
    int   m_sel{0}, m_lastClickI{-1};
    int   m_filter{0};            // 0=All 1=Unassigned 2=Assigned
    int   m_selValue{INT_MIN};    // the selected resource's value (survives a re-filter)
    std::string m_lastSearch;
    float m_scroll{0.f};
    bool  m_done{false}, m_drag{false};
    float m_ax{0}, m_ay{0};
    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{0};
    jf::JSceneGraph  m_graph;
    std::unique_ptr<jf::JLineEdit> m_search;
    std::unique_ptr<jf::JButton>   m_fbtn[3];
    std::unique_ptr<jf::JDialogButtonBox> m_box;
};
