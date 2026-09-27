#include "NavTree.h"

#include <algorithm>

using namespace jf;

// Split a string on a delimiter (used to parse the space-separated identity signature).
std::vector<std::string> splitOn(const std::string& s, char d) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(d, i);
        if (j == std::string::npos) { out.push_back(s.substr(i)); break; }
        out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// Build a tree-view node from one dashboard node object ({name, children[], expanded}).
// A structured NodeCondition from an ORIGINAL-studio document ({any, clauses:[{channel,op,value}]})
// converts to the equivalent MathEvaluator expression on load ("[$ch] op v" joined by &&/||); the
// port's own documents store the expression string directly.
std::string conditionToExpr(const jf::JJson& c) {
    if (c.isString()) return c.str();
    if (!c.isObject()) return {};
    const std::string joiner = c["any"].boolean() ? " || " : " && ";
    std::string out;
    for (const auto& cl : c["clauses"].arr()) {
        char b[192];
        const std::string op = cl["op"].str().empty() ? std::string("==") : cl["op"].str();
        std::snprintf(b, sizeof(b), "[$%s] %s %g", cl["channel"].str().c_str(), op.c_str(), cl["value"].number());
        if (!out.empty()) out += joiner;
        out += b;
    }
    return out;
}

JTreeViewNode seedNode(const jf::JJson& n) {
    JTreeViewNode node;
    node.label    = n["name"].str();
    node.expanded = true;                       // a freshly seeded tree opens showing what is in it
    if (n.contains("children"))
        for (const auto& c : n["children"].arr())
            node.children.push_back(seedNode(c));
    return node;
}

// A RULE FROM AN OLD IMPORT. TunerStudio's menus rule off groups with `subMenu = std_separator`, and the
// importer used to carry those through as tree rows. In a menu a rule is a line between entries; in a tree
// it is a ROW — nameless, pageless, and reachable with the arrow keys, which walked onto it and blanked the
// canvas. The importer no longer emits them, and documents converted before that still hold them, so they
// are dropped on load as well: nothing has to be re-imported to be rid of them.
bool isSeparator(const jf::JJson& n) {
    return n.contains("separator") && n["separator"].number() != 0.0;
}

JTreeViewNode dashNode(const jf::JJson& n) {
    JTreeViewNode node;
    node.label    = n["name"].str();
    node.expanded = n["expanded"].boolean();
    if (n.contains("condition")) { node.userData = conditionToExpr(n["condition"]); if (!node.userData.empty()) node.icon = 1; }  // visibility condition
    if (n.contains("children"))
        for (const auto& c : n["children"].arr()) {
            if (isSeparator(c)) continue;      // a document imported before they were dropped
            node.children.push_back(dashNode(c));
        }
    return node;
}

// Tree expanded-state persistence (studio parity: expand/collapse survives across runs). We store the
// set of expanded node PATHS in JSettings rather than rewriting dashboard.gui (no data loss); on load
// we apply them, on exit we snapshot the live tree.
void collectExpanded(const JTreeViewNode& n, const std::string& path, std::string& out) {
    if (n.placeholder) return;                                          // transient add-affordance: never a real path
    const std::string p = path.empty() ? n.label : path + "/" + n.label;
    if (n.expanded) { out += p; out += '\n'; }
    for (const auto& c : n.children) collectExpanded(c, p, out);
}
void applyExpanded(JTreeViewNode& n, const std::string& path, const std::set<std::string>& exp) {
    const std::string p = path.empty() ? n.label : path + "/" + n.label;
    if (!n.children.empty()) n.expanded = exp.count(p) > 0;
    for (auto& c : n.children) applyExpanded(c, p, exp);
}


// Serialise a tree-view node back to the dashboard.gui shape ({name, expanded, children[]}) — the
// inverse of dashNode. Only nodes with children carry a "children" array (leaves stay compact).
jf::JJson dashNodeToJson(const JTreeViewNode& n) {
    jf::JJson o = jf::JJson::object();
    o["name"]     = jf::JJson(n.label);
    o["expanded"] = jf::JJson(n.expanded);
    if (!n.userData.empty()) o["condition"] = jf::JJson(n.userData);   // visibility condition
    bool hasReal = false;
    for (const auto& c : n.children) if (!c.placeholder) { hasReal = true; break; }
    if (hasReal) {
        jf::JJson kids = jf::JJson::array();
        for (const auto& c : n.children) if (!c.placeholder) kids.push(dashNodeToJson(c));   // ghosts never persisted
        o["children"] = std::move(kids);
    }
    return o;
}

// Index-path (the sequence of child indices) from root down to `target`. Empty/false if not found.
// Lets us relocate a live-tree selection inside a fresh copy of the tree before mutating it, since
// setRootNode invalidates the old node pointers.
bool findIndexPath(const JTreeViewNode& node, const JTreeViewNode* target, std::vector<int>& out) {
    for (int i = 0; i < static_cast<int>(node.children.size()); ++i) {
        out.push_back(i);
        if (&node.children[i] == target) return true;
        if (findIndexPath(node.children[i], target, out)) return true;
        out.pop_back();
    }
    return false;
}

// Mutable node at an index-path inside a tree copy (nullptr if the path is invalid). Companion to
// findIndexPath: relocate a selection inside a fresh copy so we can mutate then setRootNode.
JTreeViewNode* nodeAtPath(JTreeViewNode& root, const std::vector<int>& path) {
    JTreeViewNode* cur = &root;
    for (int idx : path) {
        if (idx < 0 || idx >= static_cast<int>(cur->children.size())) return nullptr;
        cur = &cur->children[idx];
    }
    return cur;
}

// Slash-joined label path from root to `target` (e.g. "Configuration/Engine/ETB (A)"). Drives the surface
// view-switcher: a node's path is what an element's "node" prop matches against (ports treeNodePath).
bool nodePathOf(const JTreeViewNode& node, const JTreeViewNode* target, const std::string& prefix, std::string& out) {
    for (const auto& c : node.children) {
        if (c.placeholder) continue;                                   // ghosts have no persisted path
        const std::string p = prefix.empty() ? c.label : prefix + "/" + c.label;
        if (&c == target) { out = p; return true; }
        if (nodePathOf(c, target, p, out)) return true;
    }
    return false;
}

// Every node's "/"-joined path (excludes the synthetic root). Used to diff before/after an internal
// drag-reorder so the moved subtree's old→new path can be recovered and its pages re-keyed.
void collectPaths(const JTreeViewNode& node, const std::string& prefix, std::set<std::string>& out) {
    for (const auto& c : node.children) {
        if (c.placeholder) continue;                                   // transient add-affordance: never a real path
        const std::string p = prefix.empty() ? c.label : prefix + "/" + c.label;
        out.insert(p);
        collectPaths(c, p, out);
    }
}

// Clone the navigation tree into a PICKER tree: same labels + shape, but every node carries its full
// "/"-joined path in userData, so PopupSignalPicker returns that path when a node is chosen. Ghost
// placeholders are skipped. Every node is selectable (categories included), since any node — leaf or a
// category that owns a viewport — is a valid hyperlink target. Used by the Properties "node" editor.
JTreeViewNode buildNodePickTree(const JTreeViewNode& src, const std::string& prefix) {
    JTreeViewNode out;
    out.label    = src.label;
    out.expanded = true;
    for (const auto& c : src.children) {
        if (c.placeholder) continue;
        const std::string p = prefix.empty() ? c.label : prefix + "/" + c.label;
        JTreeViewNode child = buildNodePickTree(c, p);
        child.userData = p;                                // selecting the node returns its path
        out.children.push_back(std::move(child));
    }
    return out;
}

// The "/"-joined label path of the node at `idx` (a child-index chain from the root). Used to re-find a
// just-added node after a tree rebuild so it can be selected — setRootNode invalidates node pointers, but
// the label path is stable. Returns "" if the index chain doesn't resolve.
std::string joinLabels(const JTreeViewNode& root, const std::vector<int>& idx) {
    std::string p; const JTreeViewNode* c = &root;
    for (int i : idx) {
        if (i < 0 || i >= static_cast<int>(c->children.size())) return {};
        c = &c->children[i];
        p = p.empty() ? c->label : p + "/" + c->label;
    }
    return p;
}

// Edit-mode "New node…" add-affordance (ports EditTree::syncPlaceholders / makePlaceholder). While editing,
// give the root AND every real node exactly one trailing dimmed placeholder child, so any node is expandable
// and can gain children inline; otherwise strip them all. Rebuilt wholesale so it is self-correcting. The
// placeholder rows are transient — never persisted, never path-walked (app promotes them on rename).
const char* placeholderCaption() { return "New node\xE2\x80\xA6"; }   // "New node…" (UTF-8 ellipsis)
void syncPlaceholders(JTreeViewNode& root, bool editing) {
    // 1) strip every existing placeholder, anywhere.
    std::function<void(JTreeViewNode&)> strip = [&](JTreeViewNode& n) {
        n.children.erase(std::remove_if(n.children.begin(), n.children.end(),
                         [](const JTreeViewNode& c){ return c.placeholder; }), n.children.end());
        for (auto& c : n.children) strip(c);
    };
    strip(root);
    if (!editing) return;
    // 2) re-add one trailing placeholder under the root and under every (now real) node.
    std::function<void(JTreeViewNode&)> decorate = [&](JTreeViewNode& n) {
        for (auto& c : n.children) decorate(c);          // descend first, so the ghost itself gets no ghost child
        JTreeViewNode ph; ph.label = placeholderCaption(); ph.placeholder = true;
        n.children.push_back(std::move(ph));
    };
    decorate(root);
}

// Tree-edit undo: every structural tree mutation pushes a whole-tree before/after snapshot (the tree
// is small — snapshots are cheap). Edit▸Undo tries the active surface's history first and falls back to
// this stack, unifying the two histories. g_applyTree is bound once the
// tree view exists. Framework-internal edits (inline rename, drag-reorder) are not captured here.
jf::JUndoStack g_treeUndo;
std::function<void(const JTreeViewNode&)> g_applyTree;
std::function<void()>                    g_refilterTree;
