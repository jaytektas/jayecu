#pragma once

// ChannelPickerDialog — TWO LISTS: every channel the ECU sends on the left, the ones this control shows on
// the right, and the arrows between them. Cloned from ListPickerDialog for the window/modal shape (WM-managed
// borderless surface, framework title bar, JDialogButtonBox footer); the body is the two-list picker.
//
// It exists because a live control's channel set is the USER's, chosen while tuning, not the author's chosen
// while laying the page out. A trace view watching boost is watching different channels ten minutes later,
// and a picker that only opened in edit mode would mean rebuilding the page to change your mind.
//
// The right list is ORDERED and the order is the answer: it is the order the strip reads in and the order
// the traces are coloured in (LineGraphModel::colorFor is by index), so Up/Down are part of the choice, not
// a convenience. Which is also why the right side is a list and not a column of ticks in the left one.

#include <j/core/JStyle.h>
#include <j/core/JTitleBar.h>
#include <j/core/JCloseButton.h>
#include <j/core/JTextHelper.h>
#include <j/core/JLineEdit.h>
#include <j/core/JButton.h>
#include <j/core/JDataGrid.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/FocusManager.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "WrapText.h"

class ChannelPickerDialog {
public:
    // One offerable channel. `key` is what gets stored on the widget; the rest is what the columns show.
    struct Chan {
        std::string key;      // "rpm" — the binding, and the Short Name column
        std::string label;    // "Engine speed" — the Name column
        std::string group;    // "Sensors" — the Group column
        std::string units;    // "RPM"
    };

    static constexpr uint32_t kW = 900, kH = 560;
    static constexpr float kBtnW = 84.f, kPad = 12.f, kRowH = 24.f, kArrowW = 96.f, kDescH = 46.f;
    static float kBtnH()   { return jf::JStyle::current().buttonHeight; }
    static float kHeader() { return jf::JStyle::current().titleBarHeight; }
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

    // (title, catalogue, current, onAccept) lead; the hal/pos/handle tail is supplied by JAppWindow::openModal.
    // onAccept carries the chosen keys IN ORDER. Cancel calls nothing — the control keeps what it had.
    ChannelPickerDialog(std::string title, std::vector<Chan> all, std::vector<std::string> current,
                        std::function<void(std::vector<std::string>)> onAccept,
                        jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : m_title(std::move(title)), m_all(std::move(all)), m_onAccept(std::move(onAccept))
        , m_window(std::make_unique<PlatformWinType>("Channels", kW, kH, sx, sy, jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        using namespace jf;
        std::sort(m_all.begin(), m_all.end(), [](const Chan& a, const Chan& b) {
            return a.group != b.group ? a.group < b.group : a.label < b.label;
        });
        // A channel already chosen but NOT in the catalogue still belongs on the right — a page authored
        // against a module that is currently disabled must come back with its list intact, not quietly
        // shortened to whatever the ECU happens to be sending today.
        for (const std::string& k : current) m_sel.push_back(_chanFor(k));

        const std::vector<std::string> cols = { "Name", "Group", "Short Name", "Units" };
        m_search = std::make_unique<JLineEdit>(m_graph, "Filter\xE2\x80\xA6");
        m_search->setClearButtonEnabled(true);
        m_avail = std::make_unique<JDataGrid>(m_graph, cols);
        m_pick  = std::make_unique<JDataGrid>(m_graph, cols);
        const std::vector<float> w = { 200.f, 110.f, 110.f, 60.f };
        m_avail->setColumnWidths(w); m_pick->setColumnWidths(w);
        // A CLICK SELECTS; the buttons move. JDataGrid emits onRowActivated on every single click — it has
        // no double-click of its own — so wiring the move straight to it meant a channel jumped across the
        // moment you touched it and the Add button had nothing left to do. Two clicks on the SAME row
        // inside the double-click window still move it, which is the shortcut anyone tries first.
        //
        // MULTI-SELECT lives here rather than in the grid: JDataGrid keeps one selected row, so Ctrl and
        // Shift are read off the window and the set is kept alongside, painted through the grid's row
        // tints. Adding fifteen channels one at a time is not a picker, it is a chore.
        m_avail->onRowActivated.connect([this](int r) { _click(0, r); });
        m_pick->onRowActivated.connect([this](int r) { _click(1, r); });

        m_add    = std::make_unique<JButton>(m_graph, "Add \xE2\x86\x92");
        m_rem    = std::make_unique<JButton>(m_graph, "\xE2\x86\x90 Remove");
        m_up     = std::make_unique<JButton>(m_graph, "Up");
        m_down   = std::make_unique<JButton>(m_graph, "Down");
        m_add->onClicked.connect([this] { _add(); });
        m_rem->onClicked.connect([this] { _remove(); });
        m_up->onClicked.connect([this] { _move(-1); });
        m_down->onClicked.connect([this] { _move(+1); });

        m_box = std::make_unique<JDialogButtonBox>(m_graph);
        m_box->addButton("Cancel", JDialogButtonBox::Role::Reject, kBtnW);
        m_box->addButton("OK",     JDialogButtonBox::Role::Accept, kBtnW);
        m_box->onReject.connect([this] { m_done = true; });
        m_box->onAccept.connect([this] { _accept(); m_done = true; });
        _refill();
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

        const std::string before = m_search->text();
        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            _refreshFocusRoots();
            if (jRouteKey(ke, m_focus)) continue;
            if (m_box->handleKeyEvent(ke)) { if (m_done) return false; continue; }
        }
        if (m_search->text() != before) _refill();     // the filter is live, as a filter should be

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
        // A catalogue is hundreds of rows: the wheel scrolls whichever grid is under the pointer.
        if (const float wheel = m_window->consumeWheel(); wheel != 0.f) {
            if (!m_avail->handleScroll(mx, my, wheel)) m_pick->handleScroll(mx, my, wheel);
        }
        if (m_done) return false;

        _render(buf, mx, my);
        auto frame = hal.beginFrame(m_surface); hal.drawPrimitives(buf); hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    std::vector<jf::JWidget*> _controls() {
        return { m_search.get(), m_avail.get(), m_pick.get(), m_add.get(), m_rem.get(),
                 m_up.get(), m_down.get(), m_box.get() };
    }

    // One click on a row of one grid: plain replaces the selection, Ctrl toggles a row into it, Shift
    // extends from the anchor. The other grid's selection is dropped — two live selections in two lists
    // would leave Add and Remove arguing about which one they act on.
    void _click(int grid, int row) {
        std::set<int>& sel = (grid == 0) ? m_selA : m_selP;
        std::set<int>& other = (grid == 0) ? m_selP : m_selA;
        int& anchor = (grid == 0) ? m_anchorA : m_anchorP;
        other.clear();
        const bool ctrl = m_window->isCtrlDown(), shift = m_window->isShiftDown();
        if (shift && anchor >= 0) {
            sel.clear();
            for (int i = std::min(anchor, row); i <= std::max(anchor, row); ++i) sel.insert(i);
        } else if (ctrl) {
            if (!sel.insert(row).second) sel.erase(row);
            anchor = row;
        } else {
            sel.clear(); sel.insert(row); anchor = row;
        }
        _paintSelection();
        if (!ctrl && !shift && _isDoubleClick(grid, row)) { if (grid == 0) _add(); else _remove(); }
    }

    // The selection the user can SEE. The grid paints its own current row; these tints paint the rest of
    // the set, so a Shift-range does not look like one row and fourteen coincidences.
    void _paintSelection() {
        auto tint = [](const std::set<int>& sel, size_t n) {
            std::vector<jf::JDataGrid::RowTint> t(n, jf::JDataGrid::RowTint{ 0, 0, 0, 0 });
            for (int i : sel) if (i >= 0 && i < static_cast<int>(n))
                t[static_cast<size_t>(i)] = { jf::Colors::Accent[0], jf::Colors::Accent[1],
                                              jf::Colors::Accent[2], 70 };
            return t;
        };
        m_avail->setRowTints(tint(m_selA, m_availKeys.size()));
        m_pick->setRowTints(tint(m_selP, m_sel.size()));
    }

    // Two activations of the same row of the same grid, close enough together to be one gesture.
    bool _isDoubleClick(int grid, int row) {
        const auto now = std::chrono::steady_clock::now();
        const bool same = (grid == m_lastGrid && row == m_lastRow)
                       && (now - m_lastClick) < std::chrono::milliseconds(400);
        m_lastGrid = grid; m_lastRow = row; m_lastClick = now;
        if (same) { m_lastGrid = -1; m_lastRow = -1; }   // a third click starts a new pair, not a second move
        return same;
    }

    Chan _chanFor(const std::string& key) const {
        for (const Chan& c : m_all) if (c.key == key) return c;
        return Chan{ key, key, "(not sent)", "" };      // still listed, and visibly not a live channel
    }

    static std::string _lower(std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }

    // The left list is EVERYTHING MINUS WHAT IS ALREADY CHOSEN, filtered. Leaving a chosen channel on the
    // left invites adding it twice, and a strip with the same channel in it twice is a bug the user made
    // by hand.
    void _refill() {
        const std::string q = _lower(m_search->text());
        m_availKeys.clear();
        std::vector<std::vector<std::string>> rows;
        for (const Chan& c : m_all) {
            if (std::any_of(m_sel.begin(), m_sel.end(), [&](const Chan& s) { return s.key == c.key; })) continue;
            if (!q.empty() && _lower(c.label).find(q) == std::string::npos
                           && _lower(c.key).find(q) == std::string::npos
                           && _lower(c.group).find(q) == std::string::npos) continue;
            m_availKeys.push_back(c.key);
            rows.push_back({ c.label, c.group, c.key, c.units });
        }
        m_avail->setRows(rows);
        for (int i : std::set<int>(m_selA)) if (i >= static_cast<int>(rows.size())) m_selA.erase(i);
        std::vector<std::vector<std::string>> prows;
        for (const Chan& c : m_sel) prows.push_back({ c.label, c.group, c.key, c.units });
        m_pick->setRows(prows);
    }

    // Everything selected on the left, in the order the left list shows it — so adding a Shift-range of
    // eight lands in the order you read them, not in click order.
    void _add() {
        std::vector<int> rows(m_selA.begin(), m_selA.end());
        if (rows.empty() && m_avail->selectedIndex() >= 0) rows.push_back(m_avail->selectedIndex());
        for (int i : rows)
            if (i >= 0 && i < static_cast<int>(m_availKeys.size()))
                m_sel.push_back(_chanFor(m_availKeys[static_cast<size_t>(i)]));
        const int first = rows.empty() ? -1 : rows.front();
        m_selA.clear(); m_anchorA = -1;
        _refill();
        if (first >= 0) m_avail->setSelectedIndex(std::min(first, static_cast<int>(m_availKeys.size()) - 1));
        _paintSelection();
    }
    // Removed back to front, so each erase cannot shift the rows still to go.
    void _remove() {
        std::vector<int> rows(m_selP.begin(), m_selP.end());
        if (rows.empty() && m_pick->selectedIndex() >= 0) rows.push_back(m_pick->selectedIndex());
        std::sort(rows.rbegin(), rows.rend());
        for (int i : rows)
            if (i >= 0 && i < static_cast<int>(m_sel.size())) m_sel.erase(m_sel.begin() + i);
        const int first = rows.empty() ? -1 : rows.back();
        m_selP.clear(); m_anchorP = -1;
        _refill();
        if (first >= 0 && !m_sel.empty())
            m_pick->setSelectedIndex(std::min(first, static_cast<int>(m_sel.size()) - 1));
        _paintSelection();
    }
    // Up/Down move the whole selected block, keeping it together and keeping it selected — the order of
    // the right-hand list is the read order and the trace-colour order, so moving three at once is the
    // point rather than a nicety.
    void _move(int d) {
        std::vector<int> rows(m_selP.begin(), m_selP.end());
        if (rows.empty() && m_pick->selectedIndex() >= 0) rows.push_back(m_pick->selectedIndex());
        if (rows.empty()) return;
        std::sort(rows.begin(), rows.end());
        if (d < 0 ? rows.front() == 0 : rows.back() == static_cast<int>(m_sel.size()) - 1) return;
        if (d > 0) std::reverse(rows.begin(), rows.end());
        std::set<int> moved;
        for (int i : rows) { std::swap(m_sel[static_cast<size_t>(i)], m_sel[static_cast<size_t>(i + d)]); moved.insert(i + d); }
        m_selP = moved;
        m_anchorP = *moved.begin();
        _refill();
        m_pick->setSelectedIndex(*moved.begin());
        _paintSelection();
    }

    void _accept() {
        if (!m_onAccept) return;
        std::vector<std::string> keys;
        for (const Chan& c : m_sel) keys.push_back(c.key);
        m_onAccept(std::move(keys));
    }

    void _layout() {
        const float W = static_cast<float>(kW), H = static_cast<float>(kH);
        const float footerH = kBtnH() + 2.f * kPad;
        const float top = kHeader() + kPad;
        const float listY = top + kRowH + kPad;
        const float listH = H - listY - footerH - _descH() - kPad;
        const float colW = (W - 2.f * kPad - kArrowW - 2.f * kPad) * 0.5f;
        const float rx = kPad + colW + kArrowW + 2.f * kPad;
        m_search->setBounds({ kPad, top, colW, kRowH });
        m_avail->setBounds({ kPad, listY, colW, listH });
        m_pick->setBounds({ rx, listY, colW, listH });
        const float ax = kPad + colW + kPad, bh = kBtnH();
        m_add->setBounds({ ax, listY + 30.f, kArrowW, bh });
        m_rem->setBounds({ ax, listY + 30.f + bh + 8.f, kArrowW, bh });
        m_up->setBounds({ ax, listY + 30.f + 2.f * (bh + 8.f) + 20.f, kArrowW, bh });
        m_down->setBounds({ ax, listY + 30.f + 3.f * (bh + 8.f) + 20.f, kArrowW, bh });
        m_box->setBounds({ kPad, H - kPad - kBtnH(), W - 2.f * kPad, kBtnH() });
    }

    void _refreshFocusRoots() {
        std::vector<jf::JWidget*> roots = _controls();
        m_focus.setFocusRoots(std::move(roots));
    }

    // The description strip: what the highlighted row IS, in words, under both lists. A short name on its
    // own ("ltft_b1") is not enough to choose by, and the columns have no room for a sentence.
    std::string _description() const {
        const int i = m_pick->selectedIndex();
        if (i >= 0 && i < static_cast<int>(m_sel.size())) return _describe(m_sel[static_cast<size_t>(i)]);
        const int a = m_avail->selectedIndex();
        if (a >= 0 && a < static_cast<int>(m_availKeys.size())) return _describe(_chanFor(m_availKeys[static_cast<size_t>(a)]));
        return {};
    }
    static std::string _describe(const Chan& c) {
        std::string s = c.label + "  \xC2\xB7  " + c.key;
        if (!c.group.empty()) s += "  \xC2\xB7  " + c.group;
        if (!c.units.empty()) s += "  \xC2\xB7  " + c.units;
        return s;
    }

    // The description box grows by the lines its text WRAPS to; the lists above give up the room.
    float _descH() {
        return kDescH + wraptext::extra(_description(), static_cast<float>(kW) - 2.f * kPad - 16.f);
    }

    void _render(jf::JPrimitiveBuffer& buf, float mx, float my) {
        using namespace jf;
        buf.clear();
        const float W = static_cast<float>(kW), H = static_cast<float>(kH);
        const float r = JStyle::current().cornerRadius;
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface1, r, 1.f, Colors::Border);
        JTitleBar::draw(buf, 0.f, 0.f, W, kHeader(), m_title, r, 1, 0.f, _closeRect().width + 14.f);
        const JRect cr = _closeRect();
        JCloseButton::draw(buf, cr, _inRect(cr, mx, my));
        // Column captions, so the two grids are not two anonymous boxes.
        const float capY = kHeader() + kPad + kRowH + kPad - JTextHelper::lineHeight() - 2.f;
        const float colW = (W - 2.f * kPad - kArrowW - 2.f * kPad) * 0.5f;
        JTextHelper::pushText(buf, kPad + colW + kArrowW + 2.f * kPad, capY, "Selected channels", Colors::TextSecondary, colW);
        const std::string d = _description();
        if (!d.empty()) {
            const float dh = _descH(), dy = H - kBtnH() - 2.f * kPad - dh;
            const float th = wraptext::height(d, W - 2.f * kPad - 16.f);
            buf.pushRectangle(kPad, dy, W - 2.f * kPad, dh - 6.f, Colors::Surface2, r);
            wraptext::draw(buf, kPad + 8.f, dy + (dh - 6.f - th) * 0.5f, d, Colors::TextPrimary, W - 2.f * kPad - 16.f);
        }
        for (JWidget* w : _controls()) w->populateRenderPrimitives(buf);
    }

    // Declaration order is INITIALISER order: everything the constructor's init list touches, in the order
    // it touches it, then the graph, then the controls it owns (so they die before the graph does).
    std::string        m_title;
    std::vector<Chan>  m_all;                 // the catalogue, sorted by group then name
    std::function<void(std::vector<std::string>)> m_onAccept;
    std::unique_ptr<PlatformWinType>      m_window;
    jf::GpuSurfaceId                      m_surface{0};

    std::vector<Chan>  m_sel;                 // the answer, in order
    std::vector<std::string> m_availKeys;     // left-list row -> channel key (the filter makes them differ)
    jf::JSceneGraph                       m_graph;
    std::unique_ptr<jf::JLineEdit>        m_search;
    std::unique_ptr<jf::JDataGrid>        m_avail, m_pick;
    std::unique_ptr<jf::JButton>          m_add, m_rem, m_up, m_down;
    std::unique_ptr<jf::JDialogButtonBox> m_box;
    jf::JFocusManager                     m_focus;
    bool  m_focusSeeded = false, m_done = false, m_drag = false;
    float m_ax = 0.f, m_ay = 0.f;
    std::set<int> m_selA, m_selP;          // multi-selection, by row, in each grid
    int   m_anchorA = -1, m_anchorP = -1;  // where a Shift-range measures from
    int   m_lastGrid = -1, m_lastRow = -1;
    std::chrono::steady_clock::time_point m_lastClick{};
};
