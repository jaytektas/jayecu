#pragma once

// ReseedDialog — pick up what the ECU definition has gained since this project's tree was written.
//
// LEFT: everything in the meta's navigation_tree that is NOT in your tree, as a tree (so a new node
// arrives with the parents that give it meaning). RIGHT: your tree as it stands, read-only, so you can
// see where the import will land. Ctrl/Shift multi-select on the left; the footer imports the selection.
//
// WHY A CHOOSER RATHER THAN AN AUTOMATIC MERGE: an automatic reseed cannot tell "new in the definition"
// from "the user deleted this" without remembering every node ever seeded — and that hidden state is
// wrong the moment someone renames or moves a node. Asking removes the guess: a node you deleted simply
// appears in the left pane each time and you don't import it. Nothing is remembered, nothing to rot.
//
// Import is ADDITIVE ONLY — it never removes, renames or reorders what you have — and it goes through
// the tree's own undo stack, so an import you didn't want is one Ctrl+Z away.

#include <j/core/JStyle.h>
#include <j/core/JTitleBar.h>
#include <j/core/JCloseButton.h>
#include <j/core/JTextHelper.h>
#include <j/core/JLabel.h>
#include <j/core/JTreeView.h>
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
#include <set>
#include <string>
#include <vector>

class ReseedDialog {
public:
    static constexpr uint32_t kW = 720, kH = 520;
    static constexpr float kBtnW = 120.f, kPad = 12.f, kGap = 10.f;
    static float kBtnH()  { return jf::JStyle::current().buttonHeight; }
    static float kHeader(){ return jf::JStyle::current().titleBarHeight; }
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

    // (available, existing, onImport) lead; hal/pos/handle tail from openModal.
    // `available` is the pruned meta tree (new nodes only); onImport receives the LABEL PATHS the user
    // chose ("Configuration/Sensors/sensor/Wideband O2 1"), which the caller grafts into the real tree.
    ReseedDialog(JTreeViewNode available, JTreeViewNode existing,
                 std::function<void(std::vector<std::string>)> onImport,
                 jf::JGpuHal& hal, int sx, int sy, NativeWinHandleType parent)
        : m_onImport(std::move(onImport))
        , m_window(std::make_unique<PlatformWinType>("Reseed", kW, kH, sx, sy,
                                                     jf::JPlatformWindowStyle::Borderless, parent))
        , m_surface(hal.createSurface(m_window->nativeHandle(), kW, kH)) {
        using namespace jf;
        m_newLbl = std::make_unique<JLabel>(m_graph, "New in the ECU definition", 260.f);
        m_curLbl = std::make_unique<JLabel>(m_graph, "Your tree", 260.f);

        m_new = std::make_unique<JTreeView>(m_graph);
        m_new->setMultiSelect(true);                  // Ctrl/Shift — importing one node at a time is a chore
        m_new->setRootNode(std::move(available));

        m_cur = std::make_unique<JTreeView>(m_graph);  // reference only: never edited, never imported from
        m_cur->setRootNode(std::move(existing));

        m_box = std::make_unique<JDialogButtonBox>(m_graph);
        m_box->addButton("Cancel", JDialogButtonBox::Role::Reject, 84.f);
        // Action buttons carry their own onClicked (the box only signals Accept/Reject by role).
        JButton* all = m_box->addButton("Import All", JDialogButtonBox::Role::Action, kBtnW);
        all->onClicked.connect([this] { _importAll(); m_done = true; });
        m_box->addButton("Import Selected", JDialogButtonBox::Role::Accept, 140.f);
        m_box->onReject.connect([this] { m_done = true; });
        m_box->onAccept.connect([this] { _importSelected(); m_done = true; });
    }

    void destroySurface(jf::JGpuHal& hal) { hal.destroySurface(m_surface); }

    bool pollAndRender(jf::JGpuHal& hal, jf::JPrimitiveBuffer& buf) {
        using namespace jf;
        m_window->pollNativeEvents();
        if (m_window->shouldClose()) return false;
        const float mx = m_window->mouseX(), my = m_window->mouseY();
        const bool pressed  = m_window->consumePress();
        const bool released = m_window->consumeRelease();
        const bool held     = m_window->isLeftButtonDown();

        const JRect closeR = _closeRect();
        if (pressed && _inRect(closeR, mx, my)) return false;
        const bool inTitle = (my >= 0.f && my < kHeader() && mx < closeR.x);
        if (held && inTitle && !m_drag) { m_drag = true; m_ax = mx; m_ay = my; }
        if (m_drag) { auto [gx, gy] = m_window->globalCursorPos(); m_window->setPosition(gx - int(m_ax), gy - int(m_ay)); }
        if (!held) m_drag = false;

        for (const auto& ke : m_window->consumeAllKeys()) {
            if (!ke.pressed) continue;
            _refreshFocusRoots();
            if (jf::jRouteKey(ke, m_focus)) continue;
            if (m_box->handleKeyEvent(ke)) { if (m_done) return false; continue; }
        }

        // Two panes side by side under their captions; the footer sits at the bottom.
        const float W = static_cast<float>(kW), H = static_cast<float>(kH);
        const float top     = kHeader() + kPad;
        const float lblH    = JTextHelper::lineHeight() + 4.f;
        const float footerH = kBtnH() + 2.f * kPad;
        const float paneW   = (W - 2.f * kPad - kGap) * 0.5f;
        const float paneY   = top + lblH;
        const float paneH   = H - paneY - footerH;
        m_newLbl->setBounds({ kPad, top, paneW, lblH });
        m_curLbl->setBounds({ kPad + paneW + kGap, top, paneW, lblH });
        m_new->setBounds({ kPad, paneY, paneW, paneH });
        m_cur->setBounds({ kPad + paneW + kGap, paneY, paneW, paneH });
        m_box->setBounds({ kPad, H - kPad - kBtnH(), W - 2.f * kPad, kBtnH() });

        _refreshFocusRoots();
        if (pressed) jf::jRouteMouse(mx, my, m_focus);
        if (!m_focusSeeded) { m_focusSeeded = true; m_focus.focusFirst(); }
        m_new->handleMouseMove(mx, my); m_cur->handleMouseMove(mx, my); m_box->handleMouseMove(mx, my);
        if (pressed)  { m_new->handleMousePress(mx, my);   m_cur->handleMousePress(mx, my);   m_box->handleMousePress(mx, my); }
        if (released) { m_new->handleMouseRelease(mx, my); m_cur->handleMouseRelease(mx, my); m_box->handleMouseRelease(mx, my); }
        if (m_done) return false;

        _render(buf, mx, my);
        auto frame = hal.beginFrame(m_surface); hal.drawPrimitives(buf); hal.submitAndPresentFrame(frame);
        return true;
    }

private:
    // A node's label path, root-relative ("Configuration/Sensors/sensor/Wideband O2 1").
    static void _paths(const JTreeViewNode& n, const std::string& prefix, std::vector<std::string>& out) {
        const std::string p = prefix.empty() ? n.label : prefix + "/" + n.label;
        out.push_back(p);
        for (const auto& c : n.children) _paths(c, p, out);
    }
    static bool _find(const JTreeViewNode& n, const JTreeViewNode* target,
                      const std::string& prefix, std::string& out) {
        const std::string p = prefix.empty() ? n.label : prefix + "/" + n.label;
        if (&n == target) { out = p; return true; }
        for (const auto& c : n.children) if (_find(c, target, p, out)) return true;
        return false;
    }

    void _importAll() {
        std::vector<std::string> out;
        for (const auto& c : m_new->root().children) _paths(c, "", out);
        if (m_onImport) m_onImport(std::move(out));
    }
    // Selecting a parent takes its subtree with it — picking "Sensors" and getting an empty "Sensors"
    // would be a surprise, and the whole point of the left pane being a tree is that branches move whole.
    void _importSelected() {
        std::vector<std::string> out;
        for (JTreeViewNode* n : m_new->selectedNodes()) {
            std::string p;
            for (const auto& c : m_new->root().children)
                if (_find(c, n, "", p)) break;
            if (p.empty()) continue;
            out.push_back(p);
            for (const auto& c : n->children) _paths(c, p, out);
        }
        if (out.empty()) return;                        // nothing picked: treat as a cancel, import nothing
        if (m_onImport) m_onImport(std::move(out));
    }

    void _refreshFocusRoots() {
        std::vector<jf::JWidget*> roots;
        if (m_new) roots.push_back(m_new.get());
        if (m_cur) roots.push_back(m_cur.get());
        if (m_box) roots.push_back(m_box.get());
        m_focus.setFocusRoots(std::move(roots));
    }

    void _render(jf::JPrimitiveBuffer& buf, float mx, float my) {
        using namespace jf;
        buf.clear();
        const float W = static_cast<float>(kW), H = static_cast<float>(kH);
        const float r = JStyle::current().cornerRadius;
        buf.pushRectangle(0.f, 0.f, W, H, Colors::Surface1, r, 1.f, Colors::Border);
        JTitleBar::draw(buf, 0.f, 0.f, W, kHeader(), "Add missing navigation entries",
                        r, 1, 0.f, _closeRect().width + 14.f);
        const JRect cr = _closeRect();
        JCloseButton::draw(buf, cr, _inRect(cr, mx, my));
        m_newLbl->populateRenderPrimitives(buf);
        m_curLbl->populateRenderPrimitives(buf);
        m_new->populateRenderPrimitives(buf);
        m_cur->populateRenderPrimitives(buf);
        m_box->populateRenderPrimitives(buf);
    }

    std::function<void(std::vector<std::string>)> m_onImport;
    std::unique_ptr<PlatformWinType> m_window;
    jf::GpuSurfaceId m_surface{0};
    jf::JSceneGraph  m_graph;
    std::unique_ptr<jf::JLabel>    m_newLbl, m_curLbl;
    std::unique_ptr<jf::JTreeView> m_new, m_cur;
    std::unique_ptr<jf::JDialogButtonBox> m_box;
    jf::JFocusManager m_focus;
    bool  m_focusSeeded{false}, m_done{false}, m_drag{false};
    float m_ax{0}, m_ay{0};
};
