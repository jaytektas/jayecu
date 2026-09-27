#pragma once

// ExpressionEditor — a data-source / visibility EXPRESSION builder (evaluated by MathEvaluator). Layout,
// top to bottom: a FILTER box (narrows the field tree, like the Dictionary dock), the categorised field
// TREE (always visible — Telemetry/Config/Widgets), the EXPRESSION line, and a live FEEDBACK line. Double-
// click (or select + Enter from the filter) a tree leaf to insert its [sigil] token at the caret. The
// feedback line evaluates the expression on every change and reports its live value plus whether it is a
// single WRITABLE config reference (a control bound to it can push edits), a READ-ONLY reference/expression,
// or references an UNKNOWN field. OK hands the expression back via onApply; empty = always visible.

#include <j/core/JWidget.h>
#include <j/core/JStyle.h>
#include <j/core/FocusManager.h>
#include <j/core/JTextHelper.h>
#include <j/core/JTitleBar.h>   // the ONE canonical styled title bar — no custom chrome
#include <j/core/JButton.h>
#include <j/core/JLineEdit.h>
#include <j/core/JTreeView.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#include "../model/MathEvaluator.h"
#include "../model/ExprCompiler.h"   // firmware mode: compile the same source to ECU bytecode
#include "../model/Cache.h"          // isConfig/isTable — writability of a raw table/curve data source
#include "WrapText.h"

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>

class ExpressionEditor {
public:
    static constexpr uint32_t kW = 540, kH = 460;
    static constexpr float kBtnW = 92.f;
    static float kBtnH() { return jf::JStyle::current().buttonHeight; }   // HEIGHT from JStyle (single source of truth)
    static float hdrH() { return jf::JStyle::current().titleBarHeight; }

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    // Where the expression will RUN. Null (the default) = the studio evaluates it, which is the
    // visibility/enable/data-source case: the feedback line shows its live value. Non-null = it is
    // compiled for the ECU, and the feedback line shows the compile result and the program size
    // instead, because "what does it evaluate to right now" is not the question being asked of a
    // gate that runs on the MCU.
    //
    // ONE editor either way — there is one language (see ExprAst.h), so the field tree, the
    // grammar and the insertion behaviour are identical. Only the feedback differs.
    struct FirmwareTarget {
        const MetaModel* meta = nullptr;
        uint16_t         blockSize = 0;    // the field's program capacity, from its meta `length`
    };

    // `element` is the ELEMENT IN SCOPE where this expression will live — the host viewport's data source
    // ("trigger.streams[0]"), or "" at the top level. The preview evaluates inside it, so a "[*]" in a
    // template page's expression tests against the element the page is about instead of resolving to
    // nothing: without it the live test read 0 for every path, which looks like a wrong expression
    // rather than a missing subject.
    ExpressionEditor(std::string initial, jf::JTreeViewNode sigilRoot,
                     std::function<void(std::string)> onApply,
                     FirmwareTarget firmware, std::string element,
                     jf::JGpuHal& hal, int screenX, int screenY, NativeWinHandleType parent)
        : m_fw(firmware)
        , m_element(std::move(element))
        , m_onApply(std::move(onApply))
        , m_window(std::make_unique<PlatformWinType>("Expression", kW, kH, screenX, screenY,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        using namespace jf;
        indexTree(sigilRoot);                                // build the known / writable token sets
        m_search = std::make_unique<JLineEdit>(m_graph, "filter fields\xE2\x80\xA6");
        m_search->setClearButtonEnabled(true);
        // …and Enter in the FILTER inserts the field the tree has landed on (type, arrow down, Enter),
        // which the routing below also intended and also never reached.
        m_search->onReturnPressed.connect([this] {
            if (!m_tree) return;
            if (JTreeViewNode* s = m_tree->selectedNode(); s && !s->userData.empty()) insertToken(s->userData);
        });
        m_search->onTextChanged.connect([this](const std::string& q) { if (m_tree) m_tree->setFilter(q); });
        m_tree = std::make_unique<JTreeView>(m_graph);
        m_tree->setRootNode(std::move(sigilRoot));
        m_tree->onNodeActivated.connect([this](JTreeViewNode* n) { if (n && !n->userData.empty()) insertToken(n->userData); });
        m_edit = std::make_unique<JLineEdit>(m_graph, "expression \xE2\x80\x94 double-click a field to insert");
        m_edit->setClearButtonEnabled(true);
        m_edit->setText(initial);
        // ENTER IN THE EXPRESSION BOX IS OK. The key routing below has said so for as long as it has
        // existed, and it never ran: jRouteKey delivers the key to the focused control first, and a
        // JLineEdit CONSUMES Return (it commits its validator and emits this), so the dialog-level branch
        // was unreachable whenever the box had focus — which is always, since focus starts there. The field
        // that took the key is the one that says what it was for.
        m_edit->onReturnPressed.connect([this] { applyAndClose(); });
        m_btnCancel = std::make_unique<JButton>(m_graph, "Cancel", kBtnW);
        m_btnOk     = std::make_unique<JButton>(m_graph, "OK", kBtnW);
        m_btnCancel->onClicked.connect([this] { m_done = true; });
        m_btnOk->onClicked.connect([this] { applyAndClose(); });
        m_window->setResizable(true);
        m_window->setResizeTopInset(hdrH());
        m_window->setMinSize(420, 320);
        m_window->grabKeyboardFocus();
        focusField(m_edit.get());                            // typing starts in the expression box
    }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;

        const float W = static_cast<float>(m_window->width()), H = static_cast<float>(m_window->height());
        if (m_window->width() != m_lastW || m_window->height() != m_lastH) {
            hal.resizeSurface(m_surface, m_window->width(), m_window->height());
            m_lastW = m_window->width(); m_lastH = m_window->height();
        }
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease(), held = m_window->isLeftButtonDown();

        const bool inTitle = (my >= 0.f && my < hdrH());
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        // Layout — top to bottom: filter, tree, expression, feedback, buttons.
        const float pad = 12.f, gap = 6.f, ctlH = JStyle::current().controlHeight, lh = JTextHelper::lineHeight();
        const float btnY = H - kBtnH() - 10.f;
        // The feedback WRAPS (a compile error is a sentence), so it is measured first and the tree gives
        // up the room; it used to be one clipped line, with the [*] line under it drawn into the buttons.
        const float fbY  = btnY - std::max(lh, drawFeedback(nullptr, pad, 0.f, W - 2 * pad)) - 8.f;
        const float editY = fbY - ctlH - gap;
        const float filterY = hdrH() + 8.f;
        const float treeY = filterY + ctlH + gap;
        const float treeH = std::max(40.f, editY - treeY - 8.f);
        const JRect searchR{ pad, filterY, W - 2 * pad, ctlH };
        const JRect editR{ pad, editY, W - 2 * pad, ctlH };
        m_search->setBounds(searchR);
        m_tree->setBounds({ pad, treeY, W - 2 * pad, treeH });
        m_edit->setBounds(editR);
        m_btnOk->setBounds({ W - kBtnW - pad, btnY, kBtnW, kBtnH() });
        m_btnCancel->setBounds({ W - 2 * kBtnW - pad - 8.f, btnY, kBtnW, kBtnH() });

        // Keys: Escape/Enter are dialog-level; everything else goes to the focused field, except arrow /
        // Enter while the FILTER is focused, which drive the tree (type-to-filter then arrow-down + Enter).
        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            using K = JKeyEvent::JKey;
            if (ke.key == K::Escape) { m_done = true; return false; }
            _refreshFocusRoots();
            if (jf::jRouteKey(ke, m_focus)) continue;   // focused control, then Tab / Shift-Tab
            if (m_focused == m_search.get()) {
                if (ke.key == K::Up || ke.key == K::Down || ke.key == K::Left || ke.key == K::Right) { m_tree->handleKeyEvent(ke); continue; }
                if (ke.key == K::Return) { if (JTreeViewNode* s = m_tree->selectedNode(); s && !s->userData.empty()) insertToken(s->userData); continue; }
                m_search->handleKeyEvent(ke);
                continue;
            }
            // Enter with NOTHING focused (the focus ring is on neither field) still means OK. When a field
            // has it, that field's own onReturnPressed has already run — see the constructor.
            if (ke.key == K::Return) { applyAndClose(); return false; }
            m_edit->handleKeyEvent(ke);
        }

        // Mouse: focus the field under a press; deliver moves/press/release to every control.
        if (pressed) {
            if (inRect(searchR, mx, my))    focusField(m_search.get());
            else if (inRect(editR, mx, my)) focusField(m_edit.get());
        }
        m_search->handleMouseMove(mx, my); m_edit->handleMouseMove(mx, my); m_tree->handleMouseMove(mx, my);
        if (pressed) {
            m_search->handleMousePress(mx, my); m_edit->handleMousePress(mx, my); m_tree->handleMousePress(mx, my);
            m_btnCancel->handleMousePress(mx, my); m_btnOk->handleMousePress(mx, my);
        }
        if (released) {
            m_search->handleMouseRelease(mx, my); m_edit->handleMouseRelease(mx, my); m_tree->handleMouseRelease(mx, my);
            m_btnCancel->handleMouseRelease(mx, my); m_btnOk->handleMouseRelease(mx, my);
        }
        if (const float wheel = m_window->consumeWheel(); wheel != 0.f) m_tree->handleScroll(mx, my, wheel);
        // A click can steal framework focus from our line edits (the tree grabs it); re-assert ours so the
        // caret keeps blinking in the field the dialog considers focused.
        if (m_focused) m_focused->setFocused(true);
        if (m_done) return false;

        m_graph.setHostWindow(m_window->screenX(), m_window->screenY(), static_cast<std::uintptr_t>(m_window->rawWindowId()));

        buf.clear();
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface0);
        // TWO JOBS, TWO TITLES. The same window edits a page's own data source and a setting the ECU
        // runs; calling the second a "visibility expression" sent people looking for what it hides.
        jf::JTitleBar::draw(buf, 0.f, 0.f, W, hdrH(),
                            m_fw.meta ? "ECU expression" : "Data source / visibility expression",
                            jf::JStyle::current().cornerRadius, 0, 12.f);
        m_search->populateRenderPrimitives(buf);
        m_tree->populateRenderPrimitives(buf);
        m_edit->populateRenderPrimitives(buf);
        drawFeedback(&buf, pad, fbY, W - 2 * pad);
        m_btnCancel->populateRenderPrimitives(buf);
        m_btnOk->populateRenderPrimitives(buf);

        auto frame = hal.beginFrame(m_surface);
        hal.drawPrimitives(buf);
        hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    static bool inRect(const jf::JRect& r, float x, float y) { return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height; }
    static std::string trim(const std::string& s) {
        const size_t a = s.find_first_not_of(" \t"); if (a == std::string::npos) return "";
        return s.substr(a, s.find_last_not_of(" \t") - a + 1);
    }

    void focusField(jf::JLineEdit* f) {
        m_focused = f;
        m_edit->setFocused(f == m_edit.get());
        m_search->setFocused(f == m_search.get());
    }

    // Insert a field's token at the expression caret (JLineEdit resets the caret to the end on setText,
    // which lands naturally just past the inserted token), then return focus to the expression.
    void insertToken(const std::string& tok) {
        const std::string& t = m_edit->text();
        const size_t c = std::min(m_edit->caret(), t.size());
        m_edit->setText(t.substr(0, c) + tok + t.substr(c));
        focusField(m_edit.get());
    }

    // Flatten the field tree into the known-token set (every insertable token + its bare inner path) and
    // the writable subset (the '#' config leaves — a single one of these is a push-back-capable source).
    void indexTree(const jf::JTreeViewNode& n) {
        if (!n.userData.empty()) {
            const std::string& tok = n.userData;
            m_known.insert(tok);
            if (tok.size() >= 3 && tok.front() == '[' && tok.back() == ']') {
                const char sig = tok[1];
                const std::string path = tok.substr(2, tok.size() - 3);   // strip "[<sig>" and "]"
                m_known.insert(path);
                if (sig == '#') { m_writable.insert(tok); m_writable.insert(path); }
            } else if (Cache::instance().isConfig(tok) || Cache::instance().isTable(tok)) {
                // A raw leaf is a table/curve data source kept unwrapped; it's writable when config-backed —
                // its curve/table control pushes cell edits to the ECU (telemetry sources stay read-only).
                m_writable.insert(tok);
            }
        }
        for (const auto& c : n.children) indexTree(c);
    }

    // Pull the field references out of an expression (bracket sigils + bare dotted/subscripted names),
    // skipping numbers, operators, and function calls (a name immediately followed by '(').
    std::vector<std::string> refsOf(const std::string& e) const {
        std::vector<std::string> refs;
        const size_t n = e.size();
        for (size_t i = 0; i < n;) {
            const char c = e[i];
            if (c == '[') {                                  // balanced bracket sigil
                int depth = 0; size_t k = i;
                for (; k < n; ++k) { if (e[k] == '[') ++depth; else if (e[k] == ']' && --depth == 0) { ++k; break; } }
                if (depth != 0) break;
                refs.push_back(e.substr(i, k - i)); i = k; continue;
            }
            if (std::isalpha((unsigned char)c) || c == '_') {   // bare name (may carry .field / [idx])
                size_t j = i;
                while (j < n) {
                    const char d = e[j];
                    if (std::isalnum((unsigned char)d) || d == '_' || d == '.') { ++j; continue; }
                    if (d == '[') { int depth = 0; size_t k = j; for (; k < n; ++k) { if (e[k] == '[') ++depth; else if (e[k] == ']' && --depth == 0) { ++k; break; } } if (depth != 0) { j = k; } j = k; continue; }
                    break;
                }
                size_t p = j; while (p < n && std::isspace((unsigned char)e[p])) ++p;
                if (p >= n || e[p] != '(') refs.push_back(e.substr(i, j - i));   // '(' → a function, not a field
                i = j; continue;
            }
            ++i;
        }
        return refs;
    }

    // Feedback under the expression: live value + a writable / read-only / unknown-field badge.
    // A null buffer MEASURES: the same text, wrapped the same way, so the layout and the drawing agree.
    // Returns the height it takes.
    float drawFeedback(jf::JPrimitiveBuffer* buf, float x, float y, float w) {
        using namespace jf;
        static const uint8_t green[4] = {0x3f, 0xb9, 0x50, 255}, amber[4] = {0xd2, 0x9a, 0x2a, 255};
        const std::string expr = trim(m_edit->text());
        // Wrapped to what is left of the row from `px`; returns the height taken.
        auto put = [&](float px, float py, const std::string& t, const uint8_t* c) {
            const float room = std::max(40.f, x + w - px);
            if (buf && JTextHelper::hasAtlas()) return wraptext::draw(*buf, px, py, t, c, room);
            return wraptext::height(t, room);
        };

        // --- firmware mode: does it COMPILE, and does it fit? ---
        if (m_fw.meta) {
            static const uint8_t red[4] = {0xd0, 0x50, 0x50, 255};
            if (expr.empty()) {
                // Not "always armed": an empty program means the setting's OWN built-in rule
                // (launch arms standing still, learning follows its gates) — the same thing the
                // box on the page says.
                return put(x, y, "empty \xE2\x80\x94 the setting's built-in rule applies (see its help)",
                           Colors::TextSecondary);
            }
            const auto r = ExprCompiler::compile(expr, *m_fw.meta,
                                                 (uint32_t)m_fw.meta->configSize(), m_fw.blockSize);
            if (!r.ok) return put(x, y, "\xE2\x9A\xA0 " + r.error, red);
            char sb[64];
            std::snprintf(sb, sizeof(sb), "\xE2\x97\x8F compiles \xE2\x80\x94 %zu of %u bytes",
                          r.code.size(), (unsigned)m_fw.blockSize);
            float h = put(x, y, sb, green);
            // What the ECU will actually run, normalised. Seeing it is how you catch a gate that
            // parsed as something other than what you meant.
            const std::string back = ExprCompiler::decompile(r.code.data(), (uint16_t)r.code.size(),
                                                             *m_fw.meta);
            if (!back.empty() && back != expr) {
                const float bx2 = x + JTextHelper::measureWidth(sb) + 14.f;
                h = std::max(h, put(bx2, y, "\xE2\x86\x92 " + back, Colors::TextSecondary));
            }
            return h;
        }

        if (expr.empty()) return put(x, y, "no data source \xE2\x80\x94 always visible", Colors::TextSecondary);
        const std::vector<std::string> refs = refsOf(expr);
        std::string unknown;
        // A ref is checked as it will RESOLVE. "trigger.streams[*].cell_len" is in no field tree — it is a
        // template — so checking it verbatim reported the page's own bindings as unknown fields.
        for (const auto& r : refs)
            if (!m_known.count(MathEvaluator::resolveTemplate(r, m_element))) { unknown = r; break; }

        MathEvaluator::ElementScope scope(m_element);   // "[*]" means the element this page is about
        char vb[48]; std::snprintf(vb, sizeof(vb), "= %g", MathEvaluator::instance().evaluate(m_edit->text()));
        const std::string val = vb;
        float h = put(x, y, val, Colors::TextPrimary);
        const float bx = x + JTextHelper::measureWidth(val) + 14.f;

        if (!unknown.empty()) {
            h = std::max(h, put(bx, y, "\xE2\x9A\xA0 unknown field: " + unknown, amber));
        } else if (refs.size() == 1 && trim(refs[0]) == expr && m_writable.count(refs[0])) {
            h = std::max(h, put(bx, y, "\xE2\x97\x8F writable (edits push to the ECU)", green));
        } else {
            h = std::max(h, put(bx, y, "\xE2\x97\x8B read-only (display only)", Colors::TextSecondary));
        }
        // Which element the number is FOR. A template's live value is meaningless without it — and it is
        // the answer to "why does this read 0", which is otherwise a guess.
        if (expr.find("[*]") != std::string::npos) {
            const std::string el = m_element.empty()
                ? std::string("\xE2\x9A\xA0 [*] has no element here \xE2\x80\x94 open this from the page it belongs to")
                : "[*] = " + MathEvaluator::elementKey(m_element);
            h += 2.f + put(x, y + h + 2.f, el, m_element.empty() ? amber : Colors::TextSecondary);
        }
        return h;
    }

    // In firmware mode a program that does not compile must not be applied: the ECU would reject
    // it and the gate would silently revert to always-armed. The feedback line already says why.
    void applyAndClose() {
        if (m_fw.meta && !compileOk()) return;
        if (m_onApply) m_onApply(m_edit->text());
        m_done = true;
    }

    bool compileOk() const {
        if (!m_fw.meta) return true;
        return ExprCompiler::compile(m_edit->text(), *m_fw.meta,
                                     (uint32_t)m_fw.meta->configSize(), m_fw.blockSize).ok;
    }

    FirmwareTarget m_fw;
    std::string    m_element;      // the element "[*]" means here (see the ctor)
    std::function<void(std::string)> m_onApply;
    // This dialog's focus tree, declared because JWidget takes a scene graph rather than a parent, so
    // members are not otherwise discoverable. Clicks focus by themselves (JControl::handleMousePress calls
    // requestFocus, which routes to this manager while the dialog is open); this adds Tab traversal and
    // sends keys to the focused control.
    void _refreshFocusRoots() {
        std::vector<jf::JWidget*> roots;
        if (m_edit) roots.push_back(m_edit.get());
        if (m_search) roots.push_back(m_search.get());
        if (m_tree) roots.push_back(m_tree.get());
        if (m_btnCancel) roots.push_back(m_btnCancel.get());
        if (m_btnOk) roots.push_back(m_btnOk.get());
        m_focus.setFocusRoots(std::move(roots));
    }

    jf::JFocusManager m_focus;

    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{ 0 };
    jf::JSceneGraph  m_graph;
    std::unique_ptr<jf::JLineEdit> m_edit, m_search;
    std::unique_ptr<jf::JTreeView> m_tree;
    std::unique_ptr<jf::JButton>   m_btnCancel, m_btnOk;
    jf::JLineEdit*   m_focused = nullptr;               // which line edit takes keystrokes (edit or filter)

    std::set<std::string> m_known;                     // every insertable token + its bare inner path
    std::set<std::string> m_writable;                  // the '#' config subset (single-ref = writable source)

    uint32_t m_lastW{ kW }, m_lastH{ kH };
    float    m_ax{ 0 }, m_ay{ 0 };
    bool     m_done{ false }, m_drag{ false };
};
