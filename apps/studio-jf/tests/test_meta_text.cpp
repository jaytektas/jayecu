// Text fields have ONE spelling: a scalar whose datatype is ASCII. `type` is the SHAPE
// (scalar/table/array), `datatype` is the ENCODING — and a string is a scalar of ASCII, not a shape of
// its own. This pins that through BOTH producers, because one producer quietly diverging is the whole
// bug: codegen emitted scalar+ASCII while the TS importer emitted a `type: "string"` of its own,
// MetaModel only recognised the latter, and every native text field silently became a NUMBER. The
// visible symptom was a spin box where a VIN belongs; the expensive one was TuneFile saving the first
// four characters as a float and restoring garbage over the other twenty-eight.
//
//   ./build/meta_text_test [path-to-meta.json]   — also checks the real shipped meta when it finds one
//
#include "../src/model/MetaModel.h"
#include "../src/model/TsIniImporter.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("[meta-text] %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

// loadFile refuses a meta whose trailing little-endian CRC32 does not match, so a test fixture has to
// carry a real one. Writing it here pins that framing too — a meta is its JSON plus that footer.
static uint32_t crc32_buf(const std::string& s) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
    }
    return ~crc;
}

static std::string writeMeta(const std::string& json, const char* name) {
    std::string path = std::string("/tmp/") + name;
    std::ofstream f(path, std::ios::binary);
    f.write(json.data(), static_cast<std::streamsize>(json.size()));
    const uint32_t c = crc32_buf(json);
    for (int i = 0; i < 4; ++i) { const char b = static_cast<char>((c >> (8 * i)) & 0xFF); f.write(&b, 1); }
    return path;
}

// --- 1. The canonical shape classifies as text --------------------------------------------------
static void testCanonicalShape() {
    const std::string json = R"({
      "meta": { "board": "t", "layout_hash": "0" },
      "config": { "vehicle": {
        "VIN":  { "type": "scalar", "datatype": "ASCII", "offset": 100, "size": 32, "label": "VIN" },
        "Revs": { "type": "scalar", "datatype": "U16",   "offset": 200, "size": 2,  "label": "Revs" }
      } } })";
    MetaModel m;
    check(m.loadFile(writeMeta(json, "jf_metatext_canon.json")), "canonical meta loads");
    const auto& c = m.config();
    auto vin = c.find("vehicle.VIN"), revs = c.find("vehicle.Revs");
    check(vin  != c.end() &&  vin->second.isText(),  "scalar + ASCII -> text");
    check(revs != c.end() && !revs->second.isText(), "scalar + U16   -> NOT text");
    check(vin  != c.end() &&  vin->second.size == 32, "ASCII field keeps its declared 32-byte size");
}

// --- 2. A size-less descriptor sizes from its datatype, not from "" ------------------------------
static void testSizeFromDatatype() {
    const std::string json = R"({
      "meta": { "board": "t", "layout_hash": "0" },
      "config": { "m": { "N": { "type": "scalar", "datatype": "U16", "offset": 0, "label": "N" } } } })";
    MetaModel m;
    check(m.loadFile(writeMeta(json, "jf_metatext_size.json")), "meta with a size-less scalar loads");
    auto it = m.config().find("m.N");
    // The datatype used to be read one line BEFORE it was assigned, so this sized from "" and got 0.
    check(it != m.config().end() && it->second.size == 2, "size-less U16 sizes from its datatype (2)");
}

// --- 3. The TS importer emits that same shape ----------------------------------------------------
static void testImporterAgrees() {
    const std::string ini =
        "[Constants]\n"
        "page = 1\n"
        "   vinNumber = string, ASCII, 100, 32\n"
        "   revLimit  = scalar, U16, 200, \"rpm\", 1, 0, 0, 10000, 0\n";
    const jf::JJson meta = TsIniImporter::importText(ini);
    const jf::JJson& fld = meta["config"]["ts"]["vinNumber"];   // the importer files constants under "ts"
    // The point of the exercise: the importer must not invent a second spelling.
    check(fld["type"].str()     == "scalar", "importer emits type 'scalar' for a string constant");
    check(fld["datatype"].str() == "ASCII",  "importer emits datatype 'ASCII' for a string constant");
    check(fld["size"].number<int>() == 32,   "importer carries the declared length");

    MetaModel m;
    check(m.loadFile(writeMeta(meta.dump(), "jf_metatext_import.json")), "imported meta loads");
    auto it = m.config().find("ts.vinNumber");
    check(it != m.config().end() && it->second.isText(), "imported string -> text (same verdict as native)");
}

// --- 4. Whatever actually ships -------------------------------------------------------------------
static void testShippedMeta(const char* path) {
    MetaModel m;
    // Run from the build dir or the source dir — try both rather than skipping the most valuable check
    // (the meta that actually ships) because of a relative path.
    const char* candidates[] = { path, "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : candidates) if (c && m.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::printf("[meta-text] (no shipped meta found — skipped)\n"); return; }
    const char* keys[] = { "vehicle.Make", "vehicle.Model", "vehicle.Engine",
                           "vehicle.Notes", "vehicle.VIN",  "vehicle.Name" };
    bool all = true;
    for (const char* k : keys) {
        auto it = m.config().find(k);
        if (it == m.config().end() || !it->second.isText()) { all = false; std::printf("            %s is NOT text\n", k); }
    }
    check(all, "shipped meta: every config.vehicle field is text");
}

int main(int argc, char** argv) {
    testCanonicalShape();
    testSizeFromDatatype();
    testImporterAgrees();
    testShippedMeta(argc > 1 ? argv[1] : "../../shared/tuneit-meta.json");
    std::printf("\n[meta-text] %s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
