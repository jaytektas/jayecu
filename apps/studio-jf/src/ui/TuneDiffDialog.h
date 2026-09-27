#pragma once

// TuneDiffDialog — the ECU does not match the saved tune. Here is what differs; now choose.
//
// The studio used to ask this with two buttons and nothing else: keep the ECU's tune, or push yours,
// over 141 KB of config with no way to tell one changed idle target from a different engine. That is
// not a decision, it is a coin toss with an engine on the other end.
//
// So the comparison IS the prompt. It opens on connect, shows the two tunes side by side with the
// differences highlighted, pages through only the pages that differ, and then asks for one tune or the
// other, with no per-setting picking anywhere in it.
//
// THE PAGE IS THE REPORT. Not a list of paths and numbers standing in for a page — the page itself,
// drawn twice, its own controls in their own places, once against each tune. A tuner recognises the
// Idle Control page at a glance and can see WHICH of its fields is highlighted; nobody recognises
// "idle.idle_lockout_rpm" and nobody can tell from a list whether the changed field is the one that
// matters. Both sides are the same page, so the eye compares positions rather than reading rows.
//
// HOW TWO TUNES FIT ON ONE SCREEN: every control reads its value from the Cache singleton, from over
// two hundred places, so the image is swapped underneath the left render and swapped back before the
// right one (Cache::ImageScope). Neither surface knows there is a second tune; they differ only in
// which bytes were in place while they painted.
//
// ONLY PAGES THAT DIFFER, and << >> walk them — a report of every page would be a report of nothing.
// Settings the document shows on no page at all still appear, as a list on a final page, because
// "no differences" over a value no page binds would be exactly the lie this exists to prevent.

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/JTextHelper.h>
#include <j/core/JTitleBar.h>
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
#include "DialogChrome.h"
#include "WrapText.h"
#include "../model/Cache.h"
#include "../surface/Surface.h"
#include "../surface/CanvasWidget.h"
#include "../surface/PanelLibrary.h"
#include "../surface/PanelModel.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

class TuneDiffDialog {
public:
    // SIZED TO TWO PAGES, SIDE BY SIDE. Pages are authored at 1280x700 and both are scaled to fit, so
    // the readable scale is set by the column width and nothing else: the wider this is, the larger the
    // numbers. 1820 gives each column ~890 px — about 0.7 scale — and still leaves a margin on a 1920
    // screen, which is the narrowest a tuning machine tends to be.
    //
    // THE HEIGHT FOLLOWS FROM THAT, it is not a second free choice. A column taller than the page it
    // scales into is empty space that made the text smaller for nothing: 890 wide fits a 1280x700 page
    // in 487, and the rest of this figure is the header, the note and the buttons.
    static constexpr uint32_t kW = 1820, kH = 700;
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

    // result: 0 = keep the ECU's tune, 1 = push the local tune, -1 = neither (dismissed).
    //
    // The two images are taken BY VALUE and kept: they are what the two sides are drawn against, for as
    // long as the dialog is open, and the caller's copies are not ours to depend on.
    TuneDiffDialog(std::string title, std::string leftName, std::string rightName,
                   tunediff::Report report,
                   std::vector<uint8_t> leftImage, std::vector<uint8_t> rightImage,
                   PanelLibrary* lib,
                   std::function<void(int)> onResult,
                   jf::JGpuHal& hal, int screenX, int screenY, NativeWinHandleType parent)
        : m_title(std::move(title)), m_left(std::move(leftName)), m_right(std::move(rightName))
        , m_report(std::move(report)), m_imgA(std::move(leftImage)), m_imgB(std::move(rightImage))
        , m_lib(lib), m_onResult(std::move(onResult))
        , m_window(std::make_unique<PlatformWinType>(m_title, kW, kH, screenX, screenY,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        m_box = std::make_unique<jf::JDialogButtonBox>(m_graph);
        // The two answers, named as what they DO. The one that overwrites the ECU is Destructive: it is
        // the only one here that changes the thing the engine runs on.
        // Worded, as the reference does it: "<<" alone is a symbol you have to interpret while holding
        // a decision about an engine in your head.
        m_prev = m_box->addButton("<< Previous", jf::JDialogButtonBox::Role::Action, 110.f);
        m_next = m_box->addButton("Next >>",     jf::JDialogButtonBox::Role::Action, 96.f);
        jf::JButton* keep = m_box->addButton("Keep the ECU's tune", jf::JDialogButtonBox::Role::Accept, 190.f);
        jf::JButton* push = m_box->addButton("Push " + m_left + " to the ECU",
                                             jf::JDialogButtonBox::Role::Destructive, 210.f);
        // AND A WAY OUT THAT IS A BUTTON. Escape and the close box already mean "neither", but a
        // decision this size should not require knowing that: one of the other two overwrites a tune,
        // and the reader who is not ready to choose needs somewhere to click that is not either of them.
        m_box->addButton("Exit - no changes", jf::JDialogButtonBox::Role::Reject, 150.f);
        m_prev->onClicked.connect([this] { step(-1); });
        m_next->onClicked.connect([this] { step(+1); });
        keep->onClicked.connect([this] { finish(0); });
        push->onClicked.connect([this] { finish(1); });
        m_box->onReject.connect([this] { finish(-1); });
        m_chrome.setup(*m_window, hdrH(), 900, 480);   // moves, resizes, minimises, maximises (DialogChrome)
        m_window->grabKeyboardFocus();
    }

    ~TuneDiffDialog() { m_surfL.reset(); m_surfR.reset(); }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) { finish(-1); return false; }
        // A DIALOG THIS SIZE DOES NOT FIT EVERY SCREEN. kW/kH are what openModal centres on, chosen so
        // two pages are readable — but on a machine narrower than that they would put half the report
        // past the edge, and the report takes no mouse, so nothing could bring it back. The first frame
        // therefore fits the window to the screen it actually landed on and re-centres it. Once only:
        // after that the placement is the user's.
        if (!m_placed) {
            m_placed = true;
            const auto [sw, sh] = m_window->screenSize();
            const uint32_t w = std::min<uint32_t>(kW, sw > 60 ? uint32_t(sw - 40) : kW);
            const uint32_t h = std::min<uint32_t>(kH, sh > 80 ? uint32_t(sh - 60) : kH);
            if (w != kW || h != kH) {
                m_window->setSize(w, h);
                hal.resizeSurface(m_surface, w, h);
                m_sw = w; m_sh = h;
            }
            const int x = std::max(0, std::min(m_window->screenX(), sw - int(w)));
            const int y = std::max(0, std::min(m_window->screenY(), sh - int(h)));
            if (x != m_window->screenX() || y != m_window->screenY()) m_window->setPosition(x, y);
        }
        // THE WINDOW'S OWN SIZE, not the constant it asked for: the two part company the moment the
        // screen is too small, and a layout drawn to kW on a window of w puts the buttons off the edge.
        if (m_window->width() != m_sw || m_window->height() != m_sh) {   // the user resized it
            m_sw = m_window->width(); m_sh = m_window->height();
            hal.resizeSurface(m_surface, m_sw, m_sh);
        }
        const float W = static_cast<float>(m_window->width()), H = static_cast<float>(m_window->height());
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease();
        const bool held = m_window->isLeftButtonDown();
        if (m_chrome.input(*m_window, W, hdrH(), mx, my, pressed, held)) { finish(-1); return false; }   // [x] = no changes
        m_scroll.input(mx, my, pressed, held, m_window->consumeWheel());
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
        _roots();
        if (pressed) jf::jRouteMouse(mx, my, m_focus);
        if (!m_seeded) { m_seeded = true; m_focus.focusFirst(); }
        // THE PAGES ARE EVIDENCE, NOT CONTROLS. No mouse or key event is routed into either surface:
        // they show what each tune says, and the only things you can operate here are the five buttons.
        // A field you could type into on the report would be editing a tune that has not been chosen.
        m_box->handleMouseMove(mx, my);
        if (pressed)  m_box->handleMousePress(mx, my);
        if (released) m_box->handleMouseRelease(mx, my);
        if (m_done) return false;

        buf.clear();
        buf.pushRectangle(0.f, 0.f, W, H, Colors::DialogBg, 8.f, 1.f, Colors::Border);
        m_chrome.draw(buf, *m_window, W, hdrH(), m_title, mx, my);
        if (JTextHelper::hasAtlas()) _draw(buf, W, by);
        m_box->populateRenderPrimitives(buf);

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    // The tint on a differing control. Yellow, as the reference uses, and translucent so it marks the
    // control rather than replacing it — the number underneath is the whole point of looking.
    static const uint8_t* hiColour() { static const uint8_t c[4] = { 235, 200, 70,  70 }; return c; }
    static const uint8_t* hiEdge()   { static const uint8_t c[4] = { 214, 158, 20, 255 }; return c; }

    int pageCount() const {
        return static_cast<int>(m_report.pages.size()) + (m_report.unpaged.empty() ? 0 : 1);
    }
    bool onUnpaged() const { return m_page >= static_cast<int>(m_report.pages.size()); }

    // THE SURFACES ARE REBUILT PER PAGE, not repointed: a Surface is constructed around one model and
    // that is the honest way to change which page it shows. Two pages is two objects; paging is cheap
    // next to the decision being made over it.
    void ensurePage() {
        if (m_builtPage == m_page) return;
        m_builtPage = m_page;
        m_surfL.reset(); m_surfR.reset(); m_model = nullptr;
        if (!m_lib || onUnpaged()) return;
        const std::string& node = m_report.pages[static_cast<size_t>(m_page)].node;
        PanelModel* pm = m_lib->find(node);
        if (!pm || pm->elements().empty()) return;
        m_model = pm;
        auto make = [&] {
            auto s = std::make_unique<Surface>(m_graph, Cache::instance(), m_model);
            s->setMode(Surface::Mode::Run);
            s->setActiveNode(node);
            s->setFitToView(true);   // the whole page, or a difference could sit off the edge unreachably
            // Viewports on the page resolve their target through the same library, so a page built out
            // of viewports renders rather than showing two empty frames.
            s->setPageAccess([this](const std::string& p) -> PanelModel* {
                                 return m_lib ? &m_lib->forNode(p) : nullptr; },
                             [] {});
            return s;
        };
        m_surfL = make();
        m_surfR = make();
    }

    // The bindings that differ on the page in front of us.
    std::set<std::string> wanted() const {
        std::set<std::string> w;
        if (onUnpaged()) return w;
        for (const tunediff::Item& it : m_report.pages[static_cast<size_t>(m_page)].items)
            w.insert(it.path);
        return w;
    }

    // Mark every control whose binding differs. Runs AFTER the surface has painted, because that is when
    // the instances carry screen bounds — the camera bakes them at paint time — and the tint belongs over
    // the control anyway.
    //
    // WALKED DOWN THE LIVE WIDGET TREE, not across the model's element list. A panel's children are not
    // elements of the page, so a model walk finds only what sits at the top level — one checkbox, on
    // the page this was first built against — and every control inside a panel went unmarked while the
    // note underneath claimed the page was fully covered. bindPath() is the resolved binding, so a
    // templated child inside a mirrored viewport reports the setting it actually reads.
    void mark(jf::JPrimitiveBuffer& buf, CanvasWidget* w, const std::set<std::string>& want, int& n) {
        if (!w) return;
        const std::string b = w->bindPath();
        if (!b.empty() && want.find(b) != want.end()) {
            const jf::JRect r = w->bounds();
            if (r.width > 0.f && r.height > 0.f) {
                static const uint8_t clear[4] = { 0, 0, 0, 0 };
                const bool small = r.width * r.height < 40000.f;
                buf.pushRectangle(r.x - 3.f, r.y - 3.f, r.width + 6.f, r.height + 6.f,
                                  small ? hiColour() : clear, 3.f, 2.f, hiEdge());
                ++n;
            }
        }
        for (CanvasWidget* c : w->childWidgets()) mark(buf, c, want, n);
    }

    int highlight(jf::JPrimitiveBuffer& buf, Surface* s, const std::set<std::string>& want) {
        if (!s || !m_model) return 0;
        int n = 0;
        for (CanvasWidget* w : s->instances()) mark(buf, w, want, n);
        return n;
    }

    void _draw(jf::JPrimitiveBuffer& buf, float W, float bottom) {
        using namespace jf;
        ensurePage();
        const int n = pageCount();
        float y = hdrH() + 12.f;

        // WHERE YOU ARE IN THE REPORT, and how much of it there is. A page of differences with no count
        // reads as the whole story when it may be one of six.
        char hdr[220];
        const std::string where = onUnpaged() ? std::string("Settings the document shows on no page")
                                              : m_report.pages[static_cast<size_t>(m_page)].node;
        std::snprintf(hdr, sizeof hdr, "Page %d of %d   \xC2\xB7   %s   \xC2\xB7   %d setting(s) differ in total",
                      m_page + 1, std::max(1, n), where.c_str(), m_report.settings);
        // Wrapped (a long page name ran it off the edge); what is below moves down by the extra lines.
        y += wraptext::draw(buf, 16.f, y, hdr, Colors::TextPrimary, W - 32.f)
           - JTextHelper::lineHeight() + rowH() + 2.f;

        const float pad = 12.f, gap = 12.f;
        const float colW = (W - 2.f * pad - gap) / 2.f;
        const float colRx = pad + colW + gap;

        if (m_surfL && m_surfR && m_model) {
            // The column heads say WHOSE each side is, because that is the whole question. They sit over
            // the column they name — a heading anywhere else is a label for the wrong numbers.
            JTextHelper::pushText(buf, pad + 4.f,   y, m_left,  Colors::TextSecondary, colW - 8.f);
            JTextHelper::pushText(buf, colRx + 4.f, y, m_right, Colors::TextSecondary, colW - 8.f);
            y += rowH();
            const std::set<std::string> want = wanted();
            // AND SAY WHAT COULD NOT BE POINTED AT. A setting can differ on a page that reaches it
            // through something this cannot locate — a viewport, a template. Reporting only the tinted
            // ones would quietly under-count the difference the reader is deciding over. Worked out
            // BEFORE the pages are placed: the note wraps, and the pages end where its lines begin.
            char note[400];
            const int total = static_cast<int>(want.size());
            std::string missing;
            for (const tunediff::Item& it : m_report.pages[static_cast<size_t>(m_page)].items) {
                if (missing.size() > 200u) { missing += ", \xE2\x80\xA6"; break; }
                if (!missing.empty()) missing += ", ";
                missing += it.label;
            }
            const float noteExtra = wraptext::extra(
                "0000 of 0000 differing setting(s) highlighted \xE2\x80\x94 the rest are on this page but "
                "not locatable as a control: " + missing, W - 32.f);   // the longer of the two wordings
            float top = y, h = bottom - 12.f - rowH() - noteExtra - top;
            if (h < 80.f) h = 80.f;
            m_surfL->setBounds({ pad,   top, colW, h });
            m_surfR->setBounds({ colRx, top, colW, h });

            // LEFT AGAINST ONE TUNE, RIGHT AGAINST THE OTHER. The scope closes before the next one opens;
            // nothing outside these two statements ever sees a borrowed image.
            int lit = 0;
            { Cache::ImageScope scope(Cache::instance(), m_imgA); m_surfL->populateRenderPrimitives(buf); }
            lit = highlight(buf, m_surfL.get(), want);
            { Cache::ImageScope scope(Cache::instance(), m_imgB); m_surfR->populateRenderPrimitives(buf); }
            highlight(buf, m_surfR.get(), want);

            // A FRAME AROUND EACH SIDE, over the page it encloses. The fill is fully transparent rather
            // than absent: pushRectangle dereferences the fill colour it is handed, so "no fill" is a
            // colour with no alpha, not a null pointer.
            static const uint8_t clear[4] = { 0, 0, 0, 0 };
            buf.pushRectangle(pad,   top, colW, h, clear, 4.f, 1.f, Colors::Border);
            buf.pushRectangle(colRx, top, colW, h, clear, 4.f, 1.f, Colors::Border);

            if (lit >= total)
                std::snprintf(note, sizeof note, "%d of %d differing setting(s) on this page are highlighted.",
                              lit, total);
            else
                std::snprintf(note, sizeof note,
                              "%d of %d differing setting(s) highlighted \xE2\x80\x94 the rest are on this page but "
                              "not locatable as a control: %s", lit, total, missing.c_str());
            wraptext::draw(buf, 16.f, top + h + 6.f, note, Colors::TextSecondary, W - 32.f);
            m_scroll.clear();                                // a page: nothing here scrolls
            return;
        }

        // NO PAGE TO DRAW — the document does not carry one for this node, or these are the settings it
        // shows nowhere. The values are still the answer to the question, so they are listed.
        _list(buf, W, bottom, y, pad, colW, colRx);
    }

    void _list(jf::JPrimitiveBuffer& buf, float W, float bottom, float y,
               float pad, float colW, float colRx) {
        using namespace jf;
        // THE LIST HAS ITS OWN COLUMNS, so it writes its own heads over them. They used to be drawn at
        // the page columns' positions, which put "current" at the far left of a row whose value sat a
        // third of the way across — a heading for numbers that were somewhere else.
        const float labelW = pad + colW - 24.f;
        const float vxA = pad + colW * 0.55f, vxB = colRx + colW * 0.05f;
        JTextHelper::pushText(buf, 16.f, y, "Setting", Colors::TextSecondary, labelW);
        JTextHelper::pushText(buf, vxA,  y, m_left,    Colors::TextSecondary, vxB - vxA - 12.f);
        JTextHelper::pushText(buf, vxB,  y, m_right,   Colors::TextSecondary, W - vxB - 16.f);
        y += rowH();
        buf.pushRectangle(16.f, y - 4.f, W - 32.f, 1.f, Colors::Border);
        y += 4.f;

        const std::vector<tunediff::Item>& items =
            onUnpaged() ? m_report.unpaged : m_report.pages[static_cast<size_t>(m_page)].items;
        // SCROLLED, as the firmware report's list is (DialogChrome.h): it used to stop at "...more".
        const float rowsW = m_scroll.layout({ 12.f, y - 2.f, W - 24.f, bottom - 8.f - (y - 2.f) },
                                            int(items.size()), rowH());
        m_scroll.drawBar(buf);
        W = rowsW + 24.f;                                   // the rows stop short of the bar
        for (int k = m_scroll.first; k < m_scroll.last(); ++k) {
            const tunediff::Item& it = items[size_t(k)];
            buf.pushRectangle(12.f, y - 2.f, W - 24.f, rowH() - 2.f, hiColour(), 3.f);
            JTextHelper::pushText(buf, 16.f, y, it.label, Colors::TextPrimary, labelW);
            char va[64], vb[64];
            if (it.isText) {                 // a name, a VIN — printed as written, not decoded as a number
                std::snprintf(va, sizeof va, "%s", it.textA.empty() ? "(blank)" : it.textA.c_str());
                std::snprintf(vb, sizeof vb, "%s", it.textB.empty() ? "(blank)" : it.textB.c_str());
            } else if (it.table) {
                std::snprintf(va, sizeof va, "%d cell(s) differ between the two", it.cells);
                vb[0] = '\0';
            } else {
                // AS THE FIELD IS READ, not as a C format string finds convenient. "%.3g" turns 4200 rpm
                // into "4.2e+03", which is a difference nobody can weigh at the moment they are being
                // asked to overwrite an engine's tune.
                std::snprintf(va, sizeof va, "%.*f", it.decimals, it.a);
                std::snprintf(vb, sizeof vb, "%.*f", it.decimals, it.b);
            }
            // A MAP'S COUNT BELONGS TO NEITHER SIDE. It is one fact about the pair, so it spans both
            // value columns rather than sitting under one tune's heading claiming to be its value.
            JTextHelper::pushText(buf, vxA, y, va, Colors::TextPrimary,
                                  it.table ? W - vxA - 16.f : vxB - vxA - 12.f);
            if (vb[0]) JTextHelper::pushText(buf, vxB, y, vb, Colors::TextPrimary, W - vxB - 16.f);
            y += rowH();
        }
    }

    void step(int d) {
        const int n = pageCount();
        if (n <= 0) return;
        m_page = ((m_page + d) % n + n) % n;               // wraps: a report is a loop, not a dead end
        m_scroll.reset();
    }
    void finish(int r) { if (m_done) return; m_done = true; if (m_onResult) m_onResult(r); }
    void _roots() { std::vector<jf::JWidget*> v; if (m_box) v.push_back(m_box.get()); m_focus.setFocusRoots(std::move(v)); }

    std::string m_title, m_left, m_right;
    tunediff::Report m_report;
    // Mutable and owned: Cache::ImageScope swaps these in and out, so they must be non-const lvalues
    // that live as long as the dialog does.
    std::vector<uint8_t> m_imgA, m_imgB;
    PanelLibrary* m_lib{nullptr};
    std::function<void(int)> m_onResult;
    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{ 0 };
    jf::JSceneGraph  m_graph;
    std::unique_ptr<jf::JDialogButtonBox> m_box;
    jf::JButton* m_prev{nullptr};
    jf::JButton* m_next{nullptr};
    std::unique_ptr<Surface> m_surfL, m_surfR;
    PanelModel*  m_model{nullptr};
    int  m_builtPage{ -1 };
    jf::JFocusManager m_focus;
    int  m_page{ 0 };
    bool m_seeded{ false }, m_done{ false }, m_placed{ false };
    dlgchrome::DialogChrome m_chrome;
    dlgchrome::ListScroll   m_scroll;
    uint32_t m_sw{ kW }, m_sh{ kH };                  // the surface's current size
};
