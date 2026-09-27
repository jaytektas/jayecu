#pragma once

// The navigation tree's model layer: the JJson <-> JTreeViewNode conversion the document is saved/loaded
// through, the path utilities the editing operations address nodes with, and the whole-tree undo command.
//
// The tree is small, so undo snapshots the WHOLE tree before/after rather than describing an edit — Edit>Undo
// tries the active surface's history first and falls back to this stack, unifying the two.
// g_applyTree is bound once the tree view exists.

#include <j/core/JTreeView.h>
#include <j/config/Json.h>
#include <j/core/UndoStack.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

std::vector<std::string> splitOn(const std::string& s, char d);
std::string   conditionToExpr(const jf::JJson& c);

// Document <-> view.
bool isSeparator(const jf::JJson& n);   // a group rule from an import made before they were dropped
JTreeViewNode dashNode(const jf::JJson& n);
// A META navigation node ({name, target_path, children[]}) as a tree node. The firmware describes how its
// own settings are grouped; this is what an ECU with no project document yet is seeded with. target_path is
// deliberately dropped — it names a config subtree, not a page, and the pages are the user's to author.
JTreeViewNode seedNode(const jf::JJson& n);
jf::JJson     dashNodeToJson(const JTreeViewNode& n);

// Expanded-state round trip (persisted with the document so the tree reopens as it was left).
void collectExpanded(const JTreeViewNode& n, const std::string& path, std::string& out);
void applyExpanded(JTreeViewNode& n, const std::string& path, const std::set<std::string>& exp);

// Addressing a node: by index path (stable across the rebuilds an edit causes) or by label path.
bool           findIndexPath(const JTreeViewNode& node, const JTreeViewNode* target, std::vector<int>& out);
JTreeViewNode* nodeAtPath(JTreeViewNode& root, const std::vector<int>& path);
bool           nodePathOf(const JTreeViewNode& node, const JTreeViewNode* target, const std::string& prefix,
                          std::string& out);
void           collectPaths(const JTreeViewNode& node, const std::string& prefix, std::set<std::string>& out);
std::string    joinLabels(const JTreeViewNode& root, const std::vector<int>& idx);

// The "New node…" ghost rows shown only while editing.
const char* placeholderCaption();
void        syncPlaceholders(JTreeViewNode& root, bool editing);

// A pick-tree copy (used by the "move to…" pickers).
JTreeViewNode buildNodePickTree(const JTreeViewNode& src, const std::string& prefix);

// ---- tree-edit undo ----
extern jf::JUndoStack g_treeUndo;
extern std::function<void(const JTreeViewNode&)> g_applyTree;
// Re-apply the run-mode visibility filter (a node's condition against the CURRENT tune). Bound once the
// tree view and the filter exist; called after any rebuild, since a fresh tree arrives unfiltered.
extern std::function<void()> g_refilterTree;

class TreeSwap : public jf::JUndoCommand {
public:
    TreeSwap(JTreeViewNode before, JTreeViewNode after) : before_(std::move(before)), after_(std::move(after)) {}
    void redo() override { if (g_applyTree) g_applyTree(after_); }
    void undo() override { if (g_applyTree) g_applyTree(before_); }
    std::string text() const override { return "Tree Edit"; }
private:
    JTreeViewNode before_, after_;
};
