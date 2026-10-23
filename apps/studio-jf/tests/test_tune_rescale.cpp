// A tune stores RAW values, so a firmware that changes a value's SCALE changes what every stored
// number means. Loaded across that change unconverted, a 0.04 band saved at 0.002/count arrives as
// 0.02 at 0.001/count — half of what was tuned, with a clean migration report saying all was well.
//
// TuneFile::deserialise is told the layout the tune was saved on (by its layout_hash) and converts
// raw_new = raw_old * scale_old / scale_new for a scalar, a table's cells, and reports each one.
// Here the "old" firmware is the real meta with two scales changed and a different layout hash.
//
//   cmake --build build --target tune_rescale_test && ./build/tune_rescale_test
#include "model/MetaModel.h"
#include "model/TuneFile.h"

#include <j/config/Json.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-70s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

static bool writeMeta(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary);
    f << body;
    uint32_t crc = 0xFFFFFFFFu;                          // the loader checks a zlib CRC32 footer
    for (unsigned char b : body) {
        crc ^= b;
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    crc ^= 0xFFFFFFFFu;
    for (int k = 0; k < 4; ++k) f.put(char((crc >> (8 * k)) & 0xFF));
    return bool(f);
}

int main() {
    MetaModel now;
    if (!now.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }

    const std::string SC = "lambda.nb_stoich_band";      // U16, 0.001 today
    const std::string TB = "lambda.stft_kp_table";       // U16 cells, 0.1 today
    std::string body;
    { std::ifstream f(REAL_META, std::ios::binary);
      body.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      if (body.size() > 4) body.resize(body.size() - 4); }
    jf::JJson root = jf::JJson::parse(body);
    root["meta"]["layout_hash"] = jf::JJson(std::string("0ldf1rm0"));
    root["config"]["lambda"]["nb_stoich_band"]["scale"] = jf::JJson(0.002);
    root["config"]["lambda"]["stft_kp_table"]["scale"]  = jf::JJson(0.2);
    const std::string oldPath = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                              + "/jf_rescaled_old.meta";
    MetaModel old;
    ck(writeMeta(oldPath, root.dump()) && old.loadFile(oldPath), "the old firmware's meta loads");
    ck(old.layoutHash() == "0ldf1rm0" && old.config().at(SC).scale == 0.002,
       "…as a different layout, with the band at 0.002/count");
    const TableImage oldT = old.resolveTable(TB), nowT = now.resolveTable(TB);
    ck(oldT.valid && nowT.valid && oldT.cellScale == 0.2 && nowT.cellScale == 0.1,
       "…and the Kp table at 0.2/count (0.1 now)");

    // A tune saved on the OLD firmware: band 20 counts (0.04), Kp cell 0 = 7 counts (1.4), and one
    // cell big enough that doubling it would overflow a U16.
    std::vector<uint8_t> img = old.defaultImage();
    MetaModel::encodeRaw("U16", img.data() + old.config().at(SC).offset, 20);
    MetaModel::encodeRaw(oldT.cellType, img.data() + oldT.cellBase, 7);
    MetaModel::encodeRaw(oldT.cellType, img.data() + oldT.cellBase + oldT.cellSize, 40000);
    const std::vector<uint8_t> doc = TuneFile::serialise(img, old);

    auto rawAt = [&](const std::vector<uint8_t>& im, int off, const std::string& type) {
        return MetaModel::decodeRaw(type, im.data() + off);
    };
    const TuneFile::SavedMeta savedOn = [&](const std::string& h) -> const MetaModel* {
        return h == old.layoutHash() ? &old : nullptr;
    };

    std::puts("=== with the saved layout to hand: converted ===");
    {
        MigrationReport rep;
        const std::vector<uint8_t> out = TuneFile::deserialise(doc, now, rep, nullptr, savedOn);
        const double band = rawAt(out, now.config().at(SC).offset, "U16");
        ck(band == 40, "the band arrives as 40 counts — still 0.04", std::to_string(band));
        const double c0 = rawAt(out, nowT.cellBase, nowT.cellType);
        ck(c0 == 14, "Kp cell 0 arrives as 14 counts — still 1.4", std::to_string(c0));
        const double c1 = rawAt(out, nowT.cellBase + nowT.cellSize, nowT.cellType);
        ck(c1 == 65535, "a cell that outgrows its type is pinned, not wrapped", std::to_string(c1));
        bool sawSc = false, sawTb = false;
        for (const std::string& s : rep.rescaled) {
            if (s.rfind(SC + " ", 0) == 0) sawSc = true;
            if (s.rfind(TB + " ", 0) == 0) sawTb = true;
        }
        ck(sawSc && sawTb, "both are reported, once each, by name",
           std::to_string(rep.rescaled.size()) + " rescaled");
        ck(rep.rescaled.size() == 2, "…and nothing else was touched", std::to_string(rep.rescaled.size()));
        ck(rep.needed(), "a rescale makes the report worth showing");
    }

    std::puts("=== without it: carried as stored, nothing claimed ===");
    {
        MigrationReport rep;
        const std::vector<uint8_t> out = TuneFile::deserialise(doc, now, rep);
        ck(rawAt(out, now.config().at(SC).offset, "U16") == 20, "the band is copied raw");
        ck(rep.rescaled.empty(), "no rescale is reported");
    }

    std::puts("=== saved on THIS layout: never asked ===");
    {
        bool asked = false;
        const std::vector<uint8_t> same = TuneFile::serialise(now.defaultImage(), now);
        MigrationReport rep;
        TuneFile::deserialise(same, now, rep, nullptr,
                              [&](const std::string&) -> const MetaModel* { asked = true; return &old; });
        ck(!asked && rep.rescaled.empty(), "a tune on its own layout does not consult another");
    }

    std::puts("=== alignment padding is not a setting ===");
    {
        const std::vector<uint8_t> doc = TuneFile::serialise(now.defaultImage(), now);
        const std::string text(doc.begin(), doc.end());
        ck(text.find("_align_pad") == std::string::npos, "a saved tune carries no padding fields");
        // An older tune that did store them: they load as nothing, neither dropped nor applied.
        jf::JJson d = jf::JJson::parse(text);
        d["arrays"]["outputs.output[0]._align_pad_99"] = jf::JJson(0.0);
        d["scalars"]["lambda._align_pad_3"] = jf::JJson(0.0);
        const std::string t2 = d.dump();
        MigrationReport rep;
        TuneFile::deserialise(std::vector<uint8_t>(t2.begin(), t2.end()), now, rep);
        ck(rep.unmapped.empty() && rep.defaulted == 0, "an old tune's padding is not reported as dropped or new",
           std::to_string(rep.unmapped.size()) + " unmapped, " + std::to_string(rep.defaulted) + " defaulted");
    }

    std::puts("=== is this file the tune the ECU has? ===");
    {
        const std::vector<uint8_t> ecu = TuneFile::serialise(now.defaultImage(), now);
        ck(TuneFile::sameValues(ecu, ecu), "a tune is the same as itself");
        // The same values with an old studio's padding and another layout hash in the header: the same tune.
        jf::JJson d = jf::JJson::parse(std::string(ecu.begin(), ecu.end()));
        d["arrays"]["outputs.output[0]._align_pad_99"] = jf::JJson(0.0);
        d["layout_hash"] = jf::JJson(std::string("0ldf1rm0"));
        const std::string padded = d.dump();
        ck(TuneFile::sameValues(ecu, std::vector<uint8_t>(padded.begin(), padded.end())),
           "padding and the header are not values");
        // One setting different: a tune opened to be sent, not the one on the ECU.
        std::vector<uint8_t> img = now.defaultImage();
        MetaModel::encodeRaw("U16", img.data() + now.config().at(SC).offset, 77);
        const std::vector<uint8_t> edited = TuneFile::serialise(img, now);
        ck(!TuneFile::sameValues(ecu, edited), "one changed setting makes it a different tune");
        ck(!TuneFile::sameValues(ecu, {}), "an empty file is not the ECU's tune");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
