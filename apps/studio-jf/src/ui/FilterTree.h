#pragma once

// FilterTree — a search box above a JTreeView, for the Tree + Dictionary docks (the studio's "Filter…"
// / "Filter bindings…" inputs). Typing filters the tree (JTreeView::setFilter). The dock hosts this;
// callers populate + wire the inner tree via tree(). Keyboard reaches the search box / tree through the
// focus manager (both are focusable widgets in the active set with live bounds).

#include <j/core/JWidget.h>
#include <j/core/JLineEdit.h>
#include <j/core/JTreeView.h>
#include <j/core/JStyle.h>

#include <string>

class FilterTree : public jf::JWidget {
public:
    FilterTree(jf::JSceneGraph& g, const std::string& placeholder)
        : jf::JWidget(g, "FilterTree"), search_(g, placeholder, 200.f, 0.f), tree_(g) {
        addChild(&search_);                    // members are children — the framework descends from here
        addChild(&tree_);
        search_.setClearButtonEnabled(true);   // the ✕ clears the filter (its onTextChanged runs, below)
        search_.onTextChanged.connect([this](const std::string& s) { tree_.setFilter(s); });
    }

    jf::JTreeView& tree() { return tree_; }

    // What the row under the cursor MEANS. Called each paint with the hovered node, so the tree's tooltip
    // describes that row rather than the widget as a whole — the framework's hover delay then shows it.
    // Without this the dictionary was a list of names with the definition's own explanation of each one
    // sitting unread in the meta.
    void setRowTooltipResolver(std::function<std::string(const jf::JTreeViewNode&)> fn) {
        rowTip_ = std::move(fn);
    }

    void populateRenderPrimitives(jf::JPrimitiveBuffer& buf) override {
        const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox;
        const float sh = searchH();
        search_.setBounds({ b.x, b.y, b.width, sh });
        search_.populateRenderPrimitives(buf);
        tree_.setBounds({ b.x, b.y + sh + kGap, b.width, b.height - sh - kGap });
        if (rowTip_) {
            const jf::JTreeViewNode* hov = tree_.hoveredNode();
            const std::string tip = hov ? rowTip_(*hov) : std::string();
            if (tip != tree_.tooltip()) tree_.setTooltip(tip);   // changing it restarts the hover delay
        }
        tree_.populateRenderPrimitives(buf);
    }
    void handleMouseMove(float mx, float my) override    { search_.handleMouseMove(mx, my); tree_.handleMouseMove(mx, my); }
    void handleMousePress(float mx, float my) override   { (inSearch(my) ? static_cast<jf::JWidget&>(search_) : tree_).handleMousePress(mx, my); }
    void handleMouseRelease(float mx, float my) override { search_.handleMouseRelease(mx, my); tree_.handleMouseRelease(mx, my); }
    bool handleScroll(float mx, float my, float w) override { return tree_.handleScroll(mx, my, w); }


private:
    std::function<std::string(const jf::JTreeViewNode&)> rowTip_;
    bool inSearch(float my) const { const auto& b = m_graph.getLayoutConst(m_nodeId).boundingBox; return my < b.y + searchH(); }
    // 30, AND NOT `controlHeight`. Taking the style's field height looked like the principled answer
    // and was a no-op: studio.common.style sets controlHeight to 22, matching the proportions of the
    // reference software's settings fields, so the search box came back exactly as squeezed as it was.
    // It is not one of those fields. A settings row sits in a column of its own kind where 22 reads
    // evenly; this is a lone box at the top of a dock, hit with the mouse and typed into, and it wants
    // the height of a thing you aim at. Raising controlHeight instead would have moved every combo and
    // spin box in the application to fix one box in two docks.
    static float searchH() { return 30.f; }
    static constexpr float kGap = 4.f;

    jf::JLineEdit search_;
    jf::JTreeView tree_;
};
