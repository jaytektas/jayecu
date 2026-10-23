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

// The name with "-N" before the extension: what the Nth log to want the same name is called.
fs::path numbered(const fs::path& dir, const std::string& target, int n) {
    if (n <= 1) return dir / target;
    const size_t dot = target.rfind('.');
    const std::string stem = dot == std::string::npos ? target : target.substr(0, dot);
    const std::string ext  = dot == std::string::npos ? std::string() : target.substr(dot);
    return dir / (stem + "-" + std::to_string(n) + ext);
}

// The last few KB of a file. Past the 24-byte header the import copies every byte untouched, so a
// copy's tail is the card file's tail — and two different logs that happen to share a size do not
// share it (the tail is data records, timestamps and all).
constexpr size_t kTail = 4096;
bool tailOf(const fs::path& p, uint64_t size, std::vector<char>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    const uint64_t n = std::min<uint64_t>(kTail, size > 24 ? size - 24 : 0);
    f.seekg(static_cast<std::streamoff>(size - n));
    out.resize(static_cast<size_t>(n));
    f.read(out.data(), static_cast<std::streamsize>(n));
    return f.gcount() == static_cast<std::streamsize>(n);
}

// Is this card log already in the folder? Its own name first, then the numbered names a clash would
// have pushed it to. SAME NAME AND SIZE WAS NOT ENOUGH: with the card's dates stuck at one value, and
// LOG numbers starting again after a card is cleared, two different logs got the same name — and the
// second import, seeing a different size, truncated the first one to write over it.
bool alreadyLanded(const fs::path& dir, const std::string& target, const fs::path& card, uint64_t bytes) {
    std::vector<char> want;
    bool haveWant = false;
    for (int n = 1;; ++n) {
        std::error_code se;
        const fs::path p = numbered(dir, target, n);
        if (!fs::exists(p, se)) return false;          // the names are taken in order; a gap ends them
        if (fs::file_size(p, se) != bytes || se) continue;
        if (!haveWant) { if (!tailOf(card, bytes, want)) return false; haveWant = true; }
        std::vector<char> have;
        if (tailOf(p, bytes, have) && have == want) return true;
    }
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

    // WHERE IT IS, NOT HOW FULL. Asking each drive for its free space read the card's whole allocation
    // table through the ECU's USB bridge — over ten seconds on a freshly mounted card, for a number only
    // a log line used. And said once per card, not on every poll: the dialog asks every 1.5 s.
    static std::string s_lastFound;
    for (const jf::JVolumeInfo& v : jf::JVolumes::removableVolumes()) {
        if (!looksLikeCard(v.mountPoint)) continue;
        if (v.mountPoint != s_lastFound)
            JLOGC("model.cardlogs", jf::JLogLevel::Info) << "card found at " << v.mountPoint << " (" << v.filesystem << ")";
        s_lastFound = v.mountPoint;
        return v.mountPoint;
    }
    s_lastFound.clear();
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
        // ALREADY IMPORTED = the file is sitting in the log folder, same size, same data. No
        // manifest, so nothing can go stale or disagree with the disk, and deleting an import
        // honestly makes the card's copy new again.
        x.imported = alreadyLanded(importDir, x.target, e.path(), x.bytes);
        out.push_back(std::move(x));
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.epoch != b.epoch) return a.epoch > b.epoch;      // newest first
        return a.name > b.name;
    });
    return out;
}

bool CardLogs::import(const Entry& e, std::string* whyNot, const std::function<void(uint64_t)>& progress) {
    auto fail = [whyNot](const std::string& m) { if (whyNot) *whyNot = m; return false; };
    std::error_code ec;
    if (e.imported) return true;               // nothing to do, and saying so is not an error
    // NEVER OVER ANOTHER LOG. A name already taken is a different log (logsOn would have called this
    // one imported otherwise), so this one takes the next free numbered name instead.
    const fs::path dir = importDirectory();
    fs::path dest;
    for (int n = 1;; ++n) {
        dest = numbered(dir, e.target, n);
        if (!fs::exists(dest, ec)) break;
    }

    std::ifstream in(e.path, std::ios::binary);
    if (!in) return fail("cannot read " + e.name + " from the card");
    std::ofstream out(dest.string(), std::ios::binary);
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
    uint64_t copied = got > 0 ? static_cast<uint64_t>(got) : 0;

    std::vector<char> buf(256 * 1024);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n > 0) { out.write(buf.data(), n); copied += static_cast<uint64_t>(n); }
        if (progress) progress(copied);
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
