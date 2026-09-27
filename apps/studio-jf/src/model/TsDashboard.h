#pragma once

// TsDashboard — the UI half of a TunerStudio / rusEFI ".ini": [Menu] (the navigation structure) and
// [UserDefined] (the dialogs those menu entries open). TsIniImporter converts the DATA half — config,
// telemetry, tables — and this converts what the user actually looks at.
//
// The import is EAGER and total: every menu entry and every dialog is parsed once, converted to native
// widgets, and written into the project. The ini is never consulted again — it is not a runtime format, a
// fallback, or a lazily-regenerated cache. What you get is an ordinary studio dashboard you can edit.
//
// Parsing is deliberately lenient: an unknown keyword is skipped, not an error. A real ini (a full ECU
// definition can run to ~11k lines, hundreds of dialogs and menu entries) accretes syntax over years, and a strict parser would refuse
// the whole file over one line it had never seen.

#include <map>
#include <cctype>
#include <string>
#include <vector>

namespace tsdash {

// One item inside a dialog. TS spells these as keywords; the ones with no native counterpart parse into
// Kind::Unknown and are dropped at conversion rather than at parse time (so a count is still reportable).
struct Item {
    enum class Kind { Field, Panel, Gauge, Indicator, Command, Text, LiveGraph, Unknown };
    Kind        kind = Kind::Unknown;
    std::string var;          // field's bound variable / gauge's channel / command name
    std::string label;
    std::string panelId;      // Panel: the referenced dialog or indicatorPanel
    std::string placement;    // North / South / East / West / Center (TS's border-layout hint)
    std::string condition;    // "{ expr }" visibility guard, carried across verbatim
    std::string onLabel;      // indicator = {expr}, "off text", "on text" — the lit caption
    std::vector<std::string> flags;   // commandButton optional flags (showMessageOnClick, closeDialogOnClick…)
    std::string message;              // the text showMessageOnClick displays
    std::vector<std::string> lines;   // LiveGraph: its graphLine channels
};

struct Dialog {
    std::string helpTopic;      // topicHelp = "<id>" -> the [UserDefined] `help` block with that id
    std::string webHelp;        // webHelp = "<url>"  -> the dialog's "more on the web" link
    std::string id, title;
    std::string layout;       // xAxis / yAxis / border — how TS arranged the items
    std::vector<Item> items;
};

// An indicatorPanel is a fixed-column grid of lamps, referenced by dialogs like any other panel.
// A lamp: its expression, the caption for each state, and TunerStudio's four colour names
// (off background, off text, on background, on text).
struct IndicatorLamp {
    std::string expr, offLabel, onLabel;
    std::string offBg, offFg, onBg, onFg;
};
struct IndicatorPanel {
    std::string id;
    int columns = 1;
    std::vector<IndicatorLamp> lamps;
};

// The [Menu] tree. A `menu` is a top-level heading; `subMenu` is a leaf that opens a dialog; `groupMenu` /
// `groupChildMenu` nest one level deeper. Leaves carry the dialog id — that is what becomes a page.
struct MenuNode {
    std::string label;
    std::string dialogId;     // empty for a heading/group
    std::string condition;
    std::vector<MenuNode> children;
};

// [GaugeConfigurations]: <gaugeId> = <channel>, "title", "units", lo, hi, … A dialog's `gauge = <gaugeId>`
// names one of THESE, not a channel — so without this table every gauge binding looks unresolvable.
struct GaugeDef {
    std::string channel, title, units;
    // <channel>, "title", "units", lo, hi, loDanger, loWarn, hiWarn, hiDanger, vd, ld — the range is what
    // makes the needle mean anything (an RPM gauge sweeps 0..9000, not the channel's storage limits) and
    // vd is how many decimals the face shows. Kept AS WRITTEN, because a bound may be an expression
    // ({rpmHardLimit + 2000}) rather than a number.
    std::string lo, hi;
    // THE FOUR BOUNDS THAT COLOUR THE FACE, kept for the same reason and in the same form. TunerStudio
    // paints the span below loDanger and above hiDanger red, and loDanger..loWarn and hiWarn..hiDanger
    // amber; a tacho's redline IS these numbers. They were read off the line and dropped, so every
    // imported gauge came over as a bare face.
    std::string loDanger, loWarn, hiWarn, hiDanger;
    int decimals = -1;          // vd — the decimals the READOUT shows
    int labelDecimals = -1;     // ld — the decimals the tick LABELS show, which is a different number
};

// [TableEditor] / [CurveEditor]: a menu leaf may name one of THESE instead of a dialog, and the data
// lives in the cell array (zBins for a map, yBins for a curve) — the table id itself addresses nothing.
// A [TableEditor]/[CurveEditor] block. Beyond the cell array, a curve DECLARES how it is drawn: the axis
// ranges and grid divisions, the axis labels, and optionally a gauge shown in its corner. TunerStudio paints
// the declared range, which is why an untuned curve there is a readable 0..8000 grid rather than the flat
// line auto-ranging on all-zero data produces.
struct TableRef {
    std::string id, title, cells;
    // A map declares TWO names: the table id (its 2D editor) and the map id. A menu leaf that opens the MAP
    // is TunerStudio's 3D view of the same data — "VE 3D view" opens veTableMap, "VE" opens veTableDialog.
    std::string mapId;
    bool  curve = false;
    std::string xLabel, yLabel;                 // columnLabel = "X", "Y"
    double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
    std::string xMinExpr, xMaxExpr, yMinExpr, yMaxExpr;   // as written; a bound may be "{cltHighXaxis}"
    int    xDiv = 0, yDiv = 0;                  // grid divisions
    bool   hasXAxis = false, hasYAxis = false;
    std::string gauge;                          // gauge = <id from [GaugeConfigurations]>
    float  sizeW = 0.f, sizeH = 0.f;            // size = w, h — the definition's suggested editor size
    bool   textValues = false;                  // showTextValues = true -> the editable cells are shown
};

// A `readoutPanel = <id>[, columns]` block: a grid of LIVE readouts (a channel or a gauge id per row).
//     readoutPanel = ltftValuesReadoutPanel, 1
//         readout = fuelLtftCntHitGauge
//         readout = sd_error, "FRESULT", "", 0, 20, ...
struct Readout { std::string var, title, units; };
struct ReadoutPanel {
    std::string id;
    int columns = 1;
    std::vector<Readout> rows;
};

// A `help = <id>, "Title"` block: the text behind a dialog's help badge.
struct HelpTopic { std::string title, text, url; };

struct Dashboard {
    std::map<std::string, TableRef>       tables;
    std::vector<MenuNode>                 menus;
    std::map<std::string, Dialog>         dialogs;
    std::map<std::string, IndicatorPanel> panels;
    std::map<std::string, GaugeDef>       gauges;
    std::map<std::string, HelpTopic>      helps;
    // [FrontPage]: the gauges and the status lamps TunerStudio shows in its MAIN WINDOW — not in any
    // dialog. They belong to the window, so they stay put whichever page you are on.
    std::vector<IndicatorLamp>            frontIndicators;
    std::vector<std::string>              frontGauges;
    std::map<std::string, ReadoutPanel>   readouts;
    // [OutputChannels] entries that are an EXPRESSION over other channels rather than stored data:
    //     coolantTemperature = { useMetricOnInterface ? coolant : (coolant * 1.8 + 32) }
    // Nothing in the definition holds these, so they never appear in the meta; a gauge bound to one has to
    // evaluate the expression, which is what the studio's binding evaluator does anyway.
    std::map<std::string, std::string>    derived;
    // [ControllerCommands]: "name = \"payload\"[, \"payload\"…]" — raw controller instructions, sent in
    // order, with no protocol envelope of their own. A commandButton names one of these.
    std::map<std::string, std::vector<std::string>> commands;
    // [ConstantsExtensions] maintainConstantValue: a config field the TUNER keeps up to date from an
    // expression. rusEFI's calibration buttons work entirely through this — the ECU measures, publishes
    // the reading on calibrationMode/calibrationValue, and expects the tuner to write it into the
    // configuration. Nothing else does it: the ECU never touches its own config for these.
    struct MaintainedConstant { std::string target, expr; };
    std::vector<MaintainedConstant>       maintained;

    // The channel a dialog's `gauge = <id>` ultimately shows (empty if the id names no gauge).
    std::string gaugeChannel(const std::string& id) const {
        const auto it = gauges.find(id);
        return it == gauges.end() ? std::string() : it->second.channel;
    }

    size_t leafCount() const {
        size_t n = 0;
        for (const auto& m : menus) n += _leaves(m);
        return n;
    }
private:
    static size_t _leaves(const MenuNode& n) {
        if (n.children.empty()) return n.dialogId.empty() ? 0 : 1;
        size_t n2 = 0;
        for (const auto& c : n.children) n2 += _leaves(c);
        return n2;
    }
};

// ---- parsing -------------------------------------------------------------------------------------------

inline std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

inline std::string unquote(const std::string& s) {
    const std::string t = trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
    return t;
}

// Split on ',' but never inside quotes or a {condition} — a TS line is
//   field = "Label, with comma", var, { cond && other }
// An ini byte string: "Z\x00\x12\x00\x01" is FIVE bytes, not the seventeen characters it is written as.
// The escapes are the payload — unescaping them at parse time is what makes the command sendable.
inline std::string unescapeBytes(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\' || i + 1 >= in.size()) { out += in[i]; continue; }
        const char c = in[++i];
        switch (c) {
            case 'r': out += '\r'; break;
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case '0': out += '\0'; break;
            case 'x': {
                std::string hex;
                while (hex.size() < 2 && i + 1 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])))
                    hex += in[++i];
                out += static_cast<char>(std::stoi(hex.empty() ? "0" : hex, nullptr, 16));
                break;
            }
            default: out += c; break;                        // \" and \\ arrive as themselves
        }
    }
    return out;
}

inline std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int braces = 0;
    bool quoted = false;
    for (char c : s) {
        if (c == '"') quoted = !quoted;
        if (!quoted) {
            if (c == '{') ++braces;
            else if (c == '}') --braces;
        }
        if (c == ',' && !quoted && braces == 0) { out.push_back(trim(cur)); cur.clear(); continue; }
        cur += c;
    }
    out.push_back(trim(cur));
    return out;
}

// Border placement, if this argument IS one. TS puts a placement in the same slot a {condition} can
// occupy — `panel = bias, { blendParameter }` — so anything that is not one of the five region words is
// not a placement, and treating it as one turned a visibility guard into a layout region.
inline std::string placementOf(const std::string& a) {
    const std::string t = trim(a);
    return (t == "North" || t == "South" || t == "East" || t == "West" || t == "Center") ? t : std::string();
}

// A trailing "{ expr }" guard, if present, with the braces stripped.
inline std::string conditionOf(const std::vector<std::string>& args) {
    for (const auto& a : args) {
        const std::string t = trim(a);
        if (t.size() >= 2 && t.front() == '{' && t.back() == '}') return trim(t.substr(1, t.size() - 2));
    }
    return {};
}

// Strip a ';' comment, respecting quotes (labels contain semicolons).
inline std::string stripComment(const std::string& line) {
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') quoted = !quoted;
        else if (line[i] == ';' && !quoted) return line.substr(0, i);
    }
    return line;
}

inline Dashboard parse(const std::string& text) {
    Dashboard d;
    std::string section;
    Dialog*         curDialog = nullptr;
    IndicatorPanel* curPanel  = nullptr;
    Item*           curGraph  = nullptr;
    TableRef*       curTable  = nullptr;
    HelpTopic*      curHelp   = nullptr;
    ReadoutPanel*   curReadout = nullptr;
    std::map<std::string, std::string> tableAlias;   // map id -> table id, resolved once parsing is done
    // The [Menu] tree is built with two cursors: the open top-level `menu` and, inside it, the open
    // `groupMenu`. TS nests exactly this far, and indentation is decorative — the keyword is the structure.
    MenuNode* curMenu  = nullptr;
    MenuNode* curGroup = nullptr;

    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        const std::string line = trim(stripComment(raw));
        if (line.empty()) continue;
        if (line.front() == '[') {
            section = line.substr(1, line.find(']') == std::string::npos ? line.size() - 1 : line.find(']') - 1);
            curDialog = nullptr; curPanel = nullptr; curGraph = nullptr; curMenu = nullptr; curGroup = nullptr;
            curTable = nullptr; curHelp = nullptr; curReadout = nullptr;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key  = trim(line.substr(0, eq));
        const auto        args = splitArgs(line.substr(eq + 1));
        const std::string a0   = args.empty() ? std::string() : unquote(args[0]);

        if (section == "Menu") {
            if (key == "menu") {
                d.menus.push_back(MenuNode{ a0, {}, conditionOf(args), {} });
                curMenu = &d.menus.back(); curGroup = nullptr;
            } else if (key == "subMenu" && curMenu) {
                // subMenu = <dialogId>, "Label" — a leaf that opens a dialog. "std_separator" is a rule.
                // The comma is not always there: real inis write
                //     subMenu = dcMotorActuatorHw          "DC motor actuator(s) hardware", { 1 }
                // separating the id from its title with whitespace alone, which left the id carrying the
                // quoted title and the LABEL taken from the next argument — a menu row reading "{ 1 }".
                std::string id = a0, label = args.size() > 1 ? unquote(args[1]) : a0;
                if (const size_t q = id.find('"'); q != std::string::npos) {
                    label = unquote(trim(id.substr(q)));
                    id    = trim(id.substr(0, q));
                }
                // A separator is DROPPED. TunerStudio draws a rule between groups of a dropdown menu; this
                // menu becomes a navigation TREE, where the same entry is a row — one with no name, no page
                // and nothing to show. The arrow keys walked onto them and blanked the canvas, which is all
                // a rule can ever do here. The grouping survives without them: it is the tree's own nesting.
                if (id != "std_separator")
                    curMenu->children.push_back(MenuNode{ label, id, conditionOf(args), {} });
                curGroup = nullptr;
            } else if (key == "groupMenu" && curMenu) {
                curMenu->children.push_back(MenuNode{ a0, {}, conditionOf(args), {} });
                curGroup = &curMenu->children.back();
            } else if (key == "groupChildMenu" && curGroup) {
                std::string id = a0, label = args.size() > 1 ? unquote(args[1]) : a0;
                if (const size_t q = id.find('"'); q != std::string::npos) {   // id and title, no comma
                    label = unquote(trim(id.substr(q)));
                    id    = trim(id.substr(0, q));
                }
                // Dropped under groups too — same reason as above.
                if (id != "std_separator")
                    curGroup->children.push_back(MenuNode{ label, id, conditionOf(args), {} });
            }
            continue;
        }

        if (section == "TableEditor" || section == "CurveEditor") {
            if (key == "table" || key == "curve") {
                TableRef t;
                t.id    = a0;
                t.curve = (key == "curve");
                t.title = unquote(args.size() > (t.curve ? 1 : 2) ? args[t.curve ? 1 : 2] : std::string());
                const std::string mapId = (!t.curve && args.size() > 1) ? unquote(args[1]) : std::string();
                d.tables[t.id] = t;
                // A map declares TWO names for the same data — the table id and the map id — and a menu
                // leaf may use either ("VE 3D view" opens veTableMap). The alias is recorded and resolved
                // AFTER the block is parsed: copying the TableRef here copied it before its cell array had
                // been read, so the map-named page opened a table editor bound to nothing.
                if (!mapId.empty() && mapId != t.id) { tableAlias[mapId] = t.id; d.tables[t.id].mapId = mapId; }
                curTable = &d.tables[t.id];
                continue;
            }
            // The cell array: zBins for a map, yBins for a curve (a curve is one row of values).
            if (curTable && ((curTable->curve && key == "yBins") || (!curTable->curve && key == "zBins")))
                curTable->cells = a0;
            // How the editor is DRAWN. An axis bound may be an expression ({cltHighXaxis}); those are left at
            // 0 and the widget falls back to the data, which is what it did for every curve before.
            if (curTable && key == "columnLabel") {
                curTable->xLabel = a0;
                curTable->yLabel = args.size() > 1 ? unquote(args[1]) : std::string();
            } else if (curTable && (key == "xAxis" || key == "yAxis")) {
                auto num = [](const std::string& v, bool& ok) {
                    ok = false;
                    const std::string t2 = trim(v);
                    if (t2.empty() || t2.front() == '{') return 0.0;
                    try { ok = true; return std::stod(t2); } catch (...) { return 0.0; }
                };
                bool okLo = false, okHi = false;
                const double lo = num(args.size() > 0 ? args[0] : std::string(), okLo);
                const double hi = num(args.size() > 1 ? args[1] : std::string(), okHi);
                const int div = args.size() > 2 ? atoi(trim(args[2]).c_str()) : 0;
                auto raw = [&](size_t i) {
                    std::string v = args.size() > i ? trim(args[i]) : std::string();
                    if (v.size() > 1 && v.front() == '{' && v.back() == '}') v = trim(v.substr(1, v.size() - 2));
                    return v;
                };
                if (key == "xAxis") {
                    curTable->xMin = lo; curTable->xMax = hi; curTable->xDiv = div;
                    curTable->xMinExpr = raw(0); curTable->xMaxExpr = raw(1);
                    curTable->hasXAxis = !curTable->xMinExpr.empty() && !curTable->xMaxExpr.empty();
                } else {
                    curTable->yMin = lo; curTable->yMax = hi; curTable->yDiv = div;
                    curTable->yMinExpr = raw(0); curTable->yMaxExpr = raw(1);
                    curTable->hasYAxis = !curTable->yMinExpr.empty() && !curTable->yMaxExpr.empty();
                }
                (void)okLo; (void)okHi;
            } else if (curTable && key == "size") {
                curTable->sizeW = args.size() > 0 ? float(atof(trim(args[0]).c_str())) : 0.f;
                curTable->sizeH = args.size() > 1 ? float(atof(trim(args[1]).c_str())) : 0.f;
            } else if (curTable && key == "gauge") {
                curTable->gauge = a0;
            } else if (curTable && key == "showTextValues") {
                curTable->textValues = (a0 == "true" || a0 == "1");
            }
            continue;
        }

        if (section == "FrontPage") {
            if (key == "indicator") {
                IndicatorLamp l;
                l.expr = a0.size() >= 2 && a0.front() == '{' ? trim(a0.substr(1, a0.size() - 2)) : a0;
                auto arg = [&](size_t i) { return args.size() > i ? unquote(trim(args[i])) : std::string(); };
                l.offLabel = arg(1); l.onLabel = arg(2);
                l.offBg = arg(3); l.offFg = arg(4); l.onBg = arg(5); l.onFg = arg(6);
                d.frontIndicators.push_back(std::move(l));
            } else if (key.rfind("gauge", 0) == 0) {
                d.frontGauges.push_back(a0);
            }
            continue;
        }

        if (section == "ConstantsExtensions") {
            if (key != "maintainConstantValue" || args.size() < 2) continue;
            Dashboard::MaintainedConstant mc;
            mc.target = trim(a0);
            // The expression is everything after the target, braces stripped — it may itself contain
            // commas inside a function call, so it is rejoined rather than taken as args[1].
            std::string e = trim(line.substr(line.find(',', line.find('=')) + 1));
            if (e.size() >= 2 && e.front() == '{' && e.back() == '}') e = trim(e.substr(1, e.size() - 2));
            mc.expr = e;
            if (!mc.target.empty() && !mc.expr.empty()) d.maintained.push_back(std::move(mc));
            continue;
        }

        if (section == "ControllerCommands") {
            // "displayCommand = <name>, \"description\"" only marks a command as user-assignable; the
            // commands themselves are the other entries.
            if (key == "displayCommand") continue;
            std::vector<std::string> payloads;
            for (const auto& a : args) {
                const std::string p2 = unescapeBytes(unquote(trim(a)));
                if (!p2.empty()) payloads.push_back(p2);
            }
            if (!payloads.empty()) d.commands[key] = std::move(payloads);
            continue;
        }

        if (section == "OutputChannels") {
            // "name = { expr }" — a computed channel. Stored ones are the importer's business; these carry
            // no storage at all, so the dashboard is the only place they can be resolved.
            const std::string v = trim(line.substr(eq + 1));
            if (v.size() > 2 && v.front() == '{' && v.back() == '}')
                d.derived[key] = trim(v.substr(1, v.size() - 2));
            continue;
        }

        if (section == "GaugeConfigurations") {
            // Skip the section's own directives; everything else is a gauge definition.
            if (key == "gaugeCategory" || args.size() < 2) continue;
            GaugeDef g;
            g.channel = a0;
            g.title   = unquote(args[1]);
            g.units   = args.size() > 2 ? unquote(args[2]) : std::string();
            g.lo      = args.size() > 3 ? trim(args[3]) : std::string();
            g.hi      = args.size() > 4 ? trim(args[4]) : std::string();
            g.loDanger = args.size() > 5 ? trim(args[5]) : std::string();
            g.loWarn   = args.size() > 6 ? trim(args[6]) : std::string();
            g.hiWarn   = args.size() > 7 ? trim(args[7]) : std::string();
            g.hiDanger = args.size() > 8 ? trim(args[8]) : std::string();
            auto dec = [&](size_t i, int& out) {
                if (args.size() <= i) return;
                const std::string d = trim(args[i]);
                if (!d.empty() && d.find_first_not_of("0123456789") == std::string::npos) out = std::atoi(d.c_str());
            };
            dec(9,  g.decimals);          // vd
            dec(10, g.labelDecimals);     // ld — may be an expression ({ useMetric ? 1 : 2 }), then left unset
            d.gauges[key] = std::move(g);
            continue;
        }

        if (section != "UserDefined") continue;

        if (key == "dialog") {
            Dialog dlg;
            dlg.id     = a0;
            dlg.title  = args.size() > 1 ? unquote(args[1]) : std::string();
            dlg.layout = args.size() > 2 ? unquote(args[2]) : std::string();
            curDialog  = &(d.dialogs[dlg.id] = std::move(dlg));
            curPanel   = nullptr; curGraph = nullptr; curHelp = nullptr; curReadout = nullptr;
        } else if (key == "readoutPanel") {
            ReadoutPanel rp;
            rp.id      = a0;
            rp.columns = args.size() > 1 ? std::max(1, atoi(trim(args[1]).c_str())) : 1;
            curReadout = &(d.readouts[rp.id] = std::move(rp));
            curDialog = nullptr; curPanel = nullptr; curGraph = nullptr; curHelp = nullptr;
        } else if (key == "readout" && curReadout) {
            Readout r;
            r.var   = a0;
            r.title = args.size() > 1 ? unquote(args[1]) : std::string();
            r.units = args.size() > 2 ? unquote(args[2]) : std::string();
            curReadout->rows.push_back(std::move(r));
        } else if (key == "help") {
            // A help block's `text` lines belong to IT, not to the dialog above it — without closing the
            // dialog here, every help topic's prose was appended to the previous dialog as stray labels.
            HelpTopic h;
            h.title  = args.size() > 1 ? unquote(args[1]) : std::string();
            curHelp  = &(d.helps[a0] = std::move(h));
            curDialog = nullptr; curPanel = nullptr; curGraph = nullptr;
        } else if (curHelp && (key == "text" || key == "webHelp")) {
            if (key == "webHelp") { curHelp->url = unquote(a0); }
            else { if (!curHelp->text.empty()) curHelp->text += "\n"; curHelp->text += unquote(a0); }
        } else if (key == "indicatorPanel") {
            IndicatorPanel p;
            p.id      = a0;
            p.columns = args.size() > 1 ? std::max(1, atoi(trim(args[1]).c_str())) : 1;
            curPanel  = &(d.panels[p.id] = std::move(p));
            curDialog = nullptr; curGraph = nullptr; curHelp = nullptr; curReadout = nullptr;
        } else if (key == "indicator" && curPanel) {
            IndicatorLamp l;
            l.expr     = a0.size() >= 2 && a0.front() == '{' ? trim(a0.substr(1, a0.size() - 2)) : a0;
            l.offLabel = args.size() > 1 ? unquote(args[1]) : std::string();
            l.onLabel  = args.size() > 2 ? unquote(args[2]) : std::string();
            auto carg  = [&](size_t i) { return args.size() > i ? unquote(trim(args[i])) : std::string(); };
            l.offBg = carg(3); l.offFg = carg(4); l.onBg = carg(5); l.onFg = carg(6);
            curPanel->lamps.push_back(std::move(l));
        } else if (curDialog) {
            Item it;
            it.condition = conditionOf(args);
            if (key == "field") {
                // field = "Label", var[, {cond}] — a blank label with no var is a spacer, and a HEADER row
                // is `field = "Label", { cond }` with no variable at all: taking arg 1 blindly turned the
                // guard into the binding, so the field looked bound to an expression that resolves to
                // nothing.
                it.kind  = Item::Kind::Field;
                it.label = a0;
                std::string a1 = args.size() > 1 ? unquote(args[1]) : std::string();
                // A real ini writes the comma between label and variable... usually:
                //     field = "Boost input"          boostInput, 1, {boostEnabled != 0}
                // With the comma missing, the label and the variable arrive as ONE argument and the NEXT
                // argument (here a bare "1") looked like the binding. Split them back apart.
                if (const size_t q = args[0].find_last_of('"'); q != std::string::npos && q + 1 < args[0].size()) {
                    const std::string tail = trim(args[0].substr(q + 1));
                    if (!tail.empty() && (std::isalpha(static_cast<unsigned char>(tail[0])) || tail[0] == '_')) {
                        it.label = unquote(trim(args[0].substr(0, q + 1)));
                        a1 = tail;
                    }
                }
                // The guard is USUALLY its own comma-separated argument, but not always — real inis carry
                //     field = "and after delay", lambdaProtectionTimeout { lambdaProtectionEnable }
                // with no comma, so the variable and its condition arrive glued together.
                if (const size_t br = a1.find('{'); br != std::string::npos) {
                    const size_t end = a1.rfind('}');
                    if (end != std::string::npos && end > br && it.condition.empty())
                        it.condition = trim(a1.substr(br + 1, end - br - 1));
                    a1 = trim(a1.substr(0, br));
                }
                it.var   = (!a1.empty() && a1.front() == '{') ? std::string() : a1;
                if (it.var.empty() && it.label.empty()) it.kind = Item::Kind::Unknown;
            } else if (key == "panel") {
                it.kind = Item::Kind::Panel; it.panelId = a0;
                it.placement = args.size() > 1 ? placementOf(unquote(args[1])) : std::string();
            } else if (key == "gauge") {
                it.kind = Item::Kind::Gauge; it.var = a0;
                it.placement = args.size() > 1 ? placementOf(unquote(args[1])) : std::string();
            } else if (key == "indicator") {
                it.kind = Item::Kind::Indicator;
                it.var  = a0.size() >= 2 && a0.front() == '{' ? trim(a0.substr(1, a0.size() - 2)) : a0;
                it.label   = args.size() > 1 ? unquote(args[1]) : std::string();
                it.onLabel = args.size() > 2 ? unquote(args[2]) : std::string();
            } else if (key == "commandButton") {
                it.kind = Item::Kind::Command; it.label = a0;
                it.var  = args.size() > 1 ? unquote(args[1]) : std::string();
                // Optional flags follow the command; showMessageOnClick is always followed by its text.
                for (size_t i = 2; i < args.size(); ++i) {
                    const std::string f = trim(args[i]);
                    if (f.empty() || f.front() == '{') continue;                    // the enable condition
                    if (f == "showMessageOnClick" && i + 1 < args.size()) { it.message = unquote(trim(args[++i])); }
                    it.flags.push_back(unquote(f));
                }
            } else if (key == "topicHelp") {
                curDialog->helpTopic = unquote(a0);      // the "?" badge's topic, not a row of the dialog
                continue;
            } else if (key == "webHelp") {
                curDialog->webHelp = unquote(a0);
                continue;
            } else if (key == "text") {
                it.kind = Item::Kind::Text; it.label = a0;
            } else if (key == "liveGraph") {
                it.kind = Item::Kind::LiveGraph; it.var = a0;
                it.label = args.size() > 1 ? unquote(args[1]) : std::string();
                it.placement = args.size() > 2 ? placementOf(unquote(args[2])) : std::string();
            } else if (key == "graphLine" && curGraph) {
                curGraph->lines.push_back(a0);          // belongs to the liveGraph above it
                continue;
            } else {
                continue;                                // unknown keyword -> skipped, never fatal
            }
            curDialog->items.push_back(std::move(it));
            curGraph = (curDialog->items.back().kind == Item::Kind::LiveGraph) ? &curDialog->items.back()
                                                                              : nullptr;
        }
    }
    // Map aliases, now that every table is complete (cells included).
    for (const auto& [alias, id] : tableAlias) {
        const auto t = d.tables.find(id);
        if (t != d.tables.end() && !d.tables.count(alias)) d.tables[alias] = t->second;
    }
    return d;
}

}  // namespace tsdash
