#pragma once

// TsIniImporter — imports a TunerStudio / rusEFI ".ini" firmware definition into the native MetaModel JSON
// (there is no "TS mode": the ini is converted to native config/telemetry/tables, then loaded like any schema).
// A focused, lenient importer: it parses [Constants] (scalar /
// bits / array fields), [OutputChannels] (telemetry scalars), and [TableEditor]/[CurveEditor] (2D/3D maps +
// curves referencing axis arrays), skipping anything it doesn't recognise, plus [SettingContextHelp]
// (per-field hover help) and the protocol facts ([TunerStudio] signature / pageSize / blockingFactor /
// ochBlockSize) the live TS link needs. Emits the same JSON shape the MetaModel loads:
// { "config": { "ts": {field…} }, "telemetry": {name…}, "tsProtocol": {…} }. Scalars carry units,
// scale, min/max, digits and help; a non-zero TS `translate` is preserved under "translate" but the
// native decode is scale-only (an affine offset term is not in the native model yet).

#include <j/config/Json.h>

#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class TsIniImporter {
public:
    static std::optional<jf::JJson> importFile(const std::string& path, std::string* err = nullptr) {
        std::ifstream f(path);
        if (!f) { if (err) *err = "cannot open " + path; return std::nullopt; }
        std::stringstream ss; ss << f.rdbuf();
        return importText(ss.str());
    }

    static jf::JJson importText(const std::string& text) {
        std::vector<int> pageSizes;      // [Constants] pageSize = a, b, c
        std::vector<int> pageIds;        // [Constants] pageIdentifier, as the le16 the command carries
        std::vector<bool> pageBurn;      // [Constants] burnCommand — empty string = page cannot be burned
        int curPage = 1;                 // [Constants] page = N (1-based)
        std::unordered_map<std::string, std::vector<std::string>> defines;   // #define lists, by name
        Fields consts, outs, pcv;                         // [Constants], [OutputChannels], [PcVariables]
        std::vector<Table> tables;                   // [TableEditor] / [CurveEditor]
        std::unordered_map<std::string, std::string> help;   // [SettingContextHelp] name -> text
        jf::JJson proto = jf::JJson::object();       // [TunerStudio] protocol facts for the live link
        VeAnalyzeDecl va;                            // [VeAnalyze] the autotuner's contract
        std::unordered_set<std::string> logged;      // [Datalog] the channels the ECU's own definition logs
        std::string section;
        Table* curTable = nullptr;
        size_t altFrom = 0;              // first member of the current #if/#else alternative group

        for (std::string raw : splitLines(text)) {
            const std::string line = stripComment(raw);
            const std::string t = trim(line);
            // "#define name=\"a\", \"b\", ..." — TunerStudio's shared option lists. A field then says
            //     targetAfrBlends1_blendParameter = bits, U08, 64316, [0:5], $gppwm_channel_e_enum
            // and without expanding that reference the field ends up with ONE option literally named
            // "$gppwm_channel_e_enum", which is what the control then displays.
            if (t.rfind("#define", 0) == 0) {
                const std::string body = trim(t.substr(7));
                const size_t eq = body.find('=');
                if (eq != std::string::npos) {
                    const std::string name = trim(body.substr(0, eq));
                    std::vector<std::string> opts;
                    for (const auto& x : splitArgs(body.substr(eq + 1))) {
                        const std::string u = unquote(x);
                        if (!u.empty()) opts.push_back(u);
                    }
                    if (!name.empty() && !opts.empty()) defines[name] = std::move(opts);
                }
                continue;
            }
            if (t.empty()) continue;
            if (t.front() == '[') { section = t.substr(1, t.find(']') == std::string::npos ? t.size() - 1 : t.find(']') - 1); curTable = nullptr; continue; }
            const size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = trim(t.substr(0, eq));
            std::vector<std::string> a = splitArgs(t.substr(eq + 1));
            if (a.empty()) continue;

            if (section == "PcVariables") {
                // HOST-SIDE values: TunerStudio computes and stores these itself, so they carry NO OFFSET
                // — that is the tell. A [Constants] bits field is "bits, U32, 1356, [0:0]" (addressed in
                // the ECU image); a PcVariable is "bits, U08, [0:2]" (addressed nowhere). They become
                // fields in the host segment, where the studio owns the storage.
                Field fld;
                if (!parseField(a, fld)) continue;
                fld.offset = -1;                       // assigned within the host block, not the ECU image
                pcv[key] = fld;
                continue;
            }
            if (section == "Constants" || section == "OutputChannels") {
                // The ini SAYS how its multi-byte fields are stored, as a directive among the field
                // declarations. Ignoring it meant a big-endian definition imported silently wrong values
                // for every U16/U32 — and wrong bits on top of those.
                if (key == "endianness") { proto["endianness"] = a.empty() ? std::string() : unquote(a[0]); continue; }
                // Expand any "$listName" argument in place, so a field carrying a shared enum gets the
                // options themselves rather than the name of the list.
                for (size_t ai = 0; ai < a.size(); ) {
                    const std::string t2 = trim(a[ai]);
                    if (t2.size() > 1 && t2[0] == '$') {
                        const auto d = defines.find(t2.substr(1));
                        if (d != defines.end()) {
                            a.erase(a.begin() + ai);
                            a.insert(a.begin() + ai, d->second.begin(), d->second.end());
                            ai += d->second.size();
                            continue;
                        }
                    }
                    ++ai;
                }
                // PAGING. A TunerStudio config is several pages, each with its OWN offset space starting at
                // 0, and `page = N` switches which one the following fields belong to. Ignoring it made
                // every page collide at the same addresses — engineType, highSpeedOffsets and
                // ltft_table_bank1 all claiming offset 0 — so two pages' worth of fields read and wrote
                // the wrong bytes. Flatten them into one image by adding the base of each page.
                if (key == "pageSize") {
                    pageSizes.clear();
                    for (const auto& x : a) pageSizes.push_back(toInt(x));
                    continue;
                }
                if (key == "page") { curPage = std::max(1, toInt(a.empty() ? std::string("1") : a[0])); continue; }
                if (key == "nPages") continue;
                // The rest of the PAGE GEOMETRY, which the live link needs to address the ECU at all: the
                // wire identifier of each page and whether it can be burned. `pageIdentifier` is a list of
                // BYTE STRINGS sent verbatim in place of the command's %2i — rusEFI's page 2 is the bytes
                // 00 01, i.e. 0x0100, NOT the index 1 — so the identifier has to be carried, not inferred.
                if (key == "pageIdentifier") { pageIds.clear(); for (const auto& x : a) pageIds.push_back(idOf(unquote(x))); continue; }
                if (key == "burnCommand")    { pageBurn.clear(); for (const auto& x : a) pageBurn.push_back(!unquote(x).empty()); continue; }
                if (key == "blockingFactor") { proto["blockingFactor"] = toInt(a[0]); continue; }
                // "Milliseconds delay after burn command", as the ini itself annotates it. The commit is
                // ASYNCHRONOUS on rusEFI — 'B' only sets a flag and a background task writes the flash —
                // so traffic sent straight after the ack can hit a controller in the middle of a write.
                if (key == "pageActivationDelay") { proto["pageActivationDelay"] = toInt(a[0]); continue; }
                // The ECU can SAY what is wrong with its configuration — TunerStudio asks with this
                // command and prints the answer. Without it a lit "Config Error" lamp is the whole of
                // the diagnosis: something is wrong, good luck.
                if (key == "retrieveConfigError") { proto["configErrorCommand"] = unquote(a[0]); continue; }
                if (key == "ochBlockSize")   { proto["ochBlockSize"] = toInt(a[0]); continue; }
                // A DERIVED OUTPUT CHANNEL: "name = { expression }". rusEFI declares a good many of its
                // channels this way, including the one [VeAnalyze] names as the ego correction
                // (egoCorrectionForVeAnalyze = { Gego }). parseField rejects these — a[0] is "{ Gego }",
                // not "scalar" — so they were dropped in silence, and a definition that NAMES a channel
                // the studio then does not publish reads as 0 or as a stated default. For the autotuner
                // that default is 1.000, no correction: the ego fold silently stops happening and a VE
                // table held on target by closed loop looks correct.
                //
                // An alias of ONE channel is that channel, so it is carried as a second name for the same
                // bytes. A compound expression is not resolvable here — there is no evaluator in an
                // importer — and is left out, which is the honest outcome: it is then absent rather than
                // wrong, and a consumer that names it can say so.
                if (section == "OutputChannels" && !a.empty() && trim(a[0]).front() == '{') {
                    std::string e = trim(joinArgs(a));
                    if (e.size() >= 2 && e.front() == '{' && e.back() == '}') e = trim(e.substr(1, e.size() - 2));
                    if (isIdentifier(e)) {
                        if (auto it = outs.find(e); it != outs.end()) outs[key] = it->second;
                    }
                    continue;
                }
                Field fld; if (!parseField(a, fld)) continue;
                if (section == "Constants") fld.page = curPage;
                (section == "Constants" ? consts : outs)[key] = fld;
            } else if (section == "SettingContextHelp") {
                help[key] = unquote(a[0]);
            } else if (section == "TunerStudio" || section == "MegaTune") {
                if (key == "signature")           proto["signature"] = unquote(a[0]);
                else if (key == "pageSize")       proto["pageSize"] = toInt(a[0]);
                else if (key == "blockingFactor") proto["blockingFactor"] = toInt(a[0]);
                else if (key == "ochBlockSize")   proto["ochBlockSize"] = toInt(a[0]);
            } else if (section == "Datalog") {
                // WHAT IS WORTH RECORDING, as the ECU's own definition says it:
                //     entry = RPMValue, "RPM", int, "%d"
                // One line per channel, in the order the ECU's own logger writes them. The studio's
                // recorder takes these as its standard set (the telemetry `datalog` flag) instead of
                // every output channel. A trailing {condition} is not evaluated -- the importer evaluates
                // no conditions -- so a conditional channel is logged; a column too many beats one missing.
                if (key == "entry") logged.insert(trim(a[0]));
            } else if (section == "VeAnalyze") {
                // THE AUTOTUNER'S CONTRACT, STATED BY THE DEFINITION. Which map is being tuned, which
                // map holds its target, which channel reads the mixture, and — the one nobody would
                // guess — which channel reports the correction the ECU is ALREADY applying, so that a
                // mixture held on target by closed loop is still recognised as a wrong table.
                //
                //   veAnalyzeMap = table, targetTable, lambdaChannel, egoCorrectionChannel, {cond}
                //
                // The ini declares it twice, inside #if LAMBDA / #else — one pair in lambda and one in
                // AFR. The importer does not evaluate conditionals, so the FIRST wins: either is
                // correct here because the judgement is a RATIO of the two, and both halves of a pair
                // are in the same unit.
                if (key == "veAnalyzeMap" && va.table.empty() && a.size() >= 4) {
                    va.table       = unquote(a[0]);
                    va.targetTable = unquote(a[1]);
                    va.lambdaCh    = unquote(a[2]);
                    va.egoCh       = unquote(a[3]);
                } else if (key == "filter" && a.size() >= 4) {
                    // filter = name, "Display Name", channel <op> , value, , userAdjustable
                    // The channel and the operator are separated by a comma in some lines and by bare
                    // whitespace in others, in the same file — so the operator is FOUND rather than
                    // counted to. ("filter = std_Custom" declares TS's own expression filter and has
                    // none of this; it falls through the size check.)
                    FilterDecl fd;
                    fd.label = unquote(a[1]);
                    for (size_t i = 2; i < a.size() && fd.op == 0; ++i) {
                        const size_t p = a[i].find_first_of("<>");
                        if (p == std::string::npos) continue;
                        fd.op   = a[i][p];
                        fd.chan = trim(a[i].substr(0, p));
                        if (fd.chan.empty() && i > 2) fd.chan = trim(a[i - 1]);
                        for (size_t j = i + 1; j < a.size(); ++j)
                            if (!trim(a[j]).empty()) { fd.value = toDbl(a[j], 0.0); break; }
                    }
                    if (!fd.chan.empty()) va.filters.push_back(fd);
                }
            } else if (section == "TableEditor" || section == "CurveEditor") {
                if (key == "table" || key == "curve") {
                    // TWO NAMES, ONE BODY. An ini declares alternative titles for the same map inside
                    // #if/#else — "Target Lambda Table" or "Target AFR Table" — and the axes and cells
                    // follow the #endif, belonging to both. The parser does not evaluate conditionals,
                    // so a declaration immediately followed by another (no bins in between) is an
                    // ALTERNATIVE, and the body that follows lands on every member of the group.
                    // Without this the first of each pair was left with no cells and silently dropped,
                    // which is why an ini could name a table the studio then could not find.
                    if (!(curTable && curTable->xBins.empty() && curTable->zBins.empty() && curTable->yBins.empty()))
                        altFrom = tables.size();
                    tables.push_back({});
                    curTable = &tables.back();
                    curTable->cell = a.size() > 0 ? a[0] : "";
                    curTable->title = a.size() > 2 ? unquote(a[2]) : curTable->cell;
                    curTable->curve = (key == "curve");
                } else if (curTable) {
                    const auto onGroup = [&](auto&& fn) {
                        for (size_t i = altFrom; i < tables.size(); ++i) fn(tables[i]);
                    };
                    if (key == "xBins") onGroup([&](Table& tb) { tb.xBins = a.size() > 0 ? a[0] : ""; tb.xChan = a.size() > 1 ? a[1] : ""; });
                    else if (key == "yBins") onGroup([&](Table& tb) { tb.yBins = a.size() > 0 ? a[0] : ""; tb.yChan = a.size() > 1 ? a[1] : ""; });
                    else if (key == "zBins") onGroup([&](Table& tb) { tb.zBins = a.size() > 0 ? a[0] : ""; });
                }
            }
        }
        // Flatten the pages into ONE offset space: page 1 keeps its offsets, page 2 starts after it, and
        // so on. The order is the DECLARED order in pageSize, not the order the `page =` directives happen
        // to appear in (rusEFI emits page 2 and 3 before page 1).
        {
            std::vector<int> base(pageSizes.size() + 2, 0);
            int acc = 0;
            for (size_t i = 0; i < pageSizes.size(); ++i) { base[i + 1] = acc; acc += pageSizes[i]; }
            proto["configSize"] = acc;
            for (auto& [name, f] : consts)
                if (f.page >= 1 && size_t(f.page) < base.size()) f.offset += base[f.page];
            // The flat image is a CONVENIENCE for the widgets; the wire is still paged, so hand the link
            // the map back: which flat span belongs to which page id, and which of them a burn commits.
            // Reading the whole flat size out of page 0 is what the ECU answered "out of range" to.
            jf::JJson pages = jf::JJson::array();
            for (size_t i = 0; i < pageSizes.size(); ++i) {
                jf::JJson p = jf::JJson::object();
                p["size"] = pageSizes[i];
                p["base"] = base[i + 1];
                p["id"]   = i < pageIds.size() ? pageIds[i] : static_cast<int>(i);
                p["burn"] = i < pageBurn.size() ? pageBurn[i] : (i == 0);
                pages.push(p);
            }
            if (!pageSizes.empty()) { proto["pages"] = pages; proto["pageSize"] = pageSizes[0]; }
        }
        return build(consts, outs, tables, help, proto, pcv, va, logged);
    }

private:
    // "\x00\x01" (still escaped, as the ini writes it) → the le16 whose two bytes ARE those bytes, so that
    // a link emitting it little-endian puts them on the wire in the ini's order.
    static int idOf(const std::string& s) {
        std::vector<uint8_t> b;
        for (size_t i = 0; i < s.size(); ) {
            if (s[i] == '\\' && i + 3 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                b.push_back(static_cast<uint8_t>(std::stoi(s.substr(i + 2, 2), nullptr, 16)));
                i += 4;
            } else { b.push_back(static_cast<uint8_t>(s[i])); ++i; }
        }
        int v = 0;
        for (size_t i = 0; i < b.size() && i < 2; ++i) v |= int(b[i]) << (8 * i);
        return v;
    }

    struct Field {
        int page = 1;                 // [Constants] page this field belongs to (offsets restart per page)
        int bitLo = -1, bitHi = -1;   // bit-packed range within the word at `offset` (-1 = whole word)
        std::string cls, type; int offset = 0; double scale = 1.0; std::string units;
        int cols = 1, rows = 1; std::vector<std::string> options;
        int maxChars = 0;                 // string fields: declared capacity in characters (0 = unstated)
        double translate = 0.0, lo = 0.0, hi = 0.0; int digits = -1; bool hasRange = false;
    };
    using Fields = std::unordered_map<std::string, Field>;
    struct Table { std::string cell, title, xBins, yBins, zBins, xChan, yChan; bool curve = false; };

    // [VeAnalyze], as declared. `op` is the raw '<' or '>' character: the ini writes the condition
    // that REJECTS a record, so "Minimum RPM" is (RPMValue < 500) and not the reading of it.
    struct FilterDecl { std::string label, chan; char op = 0; double value = 0.0; };
    struct VeAnalyzeDecl { std::string table, targetTable, lambdaCh, egoCh; std::vector<FilterDecl> filters; };

    static int dtSize(const std::string& t) { return (t == "U08" || t == "S08") ? 1 : (t == "U32" || t == "S32" || t == "F32") ? 4 : 2; }

    // An expression that is exactly one channel name — the only kind an importer can resolve without
    // an evaluator. "{ Gego }" yes; "{ a ? b : c }" no.
    static bool isIdentifier(const std::string& s) {
        if (s.empty() || (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_')) return false;
        for (char c : s)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
        return true;
    }
    // splitArgs has already cut the line at commas; an expression can contain them, so put it back.
    static std::string joinArgs(const std::vector<std::string>& a) {
        std::string out;
        for (size_t i = 0; i < a.size(); ++i) { if (i) out += ','; out += a[i]; }
        return out;
    }

    // "name = scalar, TYPE, OFFSET, \"units\", SCALE, …" | "bits, TYPE, OFFSET, [b:b], \"o0\", …" |
    // "array, TYPE, OFFSET, [RxC], \"units\", SCALE, …". a[0] = class.
    // Is this two-option pair a plain FLAG (a tick box says it all) rather than a choice worth naming?
    // "false"/"true" carries no information a checkbox does not; "4 strokes"/"2 strokes" does.
    static bool isFlagPair(const std::vector<std::string>& o) {
        auto low = [](std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
        const std::string a = low(o[0]), b = low(o[1]);
        return (a == "false" && b == "true") || (a == "no" && b == "yes") || (a == "off" && b == "on");
    }

    static bool parseField(const std::vector<std::string>& a, Field& f) {
        f.cls = a[0];
        if (f.cls == "scalar") {
            // scalar, TYPE, offset, "units", scale, translate, lo, hi, digits
            if (a.size() < 3) return false;
            f.type = a[1]; f.offset = toInt(a[2]);
            if (a.size() > 3) f.units = unitsOf(a[3]);
            if (a.size() > 4) f.scale = toDbl(a[4], 1.0);
            if (a.size() > 5) f.translate = toDbl(a[5], 0.0);
            if (a.size() > 7) { f.lo = toDbl(a[6], 0.0); f.hi = toDbl(a[7], 0.0); f.hasRange = f.hi > f.lo; }
            if (a.size() > 8) f.digits = toInt(a[8]);
            return true;
        }
        if (f.cls == "string") {                     // string, ASCII, offset, length [, maxChars]
            if (a.size() < 4) return false;
            f.type = "string"; f.offset = toInt(a[2]); f.cols = toInt(a[3]);
            // THE FIFTH FIELD IS THE CAPACITY IN CHARACTERS, and it is not always length-minus-one.
            //     vinNumber = string, ASCII, 3332, 17, 17
            // says seventeen bytes AND seventeen usable characters — a VIN is seventeen characters, so
            // there is nothing left over for a terminator, and reserving one silently made the last
            // character of every VIN impossible to enter. Dropped before; carried now.
            if (a.size() >= 5) f.maxChars = toInt(a[4]);
            return true;
        }
        if (f.cls == "bits") {
            if (a.size() < 3) return false;
            f.type = a[1]; f.offset = toInt(a[2]);
            // "[lo:hi]" — WHICH bits of the word this field occupies. Skipping it made every flag packed
            // into a word decode as the whole word (so they all read alike), and a write to one clobber the
            // rest. Some inis omit the offset and put the range in a[2]; accept either position.
            for (size_t i = 2; i < a.size() && i <= 3; ++i)
                if (a[i].find(':') != std::string::npos) parseBits(a[i], f.bitLo, f.bitHi);
            // EVERY option, in the order declared — a `bits` field's value IS the index into this list, so
            // dropping an entry shifts every one after it. rusEFI's pin list starts NONE, INVALID, PA0,
            // matching Gpio::Unassigned=0, Invalid=1, A0=2: filtering "INVALID" out (it was hidden as junk)
            // made the studio display PA1 for a pin set to PA0, and write Invalid when PA0 was chosen.
            // A label nobody should pick is a job for the UI, not for the numbering.
            for (size_t i = 4; i < a.size(); ++i) f.options.push_back(unquote(a[i]));
            return true;
        }
        if (f.cls == "array") {
            if (a.size() < 4) return false;
            f.type = a[1]; f.offset = toInt(a[2]);
            parseDims(a[3], f.rows, f.cols);
            if (a.size() > 4) f.units = unitsOf(a[4]);
            if (a.size() > 5) f.scale = toDbl(a[5], 1.0);
            return true;
        }
        return false;
    }

    // "[0:2]" -> lo 0, hi 2. Anything malformed leaves the range unset (= the whole word).
    static void parseBits(const std::string& s, int& lo, int& hi) {
        std::string in; for (char c : s) if (c != '[' && c != ']' && !std::isspace((unsigned char)c)) in += c;
        const size_t c = in.find(':');
        if (c == std::string::npos) return;
        lo = toInt(in.substr(0, c));
        hi = toInt(in.substr(c + 1));
        if (hi < lo) std::swap(lo, hi);
    }

    static void parseDims(const std::string& s, int& rows, int& cols) {   // "[8x16]" | "[16]"
        rows = 1; cols = 1;
        std::string in; for (char c : s) if (c != '[' && c != ']' && !std::isspace((unsigned char)c)) in += c;
        const size_t x = in.find('x');
        if (x == std::string::npos) cols = std::max(1, toInt(in));
        else { rows = std::max(1, toInt(in.substr(0, x))); cols = std::max(1, toInt(in.substr(x + 1))); }
    }

    static jf::JJson scalarField(const Field& f) {
        jf::JJson o = jf::JJson::object();
        o["type"] = std::string("scalar"); o["offset"] = f.offset; o["datatype"] = f.type;
        // SIZE, explicitly. Every bounds check downstream is "offset + size > image", so a field that
        // omits it is unbounded at offset 0 — which read straight through a null image pointer.
        o["size"] = dtSize(f.type);
        o["scale"] = f.scale; o["units"] = f.units;
        // RANGE, IN RAW UNITS. A TunerStudio ini states a field's bounds in DISPLAY units — tpsMin is
        // "0 to 5" volts — while this meta states everything else about a field raw: the offset, the
        // datatype, the scale that turns raw into engineering. min/max follow that same rule (the native
        // meta has scale 0.1 with max 500 for a 50% limit), and the widgets cook them with the scale like
        // any other stored number. Writing the ini's volts straight in left the control clamped to
        // 0..0.025 V — the range divided by its own scale a second time — so a 5 V calibration could not
        // be entered at all. Convert once, here, where the scale is known.
        if (f.hasRange) {
            const double sc = f.scale != 0.0 ? f.scale : 1.0;
            o["min"] = f.lo / sc;
            o["max"] = f.hi / sc;
        }
        if (f.digits >= 0) o["digits"] = f.digits;
        if (f.translate != 0.0) o["translate"] = f.translate;   // preserved; native decode is scale-only
        if (f.cls == "bits") {
            if (f.bitLo >= 0) { o["bitLo"] = f.bitLo; o["bitHi"] = f.bitHi; }
            // A two-option bits field keeps its LABELS. TunerStudio draws every bits field as a dropdown of
            // its options — "4 strokes / 2 strokes", "Rising / Falling" — and calling a two-option one a
            // plain bool threw the labels away, leaving an unlabelled tick box where TS shows the choice.
            if (!f.options.empty()) {
                o["kind"] = std::string(f.options.size() == 2 && isFlagPair(f.options) ? "bool" : "enum");
                jf::JJson opts = jf::JJson::array();
                for (auto& s : f.options) opts.push(s);
                o["options"] = opts;
            }
            o["scale"] = 1.0; o["min"] = 0; o["max"] = f.options.empty() ? 1 : (int)f.options.size() - 1;
        }
        return o;
    }
    static jf::JJson arr1d(const Field& a) {
        jf::JJson o = jf::JJson::object();
        o["type"] = std::string("table"); o["offset"] = a.offset; o["datatype"] = a.type;
        o["scale"] = a.scale; o["size"] = std::max(1, a.cols) * dtSize(a.type);
        return o;
    }

    static jf::JJson build(const Fields& consts, const Fields& outs, const std::vector<Table>& tables,
                           const std::unordered_map<std::string, std::string>& help, const jf::JJson& proto, const Fields& pcv,
                           const VeAnalyzeDecl& va, const std::unordered_set<std::string>& logged) {
        jf::JJson mod = jf::JJson::object();
        for (const auto& [name, f] : consts)
            if (f.cls == "scalar" || f.cls == "bits") {
                jf::JJson o = scalarField(f);
                if (auto it = help.find(name); it != help.end()) o["help"] = it->second;
                mod[name] = std::move(o);
            } else if (f.cls == "string") {
                // ONE spelling for text, the same one codegen emits: a string IS a scalar whose datatype
                // is ASCII. `type` is the SHAPE (scalar/table/array), `datatype` the ENCODING — keeping
                // those orthogonal is why this needs no special case downstream. Emitting a `type:
                // "string"` of its own is what made imported text invisible to every consumer that only
                // knew the native shape.
                jf::JJson o = jf::JJson::object();
                o["type"] = std::string("scalar"); o["datatype"] = std::string("ASCII");
                o["offset"] = f.offset; o["size"] = f.cols;
                if (f.maxChars > 0) o["maxChars"] = f.maxChars;
                o["label"] = name;
                mod[name] = std::move(o);
            }

        for (const Table& t : tables) {
            auto cell = consts.find(t.curve ? t.yBins : t.zBins); if (cell == consts.end()) continue;
            // The CELLS are what makes a table; an axis that cannot be resolved costs its labels, not the
            // whole editor. Some axis arrays are declared in [PcVariables] (gearCountArray, solenoidCountArray)
            // and are not in the config image at all — dropping the table on that basis left a menu leaf
            // opening an editor bound to nothing.
            auto xa = consts.find(t.xBins);
            if (xa == consts.end()) {
                // No breakpoint array for the X axis — some are declared in [PcVariables] (gearCountArray,
                // solenoidCountArray), which is not part of the config image at all. The cells are still real
                // data, so they import as the 1-D ARRAY they are (an editor that needs no axis) rather than
                // as a table whose dimensions nothing can work out. Dropping the whole thing instead left a
                // menu leaf opening an editor bound to nothing.
                mod[t.curve ? t.yBins : t.zBins] = arr1d(cell->second);
                continue;
            }
            jf::JJson axes = jf::JJson::array();
            {
                mod[t.xBins] = arr1d(xa->second);
                jf::JJson ax = jf::JJson::object(); ax["array"] = t.xBins; ax["channel"] = t.xChan; ax["n_max"] = std::max(1, xa->second.cols); axes.push(ax);
            }
            if (!t.curve) {
                auto ya = consts.find(t.yBins);
                if (ya != consts.end()) { mod[t.yBins] = arr1d(ya->second); jf::JJson ax = jf::JJson::object(); ax["array"] = t.yBins; ax["channel"] = t.yChan; ax["n_max"] = std::max(1, ya->second.cols); axes.push(ax); }
            }
            jf::JJson te = jf::JJson::object();
            te["type"] = std::string("table"); te["offset"] = cell->second.offset; te["datatype"] = cell->second.type;
            te["scale"] = cell->second.scale; te["cols"] = std::max(1, cell->second.cols); te["rows"] = std::max(1, t.curve ? 1 : cell->second.rows);
            te["units"] = cell->second.units; te["label"] = t.title; te["axes"] = axes;
            mod[t.curve ? t.yBins : t.zBins] = te;
        }

        jf::JJson telem = jf::JJson::object();
        for (const auto& [name, f] : outs) {
            // `bits` channels count: [OutputChannels] packs status flags and small enums into words exactly
            // as [Constants] does (idleState, stftCorrectionState …). Skipping them dropped the channel
            // entirely, so anything bound to one resolved to nothing at all.
            if (f.cls != "scalar" && f.cls != "bits") continue;
            jf::JJson e = jf::JJson::object();
            e["offset"] = f.offset; e["size"] = dtSize(f.type); e["datatype"] = f.type;
            e["scale"] = f.cls == "bits" ? 1.0 : f.scale; e["units"] = f.units; e["label"] = name;
            if (logged.count(name)) e["datalog"] = true;
            if (f.bitLo >= 0) { e["bitLo"] = f.bitLo; e["bitHi"] = f.bitHi; }
            if (f.cls == "bits" && !f.options.empty()) {
                e["kind"] = std::string(f.options.size() == 2 && isFlagPair(f.options) ? "bool" : "enum");
                jf::JJson opts = jf::JJson::array(); for (auto& o : f.options) opts.push(o);
                e["options"] = opts;
            }
            telem[name] = e;
        }

        jf::JJson config = jf::JJson::object(); config["ts"] = mod;
        jf::JJson root = jf::JJson::object(); root["config"] = config; root["telemetry"] = telem;
        if (!proto.obj().empty()) root["tsProtocol"] = proto;   // signature/pageSize/blockingFactor/ochBlockSize
        // The IDENTITY block. Without it MetaModel::loadFile refuses the file outright (it requires
        // meta.layout_hash), which is why an imported ini has never actually loaded — the import reported
        // success at the parse and then failed at the load, every time.
        jf::JJson m = jf::JJson::object();
        const std::string sig = proto["signature"].str();
        m["layout_hash"] = layoutHashOf(sig.empty() ? "ts-import" : sig);
        m["product"]     = sig.empty() ? std::string("TunerStudio import") : sig;
        m["board"]       = std::string("ts");
        m["fw_version"]  = sig;
        // The config image is one TS page.
        m["config_size"] = proto.contains("configSize") ? proto["configSize"].number<int>() : 0;
        // Say what this definition IS, so nothing inherits an assumption that belongs to our own firmware:
        // a TunerStudio ECU has no float signal bus. Its channels are scaled integers, and decoding one
        // through a float32 would drop precision past 2^24 that the source never lost.
        m["floatBus"] = false;
        root["meta"] = m;

        // THE AUTOTUNER'S CONTRACT, carried across as data. A table id in [VeAnalyze] names a
        // [TableEditor] entry, and what the studio addresses is that entry's CELL ARRAY — so the id is
        // resolved here, where the table list is in hand, rather than left for the panel to re-derive.
        // An ini that declares no [VeAnalyze] simply carries none, and the panel says so.
        if (!va.table.empty()) {
            auto pathOf = [&tables](const std::string& id) -> std::string {
                for (const Table& t : tables)
                    if (t.cell == id) return "ts." + (t.curve ? t.yBins : t.zBins);
                return {};
            };
            jf::JJson v = jf::JJson::object();
            v["table"]          = pathOf(va.table);
            v["target_table"]   = pathOf(va.targetTable);
            v["lambda_channel"] = va.lambdaCh;
            // THE BASIS, BECAUSE THE FORMAT FIXES IT. A [VeAnalyze] ego channel is a percentage where
            // 100 means no correction — an ini typically says so in a comment beside the declaration.
            // Carried as data rather than assumed by the panel, which reads a bare name as a multiplier.
            jf::JJson egos = jf::JJson::array();
            if (!va.egoCh.empty()) {
                jf::JJson e = jf::JJson::object();
                e["channel"] = va.egoCh;
                e["basis"]   = std::string("percent_100");
                egos.push(e);
            }
            v["ego_channels"] = egos;
            jf::JJson fs = jf::JJson::array();
            for (const FilterDecl& f : va.filters) {
                jf::JJson o = jf::JJson::object();
                o["name"]    = f.label;
                o["channel"] = f.chan;
                o["op"]      = std::string(1, f.op == '>' ? '>' : '<');
                o["value"]   = f.value;
                fs.push(o);
            }
            v["filters"] = fs;
            if (!v["table"].str().empty()) root["autotune"] = v;
        }

        // [PcVariables] -> host-side declarations. The studio gives them storage in its own block; the ECU
        // has none, which is precisely what makes them "virtual" configuration.
        if (!pcv.empty()) {
            jf::JJson pcs = jf::JJson::array();
            for (const auto& [name, f] : pcv) {
                if (f.cls != "scalar" && f.cls != "bits") continue;   // arrays: not yet
                jf::JJson v = jf::JJson::object();
                v["name"]     = name;
                v["datatype"] = f.type.empty() ? std::string("U08") : f.type;
                v["scale"]    = f.cls == "bits" ? 1.0 : f.scale;
                v["units"]    = f.units;
                v["min"]      = f.cls == "bits" ? 0.0 : f.lo / (f.scale != 0.0 ? f.scale : 1.0);
                v["max"]      = f.cls == "bits" ? double(f.options.empty() ? 1 : int(f.options.size()) - 1)
                                                : f.hi / (f.scale != 0.0 ? f.scale : 1.0);   // raw, as above
                if (f.cls == "bits" && !f.options.empty()) {
                    v["kind"] = std::string(f.options.size() == 2 && isFlagPair(f.options) ? "bool" : "enum");
                    jf::JJson opts = jf::JJson::array(); for (auto& o : f.options) opts.push(o);
                    v["options"] = opts;
                }
                pcs.push(v);
            }
            if (!pcs.arr().empty()) root["pcVars"] = pcs;
        }
        return root;
    }

    // A stable layout id for an imported definition: the signature IS its identity (it changes whenever
    // the firmware's layout does), so hash it rather than inventing a version.
    static std::string layoutHashOf(const std::string& sig) {
        uint32_t h = 2166136261u;                      // FNV-1a
        for (unsigned char c : sig) { h ^= c; h *= 16777619u; }
        char buf[16]; std::snprintf(buf, sizeof(buf), "%08x", h);
        return std::string(buf);
    }

public:
    // Write an imported meta the way MetaModel expects to READ one: the JSON body followed by its little-
    // endian CRC32 footer. main.cpp used a plain dumpToFile, so the file failed the integrity check before
    // anything even looked at its contents. One writer, so the on-disk format lives in one place.
    static bool writeMetaFile(const jf::JJson& meta, const std::string& path) {
        const std::string body = meta.dump();
        uint32_t crc = 0xFFFFFFFFu;
        for (unsigned char b : body) {
            crc ^= b;
            for (int i = 0; i < 8; ++i) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
        crc ^= 0xFFFFFFFFu;
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        f << body;
        for (int i = 0; i < 4; ++i) f.put(static_cast<char>((crc >> (8 * i)) & 0xFF));
        return static_cast<bool>(f);
    }

private:
    // --- text helpers -------------------------------------------------------
    static std::vector<std::string> splitLines(const std::string& s) {
        std::vector<std::string> out; std::string cur;
        for (char c : s) { if (c == '\n') { out.push_back(cur); cur.clear(); } else if (c != '\r') cur += c; }
        out.push_back(cur); return out;
    }
    static std::string stripComment(const std::string& s) {   // ';' comment, but not inside quotes
        std::string o; bool q = false;
        for (char c : s) { if (c == '"') q = !q; if (c == ';' && !q) break; o += c; }
        return o;
    }
    static std::string trim(const std::string& s) {
        const size_t a = s.find_first_not_of(" \t"); if (a == std::string::npos) return "";
        return s.substr(a, s.find_last_not_of(" \t") - a + 1);
    }
    static std::string unquote(const std::string& s) {
        std::string t = trim(s);
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
        return t;
    }
    // Split on ',' respecting quotes AND braces. A metric/imperial field writes EXPRESSIONS for its units,
    // scale and limits:
    //     maxAcClt = scalar, S16, 20, {bitStringValue(unitsLabels, useMetricOnInterface)}, {…}, {…}, {…}
    // Splitting on the comma inside those braces shifted every later argument along by one, so the field
    // ended up with the head of an expression as its "units" and no limits at all.
    static std::vector<std::string> splitArgs(const std::string& s) {
        std::vector<std::string> out; std::string cur; bool q = false; int br = 0;
        for (char c : s) {
            if (c == '"') { q = !q; cur += c; }
            else if (!q && (c == '{' || c == '(')) { ++br; cur += c; }
            else if (!q && (c == '}' || c == ')')) { --br; cur += c; }
            else if (c == ',' && !q && br <= 0) { out.push_back(trim(cur)); cur.clear(); }
            else cur += c;
        }
        out.push_back(trim(cur)); return out;
    }
    static int toInt(const std::string& s) { try { return std::stoi(trim(s)); } catch (...) { return 0; } }
    // A number, or the METRIC branch of a "{ metric ? a : b }" expression. These expressions switch a field
    // between metric and imperial display; the studio scales at the CONTROL, from raw counts, so the metric
    // (native) branch is the one that describes the stored data. Anything else falls back to the default.
    static double toDbl(const std::string& s, double d) {
        std::string t = trim(s);
        if (!t.empty() && t.front() == '{' && t.back() == '}') t = trim(t.substr(1, t.size() - 2));
        try { return std::stod(t); } catch (...) {}
        const size_t q = t.find('?');
        if (q != std::string::npos) {
            const size_t c = t.find(':', q);
            const std::string a = trim(t.substr(q + 1, c == std::string::npos ? std::string::npos : c - q - 1));
            try { return std::stod(a); } catch (...) {}
        }
        return d;
    }
    // A units LABEL, or nothing if the ini computes it. "{bitStringValue(unitsLabels, ...)}" picks C or F at
    // runtime; carrying the expression text into a caption is worse than carrying no units at all.
    static std::string unitsOf(const std::string& s) {
        const std::string u = unquote(trim(s));
        return (!u.empty() && u.front() == '{') ? std::string() : u;
    }
};
