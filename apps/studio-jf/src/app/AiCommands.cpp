#include "AiCommands.h"

#include "model/Cache.h"
#include "model/ExprCompiler.h"
#include "model/MetaModel.h"
#include "model/TableImage.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

namespace aicmd {
namespace {

using jf::JJson;

JJson fail(const std::string& why) {
    JJson o = JJson::object();
    o["ok"] = JJson(false);
    o["error"] = JJson(why);
    return o;
}
JJson okObj() {
    JJson o = JJson::object();
    o["ok"] = JJson(true);
    return o;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool matches(const std::string& filter, const std::string& a, const std::string& b = {}) {
    if (filter.empty()) return true;
    const std::string f = lower(filter);
    return lower(a).find(f) != std::string::npos || lower(b).find(f) != std::string::npos;
}

// A number as the studio would show it: to the field's decimals when the meta states them, else trimmed
// of binary noise (0.1 * 147 is 14.700000000000001, which is not what anyone typed).
double tidy(double v, int digits) {
    if (!std::isfinite(v)) return v;
    const int d = (digits >= 0 && digits <= 9) ? digits : 6;
    const double p = std::pow(10.0, d);
    return std::round(v * p) / p;
}

// The first word, and the rest of the line after it.
void splitFirst(const std::string& s, std::string& head, std::string& rest) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) { head.clear(); rest.clear(); return; }
    const size_t b = s.find_first_of(" \t", a);
    head = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
    const size_t c = b == std::string::npos ? std::string::npos : s.find_first_not_of(" \t", b);
    rest = c == std::string::npos ? std::string() : s.substr(c);
    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()))) rest.pop_back();
}

// A number for a sentence: "12", "0.5" — not std::to_string's "12.000000".
std::string numText(double v) {
    std::ostringstream o;
    o << tidy(v, 6);
    return o.str();
}

bool parseNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    while (end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    return end && *end == '\0';
}

// --- settings --------------------------------------------------------------------------------------

// Every setting path the meta knows: the scalars, and each field of each element of the arrays (keyed
// by the element's stable id where it has one, as the tune keys them).
struct SettingRef { std::string path, label; };
std::vector<SettingRef> allSettings(const MetaModel& m) {
    std::vector<SettingRef> out;
    for (const auto& [key, f] : m.config())
        if (f.offset >= 0) out.push_back({ key, f.label });
    for (const auto& [key, arr] : m.configArrays())
        for (int i = 0; i < arr.count; ++i) {
            const std::string id = (i < int(arr.elementIds.size()) && !arr.elementIds[i].empty())
                                 ? arr.elementIds[i] : std::to_string(i);
            const std::string el = (i < int(arr.elementLabels.size()) && !arr.elementLabels[i].empty())
                                 ? arr.elementLabels[i] : arr.label + " " + std::to_string(i + 1);
            for (const MetaModel::ArrayField& f : arr.fields)
                out.push_back({ key + "[" + id + "]." + f.name, el + " \xC2\xB7 " + f.label });
        }
    std::sort(out.begin(), out.end(), [](const SettingRef& a, const SettingRef& b) { return a.path < b.path; });
    return out;
}

// What kind of value a setting holds, in the words a client switches on.
std::string kindOf(const MetaModel::Location& L) {
    if (L.datatype == "EXPR")  return "expression";
    if (L.datatype == "ASCII") return "text";
    if (!L.pickerSets.empty()) return "pin";
    if (L.valueKind == "signal") return "channel";
    if (L.valueKind == "enum" && !L.options.empty()) return "choice";
    if (L.valueKind == "bool") return "switch";
    return "number";
}

// The label (and id) of the option a raw value stands for.
bool optionOf(const MetaModel::Location& L, double raw, std::string& label, std::string& id) {
    const long v = std::lround(raw);
    for (const MetaModel::PickerSet& ps : L.pickerSets)
        for (size_t i = 0; i < ps.values.size() && i < ps.options.size(); ++i)
            if (ps.values[i] == v) { label = ps.options[i]; id.clear(); return true; }
    if (v >= 0 && v < long(L.options.size())) {
        label = L.options[size_t(v)];
        id = v < long(L.optionIds.size()) ? L.optionIds[size_t(v)] : std::string();
        return true;
    }
    return false;
}

// One setting, described and read. `full` adds the help text and the option list.
// `label` overrides the field's own: an array field's says which element it belongs to.
JJson describe(const Cache& c, const MetaModel& m, const std::string& path, bool full,
               const std::string& label = {}) {
    const MetaModel::Location L = m.locate(path);
    JJson o = JJson::object();
    o["path"] = JJson(path);
    if (!label.empty() || !L.label.empty()) o["label"] = JJson(label.empty() ? L.label : label);
    const std::string kind = kindOf(L);
    o["kind"] = JJson(kind);
    if (!L.units.empty()) o["units"] = JJson(L.units);
    if (kind == "expression") {
        const std::vector<uint8_t> prog = c.configBlob(path);
        o["value"] = JJson(prog.empty() ? std::string()
                                        : ExprCompiler::decompile(prog.data(), uint16_t(prog.size()), m));
    } else if (kind == "text") {
        o["value"] = JJson(c.configString(path));
    } else if (kind == "channel") {
        const long v = std::lround(c.configValue(path));
        o["value"] = JJson((v < 0 || v == 0xFFFF) ? std::string() : m.signalName(int(v)));
        o["raw"] = JJson(double(v));
    } else if (kind == "choice" || kind == "pin") {
        const double raw = c.configValue(path);
        std::string label, id;
        o["value"] = JJson(optionOf(L, raw, label, id) ? label : std::string());
        if (!id.empty()) o["id"] = JJson(id);
        o["raw"] = JJson(raw);
        if (full) {
            JJson opts = JJson::array();
            if (!L.pickerSets.empty()) {
                // The pins of every interface's list, once each: a digital pin is in several of them.
                std::vector<std::string> seen;
                for (const MetaModel::PickerSet& ps : L.pickerSets)
                    for (const std::string& s : ps.options)
                        if (std::find(seen.begin(), seen.end(), s) == seen.end()) { seen.push_back(s); opts.push(JJson(s)); }
            } else {
                for (size_t i = 0; i < L.options.size(); ++i) {
                    JJson e = JJson::object();
                    e["label"] = JJson(L.options[i]);
                    if (i < L.optionIds.size()) e["id"] = JJson(L.optionIds[i]);
                    opts.push(e);
                }
            }
            o["options"] = opts;
        }
    } else {
        const double scale = c.configScale(path);
        const int digits = c.digits(path);
        o["value"] = JJson(tidy(c.configValue(path) * scale, digits));
        if (L.maxV > L.minV) {
            o["min"] = JJson(tidy(L.minV * scale, digits));
            o["max"] = JJson(tidy(L.maxV * scale, digits));
        }
    }
    if (c.lockedWhileRunning(path)) o["locked"] = JJson(std::string("engine running"));
    if (full && !L.help.empty()) o["help"] = JJson(L.help);
    return o;
}

// Write one setting from text, in the units it is shown in. Empty `why` on success.
std::string writeSetting(Cache& c, const MetaModel& m, const std::string& path, const std::string& text) {
    const MetaModel::Location L = m.locate(path);
    if (!L.valid()) return "no setting '" + path + "'";
    if (L.kind == MetaModel::Location::Kind::Table) return "'" + path + "' is a table: use table-set or cell";
    if (c.readOnly()) return "the tune is locked (the studio's Locked switch)";
    if (c.lockedWhileRunning(path)) return "'" + path + "' cannot be changed while the engine is turning";
    const std::string kind = kindOf(L);
    if (kind == "expression") {
        const ExprCompiler::Result r = ExprCompiler::compile(text, m, uint32_t(m.configSize()), uint16_t(L.size));
        if (!r.ok) return "expression does not compile: " + r.error;
        c.setConfigBlob(path, r.code);
        return {};
    }
    if (kind == "text") { c.setConfigString(path, text); return {}; }
    if (kind == "channel") {
        if (text.empty() || lower(text) == "none") { c.setConfigValue(path, -1); return {}; }
        const auto& sm = m.signalMap();
        auto it = sm.find(text);
        if (it == sm.end()) return "no channel '" + text + "' (channels lists them)";
        c.setConfigValue(path, it->second);
        return {};
    }
    if (kind == "choice" || kind == "pin") {
        for (const MetaModel::PickerSet& ps : L.pickerSets)
            for (size_t i = 0; i < ps.options.size() && i < ps.values.size(); ++i)
                if (ps.options[i] == text) { c.setConfigValue(path, ps.values[i]); return {}; }
        for (size_t i = 0; i < L.optionIds.size(); ++i)
            if (L.optionIds[i] == text) { c.setConfigValue(path, double(i)); return {}; }
        for (size_t i = 0; i < L.options.size(); ++i)
            if (L.options[i] == text) { c.setConfigValue(path, double(i)); return {}; }
        double n = 0;
        if (kind == "choice" && parseNumber(text, n) && n >= 0 && n < double(L.options.size())) {
            c.setConfigValue(path, n);
            return {};
        }
        return "'" + text + "' is not one of its options (get " + path + " lists them)";
    }
    double v = 0;
    if (!parseNumber(text, v)) {
        if (kind == "switch" || L.maxV - L.minV == 1.0) {
            const std::string t = lower(text);
            if (t == "on" || t == "true" || t == "yes")  v = 1;
            else if (t == "off" || t == "false" || t == "no") v = 0;
            else return "'" + text + "' is not a number";
        } else {
            return "'" + text + "' is not a number";
        }
    }
    const double scale = c.configScale(path);
    const double raw = scale != 0.0 ? v / scale : v;
    if (L.maxV > L.minV && (raw < L.minV - 1e-9 || raw > L.maxV + 1e-9))
        return "out of range: " + numText(L.minV * scale) + " to " + numText(L.maxV * scale)
             + (L.units.empty() ? std::string() : " " + L.units);
    c.setConfigValue(path, raw);
    return {};
}

// --- tables ----------------------------------------------------------------------------------------

JJson axisJson(const Cache& c, const MetaModel& m, const TableImage& t, int ax, int n) {
    const TableImage::Axis& a = t.axes[size_t(ax)];
    JJson o = JJson::object();
    if (!a.label.empty()) o["label"] = JJson(a.label);
    if (!a.units.empty()) o["units"] = JJson(a.units);
    if (a.srcBase >= 0) {
        const long s = std::lround(c.readAt(a.srcBase, "S16", 1.0));
        if (s >= 0) o["channel"] = JJson(m.signalName(int(s)));
    }
    if (a.enBase >= 0) o["enabled"] = JJson(c.readAt(a.enBase, "U08", 1.0) != 0.0);
    JJson bins = JJson::array();
    for (int k = 0; k < n; ++k)
        bins.push(JJson(tidy(c.readAt(a.breaksBase + k * a.breakSize, a.breakType, a.breakScale), a.digits)));
    o["bins"] = bins;
    return o;
}

JJson tableJson(const Cache& c, const MetaModel& m, const std::string& path, bool withCells) {
    const TableImage t = c.resolveTable(path);
    JJson o = JJson::object();
    o["path"] = JJson(path);
    if (!t.label.empty()) o["label"] = JJson(t.label);
    const MetaModel::Location L = m.locate(path);
    const std::string units = !t.cellUnits.empty() ? t.cellUnits : L.units;
    if (!units.empty()) o["units"] = JJson(units);
    const int cols = c.liveCols(path), rows = c.liveRows(path), depth = c.liveDepth(path);
    o["cols"] = JJson(cols);
    o["rows"] = JJson(rows);
    if (depth > 1) o["depth"] = JJson(depth);
    if (!withCells) return o;
    const char* names[] = { "x", "y", "z" };
    const int ns[] = { cols, rows, depth };
    for (int ax = 0; ax < int(t.axes.size()) && ax < 3; ++ax) o[names[ax]] = axisJson(c, m, t, ax, ns[ax]);
    const int digits = t.cellDigits >= 0 ? t.cellDigits : L.digits;
    JJson planes = JJson::array();
    for (int z = 0; z < depth; ++z) {
        JJson grid = JJson::array();
        for (int r = 0; r < rows; ++r) {
            JJson row = JJson::array();
            for (int col = 0; col < cols; ++col) row.push(JJson(tidy(c.tableCell(path, col, r, z), digits)));
            grid.push(row);
        }
        planes.push(grid);
    }
    // A 2-D table is rows of columns; a 3-D one is planes of those.
    o["cells"] = depth > 1 ? planes : planes[0];
    return o;
}

// --- dispatch --------------------------------------------------------------------------------------

const char* kHelp =
    "jayecu studio commands (jf-busctl call '<command>'):\n"
    "  status                         link, ECU, tune and definition\n"
    "  settings [filter]              every setting (path, label, kind, units, value)\n"
    "  get <path>                     one setting in full: value, units, range, options, help\n"
    "  set <path> <value>             write it, in the units shown; options by label or id\n"
    "  tables [filter]                every table with its size\n"
    "  table <path>                   axes (bins, channel) and cells, rows of columns\n"
    "  table-set <path> <json>        write cells: [[row0...],[row1...]] (3-D: planes of those)\n"
    "  cell <path> <col> <row> <v>    write one cell (0-based)\n"
    "  channels [filter]              the live channels (name, label, units, module, rate)\n"
    "  live [name ...]                their values now; no names = every channel\n"
    "  burn                           persist the ECU's RAM tune to flash\n"
    "  save                           save the tune to the ECU's project folder\n"
    "  page <navigation path>         show a page (studio window only)\n"
    "  key on|off|auto                force the ECU's key (off = no fuel, no spark) or hand it back\n"
    "Writes go to the ECU when it is connected (to RAM: burn to keep them).";

}  // namespace

bool handle(const std::string& action, const Host& host, std::string& reply) {
    std::string verb, rest;
    splitFirst(action, verb, rest);
    static const char* kVerbs[] = { "help", "status", "settings", "get", "set", "tables", "table",
                                    "table-set", "cell", "channels", "live", "burn", "save", "page", "key" };
    if (std::find_if(std::begin(kVerbs), std::end(kVerbs), [&](const char* v) { return verb == v; })
        == std::end(kVerbs))
        return false;

    auto done = [&](const JJson& o) { reply = o.dump(); return true; };
    Cache& c = Cache::instance();
    const MetaModel* m = c.meta();

    if (verb == "help") { JJson o = okObj(); o["help"] = JJson(std::string(kHelp)); return done(o); }
    if (verb == "status") {
        JJson o = host.status ? host.status() : JJson::object();
        o["ok"] = JJson(true);
        o["definition"] = JJson(m && m->isValid() ? m->board() + " " + m->layoutHash() : std::string());
        o["tune_loaded"] = JJson(c.hasConfig());
        o["locked"] = JJson(c.readOnly());
        o["telemetry_frames"] = JJson(double(c.telemetryFrames()));
        if (c.telemetryFrames()) o["telemetry_age_ms"] = JJson(double(c.msSinceTelemetry()));
        return done(o);
    }
    // THE KEY, AND ONLY THE KEY. The ECU's console has a 'key on|off|auto' bench command — off closes the
    // firing gate (no fuel, no spark) exactly as turning the ignition off does, which is how an engine is
    // stopped from a laptop. The console also reboots into the bootloader and drives outputs, so it is NOT
    // passed through: this one command, with its three words, is.
    if (verb == "key") {
        if (rest != "on" && rest != "off" && rest != "auto") return done(fail("key on|off|auto"));
        if (!c.telemetryFrames() || c.msSinceTelemetry() > 2000) return done(fail("no live ECU to send it to"));
        c.sendCli("key " + rest);
        JJson o = okObj();
        o["key"] = JJson(rest);
        return done(o);
    }
    if (verb == "burn" || verb == "save" || verb == "page") {
        std::string why;
        const bool ok = verb == "burn" ? (host.burn && host.burn(why))
                      : verb == "save" ? (host.save && host.save(why))
                                       : (host.page && host.page(rest, why));
        if (!ok) return done(fail(why.empty() ? verb + " is not available here" : why));
        return done(okObj());
    }

    if (!m || !m->isValid() || !c.hasConfig())
        return done(fail("no tune is open: open an ECU (or connect one) first — status says what is loaded"));

    if (verb == "settings") {
        JJson list = JJson::array();
        for (const SettingRef& s : allSettings(*m))
            if (matches(rest, s.path, s.label)) list.push(describe(c, *m, s.path, false, s.label));
        JJson o = okObj();
        o["count"] = JJson(double(list.size()));
        o["settings"] = list;
        return done(o);
    }
    if (verb == "get") {
        if (rest.empty()) return done(fail("get <path>"));
        const MetaModel::Location L = m->locate(rest);
        if (!L.valid()) return done(fail("no setting '" + rest + "' (settings <filter> finds paths)"));
        if (L.kind == MetaModel::Location::Kind::Table) { JJson o = tableJson(c, *m, rest, true); o["ok"] = JJson(true); return done(o); }
        JJson o = describe(c, *m, rest, true);
        o["ok"] = JJson(true);
        return done(o);
    }
    if (verb == "set") {
        std::string path, value;
        splitFirst(rest, path, value);
        if (path.empty()) return done(fail("set <path> <value>"));
        const std::string why = writeSetting(c, *m, path, value);
        if (!why.empty()) return done(fail(why));
        JJson o = describe(c, *m, path, false);
        o["ok"] = JJson(true);
        return done(o);
    }
    if (verb == "tables") {
        JJson list = JJson::array();
        std::vector<std::string> paths = m->tableInstancePaths();
        std::sort(paths.begin(), paths.end());
        for (const std::string& p : paths) {
            const TableImage t = c.resolveTable(p);
            if (t.valid && matches(rest, p, t.label)) list.push(tableJson(c, *m, p, false));
        }
        JJson o = okObj();
        o["count"] = JJson(double(list.size()));
        o["tables"] = list;
        return done(o);
    }
    if (verb == "table") {
        if (rest.empty() || !c.isTable(rest)) return done(fail("no table '" + rest + "' (tables <filter> finds paths)"));
        JJson o = tableJson(c, *m, rest, true);
        o["ok"] = JJson(true);
        return done(o);
    }
    if (verb == "cell" || verb == "table-set") {
        std::string path, args;
        splitFirst(rest, path, args);
        if (path.empty() || !c.isTable(path)) return done(fail("no table '" + path + "'"));
        if (c.readOnly()) return done(fail("the tune is locked (the studio's Locked switch)"));
        if (c.lockedWhileRunning(path)) return done(fail("'" + path + "' cannot be changed while the engine is turning"));
        const int cols = c.liveCols(path), rows = c.liveRows(path), depth = c.liveDepth(path);
        if (verb == "cell") {
            std::istringstream in(args);
            int col = -1, row = -1; double v = 0;
            if (!(in >> col >> row >> v)) return done(fail("cell <path> <col> <row> <value>"));
            if (col < 0 || col >= cols || row < 0 || row >= rows)
                return done(fail("cell out of range: the table is " + std::to_string(cols) + " x " + std::to_string(rows)));
            c.beginEdit();
            c.setTableCell(path, col, row, v);
            c.endEdit("AI: " + path + " cell");
            JJson o = okObj();
            o["value"] = JJson(c.tableCell(path, col, row));
            return done(o);
        }
        const auto doc = JJson::tryParse(args);
        if (!doc) return done(fail("table-set <path> <json>: the cells are not valid JSON"));
        const JJson& cells = doc->isObject() ? (*doc)["cells"] : *doc;
        // Accept rows-of-columns for a 2-D table, planes of those for a 3-D one; check the shape first,
        // so a wrong-sized grid writes nothing rather than half a table.
        const bool planar = depth > 1;
        auto gridOk = [&](const JJson& g) {
            if (int(g.arr().size()) != rows) return false;
            for (const JJson& r : g.arr()) if (int(r.arr().size()) != cols) return false;
            return true;
        };
        if (planar ? (int(cells.arr().size()) != depth || !std::all_of(cells.arr().begin(), cells.arr().end(), gridOk))
                   : !gridOk(cells))
            return done(fail("the cells must be " + std::string(planar ? std::to_string(depth) + " planes of " : "")
                             + std::to_string(rows) + (rows == 1 ? " row of " : " rows of ") + std::to_string(cols)));
        c.beginEdit();
        for (int z = 0; z < depth; ++z) {
            const JJson& g = planar ? cells.arr()[size_t(z)] : cells;
            for (int r = 0; r < rows; ++r)
                for (int col = 0; col < cols; ++col)
                    c.setTableCell(path, col, r, g.arr()[size_t(r)].arr()[size_t(col)].number(), z);
        }
        c.endEdit("AI: " + path);
        JJson o = tableJson(c, *m, path, true);
        o["ok"] = JJson(true);
        return done(o);
    }
    if (verb == "channels") {
        std::map<std::string, const MetaModel::TelemField*> sorted;
        for (const auto& [name, f] : m->telemetry()) sorted[name] = &f;
        JJson list = JJson::array();
        for (const auto& [name, f] : sorted) {
            if (!matches(rest, name, f->label)) continue;
            JJson e = JJson::object();
            e["name"] = JJson(name);
            if (!f->label.empty()) e["label"] = JJson(f->label);
            if (!f->units.empty()) e["units"] = JJson(f->units);
            if (!f->module.empty()) e["module"] = JJson(f->module);
            if (f->updateHz > 0) e["hz"] = JJson(f->updateHz);
            list.push(e);
        }
        JJson o = okObj();
        o["count"] = JJson(double(list.size()));
        o["channels"] = list;
        return done(o);
    }
    if (verb == "live") {
        std::vector<std::string> names;
        std::istringstream in(rest);
        for (std::string n; in >> n;) names.push_back(n);
        if (names.empty()) {
            for (const auto& [name, f] : m->telemetry()) names.push_back(name);
            std::sort(names.begin(), names.end());
        }
        JJson values = JJson::object(), missing = JJson::array();
        for (const std::string& n : names) {
            if (!c.has(n)) { missing.push(JJson(n)); continue; }
            const auto it = m->telemetry().find(n);
            values[n] = JJson(tidy(c.value(n), it != m->telemetry().end() && it->second.scale < 1.0 ? 6 : 3));
        }
        JJson o = okObj();
        o["telemetry_frames"] = JJson(double(c.telemetryFrames()));
        if (c.telemetryFrames()) o["age_ms"] = JJson(double(c.msSinceTelemetry()));
        else o["note"] = JJson(std::string("no telemetry has arrived: is an ECU connected?"));
        o["values"] = values;
        if (!missing.arr().empty()) o["missing"] = missing;
        return done(o);
    }
    return false;
}

}  // namespace aicmd
