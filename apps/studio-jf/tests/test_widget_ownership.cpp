// The widget TREE and the widget LIFETIME, which are two different things.
//
// JWidget::m_children is the tree: one list, written by every way of putting a widget inside another
// one (adopt, JContainer::add, JScrollArea::addChildWidget, JTabWidget::addTab). Focus traversal walks
// it, effective visibility walks it, scan exclusion walks it. m_ownedChildren is lifetime only.
//
// The bug these tests exist for: collectChildren() used to be virtual, defaulting to the ADOPTED
// children, so a widget held as a MEMBER of its host — the normal way every hand-built pane in the
// studio is composed — had no edge at all unless the host remembered to override the hook. Forgetting
// was invisible: the pane painted, the mouse worked (panes route presses to their members by hand),
// and only the keyboard was dead. Five panes had the override; the ones that did not were mysteriously
// untypeable. There is now nothing to remember, and these tests hold that line.
//
//   cmake --build build --target widget_ownership_test && ./build/widget_ownership_test

#include <j/core/JWidget.h>
#include <j/core/JContainer.h>
#include <j/core/JScrollArea.h>
#include <j/core/JTabWidget.h>
#include <j/core/FocusManager.h>
#include <j/core/SceneGraph.h>
#include <j/graphics/RenderPrimitive.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++g_fail;
}

struct Probe : jf::JWidget {
    int* flag;
    Probe(jf::JSceneGraph& g, int* f = nullptr, const char* n = "probe") : jf::JWidget(g, n), flag(f) {}
    ~Probe() override { if (flag) ++*flag; }
    void populateRenderPrimitives(jf::JPrimitiveBuffer&) override {}
};

// A focusable leaf, so the traversal tests have something that can actually be a tab stop.
struct Field : jf::JWidget {
    explicit Field(jf::JSceneGraph& g, const char* n = "field") : jf::JWidget(g, n) {
        setFocusPolicy(jf::JFocusPolicy::TabFocus);
        setBounds({ 0.f, 0.f, 40.f, 20.f });
    }
    void populateRenderPrimitives(jf::JPrimitiveBuffer&) override {}
};

// A hand-built pane: children held BY VALUE, the composition style the old hook could not see.
struct Pane : jf::JWidget {
    jf::JContainer form;
    Field          a, b;
    explicit Pane(jf::JSceneGraph& g) : jf::JWidget(g, "Pane"), form(g), a(g, "a"), b(g, "b") {
        addChild(&form);
        form.add(&a);
        form.add(&b);
    }
    void populateRenderPrimitives(jf::JPrimitiveBuffer&) override {}
};

std::vector<jf::JWidget*> kids(const jf::JWidget& w) {
    std::vector<jf::JWidget*> out;
    w.collectChildren(out);
    return out;
}
bool has(const std::vector<jf::JWidget*>& v, const jf::JWidget* w) {
    return std::find(v.begin(), v.end(), w) != v.end();
}

} // namespace

int main() {
    jf::JSceneGraph graph;
    std::puts("=== widget tree + ownership ===");

    std::puts("\n-- lifetime: adopt is recursive RAII, and deregisters --");
    {
        const size_t base = jf::JWidget::s_activeWidgets.size();
        int child = 0, grand = 0;
        auto parent = std::make_unique<Probe>(graph);
        Probe* c = parent->adopt(std::make_unique<Probe>(graph, &child));
        c->adopt(std::make_unique<Probe>(graph, &grand));
        ck(jf::JWidget::s_activeWidgets.size() == base + 3, "three widgets registered");
        ck(!child && !grand, "nothing destroyed early");
        parent.reset();
        ck(child == 1 && grand == 1, "parent destroys child destroys grandchild");
        ck(jf::JWidget::s_activeWidgets.size() == base, "and all three deregistered");
    }
    {
        int d = 0;
        auto parent = std::make_unique<Probe>(graph);
        Probe* c = parent->adopt(std::make_unique<Probe>(graph, &d));
        parent->disown(c);
        ck(d == 1, "disown() destroys an adopted child early");
        ck(kids(*parent).empty(), "and takes its tree edge with it");
    }

    std::puts("\n-- the tree: one list, whichever way a child got there --");
    {
        Probe parent(graph);
        Probe member(graph);                                  // held by the caller, not owned here
        parent.addChild(&member);
        Probe* owned = parent.adopt(std::make_unique<Probe>(graph));
        const auto k = kids(parent);
        ck(k.size() == 2 && has(k, &member) && has(k, owned),
           "a member child and an adopted child are both in it", std::to_string(k.size()));
        ck(member.parentWidget() == &parent && owned->parentWidget() == &parent, "both carry the parent edge");
        ck(parent.ownedChildren().size() == 1, "but only one of them is OWNED here");
        parent.addChild(&member);
        ck(kids(parent).size() == 2, "addChild is idempotent — no duplicate edge");
    }
    {
        Probe a(graph), b(graph), child(graph);
        a.addChild(&child);
        b.addChild(&child);                                   // move it
        ck(child.parentWidget() == &b, "a widget has ONE parent: re-adding moves the edge");
        ck(kids(a).empty(), "the old parent no longer lists it");
        ck(kids(b).size() == 1, "the new one does");
    }

    std::puts("\n-- the tree survives destruction in either order --");
    {
        Probe parent(graph);
        { Probe shortLived(graph); parent.addChild(&shortLived); ck(kids(parent).size() == 1, "child added"); }
        ck(kids(parent).empty(), "a child that dies FIRST removes its own edge (the by-value member case)");
    }
    {
        Probe child(graph);
        { Probe parent(graph); parent.addChild(&child); }
        ck(child.parentWidget() == nullptr, "a parent that dies first unparents its surviving children");
    }

    std::puts("\n-- effective visibility follows the tree --");
    {
        Probe root(graph); Probe mid(graph); Probe leaf(graph);
        root.addChild(&mid); mid.addChild(&leaf);
        ck(leaf.isVisible(), "visible by default");
        root.setVisible(false);
        ck(!leaf.isVisible(), "hiding a GRANDPARENT hides the leaf");
        ck(leaf.isVisibleSelf(), "without touching the leaf's own flag");
        root.setVisible(true);
        ck(leaf.isVisible(), "and showing it again brings the leaf back");
    }
    {
        Probe host(graph); Probe leaf(graph);
        host.addChild(&leaf);
        host.setScanExcluded(true);
        ck(leaf.isScanExcluded(), "scan exclusion reaches a child through the same edge");
    }

    std::puts("\n-- containers register into the tree, both overloads --");
    {
        jf::JContainer c(graph);
        Probe member(graph);
        int owned = 0;
        c.add(&member);
        c.add(std::make_unique<Probe>(graph, &owned));
        ck(kids(c).size() == 2, "non-owning add and owning add both parent", std::to_string(kids(c).size()));
        ck(member.parentWidget() == &c, "the non-owned one is parented, not owned");
        c.clear();
        ck(owned == 1, "clear() destroys the owned row");
        ck(kids(c).empty() && member.parentWidget() == nullptr,
           "and unparents the borrowed one instead of destroying it");
    }
    {
        jf::JScrollArea s(graph);
        Probe member(graph);
        int owned = 0;
        s.addChildWidget(&member);
        s.addChildWidget(std::make_unique<Probe>(graph, &owned));
        ck(kids(s).size() == 2, "scroll area: both overloads parent");
        s.clearChildren();
        ck(owned == 1 && kids(s).empty() && member.parentWidget() == nullptr, "clearChildren matches clear()");
    }

    std::puts("\n-- tabs: every page is a child, exactly one is visible --");
    {
        jf::JTabWidget t(graph);
        Probe p0(graph), p1(graph);
        t.addTab("one", &p0);
        t.addTab("two", &p1);
        ck(kids(t).size() == 2, "both pages are children");
        ck(p0.isVisibleSelf() && !p1.isVisibleSelf(), "the first page shows, the second is hidden");
        t.setActiveTab(1);
        ck(!p0.isVisibleSelf() && p1.isVisibleSelf(), "switching tabs swaps which one is visible");
        t.removeTab(1);
        ck(p1.parentWidget() == nullptr, "a removed page loses its edge");
        ck(p1.isVisibleSelf(), "and is handed back visible — it is the caller's widget again");
        ck(p0.isVisibleSelf(), "the surviving page becomes active and shows");
    }

    std::puts("\n-- THE regression: a pane's by-value members are reachable by the keyboard --");
    {
        jf::JFocusManager fm;
        Pane pane(graph);
        fm.setFocusRoots({ &pane });
        fm.syncOrder();
        ck(fm.orderSize() == 2, "both fields are tab stops with no hook overridden anywhere",
           std::to_string(fm.orderSize()));
        fm.nextFocus();
        ck(fm.focused() == &pane.a || fm.focused() == &pane.b, "and Tab actually lands on one");

        pane.setVisible(false);
        fm.syncOrder();
        ck(fm.orderSize() == 0, "hiding the pane takes its whole subtree out of the tab order");
        pane.setVisible(true);
    }
    {
        // The same pane, one level deeper: hosted in a tab, which is what the studio does. The page
        // that is not showing must not be a tab stop.
        jf::JFocusManager fm;
        jf::JTabWidget tabs(graph);
        Pane front(graph), behind(graph);
        tabs.addTab("front", &front);
        tabs.addTab("behind", &behind);
        fm.setFocusRoots({ &tabs });
        fm.syncOrder();
        ck(fm.orderSize() == 2, "only the visible page's fields are in the order",
           std::to_string(fm.orderSize()));
        tabs.setActiveTab(1);
        fm.syncOrder();
        ck(fm.orderSize() == 2, "still two after switching tabs — the other page's, now");
        ck(!jf::JFocusManager::reachable(&front.a), "the tabbed-away field is not reachable");
        ck(jf::JFocusManager::reachable(&behind.a), "the showing one is");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "widget tree + ownership: all pass",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
