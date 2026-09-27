// WHICH FIELD HOLDS THE LUA SCRIPT — asked of the connected definition, not assumed.
//
// The Lua dock named "lua.source" in three places. That is jayecu's field; rusEFI calls its script
// ts.luaScript and gives it 48 KB rather than 4. So against a rusEFI ECU the dock showed an EMPTY
// editor, and its Apply button called setConfigString on a path that does not resolve — which returns
// without writing (Cache.cpp) — and then reported "Lua script applied". The script was right there in
// the definition the whole time.
//
// Cache::scriptPath() answers it from the meta, so the editor works on either firmware and the cap it
// enforces is that firmware's. This pins both halves against the two REAL definitions, because the
// bug was never in the logic — it was in believing one firmware's field name was the only one.
//
//   cmake --build build --target script_path_test
//   STUDIO_RUSEFI_META=<an imported rusEFI .meta> ./build/script_path_test   (that half skips without it)

#include "model/Cache.h"
#include "model/MetaModel.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[script] %-56s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// Load one definition, point the cache at it, and report what the resolver makes of it.
static void with_meta(const char* file, const char* who,
                      const std::string& want_path, size_t want_min_cap) {
    if (!file || !*file) {
        std::printf("[script] %-56s SKIP  (no definition given)\n", who);
        return;
    }
    MetaModel m;
    if (!m.loadFile(file)) {                       // a rusEFI import is not on every machine
        std::printf("[script] %-56s SKIP  (no %s)\n", who, file);
        return;
    }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    // A DEFINITION NEED NOT SHIP A DEFAULT IMAGE. jayecu's carries the shipped tune; an imported rusEFI
    // one does not — the bytes come from the ECU on connect. So the image is sized from the definition
    // when there is no default, or every write here fails its bounds check for want of a buffer.
    std::vector<uint8_t> img = m.defaultImage();
    if (img.size() < size_t(m.configSize())) img.resize(size_t(m.configSize()), 0);
    c.setConfigImage(img);

    const std::string got = c.scriptPath();
    ck(got == want_path, std::string(who) + ": resolves its own script field", got.empty() ? "(none)" : got);

    const size_t cap = c.scriptCapacity();
    ck(cap >= want_min_cap, std::string(who) + ": the cap comes from THAT definition",
       std::to_string(cap) + " chars");

    // The round trip has to work through the resolved path, or the editor is reading one field and
    // writing another — which is the shape of the original bug, not a fix for it.
    if (!got.empty()) {
        c.setConfigString(got, "-- hello\nfunction onTick() end\n");
        ck(c.configString(got) == "-- hello\nfunction onTick() end\n",
           std::string(who) + ": written and read back through the resolved path");
    }
}

int main() {
    with_meta(REAL_META,   "jayecu", "lua.source",   4000);
    with_meta(std::getenv("STUDIO_RUSEFI_META"), "rusEFI", "ts.luaScript", 40000);

    // AND THE CAPS ARE NOT THE SAME NUMBER. If they were, the editor would truncate a 48 KB rusEFI
    // script to jayecu's 4 KB on Apply and say it applied — silently losing most of the file.
    std::printf("\n[script] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
