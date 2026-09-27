#pragma once

// PopupSignalPicker — the "pick a data source / signal" chooser, shared by the Properties data-source field
// and the run-mode enum/*_src signal control: a live filter over
// a CATEGORISED signal tree (signals grouped under their module), and a SINGLE CLICK on a leaf chooses it and
// closes. It dismisses on selection, on Escape, on focus loss (click-away), or via the close [x] in the
// right of the header frame. The caller builds the categorised JTreeViewNode (leaf userData = binding path).

#include <j/core/JStyle.h>
#include <j/core/JTitleBar.h>
#include <j/core/JCloseButton.h>
#include <j/core/JClearMark.h>
#include <j/core/JLineEdit.h>
#include <j/core/JTreeView.h>
#include <j/core/MenuSystem.h>
#include <j/graphics/GpuHal.h>
#include <j/graphics/RenderPrimitive.h>

#if defined(_WIN32)
  #include <j/platforms/windows/WindowsPlatformWindow.h>
#else
  #include <j/platforms/linux/LinuxPlatformWindow.h>
#endif

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <string>

class PopupSignalPicker {
public:
    static constexpr uint32_t kW = 320, kH = 440;
    static constexpr float kPad = 10.f, kRowH = 22.f;
    // Title-bar height is the framework's ONE canonical value (JStyle::titleBarHeight) — never hardcoded.
    static float kHeader() { return jf::JStyle::current().titleBarHeight; }
    // Close [x] rect derived by the framework from the header (no per-site size).
    static jf::JRect _closeRect() { return jf::JCloseButton::rectFor({0.f, 0.f, static_cast<float>(kW), kHeader()}); }
    static bool _inRect(const jf::JRect& r, float mx, float my) {
        return mx >= r.x && mx < r.x + r.width && my >= r.y && my < r.y + r.height;
    }
    // Clear-[x] hit/paint box: a square at the right end of the search box (given its y + width).
    static jf::JRect _clearRect(float searchY, float searchW) {
        return { kPad + searchW - kRowH, searchY, kRowH, kRowH };
    }
    // THERE IS NO "CLEAR" FOOTER HERE, and there was. Unassigning belongs on the CONTROL — the combo
    // that opened this popup now carries an ✕ (JComboBox::setClearable) — because a clear buried in a
    // search dialog is two clicks away from the value it clears, and half the selectors that need one
    // never open this popup at all. This popup chooses; it does not un-choose.

#if defined(_WIN32)
    using PlatformWinType     = jf::JWindowsPlatformWindow;
    using NativeWinHandleType = HWND;
#else
    using PlatformWinType     = jf::JLinuxPlatformWindow;
    using NativeWinHandleType = xcb_window_t;
#endif

    // (root, current, onAccept, title) lead; the hal/pos/handle tail is supplied by JAppWindow::openModal.
    // `root` is a tree whose node userData carries the path/token to return (a categorised signal catalogue for
    // the source picker, or the navigation tree for the node picker). `title` labels the popup for its use.
    PopupSignalPicker(jf::JTreeViewNode root, std::string current, std::function<void(std::string)> onAccept,
                        std::string title,
                        jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : m_onAccept(std::move(onAccept)), m_title(std::move(title))
        , m_window(std::make_unique<PlatformWinType>(m_title.c_str(), kW, kH, sx, sy, jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        using namespace jf;
        (void)current;
        m_search = std::make_unique<JLineEdit>(m_graph, "Filter…");
        m_search->setClearButtonEnabled(true);
        m_tree   = std::make_unique<JTreeView>(m_graph);
        m_tree->setRootNode(std::move(root));
        m_tree->setFilterMatchesUserData(true);   // typing a raw signal name (userData) finds it
        // Restore what was open last time. The dictionary keeps ONE tree alive, so its expansion simply
        // persists; this popup builds a fresh tree on every open, so it has to put the state back itself —
        // and for EVERY branch, not just the top-level categories. Remembering only those meant a rummage
        // down to one sensor's diagnostics was thrown away the moment the picker closed, and the next open
        // dropped you back at the module list to walk the same path again.
        //
        // Keyed by the full row path, which is what identifies a branch across two builds of the tree; the
        // labels are the same because the same builder makes them.
        _restoreExpansion(m_tree->root(), {});
        m_tree->root().expanded = true;
        m_search->onTextChanged.connect([this](const std::string& q) { m_tree->setFilter(q); });
        // Single click on a bindable leaf (userData set) chooses it and closes — like the old popup.
        m_tree->onNodeActivated.connect([this](JTreeViewNode* n) {
            if (n && !n->userData.empty()) { if (m_onAccept) m_onAccept(n->userData); m_done = true; }
        });
        // Return chooses the KEYBOARD-selected leaf, the same action a click performs. Without this the
        // popup could only be driven by mouse: every key went to the filter field (below), so the tree
        // never saw Up/Down and there was no way to commit a highlighted row.
        m_tree->onEnterKey.connect([this] { _acceptSelected(); });
        // The same right-click menu the dictionary has, for the same reason: this is the same tree, and
        // after a filter has forced a dozen branches open there has to be a way to shut them again. The
        // items act on the row under the cursor, which is what you want after a rummage.
        m_tree->setContextMenu(&_contextMenu(m_graph, m_tree.get()));
        m_window->grabKeyboardFocus();   // take focus so a click-away (focus lost) can dismiss us
    }

    // Commit the current keyboard selection (a leaf carries its binding path in userData).
    void _acceptSelected() {
        JTreeViewNode* n = m_tree ? m_tree->selectedNode() : nullptr;
        if (n && !n->userData.empty()) { if (m_onAccept) m_onAccept(n->userData); m_done = true; }
    }

    ~PopupSignalPicker() {
        // Remember the open/closed state for the next open — the arrangement the USER made, not whatever a
        // search revealed. A filter expands the paths to its matches as REAL state (JTreeView.h:159), so
        // closing while a query stands would otherwise record the query's shape and every later open would
        // start splayed open down to whatever was last searched for. Clearing the filter first puts the
        // pre-search arrangement back, which is exactly what is worth remembering.
        if (m_tree) {
            m_tree->setFilter({});
            s_expanded.clear();
            _captureExpansion(m_tree->root(), {});
            s_seeded = true;
        }
    }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

private:
    // Built once, on first use, and pointed at whichever picker is open — a JMenu is a model, and the
    // tree it acts on is the one that asked for it.
    static jf::JMenu& _contextMenu(jf::JSceneGraph& g, jf::JTreeView* tree) {
        static jf::JTreeView* s_tree = nullptr;
        s_tree = tree;
        static jf::JMenu menu{ "Fields" };
        static bool built = false;
        if (!built) {
            built = true;
            menu.add(g, "Expand All")->onTriggered.connect([] { if (s_tree) s_tree->expandAll(); });
            menu.add(g, "Collapse All")->onTriggered.connect([] { if (s_tree) s_tree->collapseAll(); });
            menu.addSeparator(g);
            menu.add(g, "Expand Branch")->onTriggered.connect([] {
                if (s_tree) if (auto* n = const_cast<jf::JTreeViewNode*>(s_tree->hoveredNode())) s_tree->setBranchExpanded(*n, true);
            });
            menu.add(g, "Collapse Branch")->onTriggered.connect([] {
                if (s_tree) if (auto* n = const_cast<jf::JTreeViewNode*>(s_tree->hoveredNode())) s_tree->setBranchExpanded(*n, false);
            });
        }
        return menu;
    }

public:

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        if (m_done) return false;
        m_window->pollNativeEvents();
        if (m_window->shouldClose() || m_window->consumeFocusLost()) return false;   // click-away / WM close dismisses
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed = m_window->consumePress(), released = m_window->consumeRelease(), held = m_window->isLeftButtonDown();

        // Close [x] in the header dismisses; a press elsewhere in the header starts a window drag.
        const jf::JRect closeR = _closeRect();
        if (pressed && _inRect(closeR, mx, my)) return false;
        const bool inTitle = (my >= 0.f && my < kHeader()) && !_inRect(closeR, mx, my);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        // RIGHT-CLICK. A modal dialog polls its own window, so JAppWindow's context-menu path never runs
        // for it; the menu is opened through the same global hook that path uses (JMenuManager::onOpenMenu),
        // which puts the popup in its own window above this one.
        if (m_window->consumeRightPress() && m_tree && m_tree->contextMenu()) {
            const auto [gx, gy] = m_window->globalCursorPos();
            if (JMenuManager::instance().onOpenMenu)
                JMenuManager::instance().onOpenMenu(m_tree->contextMenu(), gx, gy, false, /*pointAnchored=*/true);
        }

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            using K = JKeyEvent::JKey;
            if (ke.key == K::Escape) return false;
            // Type to filter, arrows to move, Return to choose, Escape to dismiss — the standard
            // filter-popup contract. JTreeView already implements the navigation (Up/Down over the
            // flattened visible rows, Left/Right collapse/expand, scroll-into-view); it simply never
            // received the keys because the filter field consumed all of them.
            const bool nav = (ke.key == K::Up || ke.key == K::Down || ke.key == K::Left || ke.key == K::Right ||
                              ke.key == K::Home || ke.key == K::End || ke.key == K::PageUp || ke.key == K::PageDown ||
                              ke.key == K::Return);
            if (nav) { m_tree->handleKeyEvent(ke); if (m_done) return false; continue; }
            m_search->handleKeyEvent(ke);   // everything else types into the filter
        }

        const float searchY = kHeader() + kPad, treeY = searchY + kRowH + kPad;
        const float searchW = static_cast<float>(kW) - 2.f * kPad;
        m_search->setBounds({ kPad, searchY, searchW, kRowH });
        m_tree->setBounds({ kPad, treeY, searchW, static_cast<float>(kH) - treeY - kPad });
        m_search->handleMouseMove(mx, my); m_tree->handleMouseMove(mx, my);
        // Clear-[x] at the right end of the search box, shown only while there's a query. A press on it wipes
        // the filter (and never places the caret, so it's checked before routing the press to the search box).
        const jf::JRect clearR = _clearRect(searchY, searchW);
        const bool inClear  = !m_search->text().empty() && _inRect(clearR, mx, my);
        const bool inSearch = (my >= searchY && my < searchY + kRowH);
        if (pressed)  {
            if (inClear)       { m_search->setText(""); m_tree->setFilter(""); }
            else if (inSearch) m_search->handleMousePress(mx, my);
            else               m_tree->handleMousePress(mx, my);
            if (m_done) return false;
        }
        if (released) { m_search->handleMouseRelease(mx, my); m_tree->handleMouseRelease(mx, my); }
        if (const float wheel = m_window->consumeWheel(); wheel != 0.f) m_tree->handleScroll(mx, my, wheel);

        _render(buf, mx, my);
        auto frame = hal.beginFrame(m_surface); hal.drawPrimitives(buf); hal.submitAndPresentFrame(frame);
        return !m_done;
    }

private:
    void _render(jf::JPrimitiveBuffer& buf, float mx, float my) {
        using namespace jf;
        buf.clear();
        const float W = static_cast<float>(kW), H = static_cast<float>(kH);
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface1, JStyle::current().cornerRadius, 1.f, Colors::Border);
        const JRect cr = _closeRect();
        JTitleBar::draw(buf, 0.f, 0.f, W, kHeader(), m_title.c_str(), JStyle::current().cornerRadius, 0, kPad, W - _closeRect().x + kPad);
        JCloseButton::draw(buf, cr, _inRect(cr, mx, my));                            // framework close control

        m_search->populateRenderPrimitives(buf);
        // Clear-[x] over the right end of the search box while there's a query. The framework's one
        // clear mark, the same ✕ the search box next to it would draw if it drew its own: this used to
        // WRITE the character U+2715, which the atlas does not pack, so it came out as a question mark.
        if (!m_search->text().empty()) {
            const float searchY = kHeader() + kPad, searchW = W - 2.f * kPad;
            const JRect cx = _clearRect(searchY, searchW);
            JClearMark::draw(buf, cx, JColor{Colors::TextPrimary[0], Colors::TextPrimary[1],
                                             Colors::TextPrimary[2], 255}, _inRect(cx, mx, my));
        }
        m_tree->populateRenderPrimitives(buf);
    }

    std::function<void(std::string)> m_onAccept;
    std::string                      m_title;
    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{0};
    jf::JSceneGraph  m_graph;
    // Owned, declared after m_graph so they're destroyed before it (deregister from s_activeWidgets cleanly).
    std::unique_ptr<jf::JLineEdit> m_search;
    std::unique_ptr<jf::JTreeView> m_tree;
    bool  m_done{false}, m_drag{false};
    float m_ax{0}, m_ay{0};

    // Session-scoped memory of which categories the user left open, so reopening the picker restores its shape.
    // Branch paths that were open when the picker last closed ("Configuration>engine>cyl  (12)").
    static inline std::set<std::string> s_expanded;
    static inline bool                  s_seeded{false};

    static std::string _rowPath(const std::string& trail, const std::string& label) {
        return trail.empty() ? label : trail + ">" + label;
    }
    // First ever open: show the top level and nothing more, so the catalogue is readable rather than
    // 6000 rows deep. After that, exactly what was left open.
    static void _restoreExpansion(jf::JTreeViewNode& n, const std::string& trail) {
        for (auto& c : n.children) {
            if (c.children.empty()) continue;
            const std::string path = _rowPath(trail, c.label);
            c.expanded = s_seeded ? (s_expanded.count(path) > 0) : trail.empty();
            _restoreExpansion(c, path);
        }
    }
    static void _captureExpansion(const jf::JTreeViewNode& n, const std::string& trail) {
        for (const auto& c : n.children) {
            if (c.children.empty()) continue;
            const std::string path = _rowPath(trail, c.label);
            if (c.expanded) s_expanded.insert(path);
            _captureExpansion(c, path);
        }
    }
};
