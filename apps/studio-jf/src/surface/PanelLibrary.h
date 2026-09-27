#pragma once

// PanelLibrary — the page behind every navigation-tree node. Each tree node owns ONE page of controls;
// that page is then placed as MANY "viewport" elements across
// surfaces (one node → many viewports). It maps a node's "/"-joined path to its own PanelModel and re-keys
// a node's subtree when the tree renames/reparents it (renamePrefix). It does NOT own a file: toJson()/load()
// serialise it as the "panelLibrary" section of the ECU's single dashboard.gui document (tree + surface +
// pages together).
//
// A viewport element (a "viewport" widget, prop "node" = path) renders its node's page live; it
// resolves the page through nodeviewport::resolver(), a global hook main.cpp points at this store — so the
// data-driven widget catalog can draw a viewport without a compile-time dependency on the store.

#include <j/config/Json.h>
#include <j/core/Log.h>      // JLOGC — lazy first-access page creation (surface.lazy)

#include <cctype>
#include <optional>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "PanelModel.h"

class PanelLibrary {
public:
    // The node's page, created empty on first access (so dropping/opening a fresh node just works).
    PanelModel& forNode(const std::string& node) {
        auto it = panels_.find(node);
        if (it == panels_.end()) {
            JLOGC("surface.lazy", jf::JLogLevel::Info) << "PanelLibrary: build empty page on first access node=" << node;
            it = panels_.emplace(node, std::make_unique<PanelModel>()).first;
        }
        return *it->second;
    }
    const PanelModel* find(const std::string& node) const {
        auto it = panels_.find(node);
        return it == panels_.end() ? nullptr : it->second.get();
    }
    PanelModel* find(const std::string& node) {
        auto it = panels_.find(node);
        return it == panels_.end() ? nullptr : it->second.get();
    }
    bool has(const std::string& node) const { return panels_.count(node) != 0; }

    // Re-key a node subtree after a rename / reparent: every page keyed at oldPrefix or under
    // oldPrefix + "/" moves to newPrefix (PanelLibrary::renamePrefix). No-op when equal/empty.
    void renamePrefix(const std::string& oldPrefix, const std::string& newPrefix) {
        if (oldPrefix.empty() || oldPrefix == newPrefix) return;
        std::map<std::string, std::unique_ptr<PanelModel>> next;
        const std::string under = oldPrefix + "/";
        for (auto& [k, v] : panels_) {
            std::string nk = k;
            if (k == oldPrefix)                 nk = newPrefix;
            else if (k.rfind(under, 0) == 0)    nk = newPrefix + k.substr(oldPrefix.size());
            next.emplace(std::move(nk), std::move(v));
        }
        panels_ = std::move(next);
    }

    // After a rename/reparent: fix any viewport (or node-tagged control) INSIDE a page that references the
    // moved subtree (a page can itself host viewports of other nodes). renamePrefix moves the page keys;
    // this fixes the cross-references. Call both.
    void retagRefs(const std::string& oldPrefix, const std::string& newPrefix) {
        for (auto& [k, v] : panels_) retagNodeProp(*v, oldPrefix, newPrefix);
    }

    // Drop a node subtree's pages (node + descendants) — the tree deleted it.
    void removePrefix(const std::string& prefix) {
        if (prefix.empty()) return;
        const std::string under = prefix + "/";
        for (auto it = panels_.begin(); it != panels_.end(); ) {
            if (it->first == prefix || it->first.rfind(under, 0) == 0) it = panels_.erase(it);
            else ++it;
        }
    }

    // Free a SINGLE node's page (its exact key), leaving descendant pages intact — used when the last
    // viewport of a node is deleted (the tree node stays; revisiting re-creates an empty page).
    void remove(const std::string& node) { panels_.erase(node); }

    // Does ANY stored page contain a viewport placement of `node`? (A page may itself host viewports of
    // other nodes.) The caller also checks the open surfaces; both together say whether the node is placed at all.
    // Every stored page key, so a caller can decide which are still placed (the prune at save).
    std::vector<std::string> pageKeys() const {
        std::vector<std::string> out; out.reserve(panels_.size());
        for (const auto& [k, v] : panels_) out.push_back(k);
        return out;
    }

    bool anyPlaces(const std::string& node) const {
        for (const auto& [k, v] : panels_)
            for (const auto& e : v->elements())
                if (e.type == "viewport" && viewportPage(e) == node) return true;
        return false;
    }

    // Every page, so a caller can walk the whole layout's bindings (see the drift check on load).
    const std::map<std::string, std::unique_ptr<PanelModel>>& pages() const { return panels_; }

    // SERIALISATION IS OUT OF LINE, in PanelLibrary.cpp, and that is a performance decision rather than a
    // tidiness one. These two walk every page in the document — 598 of them, 7.5 MB — and inline they
    // were compiled into whatever unit called them, which is main.cpp: the app's WIRING unit, where the
    // optimiser is turned down because 5,000 lines of menu and dock construction take two minutes to
    // compile at -O2. That put the document parse in the one place in the build that is not optimised,
    // and the studio showed its window in 0.1 s and its interface seven seconds later. Compiled here,
    // the parse runs at the model's own -O2 whatever the caller is built with.
    //
    // { "<node path>": <PanelModel::toJson()>, … }
    jf::JJson toJson() const;
    void      load(const jf::JJson& o);
    // Read a whole dashboard.gui and hand back its parsed document — the JSON parse itself is the other
    // half of that seven seconds, so it belongs on this side of the line too. Throws like JJson::parseFile.
    static jf::JJson parseDocument(const std::string& path);
private:
    std::map<std::string, std::unique_ptr<PanelModel>> panels_;
};

// Global hook so the data-driven "viewport" widget can resolve a node's page without depending on the app
// wiring. main.cpp points this at the live PanelLibrary; returns nullptr for an unknown / not-yet-authored
// node (the widget then draws a placeholder).
namespace nodeviewport {
    using Resolver = std::function<const PanelModel*(const std::string&)>;
    inline Resolver& resolver() { static Resolver r; return r; }
}

// Global hook so a hyperlink widget can navigate to a dashboard node without depending on the app wiring.
// main.cpp points this at the tree: it selects the node at `path` (which drives the surface view-switcher),
// exactly as if the user had clicked it in the tree. Unset / empty path → no-op.
// A WIDGET'S UNIT, BY ITS PATH. "@<uid>.units" -- or the bare uid -- names another widget, and this
// answers with the display unit THAT widget is showing.
//
// It has to be a separate hook because the widget sigil resolves to a DOUBLE, and a unit is a string.
// And it has to come from the widget rather than from a channel: the unit shown is the CONTROL's setting
// -- its displayUnit pin, or Auto falling through to the global preference for that quantity -- so a
// caption resolving the same channel independently answers for ITSELF. A gauge pinned to °F beside a
// caption reading Auto -> °C is exactly the disagreement a unit caption exists to prevent, and pointing
// at the control means the caption also follows a global C/F change the moment the gauge does.
namespace widgetsigil {
    // std::nullopt = NO SUCH WIDGET (a stale or mistyped address); an empty string = the widget resolved
    // and has no unit to show. They are different failures and a caption must not draw them the same: one
    // is an authoring error worth surfacing, the other is Raw doing exactly what Raw is for.
    using UnitOf = std::function<std::optional<std::string>(const std::string& addr)>;
    inline UnitOf& unitOf() { static UnitOf f; return f; }
    // ADDRESS -> BARE UID. The same widget is written four ways depending on where the text came from --
    // the picker yields "[@<uid>.units]", a hand-typed one may be "@<uid>.units", "<uid>.units" or just
    // "<uid>" -- and all four name it.
    //
    // Order matters, and getting it wrong is silent: stripping "@" BEFORE "[" left the sigil on a
    // bracketed address ("[@uid]" -> "@uid"), the uid lookup found nothing, and the caption fell back to
    // its text with nothing to say why. Brackets first, then the sigil, then the suffix — each step
    // re-examining what the previous one exposed.
    inline std::string normalizeAddr(std::string a) {
        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
        };
        trim(a);
        if (a.size() >= 2 && a.front() == '[' && a.back() == ']') { a.erase(0, 1); a.pop_back(); trim(a); }
        if (!a.empty() && a.front() == '@') { a.erase(0, 1); trim(a); }
        static const std::string suf = ".units";
        if (a.size() > suf.size() && a.compare(a.size() - suf.size(), suf.size(), suf) == 0)
            a.erase(a.size() - suf.size());
        trim(a);
        return a;
    }
}

namespace hyperlink {
    using Navigator = std::function<void(const std::string& path)>;
    inline Navigator& navigator() { static Navigator n; return n; }

    // CAN THIS LINK BE FOLLOWED RIGHT NOW? A nav node carries a visibility condition, and the run-mode
    // filter hides the node -- and its whole subtree -- when that condition is false. A link is a way of
    // reaching a node, so it has to answer to the same condition: offering the operator a live link to a
    // page the menu has deliberately hidden (a module they have turned off, a feature their tune does not
    // have) navigates them somewhere the tree says does not exist for them.
    //
    // Also false for a path that is not in the tree at all, which is the same question asked of a link
    // whose target was renamed or deleted underneath it.
    //
    // Unset = everything reachable: a host with no nav tree (the preferences prototype editor, tests)
    // must not have its links silently disabled by a hook nobody installed.
    using Reachable = std::function<bool(const std::string& path)>;
    inline Reachable& reachable() { static Reachable r; return r; }
    inline bool canFollow(const std::string& path) {
        return path.empty() ? false : (!reachable() || reachable()(path));
    }
}
