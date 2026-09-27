#include "DictionaryTree.h"

#include "../model/ExprAst.h"   // the language's own function list — what the builder can offer

#include "../model/Cache.h"   // label/units lookups while naming the tree rows
#include "../model/SigilResolvers.h"   // AppStateSigilResolver — the '%' studio-state channel names

#include <algorithm>
#include <map>
#include <set>

using namespace jf;

// Flat list of every bindable path (telemetry channels + config scalars + tables) for the source picker.
std::vector<std::string> channelPaths(const MetaModel& meta) {
    std::vector<std::string> paths;
    for (const auto& [name, tf] : meta.telemetry())    paths.push_back(name);
    for (const auto& [path, cf] : meta.config())       paths.push_back(path);
    for (const auto& [path, ct] : meta.configTables()) paths.push_back(path);
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

namespace {
std::string modOf(const std::string& p) { const auto d = p.find('.'); std::string m = d == std::string::npos ? p : p.substr(0, d); return m.empty() ? std::string("General") : m; }
std::string afterMod(const std::string& p) { const auto d = p.find('.'); return d == std::string::npos ? p : p.substr(d + 1); }
// Type glyph before each leaf: kind from the binding's default
// control — table→grid, curve→rounded, enum→bar, toggle→pill, config→filled, telemetry/else→hollow.
int iconFor(const std::string& ctrl) {
    if (ctrl == "table")  return 1; if (ctrl == "curve")  return 2; if (ctrl == "enum") return 3;
    if (ctrl == "toggle") return 4; if (ctrl == "config") return 5; return 6;
}
JTreeViewNode leaf(const std::string& label, const std::string& path) {
    JTreeViewNode n{label, false, false, {}};
    n.userData = path;
    n.icon = iconFor(Cache::instance().controlFor(path));
    return n;
}
// A packed field's named groups become its CHILDREN: the parent row still binds the whole byte (rarely
// what you want, but it is a real field), and each group binds on its own — a 1-bit check as a
// checkbox, a 2-bit severity as a list. Without these rows the groups were addressable but invisible,
// so a sensor's six diagnostics checks could only be reached by editing the byte as a number.
JTreeViewNode withBitGroups(JTreeViewNode n, const std::vector<MetaModel::BitGroup>& groups,
                            const std::string& fieldPath) {
    for (const auto& g : groups)
        n.children.push_back(leaf(g.label.empty() ? g.name : g.label, fieldPath + "." + g.name));
    return n;
}
// A table's axis machinery — the breakpoint array and the <axis>_n / _src / _en scalars — is edited
// inside the table, so it is not offered as a field of its own.
std::set<std::string> axisOwnedPaths(const MetaModel& meta) {
    std::set<std::string> owned;
    for (const auto& [path, ct] : meta.configTables())
        for (const auto& ax : ct.axes) {
            if (!ax.array.empty())     owned.insert(ax.array);
            if (!ax.nScalar.empty())   owned.insert(ax.nScalar);
            if (!ax.srcScalar.empty()) owned.insert(ax.srcScalar);
            if (!ax.enScalar.empty())  owned.insert(ax.enScalar);
        }
    return owned;
}
}  // namespace

const char* pcPlaceholderCaption() { return "New variable\xE2\x80\xA6"; }   // "New variable…"

// The CONFIGURATION half of the dictionary, on its own — module ▸ Values / Tables / arrays ▸ element ▸
// field. Extracted so the field PICKER shows the same tree the dictionary does instead of a flat list of
// 2854 paths under a handful of module headings, where nothing says which sensor or which cylinder a row
// belongs to. One builder, so the two cannot drift.
JTreeViewNode buildConfigTree(const MetaModel& meta) {
    const std::set<std::string> axisOwned = axisOwnedPaths(meta);
    std::map<std::string, JTreeViewNode> mods;                    // module -> module node
    auto moduleNode = [&](const std::string& mod) -> JTreeViewNode& {
        JTreeViewNode& n = mods[mod]; if (n.label.empty()) n.label = mod; return n;
    };
    auto subgroup = [](JTreeViewNode& m, const char* title) -> JTreeViewNode& {
        for (auto& c : m.children) if (c.label == title) return c;
        m.children.push_back(JTreeViewNode{title, false, false, {}}); return m.children.back();
    };
    std::vector<std::string> ckeys;                              // sorted
    for (const auto& [path, cf] : meta.config()) ckeys.push_back(path);
    std::sort(ckeys.begin(), ckeys.end());
    for (const auto& path : ckeys) {                             // scalars -> module ▸ Values
        if (axisOwned.count(path)) continue;
        // Original shows the raw field-name (k.section('.',1)), not the label — match it.
        subgroup(moduleNode(modOf(path)), "Values").children.push_back(
            withBitGroups(leaf(afterMod(path), path), meta.config().at(path).bitGroups, path));
    }
    std::vector<std::string> tkeys;
    for (const auto& [path, ct] : meta.configTables()) tkeys.push_back(path);
    std::sort(tkeys.begin(), tkeys.end());
    for (const auto& path : tkeys) {                             // tables -> module ▸ Tables
        if (axisOwned.count(path)) continue;
        subgroup(moduleNode(modOf(path)), "Tables").children.push_back(leaf(afterMod(path), path));
    }
    // Arrays-of-structs (sensors, outputs, precond/cand): module ▸ "<label> (count)" ▸ element ▸
    // { element tables, scalar fields, sub-arrays ▸ row ▸ sub-field }. The JTreeView is a static tree,
    // so it is built eagerly. Tokens are arrKey[elemId].field and sub[j].field — the resolver accepts the catalog id or numeric index.
    std::vector<std::string> akeys;
    for (const auto& [key, ca] : meta.configArrays()) akeys.push_back(key);
    std::sort(akeys.begin(), akeys.end());
    for (const auto& key : akeys) {
        const auto& ca = meta.configArrays().at(key);
        JTreeViewNode arr{(ca.label.empty() ? ca.name : ca.label) + "  (" + std::to_string(ca.count) + ")", false, false, {}};
        for (int i = 0; i < ca.count; ++i) {
            const std::string elemKey = (i < int(ca.elementIds.size()) && !ca.elementIds[i].empty())
                                      ? ca.elementIds[i] : std::to_string(i);
            const std::string elemPath = key + "[" + elemKey + "]";
            const std::string elemName = (i < int(ca.elementLabels.size()) && !ca.elementLabels[i].empty())
                                      ? ca.elementLabels[i] : (ca.name + " " + std::to_string(i));
            // The ELEMENT itself is draggable, carrying its element path (sensors.sensor[clt]) — drop it
            // on a viewport to say "this view is about THIS sensor", which is what resolves a template
            // page's sensors.sensor[*].… bindings. It is not a field, so it binds no ordinary control:
            // the drop handler only accepts it onto an existing widget, never as a tiled new one.
            JTreeViewNode el{elemName, false, false, {}};
            el.userData = elemPath;
            for (const auto& t : ca.tables)                                   // element-local tables
                el.children.push_back(leaf(t.label.empty() ? t.name : t.label, elemPath + "." + t.name));
            // Fields sharing a `subcat` nest under a node of that name (created at the first such field);
            // the rest stay direct children. A subcat node is not draggable (empty userData).
            auto subnode = [](JTreeViewNode& parent, const std::string& title) -> JTreeViewNode& {
                for (auto& c : parent.children) if (c.userData.empty() && c.label == title) return c;
                parent.children.push_back(JTreeViewNode{title, false, false, {}});
                return parent.children.back();
            };
            for (const auto& fld : ca.fields) {                               // element scalar fields
                const std::string fpath = elemPath + "." + fld.name;
                JTreeViewNode node = withBitGroups(leaf(fld.label.empty() ? fld.name : fld.label, fpath),
                                                   fld.bits, fpath);
                if (fld.subcat.empty()) el.children.push_back(std::move(node));
                else                    subnode(el, fld.subcat).children.push_back(std::move(node));
            }
            // Element sub-arrays (a stream's cells, an output's candidates, a sensor's preconditions).
            // The GROUP is the run itself and is draggable, so dropping it gives one 1D Array over the
            // whole thing — which is what the path "<sub>[].<field>" names. Only the group could carry
            // that: it used to be a bare container, so the only way to place a run was one widget per
            // index, and the only way to get the strip binding was to type it.
            //
            // A run needs ONE field to be unambiguous. With several ({sig, role}) the group cannot say
            // which column it is, so each field gets its own draggable run row instead.
            for (const auto& na : ca.arrays) {
                const std::string sub = elemPath + "." + na.name + "[].";
                JTreeViewNode grp{na.name + "  (" + std::to_string(na.count) + ")", false, false, {}};
                if (na.fields.size() == 1) {
                    grp.userData = sub + na.fields[0].name;
                    grp.icon = 2;                                        // a run of values reads as a curve
                } else {
                    for (const auto& sf : na.fields) {
                        JTreeViewNode run{(sf.label.empty() ? sf.name : sf.label) + "  (all "
                                              + std::to_string(na.count) + ")", false, false, {}};
                        run.userData = sub + sf.name;
                        run.icon = 2;
                        grp.children.push_back(std::move(run));
                    }
                }
                // Then the members. Numbered from 0, because that is what the path they carry says —
                // a row labelled "cell 1" whose binding is cell[0] is a trap, and the ELEMENT rows
                // above this already count from 0.
                for (int j = 0; j < na.count; ++j) {
                    JTreeViewNode row{na.name + " " + std::to_string(j), false, false, {}};
                    for (const auto& sf : na.fields)
                        row.children.push_back(leaf(sf.label.empty() ? sf.name : sf.label,
                                                    elemPath + "." + na.name + "[" + std::to_string(j) + "]." + sf.name));
                    grp.children.push_back(std::move(row));
                }
                el.children.push_back(std::move(grp));
            }
            arr.children.push_back(std::move(el));
        }
        if (!arr.children.empty()) moduleNode(modOf(key)).children.push_back(std::move(arr));
    }
    JTreeViewNode cfg{"Configuration", true, false, {}};
    for (auto& [mod, n] : mods) cfg.children.push_back(std::move(n));
    return cfg;
}

JTreeViewNode buildDictionary(const MetaModel& meta) {
    // Two roots — Telemetry (module ▸ channels) and
    // Configuration (MODULE-FIRST: each module owns its Values (scalars) + Tables). Every leaf carries its
    // BINDING PATH in userData (label is display-only). A table's axis machinery — the breakpoint array and
    // the <axis>_n / _src / _en scalars — is edited inside the table, so it's excluded from the dictionary.
    JTreeViewNode root{"Dictionary", true, false, {}};

    // Telemetry: module ▸ channels (bind path = the channel name). meta.telemetry() is an
    // unordered_map — iterating it directly yields hash order, so SORT the keys first;
    // otherwise leaves within a module come out scrambled.
    {
        std::map<std::string, JTreeViewNode> byMod;
        std::vector<std::string> keys;
        keys.reserve(meta.telemetry().size());
        for (const auto& [name, tf] : meta.telemetry()) keys.push_back(name);
        std::sort(keys.begin(), keys.end());
        for (const auto& name : keys) {
            const auto& tf = meta.telemetry().at(name);
            const std::string mod = tf.module.empty() ? std::string("General") : tf.module;
            byMod[mod].label = mod;
            byMod[mod].children.push_back(leaf(tf.label.empty() ? name : tf.label, name));
        }
        JTreeViewNode tel{"Telemetry", true, false, {}};
        for (auto& [mod, n] : byMod) tel.children.push_back(std::move(n));
        if (!tel.children.empty()) root.children.push_back(std::move(tel));
    }

    // Configuration: module ▸ Values / Tables / arrays — built by buildConfigTree, which the field
    // picker shows too.
    {
        JTreeViewNode cfg = buildConfigTree(meta);
        if (!cfg.children.empty()) root.children.push_back(std::move(cfg));
    }

    // Commands: the CLI command catalog (auto-derived from the firmware registry). Drag a leaf to drop a
    // CommandButton bound to it. Token = "cmd:<name>" (registry order, not sorted).
    {
        JTreeViewNode cmds{"Commands", false, false, {}};
        for (const auto& c : meta.commands().arr()) {
            const std::string name = c["name"].str();
            if (name.empty()) continue;
            JTreeViewNode n{name, false, false, {}};
            n.userData = "cmd:" + name;
            n.icon = 4;                       // pill/button glyph — a command drops a CommandButton
            cmds.children.push_back(std::move(n));
        }
        if (!cmds.children.empty()) root.children.push_back(std::move(cmds));
    }
    // PcVariables — host-side values the ECU knows nothing about (see MetaModel::applyPcVars). They are
    // ordinary config paths, so they bind and display like anything else here; the difference is that the
    // SET is editable from this tree, hence the trailing placeholder row. An imported ini brings its own;
    // anything else you add yourself.
    {
        JTreeViewNode cat{"PcVariables", true, false, {}};
        for (const auto& v : meta.pcVars()) {
            JTreeViewNode n = leaf(v.name, "pc." + v.name);
            cat.children.push_back(std::move(n));
        }
        // The "add" affordance, mirroring the navigation tree's ghost rows: activating it creates a
        // variable. Marked placeholder so the app can tell it from a real leaf.
        JTreeViewNode ph{pcPlaceholderCaption(), false, false, {}};
        ph.placeholder = true;
        cat.children.push_back(std::move(ph));
        root.children.push_back(std::move(cat));
    }

    return root;
}

// Categorised tree of EVERY sigil field, for the ExpressionEditor's ? navigator. Derived from
// buildDictionary so the navigator shows THE SAME structure the user browses in the dictionary dock
// (module ▸ Values, arrays-of-structs ▸ element ▸ fields, …), with each SCALAR leaf's binding path
// rewritten as its ready-to-insert bracketed token: Telemetry → [$chan], Configuration → [#path].
// Table/curve leaves are KEPT but left as the RAW path — a table/curve control binds through
// isTable/resolveTable, not the # scalar decoder, so its data source is the bare path (a [#…] sigil
// would break it). The Commands root is dropped (a command isn't a value); a Widgets ▸ name ▸
// [@name.prop] root is appended from the @ lister.
JTreeViewNode buildSigilTree(const MetaModel& meta, const std::vector<std::string>& widgetFields,
                             bool firmware) {
    JTreeViewNode root{"Fields", true, false, {}};

    // Rewrite every SCALAR leaf as a sigil token; keep table/curve leaves as the raw path; prune emptied groups.
    std::function<bool(JTreeViewNode&, char)> tokenise = [&](JTreeViewNode& n, char sig) -> bool {
        if (!n.userData.empty()) {
            const std::string ctrl = Cache::instance().controlFor(n.userData);
            if (ctrl == "table" || ctrl == "curve") return true;   // valid source, bound by RAW path — keep, don't sigil-wrap
            n.userData = std::string("[") + sig + n.userData + "]";
            return true;
        }
        for (auto it = n.children.begin(); it != n.children.end();)
            it = tokenise(*it, sig) ? std::next(it) : n.children.erase(it);
        return !n.children.empty();
    };

    JTreeViewNode dict = buildDictionary(meta);
    for (auto& top : dict.children) {
        const char sig = top.label == "Telemetry" ? '$' : top.label == "Configuration" ? '#' : '\0';
        if (sig && tokenise(top, sig)) root.children.push_back(std::move(top));
    }
    {   // Widgets (@) — "name.prop" grouped by widget name.
        std::map<std::string, JTreeViewNode> byName;
        for (const auto& f : widgetFields) { const auto d = f.find('.'); if (d == std::string::npos) continue;
            const std::string nm = f.substr(0, d), pr = f.substr(d + 1); byName[nm].label = nm;
            JTreeViewNode n{pr, false, false, {}}; n.userData = "[@" + f + "]";
            byName[nm].children.push_back(std::move(n)); }
        JTreeViewNode wj{"Widgets", true, false, {}}; for (auto& [n, nn] : byName) wj.children.push_back(std::move(nn));
        if (!wj.children.empty()) root.children.push_back(std::move(wj));
    }
    {   // FUNCTIONS — the language itself, offered rather than remembered. The tree was every FIELD you
        // could name and nothing about what you could do with them, so "is there an age()?" was a
        // question you answered by reading the schema help of a field that happened to mention one.
        // Listed per back end: a host-only function in a firmware expression compiles to an error, and
        // an editor that offers it is an editor that sets you up to fail.
        JTreeViewNode fj{"Functions", true, false, {}};
        for (const expr_ast::FnInfo& f : expr_ast::functions()) {
            if (firmware ? !f.firmware : !f.host) continue;
            JTreeViewNode n{std::string(f.name) + "()", false, false, {}};
            n.userData = std::string(f.name) + "(";       // inserted with the paren; you type the rest
            fj.children.push_back(std::move(n));
        }
        if (!fj.children.empty()) root.children.push_back(std::move(fj));
    }
    if (firmware) {
        // TABLES — every map an expression can read, by name. table(<name>) reads it at its own axes;
        // interp(<name>, x) reads it at an x you supply. Named, never numbered: an id is the table's
        // place in the schema's own order, which nobody can read back and which moves when a table is
        // added. Grouped by module, because 167 tables in one list is the problem the tree exists for.
        std::map<std::string, JTreeViewNode> byMod;
        for (const MetaModel::TableRef& t : meta.tableRegistry()) {
            const auto us = t.name.find('_');
            const std::string mod = us == std::string::npos ? t.name : t.name.substr(0, us);
            byMod[mod].label = mod;
            JTreeViewNode n{t.label.empty() ? t.name : t.label, false, false, {}};
            n.userData = "table(" + t.name + ")";
            byMod[mod].children.push_back(std::move(n));
        }
        JTreeViewNode tj{"Tables", true, false, {}};
        for (auto& [m, mn] : byMod) tj.children.push_back(std::move(mn));
        if (!tj.children.empty()) root.children.push_back(std::move(tj));
    }
    {   // App State (%) — the studio's own link/session flags (connected, …). No meta describes them
        // because they report on the TOOL, not the ECU, so they never entered the dictionary tree — which
        // is why "[%connected]" read as an unknown field. Names come from the resolver itself
        // (SigilResolvers.h), so a new flag appears here without editing this list.
        JTreeViewNode sj{"App State", true, false, {}};
        for (const std::string& nm : AppStateSigilResolver{}.available()) {
            JTreeViewNode n{nm, false, false, {}};
            n.userData = "[%" + nm + "]";
            sj.children.push_back(std::move(n));
        }
        if (!sj.children.empty()) root.children.push_back(std::move(sj));
    }
    return root;
}

// Build a CATEGORISED source tree (module ▸ channels) for the PopupSignalPicker: signals
// grouped under their category, with a filter. Each leaf's userData is
// the binding path/token; its label is the friendly meta caption. Grouped by the channel's telemetry module
// (else the path's module prefix), sorted.
JTreeViewNode buildSourceTree(const std::vector<std::string>& paths, const MetaModel& meta) {
    JTreeViewNode root{"Sources", true, false, {}};
    std::map<std::string, JTreeViewNode> byCat;
    std::vector<std::string> sorted = paths;
    std::sort(sorted.begin(), sorted.end());
    for (const auto& p : sorted) {
        std::string cat;
        if (auto it = meta.telemetry().find(p); it != meta.telemetry().end()) cat = it->second.module;
        if (cat.empty()) { const auto d = p.find('.'); cat = (d == std::string::npos) ? std::string("General") : p.substr(0, d); }
        std::string label = meta.labelFor(p); if (label.empty()) label = p;
        byCat[cat].label = cat;
        JTreeViewNode leaf{label, false, false, {}}; leaf.userData = p;
        byCat[cat].children.push_back(std::move(leaf));
    }
    for (auto& [c, n] : byCat) root.children.push_back(std::move(n));
    return root;
}
