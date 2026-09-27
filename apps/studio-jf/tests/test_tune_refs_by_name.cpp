// A tune names what it points AT.
//
// The document has always addressed fields symbolically ("h_bridge.half[0].enable_sig") and then, until
// format 2, described their contents positionally (137). Those two halves age differently: a path survives
// anything, an ordinal survives only until the list it indexes gains a member. Declaring one signal
// (`app_state`) shifted 179 SignalIds, and a tune carried across that boundary bound half-bridge A's enable
// to etb_en_2 instead of etb_en_1 — the bridge never enabled, the throttle would not move in either
// direction, and nothing detected it because no config byte had moved. layout_hash cannot see a renumber.
//
// Format 2 stores a reference as what it MEANS. These tests hold that line: selectors and enum options go
// out as names, come back resolved against whatever catalog is in front of them, and a name this firmware
// does not have is reported as one field rather than silently binding somewhere plausible.
//
//   cmake --build build --target tune_refs_test && ./build/tune_refs_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/TuneFile.h"

#include <j/config/Json.h>

#include <fstream>
#include <cstdlib>

#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-70s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    // The meta bytes, minus the 4-byte CRC footer, so the shifted-catalog case below can rewrite them.
    std::vector<uint8_t> realMeta;
    { std::ifstream f(REAL_META, std::ios::binary);
      realMeta.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      if (realMeta.size() > 4) realMeta.resize(realMeta.size() - 4); }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    // Bind the very field that broke: half-bridge A's enable, and a table axis source.
    const std::string ENSIG = "h_bridge.half[0].enable_sig";
    const std::string XSRC  = "app.pedal_to_throttle_table_x_src";
    const int enId  = m.signalMap().count("etb_en_1") ? m.signalMap().at("etb_en_1") : -1;
    const int appId = m.signalMap().count("app_1")    ? m.signalMap().at("app_1")    : -1;
    ck(enId >= 0 && appId >= 0, "the catalog has etb_en_1 and app_1",
       std::to_string(enId) + "/" + std::to_string(appId));
    c.setConfigValue(ENSIG, enId);
    c.setConfigValue(XSRC,  appId);

    std::puts("=== a reference is written as a name ===");
    const std::vector<uint8_t> doc = TuneFile::serialise(c.configImage(), m, nullptr);
    ck(!doc.empty(), "the tune serialises");
    const std::string text(doc.begin(), doc.end());
    ck(text.find("\"jayecu-tune/2\"") != std::string::npos, "…as format 2");
    ck(text.find("\"etb_en_1\"") != std::string::npos,
       "…and the half-bridge enable is stored as 'etb_en_1', not a number");
    ck(text.find("\"app_1\"") != std::string::npos,
       "…and the pedal table's X source as 'app_1'");
    // The ordinal must be GONE from those keys — a number here is the bug this format exists to kill.
    {
        jf::JJson root = jf::JJson::parse(text);
        const jf::JJson& sc = root["scalars"];
        const jf::JJson& ar = root["arrays"];
        const jf::JJson& x  = sc[XSRC].isNull() ? ar[XSRC] : sc[XSRC];
        const jf::JJson& e  = sc[ENSIG].isNull() ? ar[ENSIG] : sc[ENSIG];
        ck(x.isString(), "the X source is a STRING in the document", x.isString() ? x.str() : "number");
        ck(e.isString(), "the enable selector is a STRING in the document", e.isString() ? e.str() : "number");
    }

    std::puts("");
    std::puts("=== and comes back as the same binding ===");
    {
        MigrationReport rep;
        std::vector<uint8_t> img = TuneFile::deserialise(doc, m, rep, nullptr);
        ck(!img.empty(), "the document loads");
        Cache& c2 = Cache::instance();
        c2.setConfigImage(img);
        ck(int(c2.configValue(ENSIG)) == enId, "the enable selector resolves back to etb_en_1",
           std::to_string(int(c2.configValue(ENSIG))));
        ck(int(c2.configValue(XSRC)) == appId, "the X source resolves back to app_1",
           std::to_string(int(c2.configValue(XSRC))));
        ck(rep.unresolved.empty(), "nothing unresolved",
           rep.unresolved.empty() ? "" : rep.unresolved.front());
    }

    std::puts("");
    std::puts("=== a RENUMBERED catalog cannot move the binding (the whole point) ===");
    {
        // A catalog where every SignalId has moved — the situation that caused the original failure:
        // declaring one signal shifted 179 ids, and a tune carried across bound the half-bridge enable to
        // its neighbour. Built by shifting the meta's signal map, which is what a firmware update does.
        std::string body(reinterpret_cast<const char*>(realMeta.data()), realMeta.size());
        const std::string tag = "\"signals\":";
        const auto sigAt = body.find(tag);
        bool built = false;
        std::string shiftedPath;
        if (sigAt != std::string::npos) {
            jf::JJson root = jf::JJson::parse(body);
            jf::JJson sigs = jf::JJson::object();
            for (const auto& [nm, v] : root["signals"].obj())
                sigs[nm] = jf::JJson(int(v.number()) + 1);      // every id moves
            root["signals"] = sigs;
            const std::string out = root.dump();
            shiftedPath = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                        + "/jf_shifted_catalog.meta";
            std::ofstream f(shiftedPath, std::ios::binary);
            f << out;
            uint32_t crc = 0xFFFFFFFFu;                          // the loader checks a zlib CRC32 footer
            for (unsigned char b : out) {
                crc ^= b;
                for (int k = 0; k < 8; ++k) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
            }
            crc ^= 0xFFFFFFFFu;
            for (int k = 0; k < 4; ++k) f.put(char((crc >> (8 * k)) & 0xFF));
            f.close();
            built = true;
        }
        MetaModel shifted;
        ck(built && shifted.loadFile(shiftedPath), "a catalog with every signal id shifted loads");
        const int newEn = shifted.signalMap().count("etb_en_1") ? shifted.signalMap().at("etb_en_1") : -1;
        ck(newEn == enId + 1, "…and etb_en_1 really did move",
           std::to_string(enId) + " -> " + std::to_string(newEn));

        MigrationReport rep;
        std::vector<uint8_t> img = TuneFile::deserialise(doc, shifted, rep, nullptr);
        ck(!img.empty(), "the tune written against the OLD catalog loads against the new one");
        Cache& c3 = Cache::instance();
        c3.setConfigImage(img);
        // The binding follows the NAME to its new id. Positionally it would still hold the old number,
        // which on this catalog is a different signal entirely — the exact bug.
        ck(int(c3.configValue(ENSIG)) == newEn,
           "the enable follows etb_en_1 to its NEW id, not the old number",
           std::to_string(int(c3.configValue(ENSIG))) + " want " + std::to_string(newEn));
        ck(int(c3.configValue(ENSIG)) != enId, "…and is NOT left pointing at the old ordinal");
    }

    std::puts("");
    std::puts("=== a board pin is named too, not stored as a pool index ===");
    {
        // sensors.sensor[].source carries no `kind` — it is a gated picker of board pins. Its number is an
        // index into the BOARD's pool, so it ages exactly like a SignalId: change the pool and the sensor
        // reads a different physical pin, with nothing to say so. Labels are unique across a field's sets,
        // so "AV1" resolves without knowing the interface that gates it.
        const std::string SRC = "sensors.sensor[clt].source";
        const MetaModel::Location L = m.locate(SRC);
        if (L.valid() && !L.pickerSets.empty()) {
            Cache& cc = Cache::instance();
            cc.setConfigImage(m.defaultImage());
            const int want = L.pickerSets.front().values.empty() ? 0 : L.pickerSets.front().values.front();
            const std::string wantLbl = L.pickerSets.front().options.front();
            cc.setConfigValue(SRC, want);

            const std::vector<uint8_t> d2 = TuneFile::serialise(cc.configImage(), m, nullptr);
            const std::string t2(d2.begin(), d2.end());
            ck(t2.find("\"" + wantLbl + "\"") != std::string::npos,
               "the sensor's source is stored as its pin label ('" + wantLbl + "')");

            MigrationReport rep;
            std::vector<uint8_t> img2 = TuneFile::deserialise(d2, m, rep, nullptr);
            Cache& c4 = Cache::instance();
            c4.setConfigImage(img2);
            ck(int(c4.configValue(SRC)) == want, "…and resolves back to the same pin",
               std::to_string(int(c4.configValue(SRC))));
        } else {
            ck(false, "sensors.sensor[clt].source resolves with picker sets",
               L.valid() ? "no picker sets" : "path did not resolve");
        }
    }

    std::puts("");
    std::puts("=== array elements are addressed by their stable id, not their position ===");
    {
        // An index is a position. Remove one sensor from the catalog — as the knock row was — and every
        // element after it shifts, so a stored value lands on its neighbour: a sensor inheriting another
        // sensor's pin, type and calibration. The document names the element instead.
        Cache& cc = Cache::instance();
        cc.setConfigImage(m.defaultImage());
        const std::vector<uint8_t> d3 = TuneFile::serialise(cc.configImage(), m, nullptr);
        const std::string t3(d3.begin(), d3.end());
        ck(t3.find("\"sensors.sensor[clt].") != std::string::npos,
           "a sensor's fields are keyed 'sensors.sensor[clt].…'");
        ck(t3.find("\"sensors.sensor[0].") == std::string::npos,
           "…and no sensor field is keyed by a bare index");

        MigrationReport rep;
        std::vector<uint8_t> img3 = TuneFile::deserialise(d3, m, rep, nullptr);
        ck(!img3.empty() && rep.unmapped.empty(),
           "the id-keyed document still loads cleanly",
           rep.unmapped.empty() ? "" : rep.unmapped.front());
    }

    std::puts("");
    std::puts("=== a name this firmware does not have is REPORTED, not guessed ===");
    {
        std::string t = text;
        const std::string from = "\"etb_en_1\"";
        const auto at = t.find(from);
        t.replace(at, from.size(), "\"etb_en_from_the_future\"");
        MigrationReport rep;
        std::vector<uint8_t> img =
            TuneFile::deserialise(std::vector<uint8_t>(t.begin(), t.end()), m, rep, nullptr);
        (void)img;
        bool named = false;
        for (const std::string& u : rep.unresolved)
            if (u.find("etb_en_from_the_future") != std::string::npos) named = true;
        ck(named, "the unknown reference is named in the report",
           rep.unresolved.empty() ? "report empty" : rep.unresolved.front());
        ck(rep.needed(), "…and the report asks to be shown to the user");
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "a tune names what it points at",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
