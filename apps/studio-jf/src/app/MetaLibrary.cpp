#include "MetaLibrary.h"

#include "Resources.h"
#include "model/FirmwareKits.h"
#include "model/MetaModel.h"
#include "model/StudioPaths.h"

#include <j/config/Json.h>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace metalib {

std::string forLayoutHash(const std::string& hash) {
    if (hash.empty()) return {};
    namespace fs = std::filesystem;
    const fs::path lib = StudioPaths::dataDir("meta");
    std::error_code ec;
    // THE NAME CARRIES THE HASH. A meta is saved as "<board> <layout_hash>.meta", so the library can
    // be searched by looking at the directory. This used to PARSE every meta in it — 16 files, 23 MB
    // of JSON — to read one string out of each, on every launch, and that was most of the wait
    // between the studio's window appearing and its interface arriving with it.
    //
    // Parsing is still the answer for a file whose name does not name its hash: an older library, or
    // one somebody has renamed. It just is not the FIRST answer any more.
    std::vector<std::string> unnamed;
    for (const auto& f : fs::directory_iterator(lib, ec)) {
        if (f.path().extension() != ".meta") continue;
        const std::string stem = f.path().stem().string();
        if (stem.size() >= hash.size()
            && stem.compare(stem.size() - hash.size(), hash.size(), hash) == 0
            && (stem.size() == hash.size() || stem[stem.size() - hash.size() - 1] == ' '))
            return f.path().string();
        unnamed.push_back(f.path().string());
    }
    for (const std::string& path : unnamed) {
        const auto j = jf::JJson::tryParseFile(path);
        if (!j || !j->isObject()) continue;
        const jf::JJson& doc = *j;                     // const: the mutating operator[] would rewrite it
        if (doc["meta"]["layout_hash"].str() == hash) return path;
    }
    // A kit's kit.json names its layout, so a studio that has never connected to this firmware still
    // has its definition if it carries the kit.
    std::vector<fwkits::Kit> kits = fwkits::scan(resources::exeDir() + "firmware", true);
    const std::vector<fwkits::Kit> dl = fwkits::scan(StudioPaths::dataDir("firmware"), false);
    kits.insert(kits.end(), dl.begin(), dl.end());
    for (const fwkits::Kit& k : kits)
        if (k.layoutHash == hash && fs::exists(k.meta, ec)) return k.meta;
    return {};
}

const MetaModel* loadedFor(const std::string& hash) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<MetaModel>> cache;   // a miss is cached too (null)
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(hash);
    if (it != cache.end()) return it->second.get();
    std::unique_ptr<MetaModel> m;
    const std::string path = forLayoutHash(hash);
    if (!path.empty()) {
        m = std::make_unique<MetaModel>();
        if (!m->loadFile(path) || m->layoutHash() != hash) m.reset();
    }
    return (cache[hash] = std::move(m)).get();
}

}  // namespace metalib
