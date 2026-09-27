// DatalogRecorder — see the header for what a "log" is (one start to one stop) and why MSL.

#include "DatalogRecorder.h"
#include "Cache.h"
#include "MetaModel.h"
#include "StudioPaths.h"
#include "ExprCompiler.h"   // the gate travels as SOURCE; the ECU takes bytecode
#include <j/config/Json.h>
#include "SigilResolvers.h"     // AppStateSigilResolver::logging — the "%logging" channel

#include <j/core/Log.h>
#include <j/config/Settings.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace {
// PERSISTED, not held in a static: these are preferences, and a preference that forgets itself when
// the app closes is a preference nobody sets twice. Same store the editor's own settings use.
constexpr const char* kAutoLog = "datalog.autoRecord";
constexpr const char* kKeep    = "datalog.keepFiles";
constexpr const char* kDir     = "datalog.directory";
constexpr const char* kTemplate  = "datalog.template";
constexpr const char* kSelection = "datalog.selection";

std::string defaultDirectory() {
    return StudioPaths::dataDir("datalogs");
}

// std::localtime returns a shared static; the reentrant form is spelled differently on each
// platform, so the difference lives here once rather than at every call site.
std::tm localTime(std::time_t t) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

// "Sat Aug 30 19:42:11 BST 2026" — the shape MegaLogViewer's own captures use, so its date parsing
// meets something familiar rather than an ISO string it has to guess at.
std::string captureDate(std::time_t t) {
    char buf[64];
    std::tm tm = localTime(t);
    if (std::strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Z %Y", &tm) == 0) return "unknown";
    return buf;
}

// Sortable, and sorts the same as chronological — which is what makes "newest first" a filename
// comparison rather than a stat of every file in the directory.
std::string stampedName(std::time_t t) {
    char buf[64];
    std::tm tm = localTime(t);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tm);
    return std::string("jayecu_") + buf + ".msl";
}

// MSL is TAB-separated, so a tab or a newline inside a name would silently add a column. Nothing in
// the meta has one today; this is here so that stays true when somebody adds a channel.
std::string clean(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return out;
}
}  // namespace

DatalogRecorder& DatalogRecorder::instance() { static DatalogRecorder r; return r; }

// OFF by default. A recorder that runs unasked fills a directory with sessions nobody meant to keep,
// and the one it would have caught is the one the user would have pressed record for anyway.
bool DatalogRecorder::autoLog()   { return jf::JSettings::instance().get<bool>(kAutoLog, false); }
void DatalogRecorder::setAutoLog(bool on) { jf::JSettings::instance().set(kAutoLog, on); }
// 0 = keep everything, and that is the default: deleting a tuner's data is not something to start
// doing because nobody chose otherwise.
int  DatalogRecorder::keepFiles() { return jf::JSettings::instance().get<int>(kKeep, 0); }
void DatalogRecorder::setKeepFiles(int n) { jf::JSettings::instance().set(kKeep, n < 0 ? 0 : n); }
std::string DatalogRecorder::directory() { return jf::JSettings::instance().get<std::string>(kDir, ""); }
void DatalogRecorder::setDirectory(const std::string& d) { jf::JSettings::instance().set(kDir, d); }

std::string DatalogRecorder::resolvedDirectory() {
    const std::string d = directory();
    const std::string dir = d.empty() ? defaultDirectory() : d;
    std::error_code ec;
    fs::create_directories(dir, ec);      // ec swallowed: start() reports the failure that follows
    return dir;
}

std::vector<std::string> DatalogRecorder::existingLogs() {
    std::vector<std::string> out;
    std::error_code ec;
    const std::string d = directory();
    const std::string dir = d.empty() ? defaultDirectory() : d;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".msl") continue;   // never touch anything we did not write
        out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end(), std::greater<>());   // stamped names sort as dates
    return out;
}

// THE TEMPLATES THIS FIRMWARE DECLARES, read from the meta once. A studio that offers sets the
// connected ECU has never heard of is offering guesses; this offers what the definition ships.
const std::vector<DatalogRecorder::Template>& DatalogRecorder::templates() {
    static std::vector<Template> cache;
    // KEYED ON THE DEFINITION, NOT THE POINTER. This cached against the MetaModel address, which is
    // the SAME object before and after a load — the studio fills one in place. So the empty answer
    // from startup was cached forever and the menu went on saying "connect an ECU" while connected.
    // The layout hash changes exactly when the channels do, which is the question being asked.
    static std::string forLayout = "\x01";        // a value no hash can be, so the first call builds
    const MetaModel* meta = Cache::instance().meta();
    const std::string layout = meta ? meta->layoutHash() : std::string();
    if (layout == forLayout) return cache;
    forLayout = layout;
    cache.clear();
    if (!meta) return cache;
    for (const jf::JJson& t : meta->datalogTemplates().arr()) {
        Template x;
        x.id      = t["id"].str();
        x.name    = t["name"].str();
        x.blurb   = t["blurb"].str();
        x.detail  = t["detail"].str();
        x.all     = t["all"].boolean(false);
        x.useFlag = t["use_datalog_flag"].boolean(false);
        x.rateHz  = static_cast<int>(t["rate_hz"].number(0));
        for (const jf::JJson& c : t["categories"].arr()) x.categories.push_back(c.str());
        for (const jf::JJson& g : t["signals"].arr())    x.signals.push_back(g.str());
        if (!x.id.empty()) cache.push_back(std::move(x));
    }
    return cache;
}

const DatalogRecorder::Template* DatalogRecorder::templateById(const std::string& id) {
    for (const Template& t : templates()) if (t.id == id) return &t;
    return nullptr;
}

std::string DatalogRecorder::templateId() {
    return jf::JSettings::instance().get<std::string>(kTemplate, "standard");
}

// A channel list as settings keep it: names joined by commas.
namespace {
std::vector<std::string> readNames(const char* key) {
    std::vector<std::string> out;
    const std::string packed = jf::JSettings::instance().get<std::string>(key, "");
    std::string tok;
    for (const char c : packed) {
        if (c == ',') { if (!tok.empty()) out.push_back(tok); tok.clear(); }
        else tok += c;
    }
    if (!tok.empty()) out.push_back(tok);
    return out;
}
void writeNames(const char* key, const std::vector<std::string>& names) {
    std::string packed;
    for (const std::string& n : names) { if (!packed.empty()) packed += ','; packed += n; }
    jf::JSettings::instance().set(key, packed);
}
}

std::vector<std::string> DatalogRecorder::customSelection() { return readNames(kSelection); }

void DatalogRecorder::setCustomSelection(const std::vector<std::string>& names) {
    writeNames(kSelection, names);
    // Edited by hand, so it is no longer any template — saying otherwise would name a set that is not
    // what the name means.
    jf::JSettings::instance().set(kTemplate, std::string());
    pushMaskToEcu();
}

// THE FIRMWARE'S CATALOG ORDER, recovered from the meta. The frame is packed field by field in this
// order, so each channel's offset ranks it — and that rank is its bit in the tune's mask.
std::vector<std::string> DatalogRecorder::catalogOrder() {
    std::vector<std::pair<int, std::string>> byOffset;
    const MetaModel* meta = Cache::instance().meta();
    if (!meta) return {};
    byOffset.reserve(meta->telemetry().size());
    for (const auto& [name, f] : meta->telemetry()) byOffset.emplace_back(f.offset, name);
    std::sort(byOffset.begin(), byOffset.end());
    std::vector<std::string> out;
    out.reserve(byOffset.size());
    for (auto& [off, name] : byOffset) { (void)off; out.push_back(std::move(name)); }
    return out;
}

std::vector<uint8_t> DatalogRecorder::maskBytesFor(const std::vector<std::string>& channels) {
    const std::vector<std::string> order = catalogOrder();
    std::vector<uint8_t> bits((order.size() + 7) / 8, 0);
    std::vector<std::string> sel = channels;
    std::sort(sel.begin(), sel.end());                           // binary_search wants it sorted
    for (size_t i = 0; i < order.size(); ++i)
        if (std::binary_search(sel.begin(), sel.end(), order[i]))
            bits[i >> 3] |= uint8_t(1u << (i & 7));
    return bits;
}

std::vector<uint8_t> DatalogRecorder::maskBytes() { return maskBytesFor(selectedChannels()); }

// THE SAME MAPPING READ BACKWARDS. An all-zero mask is not "log nothing" — the firmware's
// resolve_selection() reads it as "as shipped", so it decodes to the definition's own `datalog:`
// set, which is what the card would actually record.
std::vector<std::string> DatalogRecorder::channelsFromMask(const std::vector<uint8_t>& bits) {
    const std::vector<std::string> order = catalogOrder();
    std::vector<std::string> out;
    bool any = false;
    for (const uint8_t b : bits) if (b) { any = true; break; }
    if (!any) {
        const MetaModel* meta = Cache::instance().meta();
        if (meta) for (const auto& [name, f] : meta->telemetry()) if (f.datalog) out.push_back(name);
        std::sort(out.begin(), out.end());
        return out;
    }
    for (size_t i = 0; i < order.size(); ++i) {
        const size_t byte = i >> 3;
        if (byte < bits.size() && (bits[byte] & (1u << (i & 7)))) out.push_back(order[i]);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void DatalogRecorder::pushMaskToEcu() { pushMaskToEcu(maskBytes()); }

void DatalogRecorder::pushMaskToEcu(const std::vector<uint8_t>& bits) {
    Cache& C = Cache::instance();
    if (!C.meta() || !C.isConfig("datalog.mask[0].bits")) return;   // older firmware: nothing to write
    for (size_t i = 0; i < bits.size(); ++i) {
        const std::string path = "datalog.mask[" + std::to_string(i) + "].bits";
        if (!C.isConfig(path)) break;                              // the tune's array is shorter
        C.setConfigValue(path, bits[i]);
    }
    JLOGC("model.datalog", jf::JLogLevel::Info)
        << "pushed channel mask to the ECU (" << channelsFromMask(bits).size() << " channels)";
}

// PROFILES, stored as "name\x1fchan,chan\x1e…" — a flat string because JSettings holds strings and a
// profile is a name and a list. The separators are the ASCII ones meant for exactly this (unit and
// record), so a channel name or a profile name can never contain one by accident.
namespace {
constexpr char kRecSep = '\x1e';
constexpr char kUnitSep = '\x1f';
std::vector<std::pair<std::string, std::string>> readProfiles() {
    std::vector<std::pair<std::string, std::string>> out;
    const std::string all = jf::JSettings::instance().get<std::string>("datalog.profiles", "");
    std::string rec;
    for (const char c : all) {
        if (c == kRecSep) { if (!rec.empty()) { const size_t u = rec.find(kUnitSep);
            if (u != std::string::npos) out.emplace_back(rec.substr(0, u), rec.substr(u + 1)); } rec.clear(); }
        else rec += c;
    }
    if (!rec.empty()) { const size_t u = rec.find(kUnitSep);
        if (u != std::string::npos) out.emplace_back(rec.substr(0, u), rec.substr(u + 1)); }
    return out;
}
void writeProfiles(const std::vector<std::pair<std::string, std::string>>& v) {
    std::string all;
    for (const auto& [n, c] : v) { all += n; all += kUnitSep; all += c; all += kRecSep; }
    jf::JSettings::instance().set("datalog.profiles", all);
}
}  // namespace

std::vector<std::string> DatalogRecorder::profileNames() {
    std::vector<std::string> out;
    for (const auto& [n, c] : readProfiles()) { (void)c; out.push_back(n); }
    std::sort(out.begin(), out.end());
    return out;
}

// Through profile(), NOT by splitting the stored record on commas. The record grew fields after the
// channel list, and a comma-split of the whole thing returned the last channel with the entire rest
// of the profile glued to it — three names, one of them nonsense, which selectedChannels() then
// dropped as a channel the firmware does not have. The record has a shape; read it with the reader.
std::vector<std::string> DatalogRecorder::profileChannels(const std::string& name) {
    return profile(name).channels;
}

// A STORED PROFILE IS FIELDS, unit-separated, of which the channel list is only the first. Older
// records held nothing else and are read as a channel list with the rest at their defaults, so a
// profile saved before the gate travelled with it still opens — it simply has no gate, which is the
// truth about it.
namespace {
std::vector<std::string> splitUnits(const std::string& s) {
    std::vector<std::string> out; std::string tok;
    for (const char c : s) { if (c == kUnitSep) { out.push_back(tok); tok.clear(); } else tok += c; }
    out.push_back(tok);
    return out;
}
int fieldInt(const std::vector<std::string>& f, size_t i, int dflt) {
    if (i >= f.size() || f[i].empty()) return dflt;
    return std::atoi(f[i].c_str());
}
std::string fieldStr(const std::vector<std::string>& f, size_t i) {
    return i < f.size() ? f[i] : std::string();
}
std::vector<std::string> splitList(const std::string& packed) {
    std::vector<std::string> out; std::string tok;
    for (const char c : packed) { if (c == ',') { if (!tok.empty()) out.push_back(tok); tok.clear(); }
                                  else tok += c; }
    if (!tok.empty()) out.push_back(tok);
    return out;
}
}  // namespace

DatalogRecorder::Profile DatalogRecorder::profile(const std::string& name) {
    Profile p;
    for (const auto& [n, body] : readProfiles()) {
        if (n != name) continue;
        const std::vector<std::string> f = splitUnits(body);
        p.name      = n;
        p.channels  = splitList(fieldStr(f, 0));
        p.logWhen   = fieldStr(f, 1);
        p.logUntil  = fieldStr(f, 2);
        p.enabled   = fieldInt(f, 3, 1);
        p.rateHz    = fieldInt(f, 4, 10);
        p.minOnMs   = fieldInt(f, 5, 0);
        p.minOffMs  = fieldInt(f, 6, 0);
        p.maxOnMs   = fieldInt(f, 7, 0);
        p.rearmMs   = fieldInt(f, 8, 0);
        p.onInvalid = fieldInt(f, 9, 1);
        return p;
    }
    return p;                      // name left empty: no such profile
}

// Saving under an existing name REPLACES it. That is what "save" means to anyone who has used a
// preset before, and the alternative — silently making a second profile with the same name — is
// worse than either overwriting or refusing.
void DatalogRecorder::saveProfile(const Profile& p) {
    if (p.name.empty()) return;
    std::string chans;
    for (const std::string& c : p.channels) { if (!chans.empty()) chans += ','; chans += c; }
    std::string body = chans;
    const int nums[] = { p.enabled, p.rateHz, p.minOnMs, p.minOffMs, p.maxOnMs, p.rearmMs, p.onInvalid };
    body += kUnitSep; body += p.logWhen;
    body += kUnitSep; body += p.logUntil;
    for (const int n : nums) { body += kUnitSep; body += std::to_string(n); }

    auto v = readProfiles();
    for (auto& [n, b] : v) if (n == p.name) { b = body; writeProfiles(v); return; }
    v.emplace_back(p.name, body);
    writeProfiles(v);
}

void DatalogRecorder::saveProfile(const std::string& name) {
    saveProfile(name, selectedChannels());
}

void DatalogRecorder::saveProfile(const std::string& name, const std::vector<std::string>& channels) {
    Profile p = profile(name);          // keep whatever else that profile already said
    p.name = name;
    p.channels = channels;
    saveProfile(p);
}

DatalogRecorder::Cost DatalogRecorder::costOf(const std::vector<std::string>& channels, int rateHz) {
    Cost c;
    const MetaModel* meta = Cache::instance().meta();
    if (!meta) return c;
    int fields = 0;
    for (const std::string& n : channels) {
        const auto it = meta->telemetry().find(n);
        if (it == meta->telemetry().end()) continue;   // a name this firmware does not have
        ++c.channels;
        fields += it->second.size;
        const int hz = it->second.updateHz;
        if (hz > 0 && (c.slowestHz == 0 || hz < c.slowestHz)) {
            c.slowestHz = hz;
            c.slowestChannel = n;
        }
    }
    if (c.channels == 0) return c;
    c.recordBytes = 4 + fields + 1;                    // prefix + fields + checksum
    const int hz  = rateHz > 0 ? rateHz : 1;
    c.bytesPerSec = double(c.recordBytes) * hz;
    c.mbPerHour   = c.bytesPerSec * 3600.0 / 1e6;
    c.pastCard    = c.bytesPerSec > kCardMeasuredBytesPerSec;
    return c;
}

// The card, as the ECU reports it. Two seconds of silence is "no ECU": a stale frame from a link
// that has gone away is not evidence a card is still fitted.
DatalogRecorder::Card DatalogRecorder::ecuCard() {
    const Cache& C = Cache::instance();
    if (C.msSinceTelemetry() >= 2000)          return Card::Unknown;
    if (C.value("sd_msc_active") != 0.0)       return Card::OnUsb;
    if (C.value("sd_present")    != 0.0)       return Card::Ready;
    return Card::None;
}

const char* DatalogRecorder::cardReason(Card c) {
    switch (c) {
    case Card::Unknown: return "no ECU connected";
    case Card::None:    return "the ECU has no SD card";
    case Card::OnUsb:   return "the card is on the PC";
    case Card::Ready:   default: return "";
    }
}

// WHAT THE ECU IS SET TO. Read out of the config image the studio holds — the cache IS the ECU's
// settings once they have been read — rather than out of the studio's own JSettings, which is a
// different question ("what does the STUDIO record") with a different answer.
DatalogRecorder::Profile DatalogRecorder::currentSettings() {
    Profile p;
    p.name = "";                        // deliberately nameless: it is not a saved thing
    Cache& C = Cache::instance();
    const MetaModel* meta = C.meta();
    if (!meta) return p;

    std::vector<uint8_t> bits;
    for (size_t i = 0; ; ++i) {
        const std::string path = "datalog.mask[" + std::to_string(i) + "].bits";
        if (!C.isConfig(path)) break;
        bits.push_back(static_cast<uint8_t>(C.configValue(path)));
    }
    p.channels = channelsFromMask(bits);

    auto num = [&C](const char* path, int dflt) {
        return C.isConfig(path) ? static_cast<int>(C.configValue(path)) : dflt;
    };
    p.enabled   = num("datalog.enabled",    1);
    p.rateHz    = num("datalog.rate_hz",   10);
    p.minOnMs   = num("datalog.min_on_ms",  0);
    p.minOffMs  = num("datalog.min_off_ms", 0);
    p.maxOnMs   = num("datalog.max_on_ms",  0);
    p.rearmMs   = num("datalog.rearm_ms",   0);
    p.onInvalid = num("datalog.on_invalid", 1);

    auto expr = [meta, &C](const char* path) {
        const std::vector<uint8_t> code = C.configBlob(path);
        return code.empty() ? std::string()
                            : ExprCompiler::decompile(code.data(), (uint16_t)code.size(), *meta);
    };
    p.logWhen  = expr("datalog.log_when");
    p.logUntil = expr("datalog.log_until");
    return p;
}

// THE ONE THING HERE THAT TOUCHES THE CAR. Everything else — making, editing, copying, deleting a
// profile — is the studio's own business and happens with no link at all; a tuner works out the
// setup at a desk and installs it at the car, which is the whole reason a profile exists.
//
// The gate is compiled HERE, against the connected definition, because that is the only place the
// signal ids are known. A condition that does not compile stops the whole activation rather than
// being dropped from it: half a gate is a logger recording the wrong thing quietly.
bool DatalogRecorder::activate(const Profile& p, std::string* whyNot) {
    Cache& C = Cache::instance();
    const MetaModel* meta = C.meta();
    if (!meta) { if (whyNot) *whyNot = "no ECU definition loaded"; return false; }
    if (!C.isConfig("datalog.mask[0].bits")) {
        if (whyNot) *whyNot = "this firmware has no onboard log settings";
        return false;
    }
    // The card is checked HERE as well as in the UI, so the rule holds however activate() is
    // reached. A logger configured to write to a slot with nothing in it is not a setting.
    const Card card = ecuCard();
    if (card != Card::Ready) { if (whyNot) *whyNot = cardReason(card); return false; }

    // COMPILE BOTH FIRST, write nothing until both are good — an activation that failed half way
    // would leave the car with one condition from the new setup and one from the old.
    std::vector<uint8_t> whenCode, untilCode;
    struct { const char* path; const std::string& src; std::vector<uint8_t>& out; } gates[] = {
        { "datalog.log_when",  p.logWhen,  whenCode  },
        { "datalog.log_until", p.logUntil, untilCode },
    };
    for (auto& g : gates) {
        if (g.src.empty()) continue;
        const auto r = ExprCompiler::compile(g.src, *meta, (uint32_t)meta->configSize());
        if (!r.ok) {
            if (whyNot) *whyNot = std::string(g.path) + ": " + r.error;
            return false;
        }
        int off = 0, size = 0;
        if (meta->resolveBlob(g.path, off, size) && (int)r.code.size() > size) {
            if (whyNot) *whyNot = std::string(g.path) + ": too long for the field";
            return false;
        }
        g.out = r.code;
    }

    // The card's channels. setCustomSelection pushes the mask itself, so this is also the ECU write.
    // It does not touch the studio's own recording set (recordingSelection), which is chosen apart.
    DatalogRecorder::setCustomSelection(p.channels);
    auto num = [&C](const char* path, int v) { if (C.isConfig(path)) C.setConfigValue(path, v); };
    num("datalog.enabled",    p.enabled);
    num("datalog.rate_hz",    p.rateHz);
    num("datalog.min_on_ms",  p.minOnMs);
    num("datalog.min_off_ms", p.minOffMs);
    num("datalog.max_on_ms",  p.maxOnMs);
    num("datalog.rearm_ms",   p.rearmMs);
    num("datalog.on_invalid", p.onInvalid);
    int off = 0, size = 0;
    if (meta->resolveBlob("datalog.log_when",  off, size)) C.setConfigBlob("datalog.log_when",  whenCode);
    if (meta->resolveBlob("datalog.log_until", off, size)) C.setConfigBlob("datalog.log_until", untilCode);

    JLOGC("model.datalog", jf::JLogLevel::Info)
        << "activated onboard logging \"" << (p.name.empty() ? std::string("(current)") : p.name)
        << "\": " << p.channels.size() << " channels, gate \"" << p.logWhen << "\" / \""
        << p.logUntil << "\"";
    return true;
}

// A TEMPLATE SEEN AS A PROFILE, so the editor has one shape to show. A template names channels and
// nothing else, so everything else comes back at what the tune currently holds rather than at
// invented values — picking "Boost" should change the columns, not silently rewrite the gate.
DatalogRecorder::Profile DatalogRecorder::templateProfile(const std::string& id) {
    Profile p = currentSettings();
    const Template* t = templateById(id);
    p.name = t ? t->name : std::string();
    p.channels = templateChannels(id);
    // …and its rate, when it states one. A set chosen for 1 kHz work is not that set at 10 Hz, and
    // the name says so out loud.
    if (t && t->rateHz > 0) p.rateHz = t->rateHz;
    return p;
}

void DatalogRecorder::deleteProfile(const std::string& name) {
    auto v = readProfiles();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& kv) { return kv.first == name; }), v.end());
    writeProfiles(v);
}

bool DatalogRecorder::applyProfile(const std::string& name) {
    const std::vector<std::string> ch = profileChannels(name);
    if (ch.empty()) return false;
    setCustomSelection(ch);                       // …which clears templateId and pushes the ECU mask
    return true;
}

// WHAT A TEMPLATE MEANS, resolved without meaning it yet. A dialog that only wants to SHOW a
// template's channels must not push a mask to the ECU on the way — so the resolving and the
// committing are two functions, and applyTemplate is the second one calling the first.
std::vector<std::string> DatalogRecorder::templateChannels(const std::string& id) {
    const Template* t = templateById(id);
    const MetaModel* meta = Cache::instance().meta();
    std::vector<std::string> picked;
    if (!t || !meta) return picked;
    for (const auto& [name, f] : meta->telemetry()) {
        bool want = t->all || (t->useFlag && f.datalog);
        if (!want)
            for (const std::string& c : t->categories)
                if (f.module == c) { want = true; break; }      // the channel's own category
        if (!want)
            for (const std::string& g : t->signals)
                if (g == name) { want = true; break; }
        if (want) picked.push_back(name);
    }
    std::sort(picked.begin(), picked.end());
    return picked;
}

bool DatalogRecorder::applyTemplate(const std::string& id) {
    const std::vector<std::string> picked = templateChannels(id);
    if (picked.empty()) return false;
    setCustomSelection(picked);                                  // …which clears templateId, so:
    jf::JSettings::instance().set(kTemplate, id);                // this set IS that template, until edited
    pushMaskToEcu();
    return true;
}

std::vector<std::string> DatalogRecorder::selectedChannels() {
    const MetaModel* meta = Cache::instance().meta();
    std::vector<std::string> out;
    if (!meta) return out;

    out = customSelection();
    // A stored name the connected firmware does not have is dropped rather than written as a column
    // of nothing — a definition can change under a saved selection.
    if (!out.empty()) {
        std::vector<std::string> live;
        for (const std::string& n : out) if (meta->telemetry().count(n)) live.push_back(n);
        out.swap(live);
    }
    // Never configured, or a selection that no longer matches anything: fall back to what the
    // DEFINITION marks worth logging rather than to silence.
    if (out.empty())
        for (const auto& [name, f] : meta->telemetry()) if (f.datalog) out.push_back(name);
    if (out.empty())
        for (const auto& [name, f] : meta->telemetry()) { (void)f; out.push_back(name); }
    // Sorted so two logs from the same firmware have the same column order and compare without
    // re-mapping, whatever order the meta or the picker produced.
    std::sort(out.begin(), out.end());
    return out;
}

bool DatalogRecorder::start(std::string* whyNot) {
    if (file_) { if (whyNot) *whyNot = "already recording"; return false; }

    const MetaModel* meta = Cache::instance().meta();
    if (!meta || !meta->isValid()) {
        if (whyNot) *whyNot = "no ECU definition loaded";
        return false;
    }

    // THE COLUMNS, FIXED FOR THE LIFE OF THE FILE. Resolved once here because a log whose columns
    // change half-way through has lied about every row above the change. Sorted by name so two logs
    // from the same firmware have the same column order and can be compared without re-mapping.
    channels_ = recordingChannels();
    if (channels_.empty()) { if (whyNot) *whyNot = "the definition declares no telemetry"; return false; }

    const std::time_t now = std::time(nullptr);
    path_ = resolvedDirectory() + "/" + stampedName(now);
    file_ = std::fopen(path_.c_str(), "wb");
    if (!file_) {
        if (whyNot) *whyNot = "cannot write to " + resolvedDirectory();
        path_.clear();
        return false;
    }

    // Line 1 is the controller signature, line 2 the capture date — both quoted, as MSL has them.
    // The signature carries the layout hash as well as the version: two logs that disagree about a
    // channel are then distinguishable at a glance rather than by inference from the columns.
    std::fprintf(file_, "\"%s %s (%s) layout %s\"\n",
                 clean(meta->product()).c_str(), clean(meta->fwVersion()).c_str(),
                 clean(meta->board()).c_str(), clean(meta->layoutHash()).c_str());
    std::fprintf(file_, "\"Capture Date: %s\"\n", captureDate(now).c_str());

    // Names, then units. Time first in both, in seconds, which is what every MSL reader expects.
    std::fputs("Time", file_);
    for (const std::string& c : channels_) std::fprintf(file_, "\t%s", clean(c).c_str());
    std::fputc('\n', file_);
    std::fputs("s", file_);
    for (const std::string& c : channels_) {
        const auto it = meta->telemetry().find(c);
        std::fprintf(file_, "\t%s", it == meta->telemetry().end() ? "" : clean(it->second.units).c_str());
    }
    std::fputc('\n', file_);

    rows_  = 0;
    t0_ms_ = 0.0;
    AppStateSigilResolver::logging = true;    // the "%logging" channel finally tells the truth
    JLOGC("model.datalog", jf::JLogLevel::Info) << "recording -> " << path_
                                            << " (" << channels_.size() << " channels)";
    return true;
}

void DatalogRecorder::onFrame() {
    if (!file_) return;
    Cache& C = Cache::instance();

    // THE TIME BASE IS THE HOST'S, not the ECU's. An uptime channel resets when the ECU does, and a
    // log whose clock jumps backwards mid-file is worse than one with no clock at all.
    const double now_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (rows_ == 0) t0_ms_ = now_ms;

    std::fprintf(file_, "%.3f", (now_ms - t0_ms_) / 1000.0);
    for (const std::string& c : channels_) std::fprintf(file_, "\t%g", C.value(c));
    std::fputc('\n', file_);

    // Flushed every 64 rows rather than every row: a recording that survives a crash is the point of
    // choosing text, and at typical rates this is a second or two of exposure for a fraction of the
    // syscalls. The close on stop() flushes the rest.
    if ((++rows_ % 64u) == 0u) std::fflush(file_);
}

std::string DatalogRecorder::stop() {
    if (!file_) return {};
    std::fclose(file_);
    file_ = nullptr;
    AppStateSigilResolver::logging = false;
    const std::string done = path_;
    JLOGC("model.datalog", jf::JLogLevel::Info) << "recorded " << rows_ << " rows -> " << done;
    path_.clear();
    channels_.clear();
    prune();                       // …only now the file is closed and complete
    return done;
}

// KEEP THE LAST N. Runs when a recording ends, never on a timer, so nothing disappears while the app
// sits idle with a file open in a viewer. 0 keeps everything, which is the default: deleting a
// tuner's data is not something to do because nobody chose otherwise.
void DatalogRecorder::prune() {
    const int keep = keepFiles();
    if (keep <= 0) return;
    const std::vector<std::string> logs = existingLogs();     // newest first
    if (static_cast<int>(logs.size()) <= keep) return;
    std::error_code ec;
    for (size_t i = static_cast<size_t>(keep); i < logs.size(); ++i) {
        fs::remove(logs[i], ec);
        JLOGC("model.datalog", jf::JLogLevel::Info) << "pruned " << logs[i];
    }
}

// THE STUDIO'S OWN SET. Keyed by the definition's board so jayecu and an imported rusEFI definition keep
// separate lists; with no definition loaded there is nothing to key by, and nothing to record either.
namespace {
std::string recordingKey() {
    const MetaModel* meta = Cache::instance().meta();
    const std::string board = (meta && meta->isValid()) ? meta->board() : std::string();
    return board.empty() ? std::string("datalog.recording") : "datalog.recording." + board;
}
}

std::vector<std::string> DatalogRecorder::recordingSelection() { return readNames(recordingKey().c_str()); }

void DatalogRecorder::setRecordingSelection(const std::vector<std::string>& names) {
    writeNames(recordingKey().c_str(), names);
}

std::vector<std::string> DatalogRecorder::recordingChannels() {
    const MetaModel* meta = Cache::instance().meta();
    std::vector<std::string> out;
    if (!meta) return out;
    // In the order chosen: the picker's list is ordered, and that order is the log's column order.
    for (const std::string& n : recordingSelection())
        if (meta->telemetry().count(n)) out.push_back(n);
    if (!out.empty()) return out;
    // Nothing chosen, or nothing chosen that this definition still has: every channel, sorted so two
    // logs from the same firmware line up column for column.
    for (const auto& [name, f] : meta->telemetry()) { (void)f; out.push_back(name); }
    std::sort(out.begin(), out.end());
    return out;
}
