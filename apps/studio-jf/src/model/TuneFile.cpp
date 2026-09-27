#include "TuneFile.h"
#include "ExprCompiler.h"

#include <algorithm>
#include <cmath>
#include <cctype>

#include <j/config/Json.h>
#include <j/core/Log.h>

#include "MetaModel.h"
#include "TableImage.h"

// ---------------------------------------------------------------------------
// Format: JSON object with four keyed sections.
//
//   {
//     "format":      "jayecu-tune/2",
//     "layout_hash": "<hash>",            -- recorded for reference / migration detection
//     "scalars":     { "module.field": <raw double>, ... },
//     "tables":      { "module.table": {"c":[cells], "a":[[bins]...], "n":[...], "s":[...], "e":[...]}, ... },
//     "axes":        { "module.axis":  [<raw bp>, ...], ... },
//     "arrays":      { "module.array[0].field": <raw double>, ... },
//     "expressions": { "module.array[0].field": "rpm > 2500 and map > 50", ... }
//
// Expressions are stored as SOURCE, never bytecode: a program bakes config offsets for ONE layout,
// so bytes carried into another layout would address different fields — a silent wrong answer.
// Text recompiles, which makes a layout change cost a recompile rather than a lost gate.
//   }
//
// REFERENCES ARE STORED BY NAME (format 2). A field whose VALUE indexes a generated list — a
// kind:"signal" selector, an enum's option — used to be written as the bare ordinal, so the document
// addressed its fields symbolically ("h_bridge.half[0].enable_sig") and then described their contents
// positionally (137). Those two halves age differently: a path survives anything, an ordinal survives
// only until the list it indexes gains a member. Declaring `app_state` shifted 179 SignalIds, and a
// tune written either side of it bound half-bridge A's enable to etb_en_2 instead of etb_en_1 — a
// throttle that would not move, with nothing to detect it, because no config byte had moved.
//
// So a reference is written as what it MEANS: "etb_en_1", "Cylinder". Loading resolves the name
// against the firmware in front of it, which makes the document portable by construction rather than
// by hash agreement, and turns an unresolvable reference into one named field in the report instead of
// a silently wrong binding. There is no positional fallback: a reference reads as a name or it is
// reported. A number would have to be interpreted against a catalog nobody recorded, which is exactly
// the guess that produced the wrong binding in the first place.
//
// All values are RAW (pre-scale) — same units as the firmware struct. The meta's scale factors are applied
// at display time, not stored here.
// ---------------------------------------------------------------------------

namespace {

constexpr char FORMAT_TAG[] = "jayecu-tune/2";

// Live bin count on a resolved axis, read straight from the image (mirrors Cache::tiLiveN). Cells are
// stored at the LIVE stride, so save/restore must walk the same dims — for module maps AND in-array tables.
int tuneLiveN(const TableImage &t, int axis, const unsigned char *img, int sz)
{
    if (axis < 0 || axis >= static_cast<int>(t.axes.size()))
        return 1;
    const TableImage::Axis &a = t.axes[axis];
    auto u8 = [&](int off) -> int { return (off >= 0 && off < sz) ? int(img[off]) : 0; };
    if (a.enBase >= 0 && u8(a.enBase) == 0)
        return 1;                                          // disabled optional axis collapses to one bin
    if (a.nBase < 0)
        return std::max(1, a.nMax);                        // fixed grid
    return std::clamp(u8(a.nBase), 1, a.nMax > 0 ? a.nMax : 256);
}

// "sensors.sensor[0].cal" -> "sensors.sensor[battery].cal". tableInstancePaths() enumerates element
// tables by position because its other callers want the index; the DOCUMENT wants the stable id, for the
// same reason every other key here does — an index is a position, and positions move.
std::string stableTableKey(const MetaModel &meta, const std::string &path)
{
    const auto lb = path.find('[');
    const auto rb = path.find(']', lb == std::string::npos ? 0 : lb);
    if (lb == std::string::npos || rb == std::string::npos) return path;
    const std::string arrKey = path.substr(0, lb);
    const std::string idxTxt = path.substr(lb + 1, rb - lb - 1);
    if (idxTxt.empty() || !std::all_of(idxTxt.begin(), idxTxt.end(), [](unsigned char ch){ return std::isdigit(ch); }))
        return path;                                        // already an id
    const auto it = meta.configArrays().find(arrKey);
    if (it == meta.configArrays().end()) return path;
    const int i = std::stoi(idxTxt);
    const auto &ids = it->second.elementIds;
    if (i < 0 || i >= static_cast<int>(ids.size()) || ids[i].empty()) return path;
    return arrKey + "[" + ids[i] + "]" + path.substr(rb + 1);
}

// ---- references by name (format 2) -------------------------------------------------------------
// One rule for both directions, so the writer and the reader cannot disagree about what a reference
// is. `kind` comes from the schema: "signal" values index the signal catalog, "enum" values index the
// field's own option list. Everything else is a plain number and is left alone.

// A signal selector's "unset". The firmware reads -1 (S16) / 0xFFFF (U16) as no-source, and both must
// round-trip as the same word, so the sentinel is named rather than resolved through the catalog.
constexpr char REF_NONE[] = "none";

bool isSignalRef(const std::string &kind) { return kind == "signal"; }

// A board-pin selector names hardware ("AV1", "DIG3"). Its number is an index into the BOARD's pool, so
// it ages like a SignalId: change the pool and a sensor reads a different pin, silently. The option lists
// are interface-gated, but a label is unique across the sets of a field (checked: 2 picker fields, no
// ambiguous label), so a label resolves WITHOUT knowing the gating sibling — which also means the
// document has no ordering dependency between a field and the sibling that gates it.
bool pinToLabel(const std::vector<MetaModel::PickerSet> &sets, double raw, std::string &out)
{
    if (sets.empty()) return false;
    const long v = std::lround(raw);
    for (const MetaModel::PickerSet &ps : sets)
        for (size_t i = 0; i < ps.options.size() && i < ps.values.size(); ++i)
            if (ps.values[i] == v && !ps.options[i].empty()) { out = ps.options[i]; return true; }
    return false;
}

bool labelToPin(const std::vector<MetaModel::PickerSet> &sets, const std::string &label, double &out)
{
    for (const MetaModel::PickerSet &ps : sets)
        for (size_t i = 0; i < ps.options.size() && i < ps.values.size(); ++i)
            if (ps.options[i] == label) { out = static_cast<double>(ps.values[i]); return true; }
    return false;
}
bool isEnumRef  (const std::string &kind, const std::vector<std::string> &opts)
{ return kind == "enum" && !opts.empty(); }

// value -> name. Returns false when the value names nothing (an out-of-range ordinal, a catalog gap),
// leaving the caller to write the raw number so no information is lost on the way out.
bool refToName(const MetaModel &meta, const std::string &kind,
               const std::vector<std::string> &opts, double raw, std::string &out,
               const std::vector<MetaModel::PickerSet> &pins = {})
{
    if (pinToLabel(pins, raw, out)) return true;
    if (isSignalRef(kind)) {
        const long v = std::lround(raw);
        if (v < 0 || v == 0xFFFF) { out = REF_NONE; return true; }
        const std::string n = meta.signalName(static_cast<int>(v));
        if (n.empty()) return false;
        out = n; return true;
    }
    if (isEnumRef(kind, opts)) {
        const long v = std::lround(raw);
        if (v < 0 || v >= static_cast<long>(opts.size())) return false;
        out = opts[static_cast<size_t>(v)];
        return !out.empty();
    }
    return false;
}

// name -> value. False when this firmware has no such signal/option: the caller reports that ONE
// field rather than failing the load, because the rest of the tune is still perfectly good.
bool nameToRef(const MetaModel &meta, const std::string &kind,
               const std::vector<std::string> &opts, const std::string &name, double &out,
               const std::vector<MetaModel::PickerSet> &pins = {})
{
    if (labelToPin(pins, name, out)) return true;
    if (isSignalRef(kind)) {
        if (name == REF_NONE) { out = -1.0; return true; }
        const auto &m = meta.signalMap();
        const auto it = m.find(name);
        if (it == m.end()) return false;
        out = static_cast<double>(it->second);
        return true;
    }
    if (isEnumRef(kind, opts)) {
        for (size_t i = 0; i < opts.size(); ++i)
            if (opts[i] == name) { out = static_cast<double>(i); return true; }
        return false;
    }
    return false;
}

} // namespace

bool TuneFile::isDictFormat(const std::vector<uint8_t> &data)
{
    // A dict tune is JSON: first non-whitespace byte is '{'.
    for (uint8_t c : data)
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t')
            return c == '{';
    return false;
}

std::vector<uint8_t> TuneFile::serialise(const std::vector<uint8_t> &image, const MetaModel &meta,
                                        const std::vector<uint8_t> *host)
{
    const unsigned char *img = image.data();
    const int            sz  = static_cast<int>(image.size());
    // An IMPORTED definition has no default image (an ini declares no calibration), so this can legitimately
    // be empty. Every read below is bounds-checked against sz, but a zero-size descriptor would slip past
    // those checks at offset 0 and dereference a null data() — belt as well as braces.
    if (!img || sz <= 0)
        JLOGC("model.tune", jf::JLogLevel::Warn) << "serialise: empty config image — scalars/tables skipped";

    jf::JJson scalars = jf::JJson::object();
    jf::JJson tables  = jf::JJson::object();
    jf::JJson axes    = jf::JJson::object();
    jf::JJson arrays  = jf::JJson::object();
    // Expressions are stored as SOURCE TEXT, keyed by path — never as bytecode. Bytecode bakes
    // config offsets for ONE layout, so carrying it into another would silently address different
    // fields; source recompiles. It is also what lets a tune be read without the exact meta.
    jf::JJson expressions = jf::JJson::object();

    // --- Scalars ---
    // A field's offset picks its BLOCK: the config image, or the host (PcVariable) block, which lives at
    // an absolute base far above it. Without this the host fields fall outside the image and vanish from
    // the document — the values would exist in the session and never survive a save.
    for (const auto &[key, f] : meta.config()) {
        const bool isHost = f.offset >= int(MetaModel::kPcSegmentBase);
        if (isHost) {
            if (!host) continue;
            const int off = f.offset - int(MetaModel::kPcSegmentBase);
            if (off < 0 || off + f.size > int(host->size())) continue;
            scalars[key] = jf::JJson(MetaModel::decodeRaw(f.datatype, host->data() + off));
            continue;
        }
        if (f.size <= 0 || f.offset < 0 || f.offset + f.size > sz) continue;
        // A STRING field's bytes are text: decoding them as a number would save the first four characters
        // as a float and restore garbage over the rest. Saved as the string it is.
        if (f.isText()) {
            const char *p = reinterpret_cast<const char *>(img + f.offset);
            int n = 0;
            while (n < f.size && p[n] != '\0') ++n;
            scalars[key] = jf::JJson(std::string(p, n));
            continue;
        }
        const double rawv = MetaModel::decodeRaw(f.datatype, img + f.offset);
        std::string nm;
        if (refToName(meta, f.kind, f.options, rawv, nm)) scalars[key] = jf::JJson(nm);
        else                                              scalars[key] = jf::JJson(rawv);
    }

    // --- Tables: ONE path for EVERY table — a module map or an in-array instance (etb[i].ff_table). The
    //     firmware lays them out identically (cell grid + per-axis breakpoint array + live _n), so we save
    //     each one whole and identically: cells, and per axis its bins / count / src / enable. ---
    for (const std::string &path : meta.tableInstancePaths()) {
        const TableImage t = meta.resolveTable(path);
        if (!t.valid) continue;
        int n[3] = {1, 1, 1};
        for (int ax = 0; ax < static_cast<int>(t.axes.size()) && ax < 3; ++ax)
            n[ax] = tuneLiveN(t, ax, img, sz);
        // The cells are LAID OUT at the allocation stride and only n of them are live, so the file keeps
        // a live-sized (logical) list while the offsets step by the physical width. That is also what
        // makes a tune portable across a resize: the list says what the table means, not where it sat.
        int alloc[3] = {1, 1, 1};
        for (int ax = 0; ax < static_cast<int>(t.axes.size()) && ax < 3; ++ax)
            alloc[ax] = t.axes[ax].nMax > 0 ? t.axes[ax].nMax : n[ax];

        jf::JJson cells = jf::JJson::array();
        for (int z = 0; z < n[2]; ++z)
            for (int r = 0; r < n[1]; ++r)
                for (int c = 0; c < n[0]; ++c) {
                    const int off = t.cellBase + (z * alloc[0] * alloc[1] + r * alloc[0] + c) * t.cellSize;
                    if (off + t.cellSize <= sz)
                        cells.push(jf::JJson(MetaModel::decodeRaw(t.cellType, img + off)));
                }

        jf::JJson axesBins = jf::JJson::array(), counts = jf::JJson::array(),
                  srcs = jf::JJson::array(), enables = jf::JJson::array();
        for (int ax = 0; ax < static_cast<int>(t.axes.size()); ++ax) {
            const TableImage::Axis &a = t.axes[ax];
            jf::JJson bins = jf::JJson::array();
            for (int k = 0; k < n[ax]; ++k) {
                const int off = a.breaksBase + k * a.breakSize;
                if (off + a.breakSize <= sz)
                    bins.push(jf::JJson(MetaModel::decodeRaw(a.breakType, img + off)));   // raw, like every value here
            }
            axesBins.push(bins);
            // The RAW configured bin count (the _n byte), NOT the live count n[ax]: tuneLiveN folds in the
            // enable toggle and returns 1 for a disabled optional axis — storing that would clobber the real
            // _n down to 1 on restore (perpetual out-of-sync). Cells/bins above still use the live stride.
            counts.push(jf::JJson(a.nBase >= 0 && a.nBase < sz ? int(img[a.nBase]) : n[ax]));
            // src is an int16 SignalId selector (-1 = none) — decode 2 bytes, not one, and store it by
            // NAME like every other reference (see the format note at the top).
            {
                const double sv = (a.srcBase >= 0 && a.srcBase + 2 <= sz)
                                ? MetaModel::decodeRaw("S16", img + a.srcBase) : -1.0;
                std::string snm;
                if (refToName(meta, "signal", {}, sv, snm)) srcs.push(jf::JJson(snm));
                else                                        srcs.push(jf::JJson(int(sv)));
            }
            enables.push(jf::JJson(a.enBase >= 0 && a.enBase < sz ? int(img[a.enBase]) : -1));
        }
        jf::JJson to = jf::JJson::object();
        to["c"] = cells; to["a"] = axesBins; to["n"] = counts; to["s"] = srcs; to["e"] = enables;
        tables[stableTableKey(meta, path)] = to;
    }

    // --- 1D axis/curve arrays ---
    // The element COUNT is what a 1-D array carries — MetaModel divides the declared byte size by the
    // element size and stores that; ConfigField::size is left at 0 for every one of them, native and
    // imported alike. Bounding the loop by `size` therefore ran it zero times, and EVERY axis, curve and
    // 1-D array in the file was written as []. Module tables (which is all of them for an imported
    // definition) restore their breakpoints from exactly these keys, so a saved tune came back with the
    // bins zeroed: 1918 bytes of a bench ECU's own configuration lost through one save/load.
    for (const auto &[key, a] : meta.arrays1d()) {
        const int elemSz = MetaModel::dataSize(a.datatype);
        const int count  = a.count > 0 ? a.count : (elemSz > 0 ? a.size / elemSz : 0);
        if (elemSz <= 0 || count <= 0 || a.offset < 0 || a.offset + count * elemSz > sz) continue;
        jf::JJson vals = jf::JJson::array();
        for (int i = 0; i < count; ++i)
            vals.push(jf::JJson(MetaModel::decodeRaw(a.datatype, img + a.offset + i * elemSz)));
        axes[key] = vals;
    }

    // --- Struct-array element fields (sensors, outputs, …) ---
    for (const auto &[key, arr] : meta.configArrays()) {
        for (int i = 0; i < arr.count; ++i) {
            const int base = arr.baseOffset + i * arr.stride;
            for (const MetaModel::ArrayField &f : arr.fields) {
                // Keyed by the element's STABLE ID ("clt"), not its position. An index is a position:
                // remove one sensor from the catalog — as the knock row was — and every element after it
                // shifts, so a stored value lands on its neighbour and a sensor inherits another's pin,
                // type and calibration. The expressions section below has always done this; there was no
                // reason for the rest of the document to keep inheriting the fragile form. locate()
                // accepts either spelling, so this is a change of what we WRITE.
                const std::string k =
                    (i < static_cast<int>(arr.elementIds.size()) && !arr.elementIds[i].empty())
                        ? key + "[" + arr.elementIds[i] + "]." + f.name
                        : key + "[" + std::to_string(i) + "]." + f.name;
                // An EXPRESSION field is a compiled program, and the tune stores its SOURCE, not
                // its bytes (see below and docs/expression-evaluator-design.md). Decoding it as a
                // number would write 0 and silently destroy the gate on the next load.
                if (f.datatype == "EXPR") {
                    if (base + f.relOffset + f.size > sz) continue;
                    const uint8_t *code = img + base + f.relOffset;
                    const std::string src =
                        ExprCompiler::decompile(code, (uint16_t)f.size, meta);
                    if (src.empty()) continue;           // empty program = always armed = nothing to store
                    // Same stable-ID keying as every other array field (see above) — `k` already is it.
                    const std::string ek = k;
                    expressions[ek] = jf::JJson(src);
                    continue;
                }
                // …AND SO IS A TEXT ONE, for the same reason the EXPR case above exists: decoding an
                // element's name as a number wrote 0 and destroyed it on the next load. Every output
                // slot's name — "Fuel Pump", "Starter Motor" — went through a save as the number zero.
                if (f.datatype == "ASCII") {
                    if (base + f.relOffset + f.size > sz) continue;
                    const char *p = reinterpret_cast<const char *>(img + base + f.relOffset);
                    int n = 0;
                    while (n < f.size && p[n] != '\0') ++n;
                    arrays[k] = jf::JJson(std::string(p, n));
                    continue;
                }
                const int elemSz = MetaModel::dataSize(f.datatype);
                if (base + f.relOffset + elemSz > sz) continue;
                const double av = MetaModel::decodeRaw(f.datatype, img + base + f.relOffset);
                std::string anm;
                if (refToName(meta, f.kind, f.options, av, anm, f.pickerSets)) arrays[k] = jf::JJson(anm);
                else                                             arrays[k] = jf::JJson(av);
            }
        }
    }

    jf::JJson root = jf::JJson::object();
    root["format"]      = jf::JJson(std::string(FORMAT_TAG));
    root["layout_hash"] = jf::JJson(meta.layoutHash());
    root["scalars"]     = scalars;
    root["tables"]      = tables;
    root["axes"]        = axes;
    root["arrays"]      = arrays;
    root["expressions"] = expressions;

    const std::string s = root.dump(-1);   // compact
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint8_t> TuneFile::deserialise(const std::vector<uint8_t> &tuneJson, const MetaModel &meta,
                                           MigrationReport &report, std::vector<uint8_t> *hostOut)
{
    report = {};
    // The host (PcVariable) block, sized from the meta's declaration. Rebuilt here from the document so a
    // saved tune restores host values the same way it restores everything else.
    if (hostOut) {
        hostOut->clear();
        for (const MetaModel::Segment &sg : meta.segments())
            if (sg.kind == "host") hostOut->assign(size_t(sg.size), 0);
    }

    auto parsed = jf::JJson::tryParse(std::string(tuneJson.begin(), tuneJson.end()));
    if (!parsed || !parsed->isObject() || parsed->empty())
        return {};
    const jf::JJson &root = *parsed;

    // Start from the default image — new fields not in the tune get their firmware defaults.
    std::vector<uint8_t> image = meta.defaultImage();
    if (static_cast<int>(image.size()) < meta.configSize())
        image.resize(meta.configSize(), 0);

    unsigned char *img = image.data();
    const int      sz  = static_cast<int>(image.size());

    // --- Scalars ---
    for (const auto &[key, val] : root["scalars"].obj()) {
        auto it = meta.config().find(key);
        if (it == meta.config().end()) {
            report.unmapped.push_back(key);
        } else {
            const MetaModel::ConfigField &f = it->second;
            if (f.offset >= int(MetaModel::kPcSegmentBase)) {      // host block, not the config image
                const int off = f.offset - int(MetaModel::kPcSegmentBase);
                if (hostOut && off >= 0 && off + f.size <= int(hostOut->size())) {
                    MetaModel::encodeRaw(f.datatype, hostOut->data() + off, val.number());
                    ++report.migrated;
                }
            } else if (f.offset >= 0 && f.offset + f.size <= sz) {
                if (f.isText()) {                          // text back into its NUL-padded field
                    const std::string t = val.str();
                    // The field's own capacity — see ConfigField::capacity(). Truncating to size-1 here
                    // put back sixteen characters of a seventeen-character VIN, so a tune saved from the
                    // ECU no longer matched it the moment it was loaded again.
                    const size_t body = std::min<size_t>(t.size(), size_t(f.capacity()));
                    std::fill(img + f.offset, img + f.offset + f.size, 0);
                    std::copy(t.begin(), t.begin() + body, img + f.offset);
                } else if (val.isString() && !f.isText()) {
                    // Format 2: a reference stored by name. Resolve it against THIS firmware.
                    double rv = 0.0;
                    if (nameToRef(meta, f.kind, f.options, val.str(), rv))
                        MetaModel::encodeRaw(f.datatype, img + f.offset, rv);
                    else
                        report.unresolved.push_back(key + " -> " + val.str());
                } else {
                    MetaModel::encodeRaw(f.datatype, img + f.offset, val.number());
                }
                ++report.migrated;
            }
        }
    }

    // --- Tables: same one path, every table whole. Restore enable/src/count FIRST (they set the live
    //     dims), then bins, then cells — identically for a module map or etb[i].ff_table. ---
    auto u8write = [&](int off, int v) { if (off >= 0 && off < sz) img[off] = static_cast<unsigned char>(v); };
    for (const auto &[key, val] : root["tables"].obj()) {
        const TableImage t = meta.resolveTable(key);
        if (!t.valid) {
            report.unmapped.push_back(key);
            continue;
        }
        // New whole-table form is an object {c,a,n,s,e}; the legacy flat form is a bare cells array.
        const bool isObj = val.isObject();
        const jf::JJson &cells    = isObj ? val["c"] : val;
        const jf::JJson &axesBins = val["a"];   // null (→ empty) for the legacy flat form
        const jf::JJson &counts   = val["n"];
        const jf::JJson &srcs     = val["s"];
        const jf::JJson &enables  = val["e"];

        // Axis config (enable/src/count) + breakpoint bins: ONLY per-element tables need these from here —
        // a module map's _n/src/enable live in the scalars section and its bins in the axes section, all
        // restored from their own keys. Restoring them here too would re-apply the count and (pre-fix tunes)
        // clobber a disabled axis' real _n down to 1. So module tables only restore cells below.
        const bool isElem = key.find('[') != std::string::npos;
        if (isElem) {
            for (int ax = 0; ax < static_cast<int>(t.axes.size()); ++ax) {   // 1) axis config: enable, src, count
                const TableImage::Axis &a = t.axes[ax];
                if (ax < static_cast<int>(enables.size()) && int(enables[ax].number(-1)) >= 0) u8write(a.enBase, int(enables[ax].number()));
                // src is an int16 SignalId selector — encode 2 bytes (-1 = none round-trips as 0xFFFF).
                // Format 2 names it; format 1 numbered it. An unknown name leaves the axis unbound and
                // is reported, rather than pointing it at whatever now holds that id.
                if (ax < static_cast<int>(srcs.size()) && a.srcBase >= 0 && a.srcBase + 2 <= sz) {
                    double sv = -1.0;
                    bool ok = true;
                    if (srcs[ax].isString()) ok = nameToRef(meta, "signal", {}, srcs[ax].str(), sv);
                    else                     sv = srcs[ax].number(-1);
                    if (ok) MetaModel::encodeRaw("S16", img + a.srcBase, sv);
                    else    report.unresolved.push_back(key + ".axis" + std::to_string(ax)
                                                        + " -> " + srcs[ax].str());
                }
                if (ax < static_cast<int>(counts.size()) && a.nBase >= 0)
                    u8write(a.nBase, std::clamp(int(counts[ax].number()), 1, a.nMax > 0 ? a.nMax : 256));
            }
        }
        int n[3] = {1, 1, 1};                           // live dims (module: scalar _n; element: just restored)
        for (int ax = 0; ax < static_cast<int>(t.axes.size()) && ax < 3; ++ax)
            n[ax] = tuneLiveN(t, ax, img, sz);

        if (isElem)
            for (int ax = 0; ax < static_cast<int>(t.axes.size()) && ax < static_cast<int>(axesBins.size()); ++ax) {   // 2) breakpoint bins
                const TableImage::Axis &a = t.axes[ax];
                const jf::JJson &bins = axesBins[ax];
                for (int k = 0; k < static_cast<int>(bins.size()) && k < n[ax]; ++k) {
                    const int off = a.breaksBase + k * a.breakSize;
                    if (off + a.breakSize <= sz)
                        MetaModel::encodeRaw(a.breakType, img + off, bins[k].number());
                }
            }

        int alloc[3] = {1, 1, 1};                      // physical stride (see the writer)
        for (int ax = 0; ax < static_cast<int>(t.axes.size()) && ax < 3; ++ax)
            alloc[ax] = t.axes[ax].nMax > 0 ? t.axes[ax].nMax : n[ax];
        for (int z = 0; z < n[2]; ++z)                  // 3) cells — live LIST, physical PLACEMENT
            for (int r = 0; r < n[1]; ++r)
                for (int c = 0; c < n[0]; ++c) {
                    const int k = z * n[1] * n[0] + r * n[0] + c;
                    if (k >= static_cast<int>(cells.size())) continue;
                    const int off = t.cellBase + (z * alloc[0] * alloc[1] + r * alloc[0] + c) * t.cellSize;
                    if (off + t.cellSize <= sz)
                        MetaModel::encodeRaw(t.cellType, img + off, cells[k].number());
                }
        ++report.migrated;
    }

    // --- Axes ---
    for (const auto &[key, val] : root["axes"].obj()) {
        auto it = meta.arrays1d().find(key);
        if (it == meta.arrays1d().end()) {
            report.unmapped.push_back(key);
            continue;
        }
        const MetaModel::ConfigField &a = it->second;
        const int elemSz = MetaModel::dataSize(a.datatype);
        if (elemSz <= 0) continue;
        const jf::JJson &vals = val;
        const int count = a.count > 0 ? a.count : (elemSz > 0 ? a.size / elemSz : 0);   // size is 0 — see serialise
        for (int i = 0; i < static_cast<int>(vals.size()) && i < count; ++i) {
            const int off = a.offset + i * elemSz;
            if (off + elemSz > sz) break;
            MetaModel::encodeRaw(a.datatype, img + off, vals[i].number());
        }
        ++report.migrated;
    }

    // --- Struct-array fields ---
    for (const auto &[key, val] : root["arrays"].obj()) {
        const MetaModel::Location L = meta.locate(key);
        if (L.kind != MetaModel::Location::Kind::Scalar) {
            report.unmapped.push_back(key);
        } else {
            // A TEXT FIELD FIRST: its stored string is the value, not a name to look up. Asked in the
            // other order, a slot called "Fuel Pump" is put to nameToRef, fails to resolve, and is
            // reported as a broken reference — which is a worse way to lose it than silently.
            if (L.datatype == "ASCII" && L.size > 0 && L.offset >= 0 && L.offset + L.size <= sz) {
                const std::string t = val.str();
                const size_t body = std::min<size_t>(t.size(), size_t(L.size - 1));
                std::fill(img + L.offset, img + L.offset + L.size, 0);
                std::copy(t.begin(), t.begin() + body, img + L.offset);
                ++report.migrated;
                continue;
            }
            const int esz = MetaModel::dataSize(L.datatype);
            if (L.offset >= 0 && L.offset + esz <= sz) {
                if (val.isString()) {                    // a reference, stored by name
                    double rv = 0.0;
                    if (nameToRef(meta, L.valueKind, L.options, val.str(), rv, L.pickerSets)) {
                        MetaModel::encodeRaw(L.datatype, img + L.offset, rv);
                        ++report.migrated;
                    } else {
                        report.unresolved.push_back(key + " -> " + val.str());
                    }
                } else {
                    MetaModel::encodeRaw(L.datatype, img + L.offset, val.number());
                    ++report.migrated;
                }
            }
        }
    }

    // --- Expressions: recompile the stored SOURCE against THIS layout's meta ---
    // This is the whole migration story for gates: text survives a layout change, bytes never
    // could. A source that no longer compiles (a channel that was renamed away, a setting that no
    // longer exists) is reported as unmapped rather than written half-formed.
    for (const auto &[key, val] : root["expressions"].obj()) {
        int off = 0, size = 0;
        if (!meta.resolveBlob(key, off, size) || off < 0 || off + size > sz) {
            report.unmapped.push_back(key);
            continue;
        }
        const auto r = ExprCompiler::compile(val.str(), meta, (uint32_t)sz, (uint16_t)size);
        if (!r.ok) {
            JLOGC("model.tune", jf::JLogLevel::Warn)
                << "expression for " << key << " did not compile against this layout: " << r.error;
            report.unmapped.push_back(key);
            continue;
        }
        std::memset(img + off, 0, (size_t)size);
        std::memcpy(img + off, r.code.data(), std::min<size_t>(r.code.size(), (size_t)size));
        ++report.migrated;
    }

    // Count fields that exist in the current meta but weren't in the tune (new firmware features).
    const int totalMetaFields = static_cast<int>(meta.config().size())
                              + static_cast<int>(meta.configTables().size())
                              + static_cast<int>(meta.arrays1d().size());
    report.defaulted = std::max(0, totalMetaFields - report.migrated - static_cast<int>(report.unmapped.size()));

    return image;
}
