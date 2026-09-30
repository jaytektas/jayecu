#include "MetaModel.h"
#include <j/update/JVersion.h>   // min_studio
#include "UnitManager.h"

#include <j/core/Log.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

// ---------------------------------------------------------------------------
// Local helpers: little-endian codec, base64 decode, and small string/path parsing.

// ---------------------------------------------------------------------------

// Big-endian twins. A definition declares its own byte order, so the codec has to be able to read both;
// the LE path stays the default because every meta this firmware generates is little-endian.
static uint16_t beU16(const unsigned char *p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
static uint32_t beU32(const unsigned char *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
}
static void wrBE16(unsigned char *p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
static void wrBE32(unsigned char *p, uint32_t v)
{
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

static uint16_t leU16(const unsigned char *p)
{
    return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}
static uint32_t leU32(const unsigned char *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static void wrLE16(unsigned char *p, uint16_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
static void wrLE32(unsigned char *p, uint32_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

// Standard base64 decode (lenient: skips whitespace / non-alphabet chars, stops at padding).
static std::vector<uint8_t> base64Decode(const std::string &in)
{
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    int buf = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=') break;
        const int d = val(c);
        if (d < 0) continue;
        buf = (buf << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// Look up a key, returning a default-constructed mapped value when it is absent.
template <class M>
static typename M::mapped_type mapValueOr(const M &m, const typename M::key_type &k)
{
    const auto it = m.find(k);
    return it != m.end() ? it->second : typename M::mapped_type{};
}

// The string if the node is a string, else the given default.
static std::string strOr(const jf::JJson &v, const std::string &def)
{
    return v.isString() ? v.str() : def;
}

// Whole-string base-10 integer; ok = false if not a pure integer.
static int parseIntStrict(const std::string &s, bool &ok)
{
    ok = false;
    if (s.empty()) return 0;
    size_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') { neg = (s[i] == '-'); ++i; }
    if (i >= s.size()) return 0;
    long val = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return 0;
        val = val * 10 + (s[i] - '0');
    }
    ok = true;
    return int(neg ? -val : val);
}

static int strIndexOf(const std::vector<std::string> &v, const std::string &s)
{
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == s) return int(i);
    return -1;
}

// Split "array[key]rest" the way ^([^\[]+)\[([^\]]+)\] captures: array = up to the first '[' (>=1 char),
// key = up to the first ']' after it (>=1 char), rest = whatever follows ']'. false if no such shape.
static bool splitElem(const std::string &path, std::string &array, std::string &key, std::string &rest)
{
    const size_t lb = path.find('[');
    if (lb == std::string::npos || lb == 0) return false;   // ([^\[]+) needs >=1 char
    const size_t rb = path.find(']', lb + 1);
    if (rb == std::string::npos || rb == lb + 1) return false;   // ([^\]]+) needs >=1 char
    array = path.substr(0, lb);
    key   = path.substr(lb + 1, rb - lb - 1);
    rest  = path.substr(rb + 1);
    return true;
}

static bool isIdent(const std::string &s)   // [A-Za-z_][A-Za-z0-9_]*
{
    if (s.empty()) return false;
    if (!(std::isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}
static bool isWord(const std::string &s)    // \w+  ==  [A-Za-z0-9_]+
{
    if (s.empty()) return false;
    for (char c : s)
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

// Nested sub-struct field: ^([A-Za-z_]\w*)\[(\d+)\]\.(\w+)$  (precond/cand fields).
static bool parseSub(const std::string &s, std::string &name, std::string &idx, std::string &field)
{
    const size_t lb = s.find('[');
    if (lb == std::string::npos) return false;
    name = s.substr(0, lb);
    if (name.empty() || !(std::isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
    for (char c : name)
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    const size_t rb = s.find(']', lb + 1);
    if (rb == std::string::npos || rb == lb + 1) return false;
    idx = s.substr(lb + 1, rb - lb - 1);
    for (char c : idx)
        if (c < '0' || c > '9') return false;   // \d+
    if (rb + 1 >= s.size() || s[rb + 1] != '.') return false;
    field = s.substr(rb + 2);
    if (!isWord(field)) return false;
    return true;
}

// The segment before the first '.'.
static std::string sectionFirst(const std::string &s)
{
    const size_t dot = s.find('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}
// Everything from the second segment onward ("" if there is no '.').
static std::string sectionFrom1(const std::string &s)
{
    const size_t dot = s.find('.');
    return dot == std::string::npos ? std::string() : s.substr(dot + 1);
}

// CRC32 (ISO 3309 / zlib poly 0xEDB88320) — matches Python zlib.crc32.
static uint32_t crc32_step(uint32_t crc, unsigned char byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; ++i)
        crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    return crc;
}
static uint32_t crc32_buf(const std::vector<uint8_t> &buf)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : buf)
        crc = crc32_step(crc, b);
    return crc ^ 0xFFFFFFFFu;
}

static std::vector<MetaModel::BitGroup> parseBitGroups(const jf::JJson &o)
{
    std::vector<MetaModel::BitGroup> out;
    const jf::JJson &b = o["bits"];
    for (const auto &[name, gv] : b.obj()) {
        MetaModel::BitGroup g;
        g.name  = name;
        g.lo    = gv["bitLo"].number<int>(0);
        g.hi    = gv["bitHi"].number<int>(g.lo);
        g.kind  = gv["kind"].str();
        g.label = strOr(gv["label"], name);
        g.help  = gv["help"].str();
        for (const jf::JJson &ov : gv["options"].arr())
            g.options.push_back(ov.str());
        out.push_back(std::move(g));
    }
    return out;
}

bool MetaModel::loadFile(const std::string &path)
{
    valid_ = false;
    locCache_.clear();          // a new meta means every path resolves somewhere else
    telem_.clear();
    config_.clear();
    tables_.clear();
    arrays1d_.clear();
    structArrays_.clear();
    luaFunctions_.clear();
    luaCallbacks_.clear();
    meta_ = {};
    hardware_ = {};
    protocol_ = {};
    navTree_   = {};
    defaultImage_.clear();
    segments_.clear();
    autotune_ = {};             // a new definition states its own autotune contract, or none
    valueAutotunes_.clear();
    applies_.clear();
    signals_.clear();
    signalByIndex_.clear();
    enums_.clear();

    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // Verify the 4-byte LE CRC32 footer appended by codegen. Catches SD write corruption
    // and truncated transfers before we attempt to parse.
    if (raw.size() < 4)
        return false;
    const std::vector<uint8_t> body(raw.begin(), raw.end() - 4);
    const uint32_t stored = leU32(raw.data() + raw.size() - 4);
    if (crc32_buf(body) != stored)
        return false;   // corrupt file — refuse to load

    const std::string bodyStr(body.begin(), body.end());
    const auto docOpt = jf::JJson::tryParse(bodyStr);
    if (!docOpt || !docOpt->isObject())
        return false;
    const jf::JJson root = *docOpt;

    meta_ = root["meta"];
    needsStudio_.clear();
    // A FORMAT THIS STUDIO DOES NOT KNOW. Checked before min_studio and without needing this studio's
    // version: it is the one refusal that does not depend on someone having remembered to raise a number.
    if (const int fmt = meta_["meta_format"].number<int>(); fmt > kMetaFormat) {
        const std::string need = meta_["min_studio"].str();
        needsStudio_ = (!need.empty() && !studioVersion_.empty() &&
                        jf::JVersion::parse(need).isNewerThan(jf::JVersion::parse(studioVersion_)))
                     ? need : std::string("newer than ") + (studioVersion_.empty() ? "this one" : studioVersion_);
        meta_ = {};
        return false;
    }
    if (!studioVersion_.empty()) {
        const std::string need = meta_["min_studio"].str();
        if (!need.empty() && jf::JVersion::parse(need).isNewerThan(jf::JVersion::parse(studioVersion_))) {
            needsStudio_ = need;
            meta_ = {};
            return false;
        }
    }
    // Declared byte order. An imported TunerStudio definition states its own; anything this firmware
    // generates is little-endian, which is the default when nothing says otherwise.
    {
        const std::string bo = root.contains("tsProtocol") ? root["tsProtocol"]["endianness"].str()
                                                           : meta_["byteOrder"].str();
        // THE DEFINITION'S OWN NAME FOR ITSELF. An imported ini states a signature — "rusEFI
        // jaytek.2026.08.12.jaytek_f7.665772324" — which is what its firmware answers with and what a
        // tuner would recognise. `board` for such a meta is just "ts", which names a protocol rather
        // than an ECU and tells nobody which one they are looking at.
        if (root.contains("tsProtocol")) tsSignature_ = root["tsProtocol"]["signature"].str();
        bigEndian_ = (bo == "big" || bo == "Big" || bo == "BIG");
        // Absent = this firmware's own meta = a float bus. An importer says so explicitly when it isn't.
        floatBus_ = meta_.contains("floatBus") ? meta_["floatBus"].boolean(true) : true;
    }
    // A definition may DECLARE host-side variables (an imported ini's [PcVariables]); a project may add
    // more later through applyPcVars. Same mechanism either way — the source differs, the result does not.
    pendingPcVars_.clear();              // a meta without declarations must not inherit the last one's
    if (root.contains("pcVars")) {
        std::vector<PcVar> vars;
        for (const jf::JJson &v : root["pcVars"].arr()) {
            PcVar pv;
            pv.name     = v["name"].str();
            if (pv.name.empty()) continue;
            pv.datatype = v.contains("datatype") ? v["datatype"].str() : std::string("F32");
            pv.scale    = v["scale"].number(1.0);
            pv.minV     = v["min"].number();
            pv.maxV     = v["max"].number();
            pv.units    = v["units"].str();
            pv.label    = v["label"].str();
            pv.kind     = v["kind"].str();
            if (v.contains("default")) { pv.hasDef = true; pv.defV = v["default"].number(); }
            for (const jf::JJson &o : v["options"].arr()) pv.options.push_back(o.str());
            vars.push_back(std::move(pv));
        }
        pendingPcVars_ = std::move(vars);   // applied at the end of load, after config_ is populated
    }
    if (!meta_.contains("layout_hash"))
        return false;
    protocol_ = root["protocol"];
    navTree_   = root["navigation_tree"];
    commands_  = root["commands"];   // top-level sibling of "meta" (NOT inside meta_)
    datalogTemplates_ = root["datalog_templates"];   // the log channel sets this firmware ships

    // Hardware self-description (one meta = one board). The firmware is ADC-counts-native; configure the
    // client's raw-analog units (ADC counts <-> mV <-> V) from the board's analog front-end scaling so
    // the studio can display/enter raw inputs in the user's chosen unit (default V).
    hardware_ = root["hardware"];
    hwPool_.clear();
    for (const auto &[iface, list] : root["hw_pool_signals"].obj()) {
        std::vector<std::string> chans;
        for (const jf::JJson &c : list.arr()) chans.push_back(c.str());
        hwPool_[iface] = std::move(chans);
    }
    const jf::JJson &adc = hardware_["adc"];
    const jf::JJson &av  = hardware_["av"];
    UnitManager::instance().configureAnalogRaw(adc["full_scale"].number<int>(),
                                               av["fullscale_mv"].number<int>());

    // The default tune (base64 EcuConfig payload) — the offline baseline so every scalar/table/axis
    // shows its real default value with no ECU connected. A live read replaces it.
    defaultImage_ = base64Decode(root["default_tune"].str());

    // Cache blocks (segments). The `config` block (base 0) is the tune; other blocks (e.g. the
    // RAM-backed `learned` region, persisted to SD totems) are addressed by the same meta-path offsets but held in their own
    // Cache buffer. Older metas without a `segments` list simply yield config-only behaviour.
    for (const jf::JJson &s : root["segments"].arr()) {
        Segment seg;
        seg.id       = s["id"].str();
        seg.kind     = s["kind"].str();
        seg.base     = static_cast<uint32_t>(s["base"].number<int>());   // base fits int32 by construction
        seg.size     = s["size"].number<int>();
        seg.writable = s.contains("writable") ? s["writable"].boolean() : true;
        segments_.push_back(seg);
    }

    // The VE autotuner's contract — see MetaModel::Autotune. Native schemas emit `autotune`; a
    // TunerStudio ini's [VeAnalyze] is converted to the same shape at import, so there is one reader.
    if (root.contains("autotune")) {
        const jf::JJson &a = root["autotune"];
        autotune_.table         = a["table"].str();
        autotune_.targetTable   = a["target_table"].str();
        autotune_.targetChannel = a["target_channel"].str();
        autotune_.lambdaChannel = a["lambda_channel"].str();
        autotune_.delayTable    = a["delay_table"].str();
        // Either shape: a bare channel name (a multiplier, which is what every native declaration
        // means) or an object naming its basis. Both appear in real definitions — ours writes names,
        // the .ini importer writes objects, because a TunerStudio ego channel is a percentage.
        for (const jf::JJson &c : a["ego_channels"].arr()) {
            AutotuneEgo e;
            if (c.isObject()) {
                e.channel = c["channel"].str();
                e.basis   = egoBasisFromName(c["basis"].str());
            } else {
                e.channel = c.str();
            }
            if (!e.channel.empty()) autotune_.egoChannels.push_back(e);
        }
        for (const jf::JJson &f : a["filters"].arr()) {
            AutotuneFilter fl;
            fl.name    = f["name"].str();
            fl.channel = f["channel"].str();
            fl.above   = f["op"].str() == ">";
            fl.value   = f["value"].number<double>();
            if (!fl.channel.empty()) autotune_.filters.push_back(fl);
        }
    }

    for (const jf::JJson &t : root["value_autotune"].arr()) {
        ValueAutotune v;
        v.name         = t["name"].str();
        v.table        = t["table"].str();
        v.valueChannel = t["value_channel"].str();
        v.settleMs     = t["settle_ms"].number<double>();
        for (const jf::JJson &x : t["steady"].arr())
            v.steady.push_back({ x["channel"].str(), x["span"].number<double>() });
        for (const jf::JJson &f : t["filters"].arr()) {
            AutotuneFilter fl;
            fl.name    = f["name"].str();
            fl.channel = f["channel"].str();
            fl.above   = f["op"].str() == ">";
            fl.value   = f["value"].number<double>();
            if (!fl.channel.empty()) v.filters.push_back(fl);
        }
        if (!v.table.empty() && !v.valueChannel.empty()) valueAutotunes_.push_back(v);
    }

    // Outside-world wiring: resource/signal name -> {connector terminal, wire colour}. Drives the
    // wiring widget's colour + external-pin display. Absent on older metas -> empty map (props fallback).
    for (const auto &[k, v] : root["wiring"].obj())
        wiring_[k] = { v["pin"].str(), v["color"].str() };
    // The physical connectors: id -> shell {colour, part, desc} (drives the connector diagram).
    for (const auto &[k, v] : root["connectors"].obj())
        connectors_[k] = { v["color"].str(), v["part"].str(), v["desc"].str() };

    // Bus signal name <-> SignalId index (Axis Setup maps a chosen channel name to its index).
    const jf::JJson &sigs = root["signals"];
    for (const auto &[k, v] : sigs.obj()) {
        const int idx = v.number<int>();
        signals_[k] = idx;
        signalByIndex_[idx] = k;
    }

    // DTC hover text for the DtcDock: exact P-code -> phrase, plus OBD subsystem ranges as a
    // fallback so a code with no exact entry still resolves to its category label. Codes are the
    // hex-BCD form "P0122" (0x0122) — the same encoding the firmware stores and the dock formats.
    auto codeOf = [](const std::string &p) -> uint16_t {
        // trim + uppercase
        size_t b = 0, e = p.size();
        while (b < e && std::isspace((unsigned char)p[b])) ++b;
        while (e > b && std::isspace((unsigned char)p[e - 1])) --e;
        std::string s = p.substr(b, e - b);
        for (char &c : s) c = char(std::toupper((unsigned char)c));
        if (!s.empty() && std::string("PCBU").find(s[0]) != std::string::npos) s.erase(0, 1);
        if (s.empty()) return 0;
        uint32_t v = 0;   // parse as hex; any non-hex char => 0
        for (char c : s) {
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            v = v * 16 + uint32_t(d);
        }
        return uint16_t(v);
    };
    dtcDesc_.clear();
    const jf::JJson &dd = root["dtc_descriptions"];
    for (const auto &[k, v] : dd.obj())
        if (const uint16_t c = codeOf(k)) dtcDesc_[c] = v.str();
    dtcCats_.clear();
    for (const jf::JJson &v : root["dtc_categories"].arr()) {
        const jf::JJson &o = v;
        const uint16_t lo = codeOf(o["lo"].str()), hi = codeOf(o["hi"].str());
        if (lo && hi) dtcCats_.push_back({ lo, hi, o["label"].str() });
    }

    // The sensor type catalog: units, precision and value domain per type. The authority on what a
    // sensor reads — a generic input's channel descriptor cannot answer that (see SensorType).
    sensorTypes_.clear();
    for (const jf::JJson &v : root["sensor_types"].arr()) {
        SensorType st;
        st.id         = v["id"].str();
        st.units      = v["units"].str();
        st.digits     = v["digits"].number<int>(0);
        st.scale      = v["scale"].number(1.0);
        st.minV       = v["min"].number();
        st.maxV       = v["max"].number();
        st.selectable = v["selectable"].boolean();
        sensorTypes_.push_back(st);   // ORDER IS THE CONTRACT: index == the stored type byte
    }

    // Enum value-sets: id -> ordered labels (index = value).
    const jf::JJson &enums = root["enums"];
    for (const auto &[k, v] : enums.obj()) {
        std::vector<std::string> labels, ids;
        for (const jf::JJson &ev : v.arr()) {
            labels.push_back(ev["label"].str());
            ids.push_back(ev["id"].str());
        }
        enums_[k] = labels;
        // …and the IDS in the same order. A label is for a person ("Analogue Voltage"); the id is the key
        // everything else in the meta joins on — hw_pool_signals is keyed by "analog_voltage", so a value
        // read out of a config field can only find its pin pool through this.
        enumIds_[k] = ids;
    }

    const jf::JJson &telem = root["telemetry"];
    for (const auto &[k, v] : telem.obj()) {
        const jf::JJson &e = v;
        TelemField tf;
        tf.offset   = e["offset"].number<int>();
        tf.size     = e["size"].number<int>();
        tf.datatype = e["datatype"].str();
        tf.scale    = e["scale"].number(1.0);
        // Absent = float (this firmware's default, and what a definition that says nothing implies).
        tf.busFloat = e.contains("bus_type") ? (e["bus_type"].str() == "float") : true;
        tf.bits.lo  = e["bitLo"].number<int>(-1);   // packed channel: [OutputChannels] `bits` field
        tf.bits.hi  = e["bitHi"].number<int>(-1);
        tf.units    = e["units"].str();
        tf.module   = e["module"].str();
        tf.label    = e["label"].str();
        tf.enumId   = e["enum"].str();
        for (const auto& o : e["options"].arr()) tf.options.push_back(o.str());
        tf.minV     = e["min"].number();
        tf.maxV     = e["max"].number();
        tf.digits   = e["digits"].number<int>(-1);
        tf.datalog  = e["datalog"].boolean(false);
        tf.updateHz = static_cast<int>(e["update_hz"].number(0));
        telem_[k] = tf;
    }

    // Config: { module: { field: {type: scalar|table|struct_array, offset, datatype, ...} } }
    const jf::JJson &config = root["config"];
    for (const auto &[mod, fields] : config.obj())
        for (const auto &[name, e] : fields.obj())
            if (e.isObject() && e.contains("applies")) applies_[mod + "." + name] = e["applies"].str();
    for (const auto &[modKey, modVal] : config.obj()) {
        const jf::JJson &fields = modVal;
        for (const auto &[feKey, feVal] : fields.obj()) {
            const jf::JJson &e = feVal;
            const std::string type = e["type"].str();
            const std::string key = modKey + "." + feKey;

            if (type == "scalar") {
                ConfigField cf;
                cf.offset   = e["offset"].number<int>();
                cf.datatype = e["datatype"].str();
                cf.size     = e["size"].number<int>();
                // A descriptor that omits its size would defeat every "offset + size <= image" check in
                // the app. The datatype always knows, so take it from there rather than trusting a 0.
                // (This read the datatype one line BEFORE it was assigned, so it always sized from "".)
                if (cf.size <= 0) cf.size = dataSize(cf.datatype);
                cf.scale    = e["scale"].number(1.0);
                cf.minV     = e["min"].number();
                cf.maxV     = e["max"].number();
                cf.units    = e["units"].str();
                cf.label    = e["label"].str();
                cf.help     = e["help"].str();
                cf.digits   = e["digits"].number<int>(-1);
                cf.maxChars = e["maxChars"].number<int>(0);   // declared text capacity; 0 = unstated
                cf.kind     = e["kind"].str();
                // TEXT has two spellings. A TunerStudio import declares type "string" (handled above);
                // the native meta declares the SAME field as a scalar of datatype ASCII — which is how
                // config.vehicle (Make/Model/Engine/VIN/Notes/Name) arrives. Classify it here so one
                // notion of "this is text" reaches everything downstream: the dictionary drops a text
                // field instead of a spin box, and the tune writer saves the string instead of decoding
                // its first four characters as a float and restoring garbage over the rest.
                if (cf.kind.empty() && cf.datatype == "ASCII") cf.kind = "string";
                cf.bits.lo  = e["bitLo"].number<int>(-1);   // packed field: the value lives in [lo..hi]
                cf.bits.hi  = e["bitHi"].number<int>(-1);
                cf.bitGroups = parseBitGroups(e);          // named groups packed into this scalar
                for (const jf::JJson &ov : e["options"].arr())
                    cf.options.push_back(ov.str());
                config_[key] = cf;
            // A table is a table whether it has 1 axis (a curve's worth of data — 1 row) or 2/3 (a map).
            // 2D/3D maps carry "rows"; a 1-axis table has an `axes` ref but no "rows" (it's a single row).
            // Only a flat entry with NO axes at all (an axis breakpoint array, or a TS-imported 1darray)
            // falls through to arrays1d below.
            } else if (type == "table" && (e.contains("rows") || !e["axes"].arr().empty())) {
                ConfigTable ct;
                ct.offset   = e["offset"].number<int>();
                ct.datatype = e["datatype"].str();
                ct.cellSize = dataSize(ct.datatype);
                ct.scale    = e["scale"].number(1.0);
                ct.digits   = e["digits"].number<int>(-1);
                ct.cols     = e["cols"].number<int>();
                ct.rows     = e["rows"].number<int>();
                ct.colsMax  = e["cols_max"].number<int>(ct.cols);     // the stride cells are laid out at
                ct.rowsMax  = e["rows_max"].number<int>(0);
                ct.depthMax = e["depth_max"].number<int>(0);
                if (ct.rows == 0) ct.rows = 1;             // 1-axis table = one row (its single dimension is cols)
                ct.depth    = e["depth"].number<int>(1);   // absent / 1 = a plain 2D table
                if (ct.colsMax  <= 0) ct.colsMax  = ct.cols;
                if (ct.rowsMax  <= 0) ct.rowsMax  = ct.rows;
                if (ct.depthMax <= 0) ct.depthMax = ct.depth;
                ct.minV     = e["min"].number();
                ct.maxV     = e["max"].number();
                ct.units    = e["units"].str();
                ct.label    = e["label"].str();
                ct.help     = e["help"].str();
                for (const jf::JJson &a : e["apply_to"].arr())   // learned surface -> its base table
                    if (!a.str().empty()) ct.applyTo.push_back(a.str());
                ct.applyAdd = e["apply_mode"].str() == "add";
                const jf::JJson &axes = e["axes"];
                const std::string modPrefix = modKey + ".";
                for (const jf::JJson &av2 : axes.arr()) {
                    const jf::JJson &ao = av2;
                    Axis ax;
                    ax.array    = modPrefix + ao["array"].str();
                    ax.channel  = ao["channel"].str();
                    ax.nMax     = ao["n_max"].number<int>();
                    if (!ao["n"].isNull() && ao.contains("n"))
                        ax.nScalar = modPrefix + ao["n"].str();
                    if (ao.contains("src"))
                        ax.srcScalar = modPrefix + ao["src"].str();
                    if (ao.contains("en"))
                        ax.enScalar = modPrefix + ao["en"].str();
                    // An axis is optional iff it carries an enable toggle (the meta keys it off `en`,
                    // not a separate `optional` flag) — this drives the Axis Setup enable checkbox and
                    // the disabled-axis collapse.
                    ax.optional = !ax.enScalar.empty() || ao["optional"].boolean();
                    ct.axes.push_back(ax);
                }
                if (ct.axes.size() >= 1) { ct.xAxis = ct.axes[0].array; ct.xChannel = ct.axes[0].channel; }
                if (ct.axes.size() >= 2) { ct.yAxis = ct.axes[1].array; ct.yChannel = ct.axes[1].channel; }
                if (ct.axes.size() >= 3) { ct.zAxis = ct.axes[2].array; ct.zChannel = ct.axes[2].channel; }
                for (const jf::JJson &l : e["row_labels"].arr()) ct.rowLabels.push_back(l.str());
                for (const jf::JJson &l : e["col_labels"].arr()) ct.colLabels.push_back(l.str());
                tables_[key] = ct;
            } else if (type == "table") {                 // 1D array / axis / curve
                ConfigField af;
                af.offset   = e["offset"].number<int>();
                af.datatype = e["datatype"].str();
                af.scale    = e["scale"].number(1.0);
                af.digits   = e["digits"].number<int>(-1);
                af.minV     = e["min"].number();
                af.maxV     = e["max"].number();
                af.units    = e["units"].str();
                af.label    = e["label"].str();
                af.help     = e["help"].str();
                const int cs = dataSize(af.datatype);
                af.count    = cs > 0 ? e["size"].number<int>() / cs   // full allocation (read live bins)
                                     : e["cols"].number<int>();
                arrays1d_[key] = af;
            } else if (type == "struct_array") {           // sensors[], outputs[], … per-element config
                ConfigArray ca;
                ca.module     = modKey;
                ca.name       = feKey;
                ca.label      = strOr(e["label"], feKey);
                ca.baseOffset = e["base_offset"].number<int>();
                ca.count      = e["count"].number<int>();
                ca.stride     = e["stride"].number<int>();
                const jf::JJson &fs = e["fields"];
                for (const auto &[fldKey, fldVal] : fs.obj()) {
                    const jf::JJson &fo = fldVal;
                    ArrayField af;
                    af.name      = fldKey;
                    af.relOffset = fo["rel_offset"].number<int>();
                    af.datatype  = fo["datatype"].str();
                    af.scale     = fo["scale"].number(1.0);
                    af.digits    = fo["digits"].number<int>(-1);
                    af.units     = fo["units"].str();
                    af.label     = strOr(fo["label"], fldKey);
                    af.help      = fo["help"].str();
                    af.subcat    = fo["subcat"].str();
                    af.minV      = fo["min"].number();
                    af.maxV      = fo["max"].number();
                    if (const jf::JJson &dv = fo["default"]; !dv.isNull()) {
                        af.hasDefault = true;
                        if (dv.isArray()) for (const jf::JJson &x : dv.arr()) af.defaults.push_back(x.number());
                        else              af.defaults.push_back(dv.number());
                    }
                    af.kind      = fo["kind"].str();
                    af.typeScaled = fo["type_scaled"].isBool() && fo["type_scaled"].boolean();
                    af.size      = fo["size"].number<int>();
                    af.bits      = parseBitGroups(fo);   // named groups packed into this element field
                    for (const jf::JJson &ov : fo["options"].arr())
                        af.options.push_back(ov.str());
                    for (const jf::JJson &ov : fo["option_ids"].arr())
                        af.optionIds.push_back(ov.str());   // match on these, never on a label
                    af.enumId = fo["enum"].str();           // which SET those ids belong to
                    const jf::JJson &pk = fo["picker"];
                    if (!pk.obj().empty()) {
                        af.pickerBy = pk["by"].str();
                        for (const jf::JJson &sv : pk["sets"].arr()) {
                            const jf::JJson &so = sv;
                            PickerSet ps;
                            ps.shared = so["shared"].isBool() && so["shared"].boolean();
                            for (const jf::JJson &iv : so["ifaces"].arr())
                                ps.ifaces.push_back(iv.number<int>());
                            for (const jf::JJson &ov : so["options"].arr()) {
                                const jf::JJson &oo = ov;
                                ps.options.push_back(oo["label"].str());
                                ps.values.push_back(oo["value"].number<int>());
                                ps.pins.push_back(oo["pin"].isBool() ? (oo["pin"].boolean() ? 1 : 0) : 1);
                                ps.fixed.push_back(oo["fixed"].isBool() && oo["fixed"].boolean() ? 1 : 0);
                            }
                            af.pickerSets.push_back(ps);
                        }
                    }
                    ca.fields.push_back(af);
                }
                for (const jf::JJson &lv : e["element_labels"].arr())
                    ca.elementLabels.push_back(lv.str());
                for (const jf::JJson &iv : e["element_ids"].arr())
                    ca.elementIds.push_back(iv.str());
                for (const jf::JJson &sv : e["element_signals"].arr())
                    ca.elementSignals.push_back(sv.str());
                // null (a generic input, no build-time type) parses to "" — the same "no type" the
                // firmware spells SENSOR_TYPE_NONE.
                for (const jf::JJson &tv : e["element_types"].arr())
                    ca.elementTypes.push_back(tv.isNull() ? std::string() : tv.str());
                // Per-element interface capability — the catalogue's `interfaces:` list, already
                // collapsed to the single `locked:` one where the build settles it. An absent or
                // empty list means "unconstrained": offer the whole set rather than nothing, so an
                // older ECU's meta (which carries no such key) keeps behaving exactly as it did.
                for (const jf::JJson &fv : e["element_interfaces"].arr()) {
                    std::vector<std::string> allowed;
                    for (const jf::JJson &nv : fv.arr())
                        allowed.push_back(nv.str());
                    ca.elementInterfaces.push_back(std::move(allowed));
                }
                for (const auto &[fieldName, perElem] : e["element_options"].obj()) {
                    auto &dst = ca.elementOptions[fieldName];
                    for (const jf::JJson &ev : perElem.arr()) {
                        std::vector<int> allowed;
                        for (const jf::JJson &iv : ev.arr()) allowed.push_back(iv.number<int>());
                        dst.push_back(std::move(allowed));
                    }
                }
                // Element tables — a 1-D lookup (axis + value + live count) per declaration.
                const jf::JJson &tbls = e["tables"];
                for (const auto &[teKey, teVal] : tbls.obj()) {
                    const jf::JJson &to = teVal;
                    ElemTable t;
                    t.name      = teKey;
                    t.display   = to["display"].str();
                    t.label     = strOr(to["label"], teKey);
                    // Two meta encodings, ONE table. Both fill the same cell-grid + axes form below.
                    if (to["std"].boolean()) {             // multi-axis encoding: cell + axes{x[,y[,z]]}
                        const jf::JJson &cell = to["cell"];
                        t.minV      = cell["min"].number();
                        t.maxV      = cell["max"].number();
                        t.cellRel = cell["rel_offset"].number<int>();
                        t.cellType = cell["datatype"].str();
                        t.cellScale = cell["scale"].number(1.0);
                        const jf::JJson &axes = to["axes"];
                        auto rdAxis = [](const jf::JJson &a) {
                            ElemAxis e;
                            e.rel = a["rel_offset"].number<int>();
                            e.nRel = a["n_rel"].number<int>();
                            e.max = a["max"].number<int>();
                            e.min = a.contains("min") ? a["min"].number<int>() : 2;
                            e.datatype = a["datatype"].str();
                            e.units = a["units"].str();
                            e.label = a["label"].str();
                            e.scale = a["scale"].number(1.0);
                            e.srcRel = a.contains("src_rel") ? a["src_rel"].number<int>() : -1;
                            e.enRel  = a.contains("en_rel")  ? a["en_rel"].number<int>()  : -1;
                            if (a.contains("value_min") && a.contains("value_max")) {
                                e.vmin = a["value_min"].number();
                                e.vmax = a["value_max"].number();
                                e.hasBounds = true;
                            }
                            return e;
                        };
                        t.xAxis = rdAxis(axes["x"]);
                        if (axes.contains("y")) { t.yAxis = rdAxis(axes["y"]); t.hasY = true; }
                        if (axes.contains("z")) { t.zAxis = rdAxis(axes["z"]); t.hasZ = true; }
                    } else {                               // single-axis encoding: n + axis + value → ONE axis + cells
                        const jf::JJson &ax = to["axis"];
                        const jf::JJson &vl = to["value"];
                        t.cellRel   = vl["rel_offset"].number<int>();
                        t.cellType  = vl["datatype"].str();
                        t.cellScale = vl["scale"].number(1.0);
                        t.minV      = vl["min"].number();
                        t.maxV      = vl["max"].number();
                        t.typeScaled = vl["type_scaled"].boolean();
                        for (const jf::JJson &sv : vl["element_scales"].arr()) t.cellElemScales.push_back(sv.number());
                        for (const jf::JJson &uv : vl["element_units"].arr())  t.cellElemUnits.push_back(uv.str());
                        t.xAxis.rel      = ax["rel_offset"].number<int>();
                        t.xAxis.datatype = ax["datatype"].str();
                        t.xAxis.scale    = ax["scale"].number(1.0);
                        t.xAxis.units    = ax["units"].str();
                        for (const jf::JJson &au : ax["element_units"].arr())
                            t.xAxis.elemUnits.push_back(au.str());
                        t.xAxis.label    = ax["label"].str();
                        t.xAxis.nRel     = to["n"]["rel_offset"].number<int>();
                        t.xAxis.max      = to["points"].number<int>();
                        t.xAxis.min      = 2;
                    }
                    ca.tables.push_back(t);
                }
                // THE AXIS SCALARS AN ELEMENT TABLE OWNS, made fields of the element like every other.
                //
                // A module-level table's axis channel is a declared leaf — "idle.base_duty_table_x_src" —
                // and binds like any other field. The SAME table inside an array element carried its
                // channel, its optional-Y switch and its bin count only as rel offsets inside the table
                // descriptor, because that is all the grid editor needs. Anything that binds one by NAME
                // then found nothing: "outputs.output[3].duty_table_x_src" resolved to no field at all,
                // so the Duty Map page's X Channel dropdown had no options, read 0 and wrote nowhere —
                // the same silent no-op as a page bound to a field that does not exist.
                //
                // Named and typed exactly as codegen emits the struct members (int16 src, uint8 en,
                // uint8 count), so the page, the dictionary and the tune all see one kind of field.
                for (const ElemTable &t : ca.tables) {
                    const std::pair<const ElemAxis *, const char *> axes[3] = {
                        { &t.xAxis, "x" },
                        { t.hasY ? &t.yAxis : nullptr, "y" },
                        { t.hasZ ? &t.zAxis : nullptr, "z" } };
                    for (const auto &[axp, ax] : axes) {
                        if (!axp) continue;
                        const std::string base = t.name + "_" + ax;
                        const std::string what = (t.label.empty() ? t.name : t.label) + " " +
                                                 std::string(1, char(std::toupper(ax[0]))) + " Axis ";
                        auto add = [&ca](const std::string &nm, int rel, const char *dt, int sz,
                                         const std::string &lbl, const char *kind, double lo, double hi) {
                            for (const ArrayField &f : ca.fields) if (f.name == nm) return;  // declared wins
                            ArrayField f;
                            f.name = nm; f.relOffset = rel; f.datatype = dt; f.size = sz;
                            f.label = lbl; f.kind = kind; f.minV = lo; f.maxV = hi; f.digits = 0;
                            ca.fields.push_back(f);
                        };
                        if (axp->srcRel >= 0)
                            add(base + "_src", axp->srcRel, "S16", 2, what + "Channel", "signal", 0.0, 0.0);
                        if (axp->enRel >= 0)
                            add(base + "_en", axp->enRel, "U08", 1, what + "Enabled", "bool", 0.0, 1.0);
                        add(base + "_axis_n", axp->nRel, "U08", 1, what + "Bins", "",
                            double(std::max(1, axp->min)), double(axp->max));
                    }
                }
                // Element arrays — a fixed repeated sub-struct (precond/cand).
                const jf::JJson &narr = e["arrays"];
                for (const auto &[aeKey, aeVal] : narr.obj()) {
                    const jf::JJson &ao = aeVal;
                    ElemArray na;
                    na.name      = aeKey;
                    na.count     = ao["count"].number<int>();
                    na.relOffset = ao["rel_offset"].number<int>();
                    na.stride    = ao["stride"].number<int>();
                    const jf::JJson &sf = ao["fields"];
                    for (const auto &[seKey, seVal] : sf.obj()) {
                        const jf::JJson &so = seVal;
                        ElemSubField f;
                        f.name        = seKey;
                        f.relOffset   = so["rel_offset"].number<int>();
                        f.size        = so["size"].number<int>();
                        f.datatype    = so["datatype"].str();
                        f.scale       = so["scale"].number(1.0);
                        f.label       = strOr(so["label"], seKey);
                        f.units       = so["units"].str();
                        f.help        = so["help"].str();
                        f.minV        = so["min"].number();
                        f.maxV        = so["max"].number();
                        f.optionsFrom = so["options_from"].str();
                        f.kind        = so["kind"].str();       // the control hint, same as any field
                        for (const jf::JJson &ov : so["options"].arr())
                            f.options.push_back(ov.str());
                        na.fields.push_back(f);
                    }
                    ca.arrays.push_back(na);
                }
                structArrays_[key] = ca;
            }
        }
    }

    // The table registry, in id order: what table(<name>) compiles to and what the builder offers.
    tableRegistry_.clear();
    for (const jf::JJson &v : root["table_registry"].arr())
        tableRegistry_.push_back({ v["name"].str(), strOr(v["label"], v["name"].str()) });

    const jf::JJson &luaApi = root["lua_api"];
    for (const jf::JJson &v : luaApi["functions"].arr()) {
        const jf::JJson &o = v;
        luaFunctions_.push_back({o["name"].str(), o["sig"].str(), o["doc"].str()});
    }
    for (const jf::JJson &v : luaApi["callbacks"].arr()) {
        const jf::JJson &o = v;
        luaCallbacks_.push_back({o["name"].str(), o["sig"].str(), o["doc"].str()});
    }

    // Which element publishes which channel — the join between a bus signal and the sensor behind it.
    // Built once here rather than scanned per query: the axis dialog asks this for every breakpoint it
    // formats, and a linear walk of 15 arrays x 121 elements per frame is not a lookup.
    signalOwner_.clear();
    for (const auto &[key, ca] : structArrays_)
        for (int i = 0; i < int(ca.elementSignals.size()); ++i)
            if (!ca.elementSignals[i].empty())
                signalOwner_.emplace(ca.elementSignals[i], std::make_pair(key, i));

    path_ = path;
    // A new definition: its own declarations, and none of the project's yet — the project re-applies
    // its side-car after a meta load (main.cpp g_applyProjectPcVars).
    // Only when it declares some: a meta may carry its own host segment, which a rebuild would drop.
    definitionPcVars_ = std::move(pendingPcVars_);
    pendingPcVars_.clear();
    projectPcVars_.clear();
    pcVars_.clear();                     // config_ is new: the last meta's paths are already gone
    if (!definitionPcVars_.empty()) rebuildPcVars();
    valid_ = true;
    JLOGC("model.meta", jf::JLogLevel::Info) << "loaded " << path << ": layout=" << layoutHash()
        << " config=" << configSize() << "B telem=" << telemetrySize() << "B  |  "
        << config_.size() << " scalars, " << tables_.size() << " tables, " << structArrays_.size()
        << " arrays, " << telem_.size() << " telem channels, " << signals_.size() << " signals, "
        << segments_.size() << " segment(s), " << luaFunctions_.size() << " lua fns";
    for (const Segment &s : segments_)
        JLOGC("model.meta", jf::JLogLevel::Debug) << "  segment '" << s.id << "' (" << s.kind << ") base="
            << static_cast<unsigned>(s.base) << " size=" << s.size << "B writable=" << s.writable;
    return true;
}

char MetaModel::cmd(const std::string &name) const
{
    const std::string code = protocol_["commands"][name]["code"].str();
    return code.empty() ? char(0) : code[0];
}

void MetaModel::applyPcVars(const std::vector<PcVar> &vars)
{
    projectPcVars_.clear();
    for (const PcVar &v : vars)
        if (!isDefinitionPcVar(v.name)) projectPcVars_.push_back(v);
    rebuildPcVars();
}

void MetaModel::rebuildPcVars()
{
    locCache_.clear();          // host variables ARE config fields: adding one changes what resolves

    // Drop any previous declaration set first: this is a REPLACE, so removing a variable in the editor
    // actually removes its path rather than leaving a stale one resolvable.
    for (const PcVar &old : pcVars_)
        config_.erase("pc." + old.name);
    segments_.erase(std::remove_if(segments_.begin(), segments_.end(),
                                   [](const Segment &s) { return s.kind == "host"; }),
                    segments_.end());
    pcVars_ = definitionPcVars_;
    pcVars_.insert(pcVars_.end(), projectPcVars_.begin(), projectPcVars_.end());
    if (pcVars_.empty())
        return;

    int offset = 0;
    for (const PcVar &v : pcVars_) {
        ConfigField cf;
        cf.datatype = v.datatype.empty() ? std::string("F32") : v.datatype;
        cf.size     = dataSize(cf.datatype);
        cf.offset   = int(kPcSegmentBase) + offset;
        cf.scale    = v.scale != 0.0 ? v.scale : 1.0;
        cf.minV     = v.minV;
        cf.maxV     = v.maxV;
        cf.units    = v.units;
        cf.label    = v.label.empty() ? v.name : v.label;
        cf.help     = v.help;
        cf.kind     = v.kind;
        cf.options  = v.options;
        config_["pc." + v.name] = cf;
        offset += cf.size;
    }

    Segment seg;
    seg.id       = "pc";
    seg.kind     = "host";        // Cache: allocate locally, never read from or write to the ECU
    seg.base     = kPcSegmentBase;
    seg.size     = offset;
    seg.writable = true;
    segments_.push_back(seg);
}

bool MetaModel::signalRange(const std::string &channel, double &lo, double &hi) const
{
    const auto it = telem_.find(channel);
    if (it == telem_.end() || it->second.maxV <= it->second.minV)
        return false;
    lo = it->second.minV;
    hi = it->second.maxV;
    return true;
}

bool MetaModel::sensorForSignal(const std::string &channel, std::string &arrayKey, int &index) const
{
    const auto it = signalOwner_.find(channel);
    if (it == signalOwner_.end())
        return false;
    arrayKey = it->second.first;
    index    = it->second.second;
    return true;
}

double MetaModel::decodeRaw(const std::string &datatype, const unsigned char *p, bool be)
{
    const auto rd16 = [be](const unsigned char *q) { return be ? beU16(q) : leU16(q); };
    const auto rd32 = [be](const unsigned char *q) { return be ? beU32(q) : leU32(q); };
    if (datatype == "U08") return *p;
    if (datatype == "S08") return static_cast<int8_t>(*p);
    if (datatype == "U16") return rd16(p);
    if (datatype == "S16") return static_cast<int16_t>(rd16(p));
    if (datatype == "U32") return rd32(p);
    if (datatype == "S32") return static_cast<int32_t>(rd32(p));
    if (datatype == "F32") {
        uint32_t bits = rd32(p);
        float v;
        memcpy(&v, &bits, sizeof(v));
        return v;
    }
    return 0.0;
}

int MetaModel::dataSize(const std::string &datatype)
{
    if (datatype == "U08" || datatype == "S08") return 1;
    if (datatype == "U16" || datatype == "S16") return 2;
    return 4;   // U32/S32/F32
}

const MetaModel::ConfigArray *MetaModel::elementOf(const std::string &arrayKey, const std::string &elemKey,
                                                   int &index) const
{
    const auto it = structArrays_.find(arrayKey);
    if (it == structArrays_.end())
        return nullptr;
    bool num = false;
    const int n = parseIntStrict(elemKey, num);
    index = num ? n : strIndexOf(it->second.elementIds, elemKey);   // numeric index OR catalog id ("clt")
    if (index < 0 || index >= it->second.count)
        return nullptr;
    return &it->second;
}

const MetaModel::ElemTable *MetaModel::elementTable(const std::string &path, const ConfigArray *&arrOut,
                                                    int &index) const
{
    // "module.array[key].table" — key = numeric index or catalog id; array = up to the first '['
    std::string array, key, rest;
    if (!splitElem(path, array, key, rest) || rest.empty() || rest[0] != '.' || !isIdent(rest.substr(1)))
        return nullptr;
    const ConfigArray *ca = elementOf(array, key, index);
    if (!ca)
        return nullptr;
    const std::string tname = rest.substr(1);
    for (const ElemTable &t : ca->tables)
        if (t.name == tname) {
            arrOut = ca;
            return &t;
        }
    return nullptr;
}

const MetaModel::ArrayField *MetaModel::elementField(const std::string &path, const ConfigArray *&arrOut,
                                                     int &index) const
{
    std::string array, key, rest;
    if (!splitElem(path, array, key, rest) || rest.empty() || rest[0] != '.' || !isIdent(rest.substr(1)))
        return nullptr;
    const ConfigArray *ca = elementOf(array, key, index);
    if (!ca)
        return nullptr;
    const std::string fname = rest.substr(1);
    for (const ArrayField &f : ca->fields)
        if (f.name == fname) {
            arrOut = ca;
            return &f;
        }
    return nullptr;
}

std::string MetaModel::helpFor(const std::string &path) const
{
    if (const auto it = config_.find(path); it != config_.end())   // module.field scalar
        return it->second.help;
    if (const auto it = tables_.find(path); it != tables_.end())   // module.table
        return it->second.help;
    // module.array[i].field -> the element field's help (e.g. electronic_throttle.etb[0].at_relay_pct)
    std::string array, key, rest;
    if (splitElem(path, array, key, rest) && !rest.empty() && rest[0] == '.' && isIdent(rest.substr(1))) {
        int idx = 0;
        if (const ConfigArray *ca = elementOf(array, key, idx)) {
            const std::string fname = rest.substr(1);
            for (const ArrayField &f : ca->fields)
                if (f.name == fname)
                    return f.help;
            for (const ElemTable &t : ca->tables)          // a per-element table (its own label as help)
                if (t.name == fname)
                    return t.label;
        }
    }
    // The shapes that had no help at all, so the widgets bound to them hovered blank: a nested sub-struct
    // field (a precondition's operator) and a named bit group (one diagnostics check). Both describe
    // themselves in the meta; nothing was asking them.
    if (const ElemSubField *sf = subFieldOf(path)) return sf->help;
    if (const BitGroup *g = bitGroupOf(path))      return g->help;
    // A whole run takes its field's help: hovering the strip and hovering one of its cells should not
    // answer differently, since they describe the same values.
    // Reuse the element parse already done above (array/key/rest) — a run differs from one member only
    // in carrying "[]" where the member carries an index, so asking for member 0's help answers both.
    if (!array.empty()) {
        const size_t br = rest.find("[]");
        if (br != std::string::npos && br + 3 < rest.size() && rest[br + 2] == '.') {
            std::string one = rest;
            one.replace(br, 2, "[0]");
            if (const ElemSubField *sf2 = subFieldOf(array + "[" + key + "]" + one)) return sf2->help;
        }
    }
    return {};
}

// THE table path: any table — a singular module map OR one instance of an in-array table — resolves to the
// same offset descriptor. The two source forms (module ConfigTable vs in-array ElemTable) differ only in
// how the offsets are computed (a fixed offset vs element base + i*stride + rel); past this function the
// rest of the studio sees one structure (TableImage) and one path.
TableImage MetaModel::resolveTable(const std::string &path) const
{
    TableImage ti;

    // Module table ("module.table") — axis fields are named config scalars / 1D arrays -> their offsets.
    if (tables_.count(path)) {
        const ConfigTable t = tables_.at(path);
        ti.cellBase = t.offset; ti.cellType = t.datatype; ti.cellSize = t.cellSize; ti.cellScale = t.scale;
        ti.cellMinV = t.minV; ti.cellMaxV = t.maxV;
        ti.applyTo = t.applyTo;
        ti.applyAdd = t.applyAdd;
        ti.rowLabels = t.rowLabels;
        ti.colLabels = t.colLabels;
        ti.label = t.label;
        // THE PRECISION THE SCHEMA DECLARES. Only a sensor calibration used to carry one (the Cache fills
        // that from the sensor's type), so every other table fell through to the widget's own default of
        // no decimals — and the target-lambda map, whose whole range is 0.72 to 1.00, showed a grid of 1s
        // with a heat map underneath that was plainly not flat. The number was right; only the rendering
        // had lost three digits.
        ti.cellDigits = t.digits;
        ti.cellUnits  = t.units;
        for (const Axis &ax : t.axes) {
            TableImage::Axis a;
            const ConfigField arr = mapValueOr(arrays1d_, ax.array);
            a.breaksBase = arr.offset; a.breakType = arr.datatype;
            a.breakSize = dataSize(arr.datatype); a.breakScale = arr.scale;
            a.digits = arr.digits >= 0 ? arr.digits : 0;
            a.units = arr.units; a.label = ax.channel; a.defaultSig = ax.channel;
            a.nMax = ax.nMax; a.optional = ax.optional;
            if (!ax.nScalar.empty()) {
                a.nBase = mapValueOr(config_, ax.nScalar).offset;
                a.nMin  = std::max(1, int(mapValueOr(config_, ax.nScalar).minV));
            }
            if (!ax.srcScalar.empty()) a.srcBase = mapValueOr(config_, ax.srcScalar).offset;
            if (!ax.enScalar.empty())  a.enBase  = mapValueOr(config_, ax.enScalar).offset;
            ti.axes.push_back(a);
        }
        ti.valid = !ti.axes.empty();
        return ti;
    }

    // In-array table ("module.array[i].table") — axis fields are rel offsets within the element. ONE path:
    // a curve (1 axis) and a grid (2-3 axes) are the same table, differing only in how many axes they wear.
    const ConfigArray *arr = nullptr; int idx = 0;
    if (const ElemTable *et = elementTable(path, arr, idx); et && arr) {
        const int elemBase = arr->baseOffset + idx * arr->stride;
        ti.cellBase = elemBase + et->cellRel; ti.cellType = et->cellType;
        ti.cellSize = dataSize(et->cellType);
        // Type-scaled cells: this element's cell scale comes from its slot in the per-element vector.
        ti.cellScale = (et->typeScaled && idx >= 0 && idx < int(et->cellElemScales.size()))
                           ? et->cellElemScales[idx] : et->cellScale;
        ti.cellMinV = et->minV; ti.cellMaxV = et->maxV;
        ti.label = et->label.empty() ? et->name : et->label;
        // Which element this table belongs to, and whether its cells are the element's TYPE speaking
        // rather than the table's own schema. Cache turns that into real units/precision/bounds; the
        // meta cannot, because a generic input's type lives in the tune.
        ti.cellTypeScaled = et->typeScaled;
        ti.elemArray = arr->module.empty() ? arr->name : (arr->module + "." + arr->name);
        ti.elemIndex = idx;
        auto addAxis = [&](const ElemAxis &ax) {
            TableImage::Axis a;
            a.breaksBase = elemBase + ax.rel; a.breakType = ax.datatype;
            a.breakSize = dataSize(ax.datatype); a.breakScale = ax.scale;
            a.nBase = elemBase + ax.nRel; a.nMax = ax.max; a.nMin = std::max(1, ax.min);
            a.srcBase = ax.srcRel < 0 ? -1 : elemBase + ax.srcRel;
            a.enBase  = ax.enRel  < 0 ? -1 : elemBase + ax.enRel;
            a.optional = ax.enRel >= 0;
            // PER-ELEMENT RAW UNITS, read at the same index the cell's scale and units are. A sensor
            // cal's axis is ADC counts for an analog input and HERTZ for a frequency one, and an axis
            // labelled ADC over hertz breakpoints misnames the only number being entered.
            a.units = (idx >= 0 && idx < static_cast<int>(ax.elemUnits.size()) && !ax.elemUnits[idx].empty())
                          ? ax.elemUnits[idx] : ax.units;
            a.label = ax.label;
            a.vmin = ax.vmin; a.vmax = ax.vmax; a.hasBounds = ax.hasBounds;
            ti.axes.push_back(a);
        };
        addAxis(et->xAxis);
        if (et->hasY) addAxis(et->yAxis);
        if (et->hasZ) addAxis(et->zAxis);
        ti.valid = true;
        return ti;
    }
    return ti;
}

std::vector<std::string> MetaModel::tableInstancePaths() const
{
    std::vector<std::string> out;
    for (const auto &[k, v] : tables_)
        out.push_back(k);                                  // each singular module map once
    for (const auto &[k, arr] : structArrays_) {
        for (const ElemTable &et : arr.tables)
            for (int i = 0; i < arr.count; ++i)            // the same table, once per array element (curve or grid)
                out.push_back(k + "[" + std::to_string(i) + "]." + et.name);
    }
    return out;
}

// Byte span of a blob field — an expression program or an ASCII string. Blobs are addressed as
// BYTES, not decoded as a value: a spin box over byte 0 of a program is exactly the confusion the
// explicit `expression` type exists to prevent.
bool MetaModel::resolveBlob(const std::string &path, int &offset, int &size) const
{
    const auto sf = config_.find(path);
    if (sf != config_.end()) { offset = sf->second.offset; size = sf->second.size; return size > 0; }

    std::string array, key, rest;
    if (!splitElem(path, array, key, rest) || rest.size() < 2 || rest[0] != '.') return false;
    int idx = 0;
    const ConfigArray *ca = elementOf(array, key, idx);
    if (!ca) return false;
    const std::string field = rest.substr(1);
    for (const ArrayField &f : ca->fields) {
        if (f.name != field) continue;
        offset = ca->baseOffset + idx * ca->stride + f.relOffset;
        size   = f.size;
        return size > 0;
    }
    return false;
}



bool MetaModel::resolveRegion(const std::string &path, int &offset, int &size) const
{
    // "module.array[index]" — one whole element (covers every field + per-element table in the struct).
    std::string array, key, rest;
    if (splitElem(path, array, key, rest) && rest.empty()) {
        int idx = 0;
        if (const ConfigArray *ca = elementOf(array, key, idx)) {
            offset = ca->baseOffset + idx * ca->stride;
            size   = ca->stride;
            return true;
        }
        return false;
    }
    // "module.array" — the whole array (all elements back-to-back).
    const auto it = structArrays_.find(path);
    if (it != structArrays_.end()) {
        offset = it->second.baseOffset;
        size   = it->second.count * it->second.stride;
        return true;
    }
    return false;
}

std::vector<MetaModel::SignalSelector> MetaModel::signalSelectorFields() const
{
    std::vector<SignalSelector> out;
    // Flat config scalars of kind "signal" — or "sensor", the same control restricted to channels a
    // sensor produces. Both are selectors, and a selector missing from this index is a field the studio
    // will not offer a picker for at all.
    for (const auto &[pathKey, cf] : config_)
        if (cf.kind == "signal" || cf.kind == "sensor") {
            const std::string group = sectionFirst(pathKey);
            const std::string leaf  = cf.label.empty() ? sectionFrom1(pathKey) : cf.label;
            out.push_back({ pathKey, group, leaf });
        }
    // Array-of-structs element fields of kind "signal", expanded per element (e.g. output[3].source);
    // grouped under the array's label, leaf names the element + field.
    for (const auto &[k, ca] : structArrays_) {
        const std::string group = ca.label.empty() ? ca.name : ca.label;
        for (const ArrayField &f : ca.fields) {
            if (f.kind != "signal" && f.kind != "sensor")
                continue;
            for (int i = 0; i < ca.count; ++i) {
                // Path key prefers the stable element id ("clt"), else the numeric index — both resolve.
                const std::string key = (i < int(ca.elementIds.size()) && !ca.elementIds[i].empty())
                                            ? ca.elementIds[i] : std::to_string(i);
                const std::string path = ca.module + "." + ca.name + "[" + key + "]." + f.name;
                const std::string elem = (i < int(ca.elementLabels.size()) && !ca.elementLabels[i].empty())
                                             ? ca.elementLabels[i] : ("[" + std::to_string(i) + "]");
                out.push_back({ path, group, elem + " / " + (f.label.empty() ? f.name : f.label) });
            }
        }
    }
    return out;
}

// A STRIP — see the declaration. Accepts either a 1-D array by name, or "array[key].sub[].field" where
// the empty brackets mean the whole run rather than one member.
// THE path grammar. One parse, one walk — see the declaration for why there used to be thirteen.
//
// A path is dotted names, any of which may carry a subscript:
//
//   module.field                       a scalar
//   module.field.group                 a named bit group inside that scalar's storage word
//   module.axis                        a 1-D array, addressed whole (a "run")
//   module.axis[3]                     one element of it
//   module.array[3].field              a struct-array element's field  (key: index OR catalog id)
//   module.array[clt].field            the same element by its stable id
//   module.array[3].sub[7].field       a field of one entry of a nested repeated sub-struct
//   module.array[3].sub[].field        that field across the whole sub-array — a run
//   module.table                       a table's cell grid
//
// The forms are tried cheapest first (exact map lookups, then subscripted shapes), and every branch
// ends by filling the SAME Location — including its bounds, which is the property that makes a
// location without limits unconstructable.
// MEMOISED — see clearLocateCache(). What a path resolves to depends on the META and nothing else, so
// the answer is stable for as long as one is loaded, and this is the call every config read, unit,
// decimal and bound sits on top of: ~5 microseconds unmemoised, a hash lookup memoised.
//
// A WRAPPER, not a scope guard inside the resolver. The first attempt filled the cache from a destructor
// running at every `return L;` — and `return L` is move-eligible, so the destructor saw a moved-from
// Location and cached one with its strings emptied. Every second lookup of a path then reported no
// datatype: reads silently returned 0, and the test suite caught it in four places at once.
MetaModel::Location MetaModel::locate(const std::string &path) const
{
    if (path.empty())
        return {};
    if (const auto hit = locCache_.find(path); hit != locCache_.end())
        return hit->second;
    Location L = locateUncached(path);
    // Bounded like the evaluator's parse cache. The paths a session resolves are finite (they come from
    // bindings), but a cap turns "unbounded if something generates paths" from a leak into a re-fill.
    if (locCache_.size() > 20000) locCache_.clear();
    locCache_.emplace(path, L);
    return L;
}

MetaModel::Location MetaModel::locateUncached(const std::string &path) const
{
    Location L;
    if (path.empty())
        return L;

    // 1 — a plain scalar.
    if (const auto it = config_.find(path); it != config_.end()) {
        const ConfigField &f = it->second;
        L.kind = Location::Kind::Scalar;
        L.offset = f.offset; L.datatype = f.datatype; L.scale = f.scale;
        L.minV = f.minV; L.maxV = f.maxV; L.bits = f.bits;
        L.units = f.units; L.label = f.label; L.help = f.help; L.digits = f.digits;
        L.valueKind = f.kind; L.options = f.options;
        return L;
    }

    // 2 — a 1-D array addressed whole. Its elements are adjacent, so the stride IS the element size.
    if (const auto it = arrays1d_.find(path); it != arrays1d_.end()) {
        const ConfigField &a = it->second;
        const int esz = dataSize(a.datatype);
        if (esz > 0 && a.count > 0) {
            L.kind = Location::Kind::Run;
            L.offset = a.offset; L.datatype = a.datatype; L.scale = a.scale;
            L.minV = a.minV; L.maxV = a.maxV; L.count = a.count; L.stride = esz;
            L.units = a.units; L.label = a.label; L.help = a.help; L.digits = a.digits;
        }
        return L;
    }

    // 3 — a table's cell grid. Cell addresses are a pure function of this plus the live bin counts,
    //     which belong to the config image rather than the meta, so they are resolved by the caller.
    if (const auto it = tables_.find(path); it != tables_.end()) {
        const ConfigTable &t = it->second;
        L.kind = Location::Kind::Table;
        L.offset = t.offset; L.datatype = t.datatype; L.scale = t.scale;
        L.minV = t.minV; L.maxV = t.maxV; L.stride = t.cellSize;
        L.count = t.cols * (t.rows > 0 ? t.rows : 1) * (t.depth > 0 ? t.depth : 1);
        L.units = t.units; L.label = t.label; L.help = t.help; L.digits = t.digits;
        return L;
    }

    // 4 — a named bit group: the storage word is the parent field, the addressable thing is the range.
    //
    // The parent is resolved by ASKING FOR IT, not by looking it up in the flat scalar map. bitGroupOf()
    // has always found groups on an array element's field as well as on a flat one, but this looked the
    // parent up in config_ — flat scalars only — so every group inside an array element found its bits
    // and then failed to find anywhere to put them. 1586 of them: every sensor's diagnostics flags, every
    // output's, every trigger stream's. They are offered by the dictionary and bind a real control, so
    // what you got was a checkbox that read 0 for ever and wrote nowhere.
    if (const BitGroup *g = bitGroupOf(path)) {
        const size_t dot = path.rfind('.');
        if (dot != std::string::npos) {
            const Location P = locate(path.substr(0, dot));      // flat scalar OR array element field
            if (P.valid() && P.kind == Location::Kind::Scalar) {
                L.kind = Location::Kind::Scalar;
                L.offset = P.offset; L.datatype = P.datatype; L.scale = 1.0;   // a bit range is unscaled
                L.bits = BitRange{ g->lo, g->hi };
                L.minV = 0.0;
                L.maxV = static_cast<double>((1u << g->width()) - 1u);         // what the range can hold
                L.label = g->label; L.help = g->help; L.digits = 0;   // a flag or a level is a whole number
                return L;
            }
        }
        return L;
    }

    // 5 — one element of a 1-D array: "module.axis[3]".
    {
        std::string base, key, rest;
        if (splitElem(path, base, key, rest) && rest.empty()) {
            if (const auto it = arrays1d_.find(base); it != arrays1d_.end()) {
                const ConfigField &a = it->second;
                const int esz = dataSize(a.datatype);
                bool num = false;
                const int i = parseIntStrict(key, num);
                if (num && esz > 0 && i >= 0 && i < a.count) {
                    L.kind = Location::Kind::Scalar;
                    L.offset = a.offset + i * esz; L.datatype = a.datatype; L.scale = a.scale;
                    L.minV = a.minV; L.maxV = a.maxV;
                    L.units = a.units; L.label = a.label; L.help = a.help; L.digits = a.digits;
                }
                return L;
            }
        }
    }

    // 6 — inside a struct array: "module.array[key]." then a field, a sub-array entry, or a sub-run.
    {
        std::string array, key, rest;
        if (!splitElem(path, array, key, rest) || rest.size() < 2 || rest[0] != '.')
            return L;
        int idx = 0;
        const ConfigArray *cap = elementOf(array, key, idx);
        if (!cap)
            return L;
        const ConfigArray &ca = *cap;
        const int elemBase = ca.baseOffset + idx * ca.stride;
        const std::string tail = rest.substr(1);

        // 6a — the whole sub-array for one field: "sub[].field". The stride is the SUB-STRUCT's, not the
        //      field's: a run of {v, tag} pairs steps by the pair while pointing at one member of it.
        if (const size_t lb = tail.find("[]");
            lb != std::string::npos && lb > 0 && tail.size() >= lb + 4 && tail[lb + 2] == '.') {
            const std::string subName = tail.substr(0, lb), fieldName = tail.substr(lb + 3);
            if (isWord(subName) && isWord(fieldName))
                for (const ElemArray &na : ca.arrays)
                    if (na.name == subName)
                        for (const ElemSubField &f : na.fields)
                            if (f.name == fieldName && na.count > 0 && na.stride > 0 && !f.datatype.empty()) {
                                L.kind = Location::Kind::Run;
                                L.offset = elemBase + na.relOffset + f.relOffset;
                                L.datatype = f.datatype; L.scale = f.scale;
                                L.minV = f.minV; L.maxV = f.maxV;
                                L.count = na.count; L.stride = na.stride;
                                L.units = f.units; L.label = f.label; L.help = f.help;
                                return L;
                            }
            return L;
        }

        // 6b — one entry of a nested sub-struct: "sub[7].field".
        std::string sName, sIdx, sField;
        if (parseSub(tail, sName, sIdx, sField)) {
            const int j = std::stoi(sIdx);
            for (const ElemArray &na : ca.arrays)
                if (na.name == sName && j >= 0 && j < na.count)
                    for (const ElemSubField &f : na.fields)
                        if (f.name == sField) {
                            L.kind = Location::Kind::Scalar;
                            L.offset = elemBase + na.relOffset + j * na.stride + f.relOffset;
                            L.datatype = f.datatype; L.scale = f.scale;
                            L.minV = f.minV; L.maxV = f.maxV;
                            L.units = f.units; L.label = f.label; L.help = f.help;
                            return L;
                        }
            return L;
        }

        // 6c — a table owned by this element: "sensors.sensor[clt].cal", each sensor's own calibration
        //      curve. A per-element table is a table — same cell grid, same addressing — it just sits at
        //      a relative offset inside the element rather than at a module-level one. Its cell scale can
        //      be per-element too (a type-scaled sensor cal takes the scale of the type in that slot),
        //      which is the one thing a module table never has to say.
        for (const ElemTable &et : ca.tables)
            if (et.name == tail) {
                L.kind = Location::Kind::Table;
                L.offset = elemBase + et.cellRel;
                L.datatype = et.cellType;
                L.scale = (et.typeScaled && idx >= 0 && idx < static_cast<int>(et.cellElemScales.size()))
                              ? et.cellElemScales[idx] : et.cellScale;
                L.minV = et.minV; L.maxV = et.maxV;
                L.stride = dataSize(et.cellType);
                L.label = et.label.empty() ? et.name : et.label;
                // A type-scaled cal takes its UNITS from the type occupying this slot, exactly as it takes
                // its scale — the same per-element vector, read at the same index.
                L.units = (et.typeScaled && idx >= 0 && idx < static_cast<int>(et.cellElemUnits.size()))
                              ? et.cellElemUnits[idx] : std::string{};
                return L;
            }

        // 6d — a plain field of the element.
        for (const ArrayField &f : ca.fields)
            if (f.name == tail) {
                L.kind = Location::Kind::Scalar;
                L.offset = elemBase + f.relOffset;
                L.datatype = f.datatype; L.scale = f.scale;
                // THE BYTE LENGTH TRAVELS TOO. Meaningless for a number (its datatype says how wide it
                // is) and essential for a blob: an ASCII field is only writable if you know how many
                // NUL-padded bytes it owns, and without this the tune had no way to put a name back.
                L.size = f.size;
                L.minV = f.minV; L.maxV = f.maxV;
                L.units = f.units; L.label = f.label; L.help = f.help; L.digits = f.digits;
                L.valueKind = f.kind; L.options = f.options; L.pickerSets = f.pickerSets;
                return L;
            }
    }
    return L;
}


// One field of an element's NESTED sub-struct array — "sensors.sensor[2].precond[0].op". resolveArrayField
// already walks this shape for offsets, but the DESCRIPTIVE lookups (kind, options, label) stopped at one
// array level and reported nothing. Everything downstream then treated a precondition's operator, combine
// and signal as plain numbers: the dictionary dropped each of them as a Config Edit spin box instead of the
// operator list, the AND/OR list and the signal picker they are.
const MetaModel::ElemSubField *MetaModel::subFieldOf(const std::string &path) const
{
    std::string array, key, rest;
    if (!splitElem(path, array, key, rest) || rest.empty() || rest[0] != '.' || rest.size() < 2)
        return nullptr;
    int idx = 0;
    const ConfigArray *cap = elementOf(array, key, idx);
    if (!cap)
        return nullptr;
    std::string sName, sIdx, sField;
    if (!parseSub(rest.substr(1), sName, sIdx, sField))
        return nullptr;
    const int j = std::stoi(sIdx);
    for (const ElemArray &na : cap->arrays)
        if (na.name == sName && j >= 0 && j < na.count)
            for (const ElemSubField &f : na.fields)
                if (f.name == sField)
                    return &f;
    return nullptr;
}

// Named bit groups on a field ("bits": { raw_min: {bitLo, bitHi, kind, label, options}, ... }).

// "<field path>.<group>" — e.g. "sensors.sensor[clt].diag_enable.raw_min". Split the last segment and ask
// the field it names for that group. This is what makes a single check bindable: the storage byte holds
// six of them, and editing the byte as a number writes all six at once.
const MetaModel::BitGroup *MetaModel::bitGroupOf(const std::string &path) const
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size())
        return nullptr;
    const std::string parent = path.substr(0, dot), group = path.substr(dot + 1);
    auto pick = [&group](const std::vector<BitGroup> &gs) -> const BitGroup * {
        for (const BitGroup &g : gs) if (g.name == group) return &g;
        return nullptr;
    };
    if (const auto it = config_.find(parent); it != config_.end())
        if (const BitGroup *g = pick(it->second.bitGroups)) return g;
    std::string array, key, rest;
    if (splitElem(parent, array, key, rest) && !rest.empty() && rest[0] == '.' && rest.size() >= 2) {
        int idx = 0;
        if (const ConfigArray *cap = elementOf(array, key, idx)) {
            const std::string field = rest.substr(1);
            for (const ArrayField &f : cap->fields)
                if (f.name == field)
                    if (const BitGroup *g = pick(f.bits)) return g;
        }
    }
    return nullptr;
}

std::string MetaModel::fieldKind(const std::string &path) const
{
    if (config_.count(path))
        return config_.at(path).kind;
    // array element: "module.array[key].field" -> its ArrayField's kind
    std::string array, key, rest;
    if (!splitElem(path, array, key, rest) || rest.empty() || rest[0] != '.' || !isWord(rest.substr(1)))
        return {};
    int idx = 0;
    const ConfigArray *cap = elementOf(array, key, idx);
    if (!cap)
        return {};
    const std::string field = rest.substr(1);
    for (const ArrayField &f : cap->fields)
        if (f.name == field)
            return f.kind;
    return {};
}

// The nested variant, keyed off options_from: "signals" is the signal picker; anything else with a named
// option list is an enum. (A sub-field carries no explicit `kind` in the meta — its list IS its kind.)
std::string MetaModel::subFieldKind(const std::string &path) const
{
    if (const BitGroup *g = bitGroupOf(path)) return g->kind;   // "bool" (a flag) / "enum" (a level)
    const ElemSubField *f = subFieldOf(path);
    if (!f) return {};
    if (!f->kind.empty())                                           return f->kind;   // declared
    if (f->optionsFrom == "signals" || f->optionsFrom == "sensors") return "signal";
    if (!f->options.empty() || !f->optionsFrom.empty())             return "enum";
    return {};
}

std::string MetaModel::labelFor(const std::string &path) const
{
    if (auto it = config_.find(path); it != config_.end())     return it->second.label;
    if (auto it = telem_.find(path);  it != telem_.end())      return it->second.label;
    if (auto it = arrays1d_.find(path); it != arrays1d_.end()) return it->second.label;
    // array-of-structs field: "module.array[key].field"
    std::string array, key, rest;
    if (splitElem(path, array, key, rest) && !rest.empty() && rest[0] == '.' && isIdent(rest.substr(1))) {
        int idx = 0;
        if (const ConfigArray *cap = elementOf(array, key, idx)) {
            const std::string field = rest.substr(1);
            for (const ArrayField &f : cap->fields)
                if (f.name == field)
                    return f.label;
        }
    }
    if (const ElemSubField *sf = subFieldOf(path)) return sf->label;
    if (const BitGroup *g = bitGroupOf(path))     return g->label;
    if (const Location L = locate(path); L.kind == Location::Kind::Run)   // a whole run is labelled by
        return L.label;                                                  // its field
    return {};
}

// The ids behind enumOptions(path), index-aligned, or empty where the definition ships none. A caller
// that has to RECOGNISE an option (rather than show it) joins on these — see ArrayField::optionIds.
std::vector<std::string> MetaModel::enumOptionIds(const std::string &path) const
{
    std::string array, key, rest;
    if (splitElem(path, array, key, rest) && !rest.empty() && rest[0] == '.' && rest.size() >= 2
        && structArrays_.count(array))
        for (const ArrayField &f : structArrays_.at(array).fields)
            if (f.name == rest.substr(1))
                return f.optionIds;
    return {};
}

std::string MetaModel::enumSetId(const std::string &path) const
{
    // Telemetry channels and struct-array fields carry the set id; a top-level config scalar does not
    // record one, so it answers "" — which callers must read as "unknown", never as "not that set".
    if (const auto t = telem_.find(path); t != telem_.end()) return t->second.enumId;
    std::string array, key, rest;
    if (splitElem(path, array, key, rest) && !rest.empty() && rest[0] == '.' && rest.size() >= 2
        && structArrays_.count(array))
        for (const ArrayField &f : structArrays_.at(array).fields)
            if (f.name == rest.substr(1)) return f.enumId;
    return {};
}

std::vector<std::string> MetaModel::allowedOptionIds(const std::string &path) const
{
    // Only the sensor INTERFACE enum is constrained today, and it is recognised by its set id rather
    // than by the field name: `interface` could be spelt on any array, and an option id is unique only
    // within its own set. Everything else answers "no constraint" and pays one string compare.
    if (enumSetId(path) != "sensor_interface") return {};
    std::string array, key, rest;
    if (!splitElem(path, array, key, rest)) return {};
    int idx = 0;
    const ConfigArray *ca = elementOf(array, key, idx);
    if (!ca || idx < 0 || idx >= int(ca->elementInterfaces.size())) return {};
    return ca->elementInterfaces[idx];
}

std::vector<int> MetaModel::allowedOptionIndices(const std::string &path) const
{
    std::string array, key, rest;
    if (!splitElem(path, array, key, rest) || rest.size() < 2 || rest[0] != '.') return {};
    int idx = 0;
    const ConfigArray *ca = elementOf(array, key, idx);
    if (!ca || idx < 0) return {};
    const auto it = ca->elementOptions.find(rest.substr(1));
    if (it == ca->elementOptions.end() || idx >= int(it->second.size())) return {};
    return it->second[idx];
}

std::vector<std::string> MetaModel::enumOptions(const std::string &path) const
{
    if (config_.count(path))                          // top-level scalar enum
        return config_.at(path).options;
    if (const auto t = telem_.find(path); t != telem_.end()) {
        if (!t->second.options.empty())
            return t->second.options;                 // an enum-valued CHANNEL names its values too
        // …or it NAMES A SET instead of listing labels. A channel whose states are shared vocabulary
        // (engine_state -> engine_run_state) carries the set id, not the words; without this it fell
        // straight through to "no options" and the gauge showed a digit, even though the meta was
        // carrying "Stopped/Cranking/Running" all along a few keys away.
        if (const auto e = enums_.find(t->second.enumId); e != enums_.end())
            return e->second;
    }
    // struct-array field: "module.array[key].field"
    std::string array, key, rest;
    if (splitElem(path, array, key, rest) && !rest.empty() && rest[0] == '.' && rest.size() >= 2
        && structArrays_.count(array))
        for (const ArrayField &f : structArrays_.at(array).fields)
            if (f.name == rest.substr(1))
                return f.options;
    if (const ElemSubField *sf = subFieldOf(path))    // "...precond[0].op" — its own option list
        return sf->options;
    if (const BitGroup *g = bitGroupOf(path))        // "...diag_severity.raw_min" — its level names
        return g->options;
    return {};
}

bool MetaModel::fieldPicker(const std::string &path, std::string &byField, std::vector<PickerSet> &sets) const
{
    std::string array, key, rest;
    if (!splitElem(path, array, key, rest) || rest.empty() || rest[0] != '.' || rest.size() < 2
        || !structArrays_.count(array))
        return false;
    for (const ArrayField &f : structArrays_.at(array).fields)
        if (f.name == rest.substr(1) && !f.pickerSets.empty()) {
            byField = f.pickerBy;
            sets    = f.pickerSets;
            return true;
        }
    return false;
}

std::vector<double> MetaModel::arrayValues(const std::string &path, const std::vector<uint8_t> &image) const
{
    std::vector<double> out;
    const auto it = arrays1d_.find(path);
    if (it == arrays1d_.end())
        return out;
    const ConfigField &a = it->second;
    const int cs = dataSize(a.datatype);
    const auto *base = reinterpret_cast<const unsigned char *>(image.data());
    for (int i = 0; i < a.count; ++i) {
        const int off = a.offset + i * cs;
        if (off + cs > int(image.size()))
            break;
        out.push_back(decodeRaw(a.datatype, base + off) * a.scale);
    }
    return out;
}

void MetaModel::encodeRaw(const std::string &datatype, unsigned char *p, double raw, bool be)
{
    const auto wr16 = [be](unsigned char *q, uint16_t v) { be ? wrBE16(q, v) : wrLE16(q, v); };
    const auto wr32 = [be](unsigned char *q, uint32_t v) { be ? wrBE32(q, v) : wrLE32(q, v); };
    if (datatype == "U08")      *p = static_cast<uint8_t>(std::lround(raw));
    else if (datatype == "S08") *reinterpret_cast<int8_t *>(p) = static_cast<int8_t>(std::lround(raw));
    else if (datatype == "U16") wr16(p, static_cast<uint16_t>(std::lround(raw)));
    else if (datatype == "S16") wr16(p, static_cast<uint16_t>(static_cast<int16_t>(std::lround(raw))));
    else if (datatype == "U32") wr32(p, static_cast<uint32_t>(std::llround(raw)));
    else if (datatype == "S32") wr32(p, static_cast<uint32_t>(static_cast<int32_t>(std::llround(raw))));
    else if (datatype == "F32") {
        float v = static_cast<float>(raw);
        uint32_t bits;
        memcpy(&bits, &v, sizeof(bits));
        wr32(p, bits);
    }
}
