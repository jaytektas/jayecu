#pragma once

// FirmwareChangesDialog — before a firmware update: what you lose, and what you get.
//
// The tune comparison (TuneDiffDialog) shows two tunes side by side, page by page, the differences
// highlighted. This asks the same of two FIRMWARES, at the moment an update is about to be committed:
//
//   LEFT   GOING OUT — settings this tune has set that the new firmware no longer has, on the pages the
//          tuner has now, drawn against the firmware the ECU is running, the lost settings highlighted.
//   RIGHT  NEW       — settings the new firmware adds, on the new firmware's own pages, drawn against the
//          new firmware at the values they start at, highlighted: the list of things to go and configure.
//
// Each side is a different firmware with a different layout, so each is drawn inside its own
// Cache::DefinitionScope (meta AND image swapped for the one pass) — one side cannot read the other's
// bytes at its own offsets. And each side has its own pages, so each pages on its own.
//
// Nothing here is editable, as in the tune comparison: the pages are evidence, the buttons the decision.

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/core/JTitleBar.h>
#include <j/core/JCloseButton.h>
#include "DialogChrome.h"
#include "WrapText.h"
#include <j/core/JButton.h>
#include <j/core/JDialogButtonBox.h>
#include <j/core/FocusManager.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include "../model/TuneDiff.h"
#include "../model/FirmwareKits.h"    // fwkits::Note — what changed, in words
#include "../model/Cache.h"
#include "../surface/Surface.h"
#include "../surface/CanvasWidget.h"
#include "../surface/PanelLibrary.h"
#include "../surface/PanelModel.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class FirmwareChangesDialog {
public:
    static constexpr uint32_t kW = 1820, kH = 760;
    static float kBtnH() { return jf::JStyle::current().buttonHeight; }
    static float hdrH()  { return jf::JStyle::current().titleBarHeight; }
    static float rowH()  { return jf::JTextHelper::lineHeight() + 8.f; }

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = uint32_t;
#endif

    // One side of the report: a firmware (its meta and a tune in its layout) and the pages to show it on.
    struct Side {
        std::string heading;                  // "Going out with firmware 0.4.0"
        std::string emptyText;                // what to say when there is nothing on this side
        tunediff::Report report;
        const MetaModel* meta = nullptr;      // must outlive the dialog
        std::vector<uint8_t> image;
        std::shared_ptr<PanelLibrary> pages;  // may be null: everything is then listed
        bool valuesOnLeft = true;             // which of Item's a/b (textA/textB) carries this side's value
    };

    // result: true = go ahead with the update, false = not now. `footnote` sits over the buttons: what
    // saying yes involves (ignition off, the tune kept), said before it is said.
    //
    // `notes` is what changed IN WORDS (firmware/CHANGES.md), newest version first. Settings are only half
    // of an update: 0.4.1 changed how the output test behaves and added nothing to configure, and a report
    // of settings alone said an update had nothing in it. The notes sit under the two sides, and take the
    // dialog when neither side has anything.
    FirmwareChangesDialog(std::string title, Side left, Side right, std::string actionLabel,
                          std::string cancelLabel, std::string footnote, std::vector<fwkits::Note> notes,
                          std::function<void(bool)> onResult,
                          jf::JGpuHal& hal, int screenX, int screenY, NativeWinHandleType parent)
        : m_title(std::move(title)), m_footnote(std::move(footnote)), m_notes(std::move(notes))
        , m_onResult(std::move(onResult))
        , m_window(std::make_unique<PlatformWinType>(m_title, kW, kH, screenX, screenY,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        m_cols[0].side = std::move(left);
        m_cols[1].side = std::move(right);
        m_box = std::make_unique<jf::JDialogButtonBox>(m_graph);
        jf::JButton* go = m_box->addButton(actionLabel, jf::JDialogButtonBox::Role::Destructive, 170.f);
        m_box->addButton(cancelLabel, jf::JDialogButtonBox::Role::Reject, 110.f);
        // PAGING BELONGS TO ITS SIDE, and sits on it — next to that side's "Page 1 of 2" — because the two
        // sides have different pages. All four together under the report read as one set of controls.
        for (int i = 0; i < 2; ++i) {
            m_cols[i].prev = std::make_unique<jf::JButton>(m_graph, "<", 34.f, kBtnH());
            m_cols[i].next = std::make_unique<jf::JButton>(m_graph, ">", 34.f, kBtnH());
            m_cols[i].prev->onClicked.connect([this, i] { step(i, -1); });
            m_cols[i].next->onClicked.connect([this, i] { step(i, +1); });
        }
        go->onClicked.connect([this] { finish(true); });
        m_box->onReject.connect([this] { finish(false); });
        // A REAL DIALOG WINDOW: it moves by its title bar, closes on its [x], and resizes from its
        // edges. It was a fixed borderless rectangle with none of that — nothing to grab, nothing to
        // close, and at 1820 x 760 it covered the studio it was asking about.
        m_chrome.setup(*m_window, hdrH(), 900, 520);
        m_window->grabKeyboardFocus();
    }

    ~FirmwareChangesDialog() { for (auto& c : m_cols) { c.surf.reset(); c.prev.reset(); c.next.reset(); } }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) { finish(false); return false; }
        if (!m_placed) {                      // fit a screen narrower than the dialog (as TuneDiffDialog)
            m_placed = true;
            const auto [sw, sh] = m_window->screenSize();
            const uint32_t w = std::min<uint32_t>(kW, sw > 60 ? uint32_t(sw - 40) : kW);
            const uint32_t h = std::min<uint32_t>(kH, sh > 80 ? uint32_t(sh - 60) : kH);
            if (w != kW || h != kH) { m_window->setSize(w, h); hal.resizeSurface(m_surface, w, h); m_sw = w; m_sh = h; }
            const int x = std::max(0, std::min(m_window->screenX(), sw - int(w)));
            const int y = std::max(0, std::min(m_window->screenY(), sh - int(h)));
            if (x != m_window->screenX() || y != m_window->screenY()) m_window->setPosition(x, y);
        }
        // The surface follows the window when the user resizes it.
        if (m_window->width() != m_sw || m_window->height() != m_sh) {
            m_sw = m_window->width(); m_sh = m_window->height();
            hal.resizeSurface(m_surface, m_sw, m_sh);
        }
        const float W = float(m_window->width()), H = float(m_window->height());
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease();
        const bool held = m_window->isLeftButtonDown();
        // Title bar (DialogChrome): drag, minimise, maximise; its [x] is "Not now".
        if (m_chrome.input(*m_window, W, hdrH(), mx, my, pressed, held)) { finish(false); return false; }
        // THE LIST SCROLLS — wheel over it, or drag its bar. It used to stop at "...more", and a report
        // that cannot show what it counts is not a report.
        const float wheel = m_window->consumeWheel();
        for (Column& c : m_cols) c.scroll.input(mx, my, pressed, held, wheel);
        m_notesScroll.input(mx, my, pressed, held, wheel);
        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            _roots();
            if (jf::jRouteKey(ke, m_focus)) continue;
            if (m_box->handleKeyEvent(ke)) { if (m_done) return false; continue; }
        }
        m_graph.setHostWindow(m_window->screenX(), m_window->screenY(),
                              static_cast<std::uintptr_t>(m_window->rawWindowId()));
        const float pad = 16.f, by = H - kBtnH() - 12.f;
        m_box->setBounds({ pad, by, W - 2 * pad, kBtnH() });
        const float gap = 12.f, colW = (W - 2.f * 12.f - gap) / 2.f;
        const float colX[2] = { 12.f, 12.f + colW + gap };
        for (int i = 0; i < 2; ++i) {         // top right of each side, beside its page count
            Column& c = m_cols[i];
            const float bx = colX[i] + colW - 2.f * 34.f - 6.f, byy = hdrH() + 8.f;
            c.prev->setBounds({ bx, byy, 34.f, kBtnH() });
            c.next->setBounds({ bx + 38.f, byy, 34.f, kBtnH() });
            const bool many = c.pageCount() > 1;      // one page: nothing to page through
            c.prev->setVisible(many); c.next->setVisible(many);
        }
        _roots();
        if (pressed) jf::jRouteMouse(mx, my, m_focus);
        if (!m_seeded) { m_seeded = true; m_focus.focusFirst(); }
        m_box->handleMouseMove(mx, my);
        if (pressed)  m_box->handleMousePress(mx, my);
        if (released) m_box->handleMouseRelease(mx, my);
        for (Column& c : m_cols)
            for (jf::JButton* b : { c.prev.get(), c.next.get() }) {
                if (!b->isVisible()) continue;
                b->handleMouseMove(mx, my);
                if (pressed)  b->handleMousePress(mx, my);
                if (released) b->handleMouseRelease(mx, my);
            }
        if (m_done) return false;

        buf.clear();
        buf.pushRectangle(0.f, 0.f, W, H, Colors::DialogBg, 8.f, 1.f, Colors::Border);
        m_chrome.draw(buf, *m_window, W, hdrH(), m_title, mx, my);
        if (JTextHelper::hasAtlas()) {
            // The footnote WRAPS, and the columns end where its lines begin.
            const float footH = m_footnote.empty() ? 0.f
                              : wraptext::height(m_footnote, W - 32.f) + rowH() - JTextHelper::lineHeight();
            const float bottom = by - footH;
            // THE NOTES TAKE WHAT THE SIDES DO NOT NEED. With settings on either side the sides keep most
            // of the height; with none, each side is a heading and one sentence, and the rest is the notes.
            float colsBottom = bottom;
            if (!m_notes.empty()) {
                const float top = hdrH() + 12.f;
                const bool nothing = m_cols[0].pageCount() == 0 && m_cols[1].pageCount() == 0;
                colsBottom = nothing ? top + 4.f * rowH() : top + (bottom - top) * 0.62f;
                _notes(buf, 12.f, colsBottom + 8.f, W - 24.f, bottom - 8.f, nothing);
            }
            _column(buf, m_cols[0], colX[0], colW, colsBottom);
            _column(buf, m_cols[1], colX[1], colW, colsBottom);
            if (!m_footnote.empty())
                wraptext::draw(buf, 16.f, bottom, m_footnote, Colors::TextPrimary, W - 32.f);
        }
        m_box->populateRenderPrimitives(buf);
        for (Column& c : m_cols)
            for (jf::JButton* b : { c.prev.get(), c.next.get() })
                if (b->isVisible()) b->populateRenderPrimitives(buf);
        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    // WHAT CHANGED, IN WORDS: a heading per version and its lines, wrapped to the width and scrolled
    // (wheel or bar) — a jump over several versions is several sections, and none of them is cut short.
    void _notes(jf::JPrimitiveBuffer& buf, float x, float y, float w, float bottom, bool only) {
        using namespace jf;
        y += wraptext::draw(buf, x + 4.f, y, only ? "What changed" : "What else changed", Colors::TextPrimary, w)
             + (rowH() - JTextHelper::lineHeight());
        const float h = std::max(rowH() * 2.f, bottom - y);
        const float lh = JTextHelper::lineHeight();
        // One row per DRAWN line, so the scroll moves by lines and a long note scrolls like any other.
        struct Row { std::string text; bool head; float indent; };
        std::vector<Row> rows;
        const float textW = w - 40.f;
        for (const fwkits::Note& n : m_notes) {
            rows.push_back({ "Firmware " + n.version, true, 0.f });
            for (const std::string& c : n.changes) {
                const std::string wrapped = wraptext::wrap(c, textW - 20.f).text;
                size_t at = 0;
                bool first = true;
                while (at <= wrapped.size()) {
                    const size_t nl = wrapped.find('\n', at);
                    const std::string line = wrapped.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
                    rows.push_back({ (first ? "\xE2\x80\xA2  " : "") + line, false, first ? 8.f : 21.f });
                    first = false;
                    if (nl == std::string::npos) break;
                    at = nl + 1;
                }
            }
        }
        static const uint8_t clear[4] = { 0, 0, 0, 0 };
        buf.pushRectangle(x, y, w, h, clear, 4.f, 1.f, Colors::Border);
        const float iw = m_notesScroll.layout({ x, y, w, h }, int(rows.size()), lh + 2.f);
        m_notesScroll.drawBar(buf);
        float ry = y + 6.f;
        for (int k = m_notesScroll.first; k < m_notesScroll.last(); ++k) {
            const Row& r = rows[size_t(k)];
            JTextHelper::pushText(buf, x + 8.f + r.indent, ry, r.text,
                                  r.head ? Colors::TextPrimary : Colors::TextSecondary, iw - 16.f - r.indent);
            ry += lh + 2.f;
        }
    }

    struct Column {
        Side side;
        int page = 0, built = -1;
        std::unique_ptr<Surface> surf;
        PanelModel* model = nullptr;
        std::unique_ptr<jf::JButton> prev, next;
        dlgchrome::ListScroll scroll;         // the listed view scrolls
        int pageCount() const {
            return int(side.report.pages.size()) + (side.report.unpaged.empty() ? 0 : 1);
        }
        bool onUnpaged() const { return page >= int(side.report.pages.size()); }
    };

    // Green for what arrives, amber for what goes: the same translucent tint over the control as the
    // tune comparison, so the value underneath stays readable.
    static const uint8_t* fill(bool left) {
        static const uint8_t goes[4] = { 235, 150, 60, 70 }, comes[4] = { 80, 200, 110, 70 };
        return left ? goes : comes;
    }
    static const uint8_t* edge(bool left) {
        static const uint8_t goes[4] = { 214, 110, 20, 255 }, comes[4] = { 40, 170, 80, 255 };
        return left ? goes : comes;
    }

    void ensurePage(Column& c) {
        if (c.built == c.page) return;
        c.built = c.page;
        c.surf.reset(); c.model = nullptr;
        if (!c.side.pages || c.onUnpaged()) return;
        const std::string& node = c.side.report.pages[size_t(c.page)].node;
        PanelModel* pm = c.side.pages->find(node);
        if (!pm || pm->elements().empty()) return;
        c.model = pm;
        auto s = std::make_unique<Surface>(m_graph, Cache::instance(), pm);
        s->setMode(Surface::Mode::Run);
        s->setActiveNode(node);
        s->setFitToView(true);
        std::shared_ptr<PanelLibrary> lib = c.side.pages;
        s->setPageAccess([lib](const std::string& p) -> PanelModel* { return lib ? &lib->forNode(p) : nullptr; },
                         [] {});
        c.surf = std::move(s);
    }

    // THE HIGHLIGHT CARRIES THE SETTING'S OWN NAME, from the meta: "Swept Volume · new". A page's
    // captions are whatever its author typed next to the control, and a stale one — a box rebound to a
    // new setting but still captioned with the old — would have the report say a retired setting is
    // arriving. The tag is the firmware's word for what is highlighted, so the page's text cannot mislead.
    void mark(jf::JPrimitiveBuffer& buf, CanvasWidget* w, const std::map<std::string, std::string>& want,
              bool left, int& n) {
        if (!w) return;
        const std::string b = w->bindPath();
        const auto hit = b.empty() ? want.end() : want.find(b);
        if (hit != want.end()) {
            const jf::JRect r = w->bounds();
            if (r.width > 0.f && r.height > 0.f) {
                static const uint8_t clear[4] = { 0, 0, 0, 0 };
                const bool small = r.width * r.height < 40000.f;
                buf.pushRectangle(r.x - 3.f, r.y - 3.f, r.width + 6.f, r.height + 6.f,
                                  small ? fill(left) : clear, 3.f, 2.f, edge(left));
                const std::string tag = hit->second + (left ? "  \xC2\xB7  going" : "  \xC2\xB7  new");
                const float tw = jf::JTextHelper::measureWidth(tag) + 12.f, th = jf::JTextHelper::lineHeight() + 4.f;
                const float tx = r.x + r.width + 8.f, ty = r.y + (r.height - th) * 0.5f;
                buf.pushRectangle(tx, ty, tw, th, edge(left), 3.f);
                static const uint8_t ink[4] = { 20, 20, 20, 255 };
                jf::JTextHelper::pushText(buf, tx + 6.f, ty + 2.f, tag, ink, tw);
                ++n;
            }
        }
        for (CanvasWidget* ch : w->childWidgets()) mark(buf, ch, want, left, n);
    }

    void _column(jf::JPrimitiveBuffer& buf, Column& c, float x, float w, float bottom) {
        using namespace jf;
        const bool left = c.side.valuesOnLeft;
        float y = hdrH() + 12.f;
        // Heading, page line and empty text are sentences: each WRAPS, and moves what follows down.
        const float extraL = rowH() - JTextHelper::lineHeight();
        y += wraptext::draw(buf, x + 4.f, y, c.side.heading, Colors::TextPrimary, w - 90.f) + extraL;
        const int n = c.pageCount();
        if (n == 0) {
            wraptext::draw(buf, x + 4.f, y + 8.f, c.side.emptyText, Colors::TextSecondary, w - 8.f);
            return;
        }
        char sub[260];
        const std::string where = c.onUnpaged() ? std::string("Settings on no page")
                                                : c.side.report.pages[size_t(c.page)].node;
        std::snprintf(sub, sizeof sub, "Page %d of %d  \xC2\xB7  %s  \xC2\xB7  %d setting(s) in all",
                      c.page + 1, n, where.c_str(), c.side.report.settings);
        y += wraptext::draw(buf, x + 4.f, y, sub, Colors::TextSecondary, w - 90.f) + extraL;

        const std::vector<tunediff::Item>& items =
            c.onUnpaged() ? c.side.report.unpaged : c.side.report.pages[size_t(c.page)].items;
        const float h = std::max(80.f, bottom - 12.f - rowH() - y);
        static const uint8_t clear[4] = { 0, 0, 0, 0 };

        // THIS SIDE'S FIRMWARE, for the length of its pass: its meta and its tune, never the other's.
        if (c.side.meta) {
            Cache::DefinitionScope scope(Cache::instance(), *c.side.meta, c.side.image);
            ensurePage(c);
            if (c.surf && c.model) {
                c.scroll.clear();                                  // a page: nothing here scrolls
                c.surf->setBounds({ x, y, w, h });
                c.surf->populateRenderPrimitives(buf);
                std::map<std::string, std::string> want;          // binding -> the meta's name for it
                for (const tunediff::Item& it : items) want.emplace(it.path, it.label);
                int lit = 0;
                for (CanvasWidget* wd : c.surf->instances()) mark(buf, wd, want, left, lit);
                buf.pushRectangle(x, y, w, h, clear, 4.f, 1.f, Colors::Border);
                char note[160];
                std::snprintf(note, sizeof note, "%d of %zu highlighted on this page", lit, items.size());
                JTextHelper::pushText(buf, x + 4.f, bottom - 12.f - rowH(), note, Colors::TextSecondary, w - 8.f);
                return;
            }
        }
        // No page to draw it on: list it — the setting and this side's value. Scrolled (see input).
        buf.pushRectangle(x, y, w, h, clear, 4.f, 1.f, Colors::Border);
        w = c.scroll.layout({ x, y, w, h }, int(items.size()), rowH());
        c.scroll.drawBar(buf);
        y += 6.f;
        for (int k = c.scroll.first; k < c.scroll.last(); ++k) {
            const tunediff::Item& it = items[size_t(k)];
            buf.pushRectangle(x + 4.f, y - 2.f, w - 8.f, rowH() - 2.f, fill(left), 3.f);
            // The setting, and — when no page shows it — where the firmware files it.
            const std::string name = it.where.empty() ? it.label : it.label + "   (" + it.where + ")";
            JTextHelper::pushText(buf, x + 8.f, y, name, Colors::TextPrimary, w * 0.6f - 12.f);
            char v[96];
            if (it.table) std::snprintf(v, sizeof v, "map, %d cells", it.cells);
            else if (it.isText) std::snprintf(v, sizeof v, "%s", (left ? it.textA : it.textB).c_str());
            else std::snprintf(v, sizeof v, "%.*f", it.decimals, left ? it.a : it.b);
            JTextHelper::pushText(buf, x + w * 0.6f, y, v, Colors::TextPrimary, w * 0.4f - 12.f);
            y += rowH();
        }
    }

    void step(int col, int d) {
        Column& c = m_cols[col];
        const int n = c.pageCount();
        if (n <= 0) return;
        c.page = ((c.page + d) % n + n) % n;
        c.scroll.reset();
    }
    void finish(bool go) { if (m_done) return; m_done = true; if (m_onResult) m_onResult(go); }
    void _roots() {
        std::vector<jf::JWidget*> v;
        for (Column& c : m_cols) { if (c.prev && c.prev->isVisible()) v.push_back(c.prev.get());
                                   if (c.next && c.next->isVisible()) v.push_back(c.next.get()); }
        if (m_box) v.push_back(m_box.get());
        m_focus.setFocusRoots(std::move(v));
    }

    std::string m_title, m_footnote;
    std::vector<fwkits::Note> m_notes;
    dlgchrome::ListScroll m_notesScroll;
    std::function<void(bool)> m_onResult;
    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{ 0 };
    jf::JSceneGraph  m_graph;
    std::unique_ptr<jf::JDialogButtonBox> m_box;
    Column m_cols[2];
    jf::JFocusManager m_focus;
    bool m_seeded{ false }, m_done{ false }, m_placed{ false };
    dlgchrome::DialogChrome m_chrome;
    uint32_t m_sw{ kW }, m_sh{ kH };                  // the surface's current size
};
