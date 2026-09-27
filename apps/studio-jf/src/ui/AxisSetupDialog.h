#pragma once

// AxisSetupDialog — the table/curve "Table Axis Setup…" modal. For each
// axis a group shows: an optional Enable checkbox, a channel button that opens the searchable signal
// picker (or a value-axis note), Insert /
// Delete / Linearise buttons, a "n (min..max)" count, and a HORIZONTAL STRIP of plain-text breakpoint cells
// (the strip scrolls horizontally). Breakpoint cells are typed directly — NOT combos or spin boxes. Every edit
// writes straight through the live Cache ti* ops (undoable). The framework owns the window (title-bar drag,
// edge-resize) and any popup opened from here (setHostWindow each frame declares this window as their
// parent, so they stack above the dialog rather than behind it); the breakpoint strip is drawn + edited by
// this dialog (there is no horizontally-scrolling list widget in the toolkit).
//
// A breakpoint is bounded AND quantised by the channel the axis reads: its value domain clamps what may
// be typed, and its precision decides how fine a bin can be (app_1 is read to a tenth, so 30.777 is not
// a breakpoint that channel can ever land on). Both come from Cache::channelDomain(), resolved from the
// axis's stored src SignalId — never from the combo's display text, which is a rendering of that id.

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/FocusManager.h>
#include <j/core/JTextHelper.h>
#include <j/core/Log.h>          // JLOGC — an insert the axis refuses says why
#include <j/core/JTitleBar.h>   // the ONE canonical styled title bar — no custom chrome
#include <j/core/JButton.h>
#include <j/core/JCheckBox.h>
#include <j/core/JComboBox.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include "../surface/CanvasWidget.h"   // enumpick::signal() — the picker the run-mode *_src controls use
#include "../model/Cache.h"
#include "../model/UnitManager.h"
#include "../model/MetaModel.h"
#include "../model/TableImage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

class AxisSetupDialog {
public:
    // Nested modals, wired in main.cpp (this dialog has no window to open one from — the same reason
    // Surface::onAxisSetup exists). The callback is invoked with the seed values and receives the
    // answer; both are no-ops if nothing wired them, so the dialog still works standalone.
    static inline std::function<void(double seed, Cache::ChannelDomain, std::function<void(double)>)> onAskValue;
    static inline std::function<void(double start, double end, double inc, Cache::ChannelDomain,
                                     std::function<void(double, double, double)>)> onAskAxis;

    static constexpr uint32_t kW = 660, kH = 470;
    static constexpr float kBtnW = 96.f;
    static float kBtnH() { return jf::JStyle::current().buttonHeight; }   // HEIGHT from JStyle (single source of truth)
    static constexpr float kCellW = 56.f, kStripH = 22.f, kSbH = 9.f;   // strip cell width / height / scrollbar
    static float hdrH()  { return jf::JStyle::current().titleBarHeight; }
    static float rowH()  { return jf::JStyle::current().controlHeight; }
    static float chkH()  { return jf::JStyle::current().checkHeight; }
    static float secH() { return 24.f + rowH() + 8.f + kStripH + kSbH + 14.f; }   // one axis group's height

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    // (path, per-axis display units, onUnitPicked) lead; hal/pos/handle tail from openModal. The callback
    // hands a unit change back to whoever opened the dialog — the widget owns that choice and persists it
    // with the layout; the dialog only asks. Unwired, the change still applies for this session.
    AxisSetupDialog(std::string tablePath, std::vector<std::string> axisUnits, std::string cellUnit,
                    std::function<void(std::string, std::string)> onUnitPicked,
                    jf::JGpuHal& hal, int screenX, int screenY, NativeWinHandleType parent)
        : m_path(std::move(tablePath))
        , m_axisUnits(std::move(axisUnits))
        , m_cellUnit(std::move(cellUnit))
        , m_onUnitPicked(std::move(onUnitPicked))
        , m_window(std::make_unique<PlatformWinType>("Table Axis Setup", kW, kH, screenX, screenY,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        m_btnClose = std::make_unique<jf::JButton>(m_graph, "Close", kBtnW);
        m_btnClose->onClicked.connect([this] { m_done = true; });
        m_window->setResizable(true);
        m_window->setResizeTopInset(hdrH());
        m_window->setMinSize(520, 320);
        rebuild();
    }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;

        // Rebuild the widgets on a structural change (our own insert/delete/enable, or an external undo/redo).
        if (!m_rebuildPending)
            for (const AxisUI& a : m_axes)
                if (Cache::instance().tiLiveN(m_ti, a.axisIdx) != a.liveN) { m_rebuildPending = true; break; }
        if (m_rebuildPending) { rebuild(); m_rebuildPending = false; }

        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease(), held = m_window->isLeftButtonDown();
        const float W = static_cast<float>(m_window->width()), H = static_cast<float>(m_window->height());
        if (m_window->width() != m_lastW || m_window->height() != m_lastH) {
            hal.resizeSurface(m_surface, m_window->width(), m_window->height());
            m_lastW = m_window->width(); m_lastH = m_window->height();
        }

        // Title-bar drag.
        const bool inTitle = (my >= 0.f && my < hdrH() && mx < W - kBtnW - 12.f);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        // Keyboard: edits go to the selected breakpoint cell; Escape cancels an edit, else closes.
        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            if (handleStripKey(ke)) continue;              // strip-local editing wins
            _refreshFocusRoots();
            if (jf::jRouteKey(ke, m_focus)) continue;      // focused control, then Tab / Shift-Tab
            if (ke.key == JKeyEvent::JKey::Escape) return false;
        }

        // Combo dropdowns are framework-serviced against this window.
        m_graph.setHostWindow(m_window->screenX(), m_window->screenY(), static_cast<std::uintptr_t>(m_window->rawWindowId()));

        // Close button, and the cells' unit beside it — the footer is the one row that belongs to the
        // whole table rather than to an axis.
        const float btnY = H - kBtnH() - 12.f;
        m_btnClose->setBounds({ W - kBtnW - 12.f, btnY, kBtnW, kBtnH() });
        if (m_cellUnitCombo)
            m_cellUnitCombo->setBounds({ 12.f + 92.f, btnY + (kBtnH() - rowH()) * 0.5f, 92.f, rowH() });

        // Position each axis group's widgets + handle input.
        layoutAndInput(mx, my, pressed, released, held, W);
        if (m_cellUnitCombo) {
            m_cellUnitCombo->handleMouseMove(mx, my);
            if (pressed)  m_cellUnitCombo->handleMousePress(mx, my);
            if (released) m_cellUnitCombo->handleMouseRelease(mx, my);
        }
        if (pressed) m_btnClose->handleMousePress(mx, my);
        if (released) m_btnClose->handleMouseRelease(mx, my);
        if (m_done) return false;

        render(buf, W, H);
        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    struct AxisUI {
        int  axisIdx = 0, nMin = 2, nMax = 0, digits = 0, liveN = 0;
        bool optional = false, hasSrc = false, resizable = false;
        std::unique_ptr<jf::JCheckBox> enable;
        // The channel chooser. NOT a combo: this definition has 314 selectable signals, and a raw dropdown
        // of them is a wall to scroll even capped at a dozen rows. A button carrying the current channel
        // opens enumpick::signal() — the searchable, module-grouped picker the run-mode *_src controls
        // already use — so finding app_1 is three keystrokes rather than a hunt.
        std::unique_ptr<jf::JButton>   chan;
        // What unit this axis is READ in. A raw analog input is ADC counts in the bytes and always will
        // be, but a sender is specified in volts — so the axis can be shown and typed in any unit of its
        // own quantity. Absent when there is only one way to say the number (an rpm axis is rpm).
        std::unique_ptr<jf::JComboBox> unit;
        std::vector<std::string>       unitIds;   // parallel to the combo's labels
        std::unique_ptr<jf::JButton>   ins, del, lin, wiz;
        std::vector<std::string> sigNames;                // the pickable channels (what the picker lists)
        float scrollX = 0.f;              // horizontal strip scroll (px)
        bool  sbDrag = false;             // dragging the scrollbar thumb
        float sbGrab = 0.f;               // …and where within the thumb it was taken
        float stripX = 0, stripY = 0, stripW = 0;   // last-laid strip rect (for hit-testing)
    };

    // What this axis may hold — Cache::axisDomain is the one answer, and it covers an axis with no
    // channel at all (a calibration's raw ADC input) as well as one that traces a signal. Re-read per
    // use: a generic input's answer depends on tune bytes another dialog can change while this is open.
    Cache::ChannelDomain axisDomain(const AxisUI& a) const {
        return Cache::instance().axisDomain(m_ti, a.axisIdx);
    }
    // …and how it is SHOWN: the unit the grid that opened this dialog is using for that axis, with the
    // domain converted and the precision that unit needs. Everything the tuner reads or types here goes
    // through it, so the dialog cannot disagree with the strip behind it about what a bin says.
    Cache::AxisView axisView(const AxisUI& a) const {
        return Cache::instance().axisView(m_ti, a.axisIdx, unitPrefFor(a.axisIdx));
    }
    std::string unitPrefFor(int axisIdx) const {
        return (axisIdx >= 0 && axisIdx < static_cast<int>(m_axisUnits.size())) ? m_axisUnits[axisIdx]
                                                                                : std::string();
    }
    // Picking a unit changes only how the axis is READ: no byte moves, nothing is rewritten, and there is
    // nothing to undo. It is told to the opener so the widget can persist it with the layout.
    void pickUnit(int ai, int idx) {
        AxisUI& a = m_axes[ai];
        if (idx < 0 || idx >= static_cast<int>(a.unitIds.size())) return;
        const int axisIdx = a.axisIdx;
        if (axisIdx >= static_cast<int>(m_axisUnits.size())) m_axisUnits.resize(axisIdx + 1);
        commitEdit();                      // a half-typed value belongs to the unit it was typed in
        m_axisUnits[axisIdx] = a.unitIds[idx];
        if (m_onUnitPicked) m_onUnitPicked(kAxisUnitProp[std::clamp(axisIdx, 0, 2)], a.unitIds[idx]);
    }

    // THE CELLS' unit — the reading itself. It is the one the tuner is most likely to want changed and
    // the one they could least reach: it comes from the app-wide preference for that quantity (a kPa
    // calibration draws in bar if that is what pressure is set to), which meant leaving the table,
    // opening Preferences and coming back. "Auto" hands it back to that preference.
    void pickCellUnit(int idx) {
        if (idx < 0 || idx >= static_cast<int>(m_cellUnitIds.size())) return;
        commitEdit();
        m_cellUnit = m_cellUnitIds[idx];
        if (m_onUnitPicked) m_onUnitPicked("displayUnit", m_cellUnit);
        m_rebuildPending = true;
    }

    static std::string axisTitle(int i, const TableImage::Axis& a) {
        const std::string letter = (i == 0) ? "X" : (i == 1) ? "Y" : "Z (depth)";
        std::string t = letter + " Axis";
        if (!a.label.empty()) t += " \xE2\x80\x94 " + a.label;   // "X Axis — rpm"
        return t;
    }

    void rebuild() {
        using namespace jf;
        m_axes.clear();
        m_sel = { -1, -1 }; m_cur = -1; m_dragSel = false; m_editing = false; m_buf.clear();
        m_ti = Cache::instance().resolveTable(m_path);
        const MetaModel* m = Cache::instance().meta();
        if (!m || !m_ti.valid) return;
        std::vector<std::string> sigNames;
        for (const auto& kv : m->signalMap()) sigNames.push_back(kv.first);

        // The cells' unit picker, when the reading's quantity has more than one unit. "Auto" is the
        // app-wide preference for that quantity — which is what a calibration follows by default, and
        // what made a kPa cal draw in bar with no way to say otherwise without leaving the table.
        m_cellUnitCombo.reset();
        m_cellUnitIds.clear();
        {
            UnitManager& um = UnitManager::instance();
            const std::string src = Cache::instance().unit(m_path);
            const std::string q = src.empty() ? std::string() : um.findQuantityForUnit(src);
            const auto units = q.empty() ? std::vector<UnitManager::Unit>{} : um.getQuantity(q).units;
            if (units.size() > 1) {
                std::vector<std::string> labels{ "Auto" };
                m_cellUnitIds.push_back("Auto");
                for (const auto& u : units) { labels.push_back(u.label); m_cellUnitIds.push_back(u.id); }
                m_cellUnitCombo = std::make_unique<JComboBox>(m_graph, labels, 92.f);
                const std::string cur = m_cellUnit.empty() ? "Auto" : m_cellUnit;
                for (size_t k = 0; k < m_cellUnitIds.size(); ++k)
                    if (m_cellUnitIds[k] == cur) { m_cellUnitCombo->setCurrentIndex(static_cast<int>(k)); break; }
                m_cellUnitCombo->onIndexChanged.connect([this](int idx) { pickCellUnit(idx); });
            }
        }

        for (int ai = 0; ai < static_cast<int>(m_ti.axes.size()); ++ai) {
            const TableImage::Axis& ax = m_ti.axes[ai];
            const bool on = Cache::instance().tiEnabled(m_ti, ai);
            AxisUI a;
            a.axisIdx = ai; a.optional = ax.optional; a.hasSrc = (ax.srcBase >= 0);
            a.resizable = (ax.nBase >= 0); a.nMin = ax.nMin; a.nMax = ax.nMax; a.digits = ax.digits;
            a.liveN = Cache::instance().tiLiveN(m_ti, ai);
            if (a.optional) {
                a.enable = std::make_unique<JCheckBox>(m_graph, "Enable", 74.f);
                a.enable->setChecked(on);
                a.enable->onStateChanged.connect([this, ai](bool o) { toggleEnable(ai, o); });
            }
            if (a.hasSrc) {
                const int cur = Cache::instance().tiSrc(m_ti, ai);
                const std::string curName = cur >= 0 ? m->signalName(cur) : std::string();
                a.chan = std::make_unique<JButton>(m_graph, curName.empty() ? "(no channel)" : curName, 200.f);
                a.chan->setEnabled(on);
                a.chan->onClicked.connect([this, ai] { pickChannel(ai); });
                a.sigNames = sigNames;
            }
            // The unit picker — for ANY axis whose quantity has more than one unit, including a
            // calibration's raw input, which has no channel selector at all.
            {
                UnitManager& um = UnitManager::instance();
                const std::string own = Cache::instance().axisDomain(m_ti, ai).units;
                const std::string q = own.empty() ? std::string() : um.findQuantityForUnit(own);
                const std::vector<UnitManager::Unit> units =
                    q.empty() ? std::vector<UnitManager::Unit>{} : um.getQuantity(q).units;
                if (units.size() > 1) {
                    std::vector<std::string> labels;
                    for (const auto& u : units) { labels.push_back(u.label); a.unitIds.push_back(u.id); }
                    a.unit = std::make_unique<JComboBox>(m_graph, labels, 74.f);
                    const Cache::AxisView v = Cache::instance().axisView(m_ti, ai, unitPrefFor(ai));
                    const std::string cur = v.to.empty() ? v.from : v.to;
                    for (size_t k = 0; k < a.unitIds.size(); ++k)
                        if (a.unitIds[k] == cur) { a.unit->setCurrentIndex(static_cast<int>(k)); break; }
                    a.unit->setEnabled(on);
                    a.unit->onIndexChanged.connect([this, ai](int idx) { pickUnit(ai, idx); });
                }
            }
            const int n = a.liveN;
            a.ins = std::make_unique<JButton>(m_graph, "Insert", 62.f);
            a.del = std::make_unique<JButton>(m_graph, "Delete", 62.f);
            a.lin = std::make_unique<JButton>(m_graph, "Linearise", 78.f);
            a.wiz = std::make_unique<JButton>(m_graph, "Wizard\xE2\x80\xA6", 70.f);
            a.ins->setEnabled(on && a.resizable && (a.nMax <= 0 || n < a.nMax));
            a.del->setEnabled(on && a.resizable && n > std::max(1, a.nMin));
            a.lin->setEnabled(on && n >= 2);
            a.wiz->setEnabled(on && a.resizable);      // it sets the COUNT, so a fixed grid cannot use it
            a.ins->onClicked.connect([this, ai] { insertBin(ai); });
            a.del->onClicked.connect([this, ai] { deleteBin(ai); });
            a.lin->onClicked.connect([this, ai] { linearise(ai); });
            a.wiz->onClicked.connect([this, ai] { wizard(ai); });
            m_axes.push_back(std::move(a));
        }
    }

    // Position each axis group's widgets for this frame + route mouse input (combo/checkbox/buttons + strip).
    void layoutAndInput(float mx, float my, bool pressed, bool released, bool held, float W) {
        using namespace jf;
        const float x0 = 12.f;
        for (size_t i = 0; i < m_axes.size(); ++i) {
            AxisUI& a = m_axes[i];
            const float top = hdrH() + 10.f + static_cast<float>(i) * secH();
            const float rowY = top + 22.f;
            // Right-aligned button block + count.
            const float countW = 78.f, wizW = 70.f, linW = 78.f, delW = 62.f, insW = 62.f, gap = 6.f;
            const float unitW = a.unit ? 74.f : 0.f;
            float bx = W - 12.f - countW - gap - wizW - gap - linW - gap - delW - gap - insW;
            float comboX = x0;
            if (a.enable) { a.enable->setBounds({ x0, rowY + (rowH() - chkH()) * 0.5f, 74.f, chkH() }); comboX = x0 + 82.f; }   // checkbox at its own scheme height, centred in the row
            // [channel .....] [unit] then the button block. The unit sits beside the thing it qualifies.
            const float unitX = bx - 8.f - unitW;
            if (a.chan)   a.chan->setBounds({ comboX, rowY, std::max(60.f, unitX - comboX - 8.f), rowH() });
            if (a.unit)   a.unit->setBounds({ a.chan ? unitX : comboX, rowY, 74.f, rowH() });
            a.ins->setBounds({ bx, rowY, insW, rowH() });          bx += insW + gap;
            a.del->setBounds({ bx, rowY, delW, rowH() });          bx += delW + gap;
            a.lin->setBounds({ bx, rowY, linW, rowH() });          bx += linW + gap;
            a.wiz->setBounds({ bx, rowY, wizW, rowH() });
            // Strip rect.
            a.stripX = x0; a.stripY = rowY + rowH() + 8.f; a.stripW = W - 24.f;

            {   // Both of these follow the SELECTION, which rebuild() never sees — it only runs on a
                // structural change — so their enabled state is refreshed here, per frame.
                int slo = 0, shi = 0;
                const bool span = selSpan(static_cast<int>(i), slo, shi);
                const bool axisOn = a.optional ? (a.enable && a.enable->isChecked()) : true;
                // Linearise: the whole axis with nothing selected, or a span with something between.
                a.lin->setEnabled(axisOn && a.liveN >= 2 && (!span || shi - slo >= 2));
                // Delete: only what is selected, and only while the axis is above its floor.
                a.del->setEnabled(axisOn && a.resizable && span && a.liveN > std::max(1, a.nMin));
            }
            auto route = [&](JWidget* w) { if (!w) return; w->handleMouseMove(mx, my); if (pressed) w->handleMousePress(mx, my); if (released) w->handleMouseRelease(mx, my); };
            route(a.enable.get()); route(a.chan.get()); route(a.unit.get()); route(a.ins.get()); route(a.del.get()); route(a.lin.get()); route(a.wiz.get());

            // Strip: click anchors a selection, dragging extends it; wheel scrolls horizontally.
            const int n = a.liveN;
            const float maxScroll = std::max(0.f, n * kCellW - a.stripW);
            const auto binAt = [&](float x) {
                return std::clamp(static_cast<int>((x - a.stripX + a.scrollX) / kCellW), 0, std::max(0, n - 1));
            };
            const bool inStrip = my >= a.stripY && my < a.stripY + kStripH
                              && mx >= a.stripX && mx < a.stripX + a.stripW;
            if (pressed && inStrip) {
                commitEdit();
                m_sel = { static_cast<int>(i), binAt(mx) };
                m_cur = m_sel.second;                        // a click is a one-cell span
                m_dragSel = true;
                m_editing = false; m_buf.clear();
            }
            // Extend while the button is down, even once the pointer leaves the strip vertically —
            // a drag that slips a few pixels up should keep selecting, not stop dead.
            if (m_dragSel && held && !a.sbDrag && m_sel.first == static_cast<int>(i)) {
                m_cur = binAt(mx);
                ensureVisible();
            }
            if (released) m_dragSel = false;
            // Scrollbar: grab the thumb and it follows the pointer; click the gutter and it comes to you
            // and keeps following. Both then DRAG, which the bar did not do at all — every interaction
            // was a single jump on press, so the thumb could not be pulled anywhere.
            const float sbY = a.stripY + kStripH + 2.f;
            float tw = 0.f, tx = 0.f;
            thumbGeom(a, tw, tx);
            if (pressed && maxScroll > 0.f && my >= sbY && my < sbY + kSbH
                && mx >= a.stripX && mx < a.stripX + a.stripW) {
                a.sbDrag = true;
                a.sbGrab = (mx >= tx && mx < tx + tw) ? (mx - tx)    // where you took hold of it
                                                      : tw * 0.5f;   // gutter: bring it under the pointer
                a.scrollX = scrollFor(a, mx, tw);
            }
            if (a.sbDrag && held) a.scrollX = scrollFor(a, mx, tw);
            if (!held) a.sbDrag = false;
            a.scrollX = std::clamp(a.scrollX, 0.f, maxScroll);
        }
        if (const float wheel = m_window->consumeWheel(); wheel != 0.f)
            for (AxisUI& a : m_axes)
                if (my >= a.stripY && my < a.stripY + kStripH) { a.scrollX = std::clamp(a.scrollX - wheel * kCellW, 0.f, std::max(0.f, a.liveN * kCellW - a.stripW)); }
    }

    // Type into the selected breakpoint cell. Returns true if the key was consumed by the strip.
    bool handleStripKey(const jf::JKeyEvent& ke) {
        using K = jf::JKeyEvent::JKey;
        if (m_sel.first < 0 || m_sel.first >= static_cast<int>(m_axes.size())) return false;
        AxisUI& a = m_axes[m_sel.first];
        const int n = a.liveN;
        const char ch = ke.utf8[0];
        // Shift extends the span from the anchor; a plain arrow collapses it back to one cell.
        const auto move = [&](int to) {
            commitEdit();
            m_cur = std::clamp(to, 0, std::max(0, n - 1));
            if (!ke.shift) m_sel.second = m_cur;
            ensureVisible();
        };
        if (ke.key == K::Left)   { move(m_cur - 1); return true; }
        if (ke.key == K::Right)  { move(m_cur + 1); return true; }
        if (ke.key == K::Home)   { move(0); return true; }
        if (ke.key == K::End)    { move(n - 1); return true; }
        if (ke.key == K::Return) { move(m_cur + 1); return true; }
        if (ke.key == K::Escape) { if (m_editing) { m_editing = false; m_buf.clear(); return true; } return false; }
        if (ke.key == K::Backspace) { if (m_editing && !m_buf.empty()) m_buf.pop_back(); return true; }
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') {
            if (!m_editing) { m_editing = true; m_buf.clear(); m_sel.second = m_cur; }   // typing is one cell
            m_buf += ch;
            return true;
        }
        return false;
    }
    // Scrollbar geometry, computed ONCE and used by both the hit-test and the render — they disagreed
    // before, which is half of why the bar did not track the pointer.
    static void thumbGeom(const AxisUI& a, float& tw, float& tx) {
        const float content = std::max(1.f, a.liveN * kCellW);
        const float maxScroll = std::max(0.f, content - a.stripW);
        tw = std::min(a.stripW, std::max(24.f, a.stripW * (a.stripW / content)));
        tx = a.stripX + (maxScroll > 0.f ? (a.stripW - tw) * (a.scrollX / maxScroll) : 0.f);
    }
    // Where the thumb's LEFT EDGE has to sit for the pointer to stay where it was grabbed. The travel is
    // (stripW - thumbW), not stripW: mapping the cursor across the whole strip made the thumb lag the
    // pointer by half its own width and never quite reach either end.
    static float scrollFor(const AxisUI& a, float mx, float tw) {
        const float maxScroll = std::max(0.f, a.liveN * kCellW - a.stripW);
        const float travel = std::max(1.f, a.stripW - tw);
        return std::clamp((mx - a.sbGrab - a.stripX) / travel * maxScroll, 0.f, maxScroll);
    }

    // The selected span on axis `ai`, or false when the selection is elsewhere / absent.
    bool selSpan(int ai, int& lo, int& hi) const {
        if (m_sel.first != ai || m_sel.second < 0 || m_cur < 0) return false;
        lo = std::min(m_sel.second, m_cur);
        hi = std::max(m_sel.second, m_cur);
        return true;
    }

    void ensureVisible() {
        if (m_sel.first < 0) return;
        AxisUI& a = m_axes[m_sel.first];
        const float cx = m_cur * kCellW;
        if (cx < a.scrollX) a.scrollX = cx;
        else if (cx + kCellW > a.scrollX + a.stripW) a.scrollX = cx + kCellW - a.stripW;
    }
    void commitEdit() {
        if (!m_editing || m_sel.first < 0) { m_editing = false; m_buf.clear(); return; }
        AxisUI& a = m_axes[m_sel.first];
        double v; try { v = std::stod(m_buf); } catch (...) { m_editing = false; m_buf.clear(); return; }
        std::vector<double> bins = Cache::instance().tiBins(m_ti, a.axisIdx);
        const int i = m_cur, n = static_cast<int>(bins.size());
        if (i < 0 || i >= n) { m_editing = false; m_buf.clear(); return; }
        // Typed in the unit the strip is showing; the bytes are the axis's own quantity, so convert first.
        v = Cache::instance().snapBin(m_ti, a.axisIdx, axisView(a).toStorage(v));
        // Write the value as typed — do NOT clamp to the neighbours. Forcing v >= bins[i-1] turned a
        // legitimate entry (e.g. 98.5 after a 99) into a DUPLICATE of the neighbour, silently eating the
        // value. The table's inline axis edit already writes verbatim; the dialog now matches it. Ascending
        // order is the tuner's responsibility, same as editing cells.
        bins[i] = v;
        Cache& c = Cache::instance();
        Cache::WriteGuard wg(c);   // axis setup is structural — allowed even while the tune is locked
        c.beginEdit(); c.tiWriteBins(m_ti, a.axisIdx, bins); c.endEdit("Edit axis breakpoints"); c.announceReload();
        m_editing = false; m_buf.clear();
    }

    // Ramp evenly between the ends of the SELECTED span, leaving everything outside it alone. Without
    // a selection it is the whole axis, as before — the old behaviour was only ever that, so tidying a
    // ragged stretch in the middle meant re-flattening the ends the tuner had placed deliberately.
    // The span's own endpoints are kept: they are what you dragged the selection out to.
    void linearise(int ai) {
        AxisUI& a = m_axes[ai];
        std::vector<double> bins = Cache::instance().tiBins(m_ti, a.axisIdx);
        const int count = static_cast<int>(bins.size());
        if (count < 2) return;
        int lo = 0, hi = count - 1;
        if (selSpan(ai, lo, hi)) {
            // A span of one or two cells has nothing BETWEEN its ends, so there is nothing to ramp.
            // Do nothing — falling back to the whole axis here would answer a two-cell selection by
            // flattening every bin, which is the last thing that click meant.
            if (hi - lo < 2) return;
        } else {
            lo = 0; hi = count - 1;                      // nothing selected: the axis end to end, as before
        }
        const int n = hi - lo + 1;
        const double first = bins[lo], last = bins[hi];
        // A ramp DIVIDES: 0..100 over 7 bins is 16.666… — every step goes through the same rule as a
        // typed one, so Linearise cannot mint a bin the axis would refuse.
        for (int i = 0; i < n; ++i)
            bins[lo + i] = Cache::instance().snapBin(m_ti, a.axisIdx, first + (last - first) * i / (n - 1));
        Cache& c = Cache::instance();
        Cache::WriteGuard wg(c);   // structural — allowed while locked
        c.beginEdit(); c.tiWriteBins(m_ti, a.axisIdx, bins); c.endEdit("Linearise axis"); c.announceReload();
    }
    // Open the shared signal picker on this axis's channel — the same hook, tree and filter the run-mode
    // *_src controls use, so a channel is chosen the same way everywhere in the studio.
    void pickChannel(int ai) {
        AxisUI& a = m_axes[ai];
        if (!enumpick::signal() || a.sigNames.empty()) return;
        const MetaModel* m = Cache::instance().meta();
        const int cur = Cache::instance().tiSrc(m_ti, a.axisIdx);
        const std::string curName = (cur >= 0 && m) ? m->signalName(cur) : std::string();
        const int axisIdx = a.axisIdx;
        enumpick::signal()(a.sigNames, curName, [this, axisIdx](std::string picked) {
            setChannel(axisIdx, std::move(picked));
        });
    }

    // By NAME, because that is what the picker returns. An index into a list this dialog happens to hold
    // would break the moment the picker's list were built from a different source than this one.
    void setChannel(int axisIdx, std::string name) {
        Cache& c = Cache::instance();
        const MetaModel* m = c.meta();
        if (!m) return;
        const auto it = m->signalMap().find(name);
        if (it == m->signalMap().end()) return;
        Cache::WriteGuard wg(c);   // structural — allowed while locked
        c.beginEdit(); c.tiSetSrc(m_ti, axisIdx, it->second); c.endEdit("Set axis channel"); c.announceReload();
        m_rebuildPending = true;   // the button's label — and the axis's range and precision — follow it
    }
    void toggleEnable(int ai, bool on) {
        Cache& c = Cache::instance();
        Cache::WriteGuard wg(c);   // structural — allowed while locked
        c.beginEdit(); c.tiSetEnabled(m_ti, m_axes[ai].axisIdx, on); c.endEdit("Toggle axis"); c.announceReload();
        m_rebuildPending = true;
    }
    // Insert: ASK first, then insert. The old midpoint / last+step guess is still what the field opens
    // on — it is a reasonable proposal — but it is a proposal, not a fact, so the table must not change
    // until it is accepted. Inserting first meant the bin appeared behind the dialog, Cancel left it
    // there, and it took two undos to be rid of: the value and the insert.
    void insertBin(int ai) {
        AxisUI& a = m_axes[ai];
        int lo = 0, hi = 0;
        const int seedAt = selSpan(ai, lo, hi) ? lo : a.liveN;
        Cache& c = Cache::instance();
        const int axisIdx = a.axisIdx;
        // The SELECTION still chooses the proposal — the midpoint of a selected span is the bin you
        // usually want — but it does not choose the POSITION. Where the value goes is the value's own
        // business; see _insertAt.
        const double seed = c.tiInsertSeed(m_ti, axisIdx, seedAt);
        if (!onAskValue) { _insertAt(axisIdx, seed); return; }   // unwired: the proposal stands
        // The prompt speaks the strip's unit: its Min/Max, its seed and what you type are all volts if
        // the axis is being shown in volts. _insertAt converts back, because the bytes never move.
        const Cache::AxisView view = axisView(a);
        Cache::ChannelDomain shown = axisDomain(a);
        shown.lo = view.lo; shown.hi = view.hi; shown.hasRange = view.hasRange;
        shown.digits = view.digits; shown.units = view.label();
        onAskValue(view.toDisplay(seed), shown, [this, axisIdx, view](double v) {
            _insertAt(axisIdx, view.toStorage(v));
        });
    }

    // Where a value belongs on an axis that is already in order: after every bin it is not before.
    // Direction is READ from the axis rather than assumed — a descending axis (a vacuum ladder) is
    // just as sorted as a rising one, and inserting into it by the ascending rule would break it.
    // A ladder that is ALREADY out of order has no such answer; the value goes on the end, which is
    // where an append would have put it anyway, and the out-of-order bins stay flagged in the strip.
    static int orderedSlot(const std::vector<double>& bins, double v) {
        if (bins.size() < 2) return static_cast<int>(bins.size());
        const bool desc = bins.back() < bins.front();
        for (std::size_t i = 1; i < bins.size(); ++i)
            if (desc ? (bins[i] > bins[i - 1]) : (bins[i] < bins[i - 1]))
                return static_cast<int>(bins.size());        // not sorted: nothing to insert INTO
        for (std::size_t i = 0; i < bins.size(); ++i)
            if (desc ? (v > bins[i]) : (v < bins[i])) return static_cast<int>(i);
        return static_cast<int>(bins.size());
    }

    // Does bin `idx` break the axis's own direction? The direction is the one the ENDS describe, so a
    // single stray value is flagged rather than re-interpreting the whole ladder around it. Takes the
    // strip's already-loaded bins — this is called per cell, per frame.
    static bool _breaksOrder(const std::vector<double>& bins, int idx) {
        if (idx < 0 || idx >= static_cast<int>(bins.size()) || bins.size() < 2) return false;
        const bool desc = bins.back() < bins.front();
        if (idx > 0        && (desc ? bins[idx] > bins[idx - 1] : bins[idx] < bins[idx - 1])) return true;
        if (idx + 1 < static_cast<int>(bins.size())
                           && (desc ? bins[idx] < bins[idx + 1] : bins[idx] > bins[idx + 1])) return true;
        return false;
    }

    // The insert and its value are ONE edit, so it is one undo — and nothing has happened at all if the
    // dialog was cancelled.
    //
    // THE POSITION COMES FROM THE VALUE. It used to come from the cursor (or the end, with nothing
    // selected) and the typed value was then written into that slot, so typing 3 on an axis running
    // 0..100 appended it AFTER 100 and left the ladder unsorted — and an unsorted ladder is not a
    // cosmetic problem: the cell that tiInsertBin created and interpolated sits at the slot the bin
    // went to, so from the first out-of-order bin onward every cell reads against the wrong
    // breakpoint. Snapping happens BEFORE the slot is chosen, because a value clamped onto the
    // axis ceiling belongs where it lands, not where it was typed.
    void _insertAt(int axisIdx, double v) {
        Cache& c = Cache::instance();
        Cache::WriteGuard wg(c);   // structural — allowed while locked
        const TableImage t0 = c.resolveTable(m_path);
        const double sv = c.snapBin(t0, axisIdx, v);           // the axis holds the rule, not the form
        const std::vector<double> before = c.tiBins(t0, axisIdx);
        // A bin the axis already has is not an insert. Adding it would put two identical breakpoints
        // side by side, and a lookup lands on the LAST of them — so every cell behind the duplicate
        // becomes unreachable, which is a table quietly losing a column.
        for (double b : before)
            if (std::fabs(b - sv) < 1e-9) {
                JLOGC("ui.axis", jf::JLogLevel::Info)
                    << "insert skipped: the axis already has a bin at " << sv;
                return;
            }
        const int at = orderedSlot(before, sv);
        c.beginEdit();
        if (c.tiInsertBin(t0, axisIdx, at)) {
            const TableImage t = c.resolveTable(m_path);
            std::vector<double> b = c.tiBins(t, axisIdx);
            if (at >= 0 && at < static_cast<int>(b.size())) {
                b[at] = sv;
                c.tiWriteBins(t, axisIdx, b);
            }
        }
        c.endEdit("Insert bin");
        c.announceReload();
        m_rebuildPending = true;
    }

    // Wizard: start, end, increment — the only operation that sets the axis's SIZE as well as its
    // values, which is what makes building one from scratch a form rather than twenty Inserts.
    void wizard(int ai) {
        if (!onAskAxis) return;
        AxisUI& a = m_axes[ai];
        Cache& c = Cache::instance();
        const std::vector<double> bins = c.tiBins(m_ti, a.axisIdx);
        const double start = bins.empty() ? 0.0 : bins.front();
        const double end   = bins.empty() ? 100.0 : bins.back();
        const double inc   = (bins.size() > 1 && end > start)
                           ? (end - start) / double(bins.size() - 1) : 1.0;
        const int axisIdx = a.axisIdx;
        // In the strip's unit, like everything else here: laying out an axis in volts while the strip
        // reads volts is the whole point, and the conversion back happens before anything is written.
        const Cache::AxisView view = axisView(a);
        Cache::ChannelDomain shown = axisDomain(a);
        shown.lo = view.lo; shown.hi = view.hi; shown.hasRange = view.hasRange;
        shown.digits = view.digits; shown.units = view.label();
        const double vStart = view.toDisplay(start), vEnd = view.toDisplay(end);
        // An increment is a DISTANCE, so it converts as a difference — a unit with an offset would
        // otherwise turn a step into a step-plus-offset.
        const double vInc = std::max(std::pow(10.0, -view.digits),
                                     std::fabs(view.toDisplay(start + inc) - vStart));
        onAskAxis(vStart, vEnd, vInc, shown, [this, axisIdx, view](double s0, double e0, double st) {
            const double storeStart = view.toStorage(s0), storeEnd = view.toStorage(e0);
            const double storeStep  = std::fabs(view.toStorage(s0 + st) - storeStart);
            _buildAxis(axisIdx, storeStart, storeEnd, storeStep);
        });
    }

    // start/end/increment in STORAGE units, whatever the strip was showing when they were typed.
    void _buildAxis(int axisIdx, double s0, double e0, double st) {
        Cache& c = Cache::instance();
        Cache::WriteGuard wg(c);
        c.beginEdit();                                 // ONE undo step for the resize AND the values
        c.tiBuildAxis(c.resolveTable(m_path), axisIdx, s0, e0, st);
        c.endEdit("Axis wizard");
        c.announceReload();
        m_rebuildPending = true;
    }
    // Delete the SELECTED span, not one bin. Deleting six bins used to be six clicks, and with nothing
    // selected it silently took the last bin — an edit nobody asked for, aimed at a cell they were not
    // looking at. Now it needs a selection (the button says so by greying out) and removes exactly it.
    void deleteBin(int ai) {
        AxisUI& a = m_axes[ai];
        int lo = 0, hi = 0;
        if (!selSpan(ai, lo, hi)) return;                 // nothing selected: nothing to delete
        Cache& c = Cache::instance();
        const int floorN = std::max(1, a.nMin);           // the schema's floor still holds
        Cache::WriteGuard wg(c);                          // structural — allowed while locked
        c.beginEdit();                                    // one undo step for the whole span
        int n = c.tiLiveN(c.resolveTable(m_path), a.axisIdx);
        // Top down: removing from the high end leaves every index below it still valid.
        for (int i = hi; i >= lo && n > floorN; --i)
            if (c.tiRemoveBin(c.resolveTable(m_path), a.axisIdx, i)) --n;
        c.endEdit("Delete bins");
        c.announceReload();
        // Put the cursor where the span was, so a repeated delete keeps eating forwards rather than
        // pointing at a bin that no longer exists.
        m_sel = { ai, std::clamp(lo, 0, std::max(0, n - 1)) };
        m_cur = m_sel.second;
        m_rebuildPending = true;
    }

    void render(jf::JPrimitiveBuffer& buf, float W, float H) {
        using namespace jf;
        buf.clear();
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface0);
        const float lh = JTextHelper::lineHeight();
        jf::JTitleBar::draw(buf, 0.f, 0.f, W, hdrH(), (m_ti.label.empty() ? m_path : m_ti.label), jf::JStyle::current().cornerRadius, 0, 12.f);
        if (!m_ti.valid) { m_btnClose->populateRenderPrimitives(buf); return; }

        char vb[48];
        for (size_t i = 0; i < m_axes.size(); ++i) {
            AxisUI& a = m_axes[i];
            const float top = hdrH() + 10.f + static_cast<float>(i) * secH();
            const bool on = a.optional ? (a.enable && a.enable->isChecked()) : true;
            // Group border + title.
            buf.pushRectangle(8.f, top, W - 16.f, secH() - 8.f, Colors::Surface0, 5.f, 1.f, Colors::Border);
            if (JTextHelper::hasAtlas())
                JTextHelper::pushText(buf, 16.f, top + 4.f, axisTitle(a.axisIdx, m_ti.axes[a.axisIdx]), Colors::TextSecondary, W - 32.f);
            // Value-axis note (no channel).
            // Value-axis note, placed AFTER the unit picker rather than under it — a calibration's raw
            // axis has no channel and does have a unit, so the two share this row.
            if (!a.hasSrc && JTextHelper::hasAtlas()) {
                const float noteX = a.unit ? a.unit->getBoundingBox().x + a.unit->getBoundingBox().width + 10.f
                                           : 16.f;
                JTextHelper::pushText(buf, noteX, top + 22.f + (rowH() - lh) * 0.5f,
                                      "(value axis \xE2\x80\x94 no channel)", Colors::TextSecondary, 200.f);
            }
            // Count "n (min..max)".
            if (JTextHelper::hasAtlas()) {
                const int mn = std::max(1, a.nMin), mx2 = a.nMax > 0 ? a.nMax : a.liveN;
                std::snprintf(vb, sizeof(vb), "%d (%d..%d)", a.liveN, mn, mx2);
                JTextHelper::pushText(buf, W - 78.f + 2.f, top + 22.f + (rowH() - lh) * 0.5f, vb, Colors::TextSecondary, 76.f);
            }
            // Breakpoint strip. The channel's domain is resolved ONCE per axis, not per cell — it
            // reads a config byte for a generic input, and this runs every frame.
            const std::vector<double> bins = Cache::instance().tiBins(m_ti, a.axisIdx);
            const Cache::ChannelDomain dom = axisDomain(a);
            const Cache::AxisView view = axisView(a);
            buf.pushRectangle(a.stripX, a.stripY, a.stripW, kStripH, Colors::Surface1, 3.f, 1.f, Colors::Border);
            buf.pushClip(a.stripX + 1.f, a.stripY + 1.f, a.stripW - 2.f, kStripH - 2.f);
            for (int b = 0; b < a.liveN; ++b) {
                const float cx = a.stripX + b * kCellW - a.scrollX;
                if (cx + kCellW < a.stripX || cx > a.stripX + a.stripW) continue;   // off-strip
                // The cursor cell wears the accent; the rest of the span is the framework's selection
                // fill, so a dragged range reads as one block with a clear active end.
                int slo = 0, shi = -1;
                const bool inSpan = selSpan(static_cast<int>(i), slo, shi) && b >= slo && b <= shi;
                const bool sel = (m_sel.first == static_cast<int>(i) && m_cur == b);
                if (sel)          buf.pushRectangle(cx, a.stripY, kCellW, kStripH, on ? Colors::Accent : Colors::Surface2, 2.f);
                else if (inSpan)  buf.pushRectangle(cx, a.stripY, kCellW, kStripH, on ? Colors::SelectionFill : Colors::Surface2, 2.f);
                if (b > 0) buf.pushRectangle(cx, a.stripY + 3.f, 1.f, kStripH - 6.f, Colors::Border);   // cell divider
                std::string cell;
                const double bv = (b < static_cast<int>(bins.size()) ? bins[b] : 0.0);
                if (sel && m_editing) cell = m_buf + "_";
                else cell = view.text(bv);
                // A bin this channel cannot reach — outside its range, or finer than it is read. Not
                // rewritten (a channel change must stay free to undo by re-picking), but SAID, because
                // the alternative is an axis quietly claiming values the signal never produces: 120.0
                // on a pedal that stops at 100, or 30.444 on one read to a tenth. Retyping it or
                // Linearise brings it back onto the grid.
                // ORDER counts too. An axis is a sorted ladder — every lookup assumes it — so a bin
                // that breaks the run makes the cells behind it read against the wrong breakpoints.
                // It used to be the one kind of illegal bin the strip stayed silent about, which is
                // how an insert that appended its value past the top corrupted a table in silence.
                const bool outOfOrder = _breaksOrder(bins, b);
                const bool illegal = !(sel && m_editing)
                                  && (outOfOrder
                                      || (dom.known()
                                          && ((dom.hasRange && (bv < dom.lo || bv > dom.hi))
                                              || (dom.digits >= 0 && std::fabs(bv - snapBreak(bv, dom.digits)) > 1e-3))));
                if (JTextHelper::hasAtlas()) {
                    const uint8_t* col = !on ? Colors::TextSecondary : (sel && m_editing) ? Colors::Surface0
                                       : illegal ? Colors::Warning : Colors::TextPrimary;
                    JTextHelper::pushText(buf, cx + (kCellW - JTextHelper::measureWidth(cell)) * 0.5f, a.stripY + (kStripH - lh) * 0.5f, cell, col, kCellW);
                }
            }
            buf.popClip();
            // Horizontal scrollbar.
            const float maxScroll = std::max(0.f, a.liveN * kCellW - a.stripW);
            if (maxScroll > 0.f) {
                const float sbY = a.stripY + kStripH + 2.f;
                buf.pushRectangle(a.stripX, sbY, a.stripW, kSbH, Colors::Surface1, 3.f);
                float tw = 0.f, tx = 0.f;
                thumbGeom(a, tw, tx);      // the same geometry the hit-test used
                buf.pushRectangle(tx, sbY, tw, kSbH, a.sbDrag ? Colors::Accent : Colors::Surface3, 3.f);
            }
        }
        // Widgets on top (combos draw their arrow etc.).
        for (const AxisUI& a : m_axes) {
            if (a.enable) a.enable->populateRenderPrimitives(buf);
            if (a.chan)   a.chan->populateRenderPrimitives(buf);
            if (a.unit)   a.unit->populateRenderPrimitives(buf);
            if (a.ins) a.ins->populateRenderPrimitives(buf);
            if (a.del) a.del->populateRenderPrimitives(buf);
            if (a.lin) a.lin->populateRenderPrimitives(buf);
            if (a.wiz) a.wiz->populateRenderPrimitives(buf);
        }
        if (m_cellUnitCombo && JTextHelper::hasAtlas()) {
            const float by = H - kBtnH() - 12.f;
            JTextHelper::pushText(buf, 14.f, by + (kBtnH() - lh) * 0.5f, "Values in", Colors::TextSecondary, 88.f);
            m_cellUnitCombo->populateRenderPrimitives(buf);
        }
        m_btnClose->populateRenderPrimitives(buf);
    }

    static constexpr const char* kAxisUnitProp[3] = { "displayUnitX", "displayUnitY", "displayUnitZ" };

    std::string m_path;
    std::vector<std::string> m_axisUnits;   // per storage axis, from the widget that opened this
    std::string m_cellUnit;                 // …and the cells' ("" / "Auto" = the quantity preference)
    std::vector<std::string> m_cellUnitIds;
    std::unique_ptr<jf::JComboBox> m_cellUnitCombo;
    std::function<void(std::string, std::string)> m_onUnitPicked;
    // This dialog's focus tree, declared because JWidget takes a scene graph rather than a parent, so
    // members are not otherwise discoverable. Clicks focus by themselves (JControl::handleMousePress calls
    // requestFocus, which routes to this manager while the dialog is open); this adds Tab traversal and
    // sends keys to the focused control.
    void _refreshFocusRoots() {
        std::vector<jf::JWidget*> roots;
        if (m_btnClose) roots.push_back(m_btnClose.get());
        if (m_cellUnitCombo) roots.push_back(m_cellUnitCombo.get());
        for (const auto& a : m_axes) {              // each axis strip's own controls
            if (a.enable) roots.push_back(a.enable.get());
            if (a.chan)   roots.push_back(a.chan.get());
            if (a.unit)   roots.push_back(a.unit.get());
            if (a.ins)    roots.push_back(a.ins.get());
            if (a.del)    roots.push_back(a.del.get());
            if (a.lin)    roots.push_back(a.lin.get());
            if (a.wiz)    roots.push_back(a.wiz.get());
        }
        m_focus.setFocusRoots(std::move(roots));
    }

    jf::JFocusManager m_focus;

    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{ 0 };
    jf::JSceneGraph  m_graph;                         // before the widgets so it outlives them
    std::unique_ptr<jf::JButton> m_btnClose;
    std::vector<AxisUI> m_axes;
    TableImage m_ti;
    // Selection is an ANCHOR and a CURSOR, not one cell: {axis, anchor bin} plus m_cur, the end that
    // moves under a drag or Shift+Arrow. The span between them is what Linearise ramps, so a ragged
    // stretch in the middle of an axis can be straightened without touching the ends the tuner set.
    std::pair<int, int> m_sel{ -1, -1 };
    int  m_cur{ -1 };                                 // cursor bin — typing edits THIS cell
    bool m_dragSel{ false };                          // mouse-down inside the strip: extend while held
    std::string m_buf;                                // inline edit buffer for the selected cell
    bool m_editing{ false };
    uint32_t m_lastW{ kW }, m_lastH{ kH };
    float    m_ax{ 0 }, m_ay{ 0 };
    bool     m_done{ false }, m_drag{ false }, m_rebuildPending{ false };
};
