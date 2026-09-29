#include "FirmwareKits.h"

#include <j/update/JVersion.h>   // one version rule for studio and firmware

#include <j/config/Json.h>

#include <algorithm>
#include <filesystem>

namespace fwkits {

namespace fs = std::filesystem;

std::vector<Kit> scan(const std::string& root, bool shipped) {
    std::vector<Kit> kits;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return kits;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;
        const fs::path dir = entry.path();
        const auto parsed = jf::JJson::tryParseFile((dir / "kit.json").string());
        if (!parsed || !parsed->isObject()) continue;
        const jf::JJson& j = *parsed;
        Kit k;
        k.board      = j["board"].str();
        k.version    = j["version"].str();
        k.build      = j["build"].str();
        k.layoutHash = j["layout_hash"].str();
        k.minStudio  = j["min_studio"].str();
        k.dir        = dir.string();
        k.shipped    = shipped;
        for (const jf::JJson& n : j["notes"].arr()) {
            Note note{ n["version"].str(), {} };
            for (const jf::JJson& c : n["changes"].arr()) if (!c.str().empty()) note.changes.push_back(c.str());
            if (jf::JVersion::parse(note.version).valid) k.notes.push_back(std::move(note));
        }
        std::sort(k.notes.begin(), k.notes.end(), [](const Note& a, const Note& b) {
            return jf::JVersion::parse(a.version).isNewerThan(jf::JVersion::parse(b.version)); });
        auto file = [&](const char* key) -> std::string {
            const std::string name = j[key].str();
            if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
                return {};                                       // a bare file name inside the kit, nothing else
            const fs::path p = dir / name;
            return fs::is_regular_file(p, ec) ? p.string() : std::string();
        };
        k.firmware  = file("firmware");
        k.meta      = file("meta");
        k.dashboard = file("dashboard");
        // The dashboard is optional (a board may not have an authored one yet); firmware and meta are not.
        if (k.board.empty() || !jf::JVersion::parse(k.version).valid || k.firmware.empty() || k.meta.empty())
            continue;
        kits.push_back(std::move(k));
    }
    return kits;
}

bool newestFor(const std::vector<Kit>& kits, const std::string& board, const std::string& studioVersion,
               Kit& out) {
    const jf::JVersion studio = jf::JVersion::parse(studioVersion);
    const Kit* best = nullptr;
    for (const Kit& k : kits) {
        if (k.board != board) continue;
        if (!k.minStudio.empty() && studio.valid &&
            jf::JVersion::parse(k.minStudio).isNewerThan(studio))
            continue;                                            // needs a newer studio than this one
        if (!best) { best = &k; continue; }
        const auto v = jf::JVersion::parse(k.version), b = jf::JVersion::parse(best->version);
        if (v.isNewerThan(b) || (!b.isNewerThan(v) && best->shipped && !k.shipped))
            best = &k;
    }
    if (!best) return false;
    out = *best;
    return true;
}

bool isNewerThan(const std::string& kitVersion, const std::string& ecuVersion) {
    const auto k = jf::JVersion::parse(kitVersion), e = jf::JVersion::parse(ecuVersion);
    return k.valid && e.valid && k.isNewerThan(e);
}

std::vector<Note> notesSince(const Kit& kit, const std::string& ecuVersion) {
    const jf::JVersion from = jf::JVersion::parse(ecuVersion), to = jf::JVersion::parse(kit.version);
    std::vector<Note> out;
    for (const Note& n : kit.notes) {
        const jf::JVersion v = jf::JVersion::parse(n.version);
        if (v.isNewerThan(to)) continue;                               // not in this kit
        if (from.valid ? v.isNewerThan(from) : n.version == kit.version) out.push_back(n);
    }
    return out;
}

}  // namespace fwkits
