// Headless regression for BIT-PACKED config fields: several flags share one word, so each must decode its
// own bit range and — the part that actually corrupts a tune — a write must preserve the bits either side.
//
// This is the shape a TunerStudio ini uses everywhere:
//     isInjectionEnabled = bits, U32, 1356, [0:0], "false", "true"
//     isIgnitionEnabled  = bits, U32, 1356, [1:1], "false", "true"
// Two fields, ONE word. Before the bit range was carried through the model, both decoded the whole word
// (so every flag in it read the same number) and writing either one zeroed the rest.

#include "model/MetaModel.h"
#include "model/Cache.h"
#include "model/TuneFile.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(x) do { const bool _ok = (x); std::printf("[bits] %-62s %s\n", #x, _ok ? "PASS" : "FAIL"); \
                      if (!_ok) ++fails; } while (0)

// A minimal meta: one 32-bit word at offset 0 carrying a bool at bit 0, a bool at bit 1, and a 3-bit
// enum at bits 4..6 — plus an ordinary scalar for the unpacked path.
static const char *kMeta = R"({
  "meta": { "layout_hash": "bitstest", "config_size": 64 },
  "segments": [ {"id":"pc","base":1342177280,"size":64,"kind":"host","writable":true} ],
  "config": { "t": {
     "flagA":  {"type":"scalar","offset":0,"datatype":"U32","scale":1,"bitLo":0,"bitHi":0,"kind":"bool","min":0,"max":1},
     "flagB":  {"type":"scalar","offset":0,"datatype":"U32","scale":1,"bitLo":1,"bitHi":1,"kind":"bool","min":0,"max":1},
     "mode":   {"type":"scalar","offset":0,"datatype":"U32","scale":1,"bitLo":4,"bitHi":6,"min":0,"max":7},
     "plain":  {"type":"scalar","offset":8,"datatype":"U16","scale":0.1,"min":0,"max":100},
     "hostval": {"type":"scalar","offset":1342177280,"datatype":"U16","scale":1,"min":0,"max":1000}
  } },
  "telemetry": {}
})";

int main()
{
    const std::string metaPath = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                               + "/jf_bits_test.meta";
    // A .meta carries a zlib CRC32 footer (the loader rejects a truncated/corrupt transfer), so the
    // fixture has to be written the same way a real one is.
    {
        const std::string body(kMeta);
        uint32_t crc = 0xFFFFFFFFu;
        for (unsigned char b : body) {
            crc ^= b;
            for (int i = 0; i < 8; ++i) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
        crc ^= 0xFFFFFFFFu;
        std::ofstream f(metaPath, std::ios::binary);
        f << body;
        for (int i = 0; i < 4; ++i) f.put(static_cast<char>((crc >> (8 * i)) & 0xFF));
    }
    MetaModel meta;
    if (!meta.loadFile(metaPath)) { std::printf("[bits] meta failed to load\n"); return 2; }

    CHECK(meta.config().count("t.flagA") == 1);
    const auto &fa = meta.config().at("t.flagA");
    const auto &md = meta.config().at("t.mode");
    CHECK(fa.bits.packed() && fa.bits.lo == 0 && fa.bits.hi == 0);
    CHECK(md.bits.packed() && md.bits.lo == 4 && md.bits.hi == 6);
    CHECK(md.bits.mask() == 0x7u);                       // a 3-bit field holds 0..7
    CHECK(!meta.config().at("t.plain").bits.packed());      // an ordinary scalar is not a bit range

    Cache &c = Cache::instance();
    c.setMeta(&meta);
    c.setConfigImage(std::vector<uint8_t>(64, 0));   // a zeroed tune image to write into

    // Set each field in turn; every one must survive the others being written.
    c.setConfigValue("t.flagA", 1);
    CHECK(c.configValue("t.flagA") == 1);

    c.setConfigValue("t.flagB", 1);
    CHECK(c.configValue("t.flagB") == 1);
    CHECK(c.configValue("t.flagA") == 1);              // <- the neighbour a whole-word write would have zeroed

    c.setConfigValue("t.mode", 5);
    CHECK(c.configValue("t.mode")  == 5);
    CHECK(c.configValue("t.flagA") == 1);
    CHECK(c.configValue("t.flagB") == 1);

    // Clearing one bit must leave the others alone.
    c.setConfigValue("t.flagA", 0);
    CHECK(c.configValue("t.flagA") == 0);
    CHECK(c.configValue("t.flagB") == 1);
    CHECK(c.configValue("t.mode")  == 5);

    // The word itself: flagB (bit 1) + mode 5 (bits 4..6) = 0b0101'0010 = 0x52.
    CHECK(c.meta()->locate("t.plain").valid());   // resolveField went with the FieldRef family
    CHECK(c.readAt(0, "U32", 1.0) == 0x52);            // whole-word read sees exactly the packed bits

    // A value wider than the range is masked, not allowed to bleed into the next field.
    c.setConfigValue("t.mode", 7);
    CHECK(c.configValue("t.mode") == 7);
    CHECK(c.readAt(0, "U32", 1.0) == 0x72);            // bits 4..6 all set, flagB still there

    // The unpacked path is untouched by any of this.
    c.setConfigValue("t.plain", 95);                   // RAW in, RAW out (0.1 scale is the widget's job)
    CHECK(c.configValue("t.plain") == 95);
    // min/max are authored in the SAME raw counts the bytes hold (max 100 == 10.0 units at 0.1), and
    // every door to the bytes clamps — an over-range write saturates rather than wrapping.
    c.setConfigValue("t.plain", 125);
    CHECK(c.configValue("t.plain") == 100);
    CHECK(c.readAt(0, "U32", 1.0) == 0x72);             // the packed word is untouched by a different field

    // ---- width: a packed field in a U16/U08 word must not read past its own storage ----------------
    // A SIGNED small word decodes to a negative double, which sign-extends to 1-bits above the word. Only
    // masking to the field's width keeps a range near the top honest.
    CHECK((BitRange{15, 15}).extract(-1.0, 2) == 1);      // S16 = 0xFFFF -> top bit set
    CHECK((BitRange{15, 15}).extract(-1.0, 4) == 1);
    CHECK((BitRange{7, 7}).extract(-1.0, 1) == 1);        // S08 = 0xFF
    CHECK((BitRange{3, 0}).packed() == false);            // hi < lo is not a range
    CHECK((BitRange{}).extract(1234.0) == 1234.0);        // unset = the whole word, untouched

    // ---- byte order: the definition declares it, and the codec must honour it --------------------------
    // 0x1234 stored big-endian is bytes 12 34; read little-endian it is 0x3412 — a plausible-looking wrong
    // number, which is exactly why an ignored `endianness` declaration is dangerous rather than untidy.
    {
        unsigned char buf[4] = { 0x12, 0x34, 0x56, 0x78 };
        CHECK(MetaModel::decodeRaw("U16", buf, /*be=*/true)  == 0x1234);
        CHECK(MetaModel::decodeRaw("U16", buf, /*be=*/false) == 0x3412);
        CHECK(MetaModel::decodeRaw("U32", buf, /*be=*/true)  == 0x12345678);
        CHECK(MetaModel::decodeRaw("U32", buf, /*be=*/false) == 0x78563412);

        unsigned char out[4] = {0};
        MetaModel::encodeRaw("U32", out, 0x12345678, /*be=*/true);
        CHECK(out[0] == 0x12 && out[3] == 0x78);                       // round trip, big-endian
        CHECK(MetaModel::decodeRaw("U32", out, true) == 0x12345678);

        // And a packed field on top of a big-endian word: bits are of the DECODED value, so the two
        // concerns compose rather than interfering.
        unsigned char w[2] = { 0x00, 0x52 };                            // BE 0x0052 = flagB + mode 5
        CHECK((BitRange{1, 1}).extract(MetaModel::decodeRaw("U16", w, true), 2) == 1);
        CHECK((BitRange{4, 6}).extract(MetaModel::decodeRaw("U16", w, true), 2) == 5);
    }

    // ---- HOST (PcVariable) segment: addressed like config, never synced to the ECU ---------------------
    // The theory this follows: meta declares PATHS, the tune carries VALUES, the dashboard holds CONTROLS.
    // A PcVariable is therefore an ordinary config path whose bytes live in a block the ECU knows nothing
    // about — so it resolves, reads and writes exactly like any other field, with no new address space,
    // no new sigil and no special case in any widget.
    {
        c.initSegments();                                   // allocates the host block from the meta

        // Resolves like any other config path, and lands in the HOST segment (base 0x50000000) rather
        // than the config image — which is the whole point of the pc block.
        CHECK(c.meta()->locate("t.hostval").valid());
        CHECK(c.meta()->locate("t.hostval").offset == 1342177280);

        c.flushWrites();                                    // drain what the earlier config edits queued
        CHECK(!c.hasPendingWrites());

        c.setConfigValue("t.hostval", 700);
        CHECK(c.configValue("t.hostval") == 700);           // reads back through the ordinary path
        CHECK(!c.hasPendingWrites());                       // ...but queued NOTHING for the ECU

        // An ordinary config edit still queues, so the host rule is not a blanket "stop writing".
        c.setConfigValue("t.plain", 42);
        CHECK(c.hasPendingWrites());
    }

    // ---- PcVariables: declared as an OVERLAY, valued in the tune ---------------------------------------
    // The generated meta cannot be hand-edited, so a project's host-side variables arrive as an overlay
    // applied after load. They become ordinary config paths in a host segment — and because a tune is
    // keyed by PATH, inserting or reordering a variable never moves anyone else's value.
    {
        std::vector<MetaModel::PcVar> vars;
        MetaModel::PcVar a; a.name = "boost_target_offset"; a.datatype = "S16"; a.scale = 0.1;
                            a.minV = -500; a.maxV = 500; a.units = "kPa";   // RAW counts: +/-50.0 kPa
        MetaModel::PcVar b; b.name = "workshop_mode"; b.datatype = "U08"; b.kind = "bool"; b.maxV = 1;
        vars.push_back(a); vars.push_back(b);
        meta.applyPcVars(vars);

        CHECK(meta.config().count("pc.boost_target_offset") == 1);
        CHECK(meta.config().count("pc.workshop_mode") == 1);
        CHECK(meta.config().at("pc.boost_target_offset").offset == int(MetaModel::kPcSegmentBase));
        bool hostSeg = false;
        for (const auto &sg : meta.segments()) if (sg.kind == "host") hostSeg = true;
        CHECK(hostSeg);

        c.initSegments();
        c.setConfigValue("pc.boost_target_offset", 120);     // RAW counts (0.1 kPa each)
        c.setConfigValue("pc.workshop_mode", 1);
        CHECK(c.configValue("pc.boost_target_offset") == 120);
        c.setConfigValue("pc.boost_target_offset", 9000);    // an overlay field clamps like any other
        CHECK(c.configValue("pc.boost_target_offset") == 500);
        c.setConfigValue("pc.boost_target_offset", 120);
        CHECK(c.configValue("pc.workshop_mode") == 1);

        // The values must survive a tune save/load, since the tune is where values live.
        std::vector<uint8_t> hostBytes;
        for (const auto &sg : meta.segments())
            if (sg.kind == "host") { hostBytes.assign(size_t(sg.size), 0); }
        // read them back out of the cache block the same way the app would
        for (size_t i = 0; i < hostBytes.size(); ++i)
            hostBytes[i] = 0;
        MetaModel::encodeRaw("S16", hostBytes.data(), 120);
        MetaModel::encodeRaw("U08", hostBytes.data() + 2, 1);

        const auto doc = TuneFile::serialise(c.configImage(), meta, &hostBytes);
        CHECK(!doc.empty());
        MigrationReport rep;
        std::vector<uint8_t> restored;
        TuneFile::deserialise(doc, meta, rep, &restored);
        CHECK(restored.size() == hostBytes.size());
        CHECK(MetaModel::decodeRaw("S16", restored.data()) == 120);
        CHECK(MetaModel::decodeRaw("U08", restored.data() + 2) == 1);

        // Removing a variable removes its PATH — no stale resolvable field left behind.
        vars.pop_back();
        meta.applyPcVars(vars);
        CHECK(meta.config().count("pc.workshop_mode") == 0);
        CHECK(meta.config().count("pc.boost_target_offset") == 1);
    }

    // ---- STRING config fields: text in the image, text in the tune ------------------------------------
    // A VIN or an engine name is an ordinary config path whose bytes are ASCII. Decoding one as a number
    // would save its first four characters as a float and restore garbage over the rest, so the round trip
    // is what this pins.
    {
        MetaModel sm;
        const std::string sp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                             + "/jf_str_test.meta";
        const std::string body = R"({
          "meta": { "layout_hash": "strtest", "config_size": 64 },
          "config": { "t": { "vin": {"type":"scalar","datatype":"ASCII","offset":0,"size":18} } },
          "telemetry": {}
        })";
        uint32_t crc = 0xFFFFFFFFu;
        for (unsigned char b : body) { crc ^= b; for (int i = 0; i < 8; ++i) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1); }
        crc ^= 0xFFFFFFFFu;
        { std::ofstream f(sp, std::ios::binary); f << body; for (int i = 0; i < 4; ++i) f.put(char((crc >> (8 * i)) & 0xFF)); }
        CHECK(sm.loadFile(sp));
        CHECK(sm.config().count("t.vin") == 1);
        CHECK(sm.config().at("t.vin").isText());

        Cache &sc = Cache::instance();
        sc.setMeta(&sm);
        sc.setConfigImage(std::vector<uint8_t>(64, 0));
        sc.setConfigString("t.vin", "WBA3B5C50DF123456");
        CHECK(sc.configString("t.vin") == "WBA3B5C50DF123456");
        CHECK(sc.configString("t.vin").size() == 17);          // fits, with room for the terminator

        const auto doc = TuneFile::serialise(sc.configImage(), sm);
        const std::string docText(doc.begin(), doc.end());
        CHECK(docText.find("WBA3B5C50DF123456") != std::string::npos);   // saved as TEXT, not as a number

        MigrationReport rep2;
        sc.setConfigImage(std::vector<uint8_t>(64, 0));
        CHECK(sc.configString("t.vin").empty());               // cleared...
        sc.setConfigImage(TuneFile::deserialise(doc, sm, rep2));        // ...and restored by the load
        CHECK(sc.configString("t.vin") == "WBA3B5C50DF123456");
    }

    std::printf("[bits] %d failure(s)\n", fails);
    return fails;
}
