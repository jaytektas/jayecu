#pragma once

// DashboardOrigin — which shipped dashboard an ECU's own pages came from, and whether they have been
// edited since. It is what lets a studio or firmware update bring new pages to an ECU that already has some.
//
// An ECU's pages are its own copy (ecus/<uid>/dashboard.gui), taken from the shipped dashboard the first
// time it is opened and the user's from then on. Nothing ever replaced that copy, so a page fixed in a
// release reached new ECUs only: the one on the bench kept the old page for ever. What was missing is
// the one fact that makes replacing it safe — did the user change it? — and the file cannot answer that
// by itself, because the studio rewrites it on every save whether the layout moved or not.
//
// So it is recorded, beside the dashboard (dashboard.origin.json):
//     {"crc": "<CRC-32 of the shipped file it was installed from>", "edited": false, "declined": "<crc>"}
//   crc       set when a shipped dashboard is installed; "" for pages that did not come from one.
//   edited    set when the layout is saved with changes of the user's own.
//   declined  a shipped dashboard the user was offered and said no to — not offered again.
// No record at all is a copy made before this existed: the studio cannot tell whether it was edited,
// so it asks rather than assumes.

#include "../comms/Crc32.h"
#include "../model/FirmwareKits.h"
#include <j/config/Json.h>
#include <j/update/JVersion.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace dashorigin {

struct Origin {
    bool        known = false;    // a record exists
    std::string crc;              // the shipped file these pages were installed from ("" = not from one)
    bool        edited = false;
    std::string declined;
};

inline std::filesystem::path recordPath(const std::string& dashboardPath) {
    return std::filesystem::path(dashboardPath).parent_path() / "dashboard.origin.json";
}

// CRC-32 of a file, as 8 hex digits; "" when it cannot be read. Identity only — which shipped file is this.
inline std::string fileCrc(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    const std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08x", crc32_ieee::compute(s.data(), int(s.size())));
    return hex;
}

inline Origin load(const std::string& dashboardPath) {
    Origin o;
    const auto j = jf::JJson::tryParseFile(recordPath(dashboardPath).string());
    if (!j || !j->isObject()) return o;
    o.known    = true;
    o.crc      = (*j)["crc"].str();
    o.edited   = (*j)["edited"].boolean();
    o.declined = (*j)["declined"].str();
    return o;
}

inline void save(const std::string& dashboardPath, const Origin& o) {
    jf::JJson j = jf::JJson::object();
    j["crc"] = o.crc;
    j["edited"] = o.edited;
    j["declined"] = o.declined;
    j.dumpToFile(recordPath(dashboardPath).string(), 2);
}

// A shipped dashboard has just been copied in as this ECU's pages.
inline void installed(const std::string& dashboardPath, const std::string& fromShipped) {
    Origin o;
    o.crc = fileCrc(fromShipped);
    save(dashboardPath, o);
}

// The layout is being saved. Changes of the user's own make the pages theirs; a save with none (the tune
// saved, the studio closing) changes nothing here.
inline void saving(const std::string& dashboardPath, bool layoutChanged) {
    if (!layoutChanged) return;
    Origin o = load(dashboardPath);
    if (o.known && o.edited) return;
    o.edited = true;                     // no record: pages the user built or changed — theirs either way
    save(dashboardPath, o);
}

// The newest shipped pages drawn for this board AND layout: the newest kit this studio can use that
// carries one (a kit for the same layout may be a newer firmware — 0.4.2 re-draws 0.4.1's pages), else
// the library's "<board> <hash>.gui". "" when there are none.
inline std::string newestFor(const std::vector<fwkits::Kit>& kits, const std::string& board,
                             const std::string& layoutHash, const std::string& studioVersion,
                             const std::filesystem::path& libraryExact) {
    if (board.empty() || layoutHash.empty()) return {};
    const fwkits::Kit* best = nullptr;
    std::error_code ec;
    for (const fwkits::Kit& k : kits) {
        if (k.board != board || k.layoutHash != layoutHash || k.dashboard.empty()) continue;
        if (!k.minStudio.empty() &&
            jf::JVersion::parse(k.minStudio).isNewerThan(jf::JVersion::parse(studioVersion))) continue;
        if (!std::filesystem::exists(k.dashboard, ec)) continue;
        if (!best || jf::JVersion::parse(k.version).isNewerThan(jf::JVersion::parse(best->version))) best = &k;
    }
    if (best) return best->dashboard;
    return std::filesystem::exists(libraryExact, ec) ? libraryExact.string() : std::string();
}

}  // namespace dashorigin
