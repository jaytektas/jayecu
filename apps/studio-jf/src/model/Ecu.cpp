#include "Ecu.h"
#include "StudioPaths.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

// ISO 8601 timestamp for "now" (UTC), e.g. "2026-07-01T12:34:56". Kept sortable so the
// most-recently-seen ordering in list() is a plain lexicographic string compare.
std::string nowIso()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buf);
}

std::vector<uint8_t> readAllBytes(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

} // namespace

std::string Ecu::ecuRoot()
{
    // Fixed data location so the studio finds the same ECU folders across builds.
    return StudioPaths::dataDir("ecus");
}


std::string Ecu::dashboardPath() const { return dir_ + "/dashboard.gui"; }

std::string Ecu::restoreDir() const
{
    const std::string d = dir_ + "/restore";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

std::vector<std::string> Ecu::tuneNames() const
{
    std::vector<std::string> names;
    const std::string d = dir_ + "/tunes";
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(d, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string fn = entry.path().filename().string();
        if (fn.size() > 5 && fn.compare(fn.size() - 5, 5, ".tune") == 0)
            names.push_back(fn.substr(0, fn.size() - 5));   // strip ".tune"
    }
    return names;
}


std::vector<uint8_t> Ecu::loadTune(const std::string &name) const
{
    return readAllBytes(dir_ + "/tunes/" + name + ".tune");
}

void Ecu::saveTune(const std::string &name, const std::vector<uint8_t> &data)
{
    if (name.empty() || data.empty()) return;
    std::error_code ec;
    fs::create_directories(dir_ + "/tunes", ec);
    std::ofstream f(dir_ + "/tunes/" + name + ".tune", std::ios::binary);
    if (f)
        f.write(reinterpret_cast<const char *>(data.data()),
                static_cast<std::streamsize>(data.size()));
}



void Ecu::setCurrentTune(const std::string &name)
{
    j_["current_tune"] = jf::JJson(name);
    save();
}

void Ecu::setLabel(const std::string &l) {
    if (l.empty() || j_["label"].str() == l) return;
    j_["label"] = l;
    save();
}

void Ecu::setLastActiveTune(const std::string &name)
{
    j_["last_active_tune"] = jf::JJson(name);
    save();
}

void Ecu::setLastPort(const std::string &port)
{
    j_["last_port"] = jf::JJson(port);
    save();
}


void Ecu::save() const
{
    std::ofstream f(dir_ + "/ecu.json", std::ios::binary);
    if (f)
        f << j_.dump(2);
}

Ecu *Ecu::open(const std::string &uid)
{
    if (uid.empty()) return nullptr;
    const std::string dir = ecuRoot() + "/" + uid;
    auto parsed = jf::JJson::tryParseFile(dir + "/ecu.json");
    if (!parsed || !parsed->isObject() || parsed->empty()) return nullptr;
    auto *e = new Ecu;
    e->uid_ = uid;
    e->dir_ = dir;
    e->j_   = *parsed;
    return e;
}

Ecu *Ecu::openOrCreate(const std::string &uid, const std::string &board)
{
    if (uid.empty()) return nullptr;
    if (Ecu *e = open(uid)) return e;
    const std::string dir = ecuRoot() + "/" + uid;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto *e = new Ecu;
    e->uid_ = uid;
    e->dir_ = dir;
    e->j_["board"]     = jf::JJson(board);
    e->j_["last_seen"] = jf::JJson(nowIso());
    e->save();
    return e;
}

std::vector<Ecu::Summary> Ecu::list()
{
    std::vector<Summary> out;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(ecuRoot(), ec)) {
        if (!entry.is_directory()) continue;
        auto parsed = jf::JJson::tryParseFile(entry.path().string() + "/ecu.json");
        if (!parsed || !parsed->isObject() || parsed->empty()) continue;
        const jf::JJson &o = *parsed;
        // The label if the entry has one, the board otherwise — see Ecu::label(). Open ECU… lists these,
        // and "ts" in that list names a protocol rather than any particular ECU.
        const std::string lbl = o["label"].str();
        out.push_back({entry.path().filename().string(),
                       lbl.empty() ? o["board"].str() : lbl,
                       o["current_tune"].str(),
                       o["last_seen"].str()});
    }
    std::sort(out.begin(), out.end(),
              [](const Summary &a, const Summary &b) { return a.lastSeen > b.lastSeen; });
    return out;
}
