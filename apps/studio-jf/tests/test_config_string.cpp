// A STRING FIELD INSIDE A STRUCT ARRAY IS STILL A FIELD.
//
// Cache::configString/setConfigString looked their path up in the flat config map, which holds the
// DECLARED fields — so a string that only exists per array element was invisible to them. An output
// slot's name is exactly that: outputs.output[3].name resolves through resolveBlob, the same route an
// expression program takes, and appears nowhere in that map. The Output Setup page's Name box showed
// nothing and swallowed every keystroke, because the write had no field to land in and failed silently.
//
//   cmake --build build --target config_string_test && ./build/config_string_test

#include "model/Cache.h"
#include "model/MetaModel.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[cfgstr] %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    // The path the Output Setup page binds, with the slot index already resolved.
    const std::string path = "outputs.output[3].name";
    ck(!m.config().count(path),
       "the flat config map does NOT hold this path", "which is why the old lookup failed");

    c.setConfigString(path, "Fuel Pump");
    ck(c.configString(path) == "Fuel Pump", "written, then read back", c.configString(path));

    // A neighbouring element must be untouched — the offset arithmetic is the whole risk here.
    ck(c.configString("outputs.output[4].name").empty(), "the next slot is untouched");

    // Longer than the field: truncated, and still NUL-terminated (never a run-on into the next field).
    const std::string longer(200, 'x');
    c.setConfigString(path, longer);
    const std::string got = c.configString(path);
    ck(!got.empty() && got.size() < longer.size(), "an over-long name is truncated to the field",
       std::to_string(got.size()) + " chars");
    ck(c.configString("outputs.output[4].name").empty(), "…and does not spill into the next slot");

    c.setConfigString(path, "");
    ck(c.configString(path).empty(), "cleared");

    std::printf(fails ? "\n[cfgstr] %d FAILED\n" : "\n[cfgstr] all passed\n", fails);
    return fails ? 1 : 0;
}
