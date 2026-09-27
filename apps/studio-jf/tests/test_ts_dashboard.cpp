// Parses the [Menu] + [UserDefined] halves of a TunerStudio-format ini and reports what it found, so the
// eager dashboard import can be checked against a REAL file (a full ECU definition runs to thousands of
// lines) rather than a fixture that only contains syntax someone remembered to write down.
//
//   ./build/ts_dashboard_test <path-to.ini>     — summary + a sample of the tree
//   ./build/ts_dashboard_test                   — the built-in syntax cases only

#include "../src/model/TsDashboard.h"
#include "../src/model/TsIniImporter.h"
#include "../src/model/TsDashboardConvert.h"
#include "../src/model/Cache.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>

using namespace tsdash;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("[ts-dash] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

static void syntaxCases() {
    const std::string ini = R"([Menu]
    menu = "&Setup"
        subMenu = baseEngine, "Engine Basics"
        groupMenu = "Safety cuts"
            groupChildMenu = revLimiter, "Rev limiter"
            groupChildMenu = leanCut, "Lean cut", 0, { fuelEnabled }
        subMenu = std_separator

[UserDefined]
    indicatorPanel = injStatusPanel, 2
        indicator = {injFault}, "Injectors OK", "Injector fault"

    dialog = injSetupDialog, "Injector setup", yAxis
        panel = injStatusPanel
        field = "Flow rate", injFlowRate, { fuelEnabled }
        gauge = MapGauge, South
        liveGraph = injTraceGraph, "Trace", South
            graphLine = pulseWidth
            graphLine = dutyCycle
        commandButton = "Save", cmd_save
        notAKeyword = ignored, 1, 2
)";

    const Dashboard d = parse(ini);

    check(d.menus.size() == 1 && d.menus[0].label == "&Setup", "one top-level menu, label read");
    // std_separator is DROPPED: the menu becomes a navigation tree, where a rule would be a row with no
    // name and no page (see TsDashboard.h). The grouping survives as the tree's own nesting.
    check(d.menus[0].children.size() == 2, "subMenu + groupMenu are children; std_separator is not");
    {
        bool sepRow = false;
        for (const auto& c : d.menus[0].children) if (c.dialogId == "std_separator") sepRow = true;
        check(!sepRow, "the separator does not become a menu entry");
    }
    check(d.menus[0].children[0].dialogId == "baseEngine", "subMenu leaf carries its dialog id");
    check(d.menus[0].children[0].label == "Engine Basics", "subMenu leaf takes the QUOTED label");
    const auto& grp = d.menus[0].children[1];
    check(grp.dialogId.empty() && grp.children.size() == 2, "groupMenu is a heading with two children");
    check(grp.children[1].condition == "fuelEnabled", "trailing {condition} captured");
    check(d.leafCount() == 3, "leaf count = pages the import must build");

    check(d.panels.count("injStatusPanel") == 1, "indicatorPanel parsed");
    check(d.panels.at("injStatusPanel").columns == 2, "indicatorPanel column count");
    check(d.panels.at("injStatusPanel").lamps.size() == 1, "indicator lamp under its panel");
    check(d.panels.at("injStatusPanel").lamps[0].expr == "injFault", "{expr} unwrapped");

    check(d.dialogs.count("injSetupDialog") == 1, "dialog parsed");
    const Dialog& dlg = d.dialogs.at("injSetupDialog");
    check(dlg.title == "Injector setup" && dlg.layout == "yAxis", "dialog title + layout");
    check(dlg.items.size() == 5, "5 known items; the unknown keyword is skipped, not fatal");
    check(dlg.items[0].kind == Item::Kind::Panel && dlg.items[0].panelId == "injStatusPanel",
          "panel item references its panel");
    check(dlg.items[1].kind == Item::Kind::Field && dlg.items[1].var == "injFlowRate",
          "field binds its variable");
    check(dlg.items[1].condition == "fuelEnabled", "field condition captured");
    check(dlg.items[2].kind == Item::Kind::Gauge && dlg.items[2].placement == "South", "gauge + placement");
    check(dlg.items[3].kind == Item::Kind::LiveGraph && dlg.items[3].lines.size() == 2,
          "graphLine rows attach to the liveGraph above them");
    check(dlg.items[4].kind == Item::Kind::Command && dlg.items[4].var == "cmd_save", "commandButton");

    // The ini's keyboard mnemonic ("&Setup" = Alt+S) has no counterpart here, so it is stripped — but
    // ONLY when leading, because these labels also carry literal ampersands.
    {
        const Dashboard mn = parse("[Menu]\n"
                                   "  menu = \"&Setup\"\n"
                                   "    subMenu = a, \"Delay & Dwell\"\n"
                                   "    subMenu = b, \"Research&Development\"\n");
        check(mn.menus[0].label == "&Setup", "the PARSER keeps the ini faithful (& intact)");
        check(mn.menus[0].children[0].label == "Delay & Dwell", "a literal ampersand is untouched");
        check(mn.menus[0].children[1].label == "Research&Development", "a mid-word ampersand is untouched");
    }

    // A label containing a comma must survive the arg split.
    const Dashboard q = parse("[UserDefined]\ndialog = d, \"T\"\nfield = \"Boost, target\", bt\n");
    check(q.dialogs.at("d").items[0].label == "Boost, target", "comma inside a quoted label");
}

int main(int argc, char** argv) {
    syntaxCases();

    if (argc > 1) {
        std::ifstream f(argv[1]);
        if (!f) { std::printf("[ts-dash] cannot open %s\n", argv[1]); return 1; }
        std::stringstream ss; ss << f.rdbuf();
        const Dashboard d = parse(ss.str());

        size_t items = 0, byKind[8] = {0};
        for (const auto& [id, dlg] : d.dialogs)
            for (const auto& it : dlg.items) { ++items; byKind[static_cast<int>(it.kind)]++; }
        size_t lamps = 0;
        for (const auto& [id, p] : d.panels) lamps += p.lamps.size();

        std::printf("\n[ts-dash] %s\n", argv[1]);
        std::printf("  menus %zu, pages(leaves) %zu, dialogs %zu, indicatorPanels %zu (%zu lamps), gauges %zu\n",
                    d.menus.size(), d.leafCount(), d.dialogs.size(), d.panels.size(), lamps, d.gauges.size());
        std::printf("  items %zu — field %zu, panel %zu, gauge %zu, indicator %zu, command %zu, text %zu, graph %zu\n",
                    items, byKind[0], byKind[1], byKind[2], byKind[3], byKind[4], byKind[5], byKind[6]);

        // ---- RESOLUTION REPORT ------------------------------------------------------------------------
        // Cross-reference every symbol the UI half names against the meta the data half produced. A binding
        // or condition that resolves to nothing does not fail loudly: an unbound widget shows no value, and
        // an unresolvable condition evaluates to 0 -> false -> the widget never appears at all. Both look
        // like "the importer lost it". So the import must be able to state exactly what will not behave, up
        // front, instead of leaving it to be discovered one page at a time.
        std::string err;
        auto meta = TsIniImporter::importFile(argv[1], &err);
        if (!meta) { std::printf("[ts-dash] meta import failed: %s\n", err.c_str()); return 1; }
        const auto& cfg  = (*meta)["config"]["ts"];
        const auto& telem = (*meta)["telemetry"];
        // A [PcVariables] declaration resolves too: the studio gives it host-side storage, so a widget or
        // condition naming one is bound to something real — just not to anything in the ECU.
        std::set<std::string> pcNames;
        for (const jf::JJson& v : (*meta)["pcVars"].arr()) pcNames.insert(v["name"].str());
        auto resolves = [&](const std::string& n) {
            return !n.empty() && (cfg.contains(n) || telem.contains(n) || pcNames.count(n));
        };

        // Identifiers inside a condition expression, minus numbers and TS helper calls.
        auto identsOf = [](const std::string& e) {
            std::vector<std::string> out;
            for (size_t i = 0; i < e.size();) {
                if (std::isalpha((unsigned char)e[i]) || e[i] == '_') {
                    size_t j = i;
                    while (j < e.size() && (std::isalnum((unsigned char)e[j]) || e[j] == '_')) ++j;
                    const bool call = j < e.size() && e[j] == '(';        // bitStringValue(...) etc.
                    if (!call) out.push_back(e.substr(i, j - i));
                    i = j;
                } else ++i;
            }
            return out;
        };

        std::map<std::string, int> badBind, badCond;
        size_t binds = 0, conds = 0, condIdents = 0;
        for (const auto& [id, dlg] : d.dialogs) {
            for (const auto& it : dlg.items) {
                if (it.kind == Item::Kind::Gauge && !it.var.empty()) {
                    ++binds;
                    const std::string ch = d.gaugeChannel(it.var);   // gauge id -> its channel
                    if (!resolves(ch.empty() ? it.var : ch)) badBind[it.var]++;
                } else if (it.kind == Item::Kind::Field && !it.var.empty()) {
                    ++binds;
                    if (!resolves(it.var)) badBind[it.var]++;
                }
                for (const auto& ln : it.lines) { ++binds; if (!resolves(ln)) badBind[ln]++; }
                if (!it.condition.empty()) {
                    ++conds;
                    for (const auto& sym : identsOf(it.condition)) {
                        ++condIdents;
                        if (!resolves(sym)) badCond[sym]++;
                    }
                }
            }
        }
        size_t badBindTotal = 0, badCondTotal = 0;
        for (const auto& [k, n] : badBind) badBindTotal += n;
        for (const auto& [k, n] : badCond) badCondTotal += n;

        std::printf("  pcVariables %zu (host-side, no ECU storage)\n", pcNames.size());
        std::printf("  bindings   %zu, unresolved %zu (%zu distinct)\n", binds, badBindTotal, badBind.size());
        std::printf("  conditions %zu using %zu identifier(s), unresolved %zu (%zu distinct)\n",
                    conds, condIdents, badCondTotal, badCond.size());
        auto top = [](const std::map<std::string,int>& m, const char* what) {
            std::vector<std::pair<std::string,int>> v(m.begin(), m.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < v.size() && i < 8; ++i)
                std::printf("    %-40s %s x%d\n", v[i].first.c_str(), what, v[i].second);
        };
        top(badBind, "binding");
        top(badCond, "condition");

        // Every menu leaf must resolve to a dialog, or its page would come out empty.
        size_t missing = 0;
        std::vector<const MenuNode*> stack;
        for (const auto& m : d.menus) stack.push_back(&m);
        while (!stack.empty()) {
            const MenuNode* n = stack.back(); stack.pop_back();
            if (n->children.empty() && !n->dialogId.empty() && !d.dialogs.count(n->dialogId)) {
                if (missing < 5) std::printf("  UNRESOLVED leaf '%s' -> dialog '%s'\n",
                                             n->label.c_str(), n->dialogId.c_str());
                ++missing;
            }
            for (const auto& c : n->children) stack.push_back(&c);
        }
        std::printf("  unresolved leaves: %zu\n", missing);

        // ---- CONVERSION: the dashboard the import will actually produce ---------------------------------
        // Verified against the real ini rather than a fixture, because the interesting cases are the ones
        // nobody would think to write down: a menu leaf naming a table instead of a dialog, a panel that
        // references another panel, an indicatorPanel's lamp grid.
        {
            const std::string metaPath = "/tmp/ts_convert_probe.meta";
            TsIniImporter::writeMetaFile(*meta, metaPath);
            MetaModel m;
            if (!m.loadFile(metaPath)) { std::printf("  convert: meta failed to load\n"); return 1; }

            // controlFor() reads the LIVE Cache — without this every control defaults to "value" and the
            // conversion looks like it ignored the bindings.
            Cache::instance().setMeta(&m);

            tsconvert::Report rep;
            const jf::JJson doc = tsconvert::buildDashboard(d, m, &rep);
            std::printf("\n  CONVERTED: %d pages, %d widgets (%d table pages, %d empty, %d items skipped)\n",
                        rep.pages, rep.widgets, rep.tables, rep.emptyPages, rep.skippedItems);
            std::printf("  tree roots: %zu, library pages: %zu\n",
                        doc["tree"].arr().size(), doc["panelLibrary"].obj().size());

            // ---- STRING-EXPRESSION LAMP LABELS (spec §23) -------------------------------------------
            // "indicator = {expr}, \"ETB OK\", { ETB: bitStringValue(etbCutCodeList, …) }" — the ON caption
            // is TEXT PLUS A LOOKUP, not a string. Carried across literally, the lamp painted the middle
            // of its own source ("e(et"), because a 63-character caption centred in a 120px box starts
            // well to the left of it. The prefix and the two halves of the lookup must survive instead.
            {
                int checked = 0, literalExpr = 0;
                std::function<void(const jf::JJson&)> scan = [&](const jf::JJson& e) {
                    if (e["type"].str() == "indicator") {
                        const std::string on = e["props"]["onTitle"].str();
                        if (!on.empty() && on.front() == '{') ++literalExpr;      // never acceptable
                        if (!e["props"]["onTitleList"].str().empty()) {
                            ++checked;
                            check(!e["props"]["onTitleValue"].str().empty(),
                                  "string-expression lamp carries the index expression");
                        }
                    }
                    if (e["props"].contains("children"))
                        if (auto kids = jf::JJson::tryParse(e["props"]["children"].str()); kids)
                            for (const auto& k : kids->arr()) scan(k);
                };
                for (const auto& [name, page] : doc["panelLibrary"].obj())
                    if (auto m2 = jf::JJson::tryParse(page.str()); m2)
                        for (const auto& w : (*m2)["widgets"].arr()) scan(w);
                for (const auto& s2 : doc["surfaces"]["pool"].arr())
                    for (const auto& w : s2["model"]["widgets"].arr()) scan(w);
                std::printf("  string-expression lamps: %d resolved, %d left as literal expressions\n",
                            checked, literalExpr);
                check(literalExpr == 0, "no lamp caption is a raw {expression}");
                check(checked > 0,      "the definition's string-expression lamps were recognised");
            }

            // What the widgets came out AS — the mix is the useful signal: all-"value" would mean
            // controlFor never ran, and no tables would mean the table leaves silently produced nothing.
            // Children live inside a panel's "children" prop as JSON, so a flat scan would report only the
            // top-level panels and make a correct conversion look empty.
            std::map<std::string, int> byType;
            int maxDepth = 0;
            std::function<void(const jf::JJson&, int)> walk = [&](const jf::JJson& w, int depth) {
                byType[w["type"].str()]++;
                maxDepth = std::max(maxDepth, depth);
                if (!w.contains("props")) return;
                const std::string kids = w["props"]["children"].str();
                if (kids.empty()) return;
                if (auto arr = jf::JJson::tryParse(kids); arr && arr->isArray())
                    for (const jf::JJson& c : arr->arr()) walk(c, depth + 1);
            };
            for (const auto& [k, page] : doc["panelLibrary"].obj())
                for (const jf::JJson& w : page["widgets"].arr()) walk(w, 1);
            std::printf("  deepest nesting: %d\n", maxDepth);

            // The children prop is JSON-inside-a-prop, so a malformed dump would produce panels that look
            // right in the document and come up EMPTY in the app. Check every panel parses back.
            int panels = 0, badChildren = 0, withKids = 0;
            std::function<void(const jf::JJson&)> verify = [&](const jf::JJson& w) {
                if (w["type"].str() == "panel") {
                    ++panels;
                    const std::string kids = w["props"]["children"].str();
                    auto arr = jf::JJson::tryParse(kids);
                    if (!arr || !arr->isArray()) { ++badChildren; return; }
                    if (!arr->arr().empty()) ++withKids;
                    for (const jf::JJson& c : arr->arr()) verify(c);
                }
            };
            for (const auto& [k, page] : doc["panelLibrary"].obj())
                for (const jf::JJson& w : page["widgets"].arr()) verify(w);
            std::printf("  panels %d, with children %d, UNPARSEABLE children %d\n",
                        panels, withKids, badChildren);

            // Every page needs a viewport on a surface or it can never be seen: the Surface renders the
            // viewport whose node is ACTIVE, and a library page with no viewport is unreachable.
            const jf::JJson& mainModel = doc["surfaces"]["pool"].arr()[0]["model"];
            std::set<std::string> viewportNodes;
            for (const jf::JJson& w : mainModel["widgets"].arr())
                if (w["type"].str() == "viewport") viewportNodes.insert(w["props"]["node"].str());
            int unreachable = 0;
            for (const auto& [path, page] : doc["panelLibrary"].obj())
                if (!viewportNodes.count(path)) ++unreachable;
            std::printf("  viewports on Main: %zu, pages with NO viewport: %d\n",
                        viewportNodes.size(), unreachable);
            std::printf("  widget types:");
            for (const auto& [t, n] : byType) std::printf(" %s=%d", t.c_str(), n);
            std::printf("\n");
            std::printf("  menu names:");
            for (const jf::JJson& t : doc["tree"].arr()) std::printf(" '%s'", t["name"].str().c_str());
            std::printf("\n");
            for (const auto& e : rep.emptyNames) std::printf("    EMPTY  %s\n", e.c_str());
            for (const auto& [k, n] : rep.skippedKinds) std::printf("    SKIP   %-40s x%d\n", k.c_str(), n);
        }
    }

    std::printf("[ts-dash] %d failure(s)\n", fails);
    return fails;
}
