#include "FirmwareFetch.h"

#include <j/update/JSha256.h>
#include <j/update/JVersion.h>
#include "StudioVersion.h"

#include <j/config/Json.h>
#include <j/core/MainThreadDispatcher.h>
#include <j/io/HttpClient.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

namespace fwfetch {

namespace fs = std::filesystem;

std::string releasesUrl() {
    const char* over = std::getenv("JAYECU_FIRMWARE_URL");
    return (over && *over) ? std::string(over) : std::string(kReleasesApi);
}

namespace {

const std::vector<jf::JHttpHeader> kHeaders = {
    { "User-Agent", "jayecu-studio/" STUDIO_VERSION },   // GitHub refuses requests without one
};

std::string httpWhy(const jf::JHttpResponse& r) {
    return r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
}

bool writeAll(const fs::path& p, const std::vector<uint8_t>& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

// Every release, not just the latest: an ECU keeps the firmware it has, however many releases later.
// The list URL is the latest-release URL with its tail changed (JAYECU_FIRMWARE_URL included, so the
// fake release server serves this too); an answer that is a single release is taken as a list of one.
std::string releasesListUrl() {
    std::string u = releasesUrl();
    const std::string tail = "/releases/latest";
    if (u.size() > tail.size() && u.compare(u.size() - tail.size(), tail.size(), tail) == 0)
        u = u.substr(0, u.size() - tail.size()) + "/releases?per_page=100";
    return u;
}

Result run(const std::vector<std::string>& boards, const std::vector<fwkits::Kit>& have,
           const std::string& studioVersion, const std::string& destRoot, bool includeBeta) {
    Result res;
    // WHICH RELEASES. Normally the latest release — GitHub's /releases/latest, which leaves out drafts and
    // pre-releases, so a beta is never offered to someone who did not ask. With the beta channel on, every
    // release, pre-releases included, and the newest kit for each board wins whichever release carries it.
    const std::string url = includeBeta ? releasesListUrl() : releasesUrl();
    const jf::JHttpResponse rel = jf::JHttpClient::getSync(url, 15000,
        { kHeaders[0], { "Accept", "application/vnd.github+json" } });
    if (!rel.error.empty()) { res.error = rel.error; return res; }
    if (rel.status == 404) { res.noReleases = true; return res; }
    if (!rel.ok()) { res.error = httpWhy(rel); return res; }

    const auto parsed = jf::JJson::tryParse(rel.text());
    if (!parsed || !(parsed->isObject() || parsed->isArray())) { res.error = "the answer was not a release"; return res; }
    const jf::JJson& doc = *parsed;                    // const: a non-const JJson inserts on a missing key

    std::vector<std::map<std::string, std::string>> releases;   // per release: asset name -> download URL
    auto take = [&](const jf::JJson& r) {
        if (r["draft"].isBool() && r["draft"].boolean()) return;
        if (!includeBeta && r["prerelease"].isBool() && r["prerelease"].boolean()) return;
        std::map<std::string, std::string> u;
        for (const auto& a : r["assets"].arr()) u[a["name"].str()] = a["browser_download_url"].str();
        releases.push_back(std::move(u));
        if (res.latest.empty()) res.latest = r["tag_name"].str();
    };
    if (doc.isObject()) take(doc); else for (const auto& r : doc.arr()) take(r);

    // The newest kit per board across those releases, by the version in its name — before downloading
    // anything, so the beta channel fetches one kit per board, not every beta ever published.
    struct Pick { size_t rel; std::string name, version; };
    std::map<std::string, Pick> pick;
    const std::string tail = "-kit.json";
    for (size_t ri = 0; ri < releases.size(); ++ri)
        for (const auto& [name, u] : releases[ri]) {
            if (name.size() <= tail.size() || name.compare(name.size() - tail.size(), tail.size(), tail) != 0) continue;
            for (const auto& b : boards) {
                if (name.rfind(b + "-", 0) != 0) continue;
                const std::string ver = name.substr(b.size() + 1, name.size() - b.size() - 1 - tail.size());
                if (!jf::JVersion::parse(ver).valid) continue;
                auto it = pick.find(b);
                if (it == pick.end() || jf::JVersion::parse(ver).isNewerThan(jf::JVersion::parse(it->second.version)))
                    pick[b] = { ri, name, ver };
            }
        }

    for (const auto& [boardName, pk] : pick) {
        const std::string* board = &boardName;
        std::map<std::string, std::string>& urls = releases[pk.rel];
        const std::string& name = pk.name;
        std::string sums;                              // this release's, fetched once and only if wanted
        auto sumsText = [&]() -> const std::string& {
            if (sums.empty() && urls.count("SHA256SUMS")) {
                const auto r = jf::JHttpClient::getSync(urls["SHA256SUMS"], 30000, kHeaders);
                if (r.ok()) sums = r.text();
            }
            return sums;
        };
        // Download one asset and prove it is the published file. Empty on any failure (`why` says which).
        auto fetchChecked = [&](const std::string& fname, std::string& why) -> std::vector<uint8_t> {
            if (!urls.count(fname)) { why = fname + " is not in the release"; return {}; }
            const std::string want = jf::JSha256::sumFor(sumsText(), fname);
            if (want.empty()) { why = "the release's checksums do not list " + fname; return {}; }
            const auto r = jf::JHttpClient::getSync(urls[fname], 10 * 60 * 1000, kHeaders);
            if (!r.ok()) { why = fname + ": " + httpWhy(r); return {}; }
            if (jf::JSha256::hex(r.body) != want) { why = fname + " does not match its checksum"; return {}; }
            return r.body;
        };
        std::string why;
        const std::vector<uint8_t> kitRaw = fetchChecked(name, why);
        if (kitRaw.empty()) { res.error = why; continue; }
        const auto kj = jf::JJson::tryParse(std::string(kitRaw.begin(), kitRaw.end()));
        if (!kj || !kj->isObject()) { res.error = name + " is not a kit label"; continue; }
        const jf::JJson& kit = *kj;
        const std::string version = kit["version"].str(), minStudio = kit["min_studio"].str();
        if (kit["board"].str() != *board || !jf::JVersion::parse(version).valid) {
            res.error = name + " does not describe a " + *board + " kit"; continue;
        }

        fwkits::Kit best;
        if (fwkits::newestFor(have, *board, studioVersion, best) && !fwkits::isNewerThan(version, best.version))
            continue;                                  // already have this or newer
        if (!minStudio.empty() &&
            jf::JVersion::parse(minStudio).isNewerThan(jf::JVersion::parse(studioVersion))) {
            res.needsStudio.push_back(*board + " " + version + " needs studio " + minStudio);
            continue;
        }

        // Every file the label names, each checked, before anything touches the kit folder.
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files = { { "kit.json", kitRaw } };
        bool ok = true;
        for (const char* key : { "firmware", "meta", "dashboard" }) {
            const std::string f = kit[key].str();
            if (f.empty()) {
                if (std::string(key) != "dashboard") { res.error = name + " names no " + key; ok = false; }
                continue;
            }
            if (f.find('/') != std::string::npos || f.find('\\') != std::string::npos) {
                res.error = name + " names a file outside the kit"; ok = false; break;
            }
            std::vector<uint8_t> data = fetchChecked(f, why);
            if (data.empty()) { res.error = why; ok = false; break; }
            files.emplace_back(f, std::move(data));
        }
        if (!ok) continue;

        // Write beside the destination, then rename into place: a kit folder is either whole or absent.
        std::error_code ec;
        const fs::path dest = fs::path(destRoot) / (*board + "-" + version);
        const fs::path tmp  = fs::path(destRoot) / (".partial-" + *board + "-" + version);
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        for (const auto& [fname, data] : files)
            if (!writeAll(tmp / fname, data)) { res.error = "cannot write " + (tmp / fname).string(); ok = false; break; }
        if (ok) {
            fs::remove_all(dest, ec);
            fs::rename(tmp, dest, ec);
            if (ec) { res.error = "cannot place the kit at " + dest.string() + ": " + ec.message(); ok = false; }
        }
        if (!ok) { fs::remove_all(tmp, ec); continue; }
        res.installed.push_back(*board + " " + version);
    }
    return res;
}

VersionFiles runVersion(const std::string& board, const std::string& version, const std::string& layoutHash,
                        bool wantMeta, bool wantDashboard) {
    VersionFiles out;
    const jf::JHttpResponse rel = jf::JHttpClient::getSync(releasesListUrl(), 15000,
        { kHeaders[0], { "Accept", "application/vnd.github+json" } });
    if (!rel.ok()) { out.error = rel.status == 404 ? "no firmware releases are published" : httpWhy(rel); return out; }
    const auto parsed = jf::JJson::tryParse(rel.text());
    if (!parsed) { out.error = "the answer was not a list of releases"; return out; }
    const jf::JJson& doc = *parsed;
    const std::string kitName = board + "-" + version + "-kit.json";

    std::map<std::string, std::string> urls;          // the release that carries this version's kit
    auto take = [&](const jf::JJson& r) {
        std::map<std::string, std::string> u;
        for (const auto& a : r["assets"].arr()) u[a["name"].str()] = a["browser_download_url"].str();
        if (u.count(kitName)) urls = std::move(u);
    };
    if (doc.isObject()) take(doc);
    else for (const auto& r : doc.arr()) { take(r); if (!urls.empty()) break; }
    if (urls.empty()) { out.error = "no release carries " + board + " " + version; return out; }

    std::string sums;
    if (urls.count("SHA256SUMS")) {
        const auto r = jf::JHttpClient::getSync(urls["SHA256SUMS"], 30000, kHeaders);
        if (r.ok()) sums = r.text();
    }
    auto fetchChecked = [&](const std::string& name, std::string& why) -> std::vector<uint8_t> {
        if (!urls.count(name)) { why = name + " is not in the release"; return {}; }
        const std::string want = jf::JSha256::sumFor(sums, name);
        if (want.empty()) { why = "the release's checksums do not list " + name; return {}; }
        const auto r = jf::JHttpClient::getSync(urls[name], 10 * 60 * 1000, kHeaders);
        if (!r.ok()) { why = name + ": " + httpWhy(r); return {}; }
        if (jf::JSha256::hex(r.body) != want) { why = name + " does not match its checksum"; return {}; }
        return r.body;
    };

    const std::vector<uint8_t> kitRaw = fetchChecked(kitName, out.error);
    if (kitRaw.empty()) return out;
    const auto kj = jf::JJson::tryParse(std::string(kitRaw.begin(), kitRaw.end()));
    if (!kj || !kj->isObject()) { out.error = kitName + " is not a kit label"; return out; }
    const jf::JJson& kit = *kj;
    if (kit["board"].str() != board || kit["version"].str() != version) {
        out.error = kitName + " does not describe " + board + " " + version; return out;
    }
    if (!layoutHash.empty() && kit["layout_hash"].str() != layoutHash) {
        out.error = board + " " + version + " in the release has layout " + kit["layout_hash"].str() +
                    ", this ECU has " + layoutHash;
        return out;
    }
    auto file = [&](const char* key, std::vector<uint8_t>& into) {
        const std::string f = kit[key].str();
        if (f.empty() || f.find('/') != std::string::npos || f.find('\\') != std::string::npos) return;
        std::string why;
        into = fetchChecked(f, why);
        if (into.empty() && out.error.empty()) out.error = why;
    };
    if (wantMeta)      file("meta", out.meta);
    if (wantDashboard) file("dashboard", out.dashboard);
    return out;
}

}  // namespace

void fetchVersion(std::string board, std::string version, std::string layoutHash,
                  bool wantMeta, bool wantDashboard, std::function<void(const VersionFiles&)> done) {
    std::thread([=] {
        VersionFiles r = runVersion(board, version, layoutHash, wantMeta, wantDashboard);
        jf::JMainThreadDispatcher::instance().post([r = std::move(r), done] { if (done) done(r); });
    }).detach();
}

void fetchLatest(std::vector<std::string> boards, std::vector<fwkits::Kit> have,
                 std::string studioVersion, std::string destRoot, bool includeBeta,
                 std::function<void(const Result&)> done) {
    std::thread([boards = std::move(boards), have = std::move(have), studioVersion = std::move(studioVersion),
                 destRoot = std::move(destRoot), includeBeta, done = std::move(done)] {
        Result r = run(boards, have, studioVersion, destRoot, includeBeta);
        jf::JMainThreadDispatcher::instance().post([r = std::move(r), done] { if (done) done(r); });
    }).detach();
}

}  // namespace fwfetch
