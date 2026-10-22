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

void Ecu::setDfuSerial(const std::string &serial)
{
    if (serial.empty() || j_["dfu_serial"].str() == serial) return;
    j_["dfu_serial"] = jf::JJson(serial);
    save();
}

Ecu::Summary Ecu::byDfuSerial(const std::string &serial)
{
    if (serial.empty()) return {};
    for (const Summary &s : list())
        if (s.dfuSerial == serial) return s;
    return {};
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
    if (Ecu *e = open(uid)) {
        // THE BOARD IS WHAT THE FIRMWARE SAYS IT IS, NOW. A chip keeps its uid across a reflash to another
        // board's firmware (a bench board moved from one build to another; a recovery that picked the
        // wrong board), and an entry that kept the board it was first filed under then named the wrong
        // one in every list and in the studio's own status.
        if (!board.empty() && e->j_["board"].str() != board) {
            e->j_["board"] = jf::JJson(board);
            e->save();
        }
        return e;
    }
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

namespace {

// The summary of the entry in `dir`, from its ecu.json and the vehicle fields of its tune: the current
// tune, else the last active one, else any. Empty uid when the entry cannot be read.
Ecu::Summary readSummary(const fs::path &dir)
{
    Ecu::Summary s;
    auto parsed = jf::JJson::tryParseFile((dir / "ecu.json").string());
    if (!parsed || !parsed->isObject() || parsed->empty()) return s;
    const jf::JJson &o = *parsed;
    s.uid      = dir.filename().string();
    s.board    = o["board"].str();
    s.label    = o["label"].str().empty() ? s.board : o["label"].str();
    s.lastSeen = o["last_seen"].str();
    s.lastPort = o["last_port"].str();
    s.dfuSerial = o["dfu_serial"].str();
    std::string tune = o["current_tune"].str();
    if (tune.empty() || !fs::exists(dir / "tunes" / (tune + ".tune"))) tune = o["last_active_tune"].str();
    if (tune.empty() || !fs::exists(dir / "tunes" / (tune + ".tune"))) {
        tune.clear();
        std::error_code ec;
        for (const auto &t : fs::directory_iterator(dir / "tunes", ec))
            if (t.path().extension() == ".tune") { tune = t.path().stem().string(); break; }
    }
    s.currentTune = tune;
    if (tune.empty()) return s;
    auto t = jf::JJson::tryParseFile((dir / "tunes" / (tune + ".tune")).string());
    if (!t || !t->isObject()) return s;
    const jf::JJson &sc = (*t)["scalars"];
    s.vehicle = { sc["vehicle.Name"].str(), sc["vehicle.Make"].str(), sc["vehicle.Model"].str(),
                  sc["vehicle.Engine"].str(), sc["vehicle.VIN"].str(), sc["vehicle.Notes"].str() };
    return s;
}

} // namespace

std::vector<Ecu::Summary> Ecu::list()
{
    std::vector<Summary> out;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(ecuRoot(), ec)) {
        if (!entry.is_directory()) continue;
        Summary s = readSummary(entry.path());
        if (!s.uid.empty()) out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(),
              [](const Summary &a, const Summary &b) { return a.lastSeen > b.lastSeen; });
    return out;
}

Ecu::Summary Ecu::summary(const std::string &uid)
{
    if (uid.empty()) return {};
    return readSummary(fs::path(ecuRoot()) / uid);
}
