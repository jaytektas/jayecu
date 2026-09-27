// CardLogs — see the header for why this is a folder and not a protocol.

#include "CardLogs.h"
#include "DatalogRecorder.h"

#include <j/io/Volumes.h>
#include <j/config/Settings.h>
#include <j/core/Log.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr const char* kCardPath = "datalog.cardPath";

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
bool isMlg(const fs::path& p) { return upper(p.extension().string()) == ".MLG"; }

// The file's own modification time as a unix epoch. FAT's resolution is two seconds, which is
// plenty to date a log by — this is a capture date, not a measurement.
int64_t epochOf(const fs::path& p) {
    std::error_code ec;
    const auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

// DATE FIRST, so an imported log sorts beside the studio's own recordings of the same session
// rather than in a block of LOGnnnn names at the other end of the folder. The card's own name is
// kept on the end because it is what the ECU called it, and that is worth not losing.
std::string targetName(const std::string& cardName, int64_t epoch) {
    char stamp[32] = "unknown";
    if (epoch > 0) {
        const std::time_t t = static_cast<std::time_t>(epoch);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm);
    }
    std::string base = cardName;
    const size_t dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return std::string("jayecu_") + stamp + "_" + base + ".mlg";
}
}  // namespace

std::string CardLogs::importDirectory() { return DatalogRecorder::resolvedDirectory(); }

std::string CardLogs::rememberedPath() {
    return jf::JSettings::instance().get<std::string>(kCardPath, "");
}
void CardLogs::rememberPath(const std::string& dir) {
    jf::JSettings::instance().set(kCardPath, dir);
}

bool CardLogs::looksLikeCard(const std::string& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) return false;
        if (e.is_regular_file(ec) && isMlg(e.path())) return true;
    }
    return false;
}

std::string CardLogs::findCard() {
    // The remembered one first: a tuner who pointed at a card once should not have to again, and a
    // machine with no automounter would otherwise never find it at all.
    const std::string remembered = rememberedPath();
    if (looksLikeCard(remembered)) return remembered;

    for (const jf::JVolumeInfo& v : jf::JVolumes::removableVolumes()) {
        if (!looksLikeCard(v.mountPoint)) continue;
        JLOGC("model.cardlogs", jf::JLogLevel::Info)
            << "card found at " << v.mountPoint << " (" << v.filesystem << ", "
            << (v.freeBytes >> 20) << " MB free of " << (v.totalBytes >> 20) << " MB)";
        return v.mountPoint;
    }
    return {};
}

std::vector<CardLogs::Entry> CardLogs::logsOn(const std::string& dir) {
    std::vector<Entry> out;
    if (dir.empty()) return out;
    std::error_code ec;
    const fs::path importDir = importDirectory();
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec) || !isMlg(e.path())) continue;
        Entry x;
        x.path   = e.path().string();
        x.name   = e.path().filename().string();
        x.bytes  = static_cast<uint64_t>(e.file_size(ec));
        x.epoch  = epochOf(e.path());
        x.target = targetName(x.name, x.epoch);
        // ALREADY IMPORTED = the file is sitting in the log folder at the same size. No manifest,
        // so nothing can go stale or disagree with the disk, and deleting an import honestly makes
        // the card's copy new again.
        std::error_code se;
        const fs::path landed = importDir / fs::path(x.target);
        x.imported = fs::exists(landed, se) && fs::file_size(landed, se) == x.bytes;
        out.push_back(std::move(x));
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.epoch != b.epoch) return a.epoch > b.epoch;      // newest first
        return a.name > b.name;
    });
    return out;
}

bool CardLogs::import(const Entry& e, std::string* whyNot) {
    auto fail = [whyNot](const std::string& m) { if (whyNot) *whyNot = m; return false; };
    std::error_code ec;
    if (e.imported) return true;               // nothing to do, and saying so is not an error
    const fs::path dest = fs::path(importDirectory()) / fs::path(e.target);

    std::ifstream in(e.path, std::ios::binary);
    if (!in) return fail("cannot read " + e.name + " from the card");
    std::ofstream out(dest.string(), std::ios::binary | std::ios::trunc);
    if (!out) return fail("cannot write into " + importDirectory());

    // THE HEADER, PATCHED AS IT PASSES. MLG v2 puts a unix epoch at bytes 8..11, big-endian, and the
    // ECU writes zero there because it opens the file before anything asks the clock. The filesystem
    // timestamp is that same moment — the ECU's RTC stamped it through FatFS — so this is not
    // invention; it is the date the file already carries, written where a reader looks for it.
    // Everything past the first 24 bytes is copied untouched.
    char head[24] = {};
    in.read(head, sizeof head);
    const std::streamsize got = in.gcount();
    if (got == static_cast<std::streamsize>(sizeof head) && e.epoch > 0 &&
        head[0] == 'M' && head[1] == 'L' && head[2] == 'V' && head[3] == 'L' && head[4] == 'G') {
        const uint32_t ep = static_cast<uint32_t>(e.epoch);
        head[8]  = static_cast<char>((ep >> 24) & 0xFF);
        head[9]  = static_cast<char>((ep >> 16) & 0xFF);
        head[10] = static_cast<char>((ep >>  8) & 0xFF);
        head[11] = static_cast<char>( ep        & 0xFF);
    }
    if (got > 0) out.write(head, got);

    std::vector<char> buf(64 * 1024);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n > 0) out.write(buf.data(), n);
    }
    out.flush();
    if (!out) { out.close(); fs::remove(dest, ec); return fail("write failed \xE2\x80\x94 is the disk full?"); }
    out.close();

    JLOGC("model.cardlogs", jf::JLogLevel::Info)
        << "imported " << e.name << " -> " << dest.string() << " (" << e.bytes << " bytes)";
    return true;
}

bool CardLogs::erase(const Entry& e, bool force, std::string* whyNot) {
    if (!e.imported && !force) {
        if (whyNot) *whyNot = e.name + " has not been imported \xE2\x80\x94 the card is still the only copy";
        return false;
    }
    std::error_code ec;
    if (!fs::remove(e.path, ec)) {
        if (whyNot) *whyNot = "could not delete " + e.name + " (" + ec.message() + ")";
        return false;
    }
    JLOGC("model.cardlogs", jf::JLogLevel::Info) << "deleted " << e.name << " from the card";
    return true;
}
