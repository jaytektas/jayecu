// Which ECU firmware kit the studio offers. A kit is a folder with a kit.json; what matters is that a
// half-copied kit is never offered, that the newest usable one wins, that a kit needing a newer studio
// is passed over, and that on a tie the downloaded copy beats the one shipped with the studio.
//
//   ./build/firmware_kits_test
//
#include "../src/model/FirmwareKits.h"
#include "../src/model/MetaModel.h"
#include "../src/comms/Crc32.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("[firmware-kits] %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

static void kit(const fs::path& root, const std::string& name, const std::string& board, const std::string& ver,
                const std::string& minStudio = "", bool withFirmware = true) {
    const fs::path d = root / name;
    fs::create_directories(d);
    std::ofstream(d / "kit.json") << "{\"board\":\"" << board << "\",\"version\":\"" << ver
        << "\",\"layout_hash\":\"abcd1234\",\"min_studio\":\"" << minStudio
        << "\",\"firmware\":\"fw.bin\",\"meta\":\"m.meta\",\"dashboard\":\"d.gui\"}";
    if (withFirmware) std::ofstream(d / "fw.bin") << "x";
    std::ofstream(d / "m.meta") << "{}";
    std::ofstream(d / "d.gui") << "{}";
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "firmware_kits_test";
    fs::remove_all(tmp);
    const fs::path shipped = tmp / "shipped", dl = tmp / "downloaded";

    kit(shipped, "a", "jaytek_v1", "0.2.0");
    kit(shipped, "b", "proteus_f7", "0.3.0");
    kit(dl,      "c", "jaytek_v1", "0.2.0");                 // same version as the shipped one
    kit(dl,      "d", "jaytek_v1", "0.9.0", "9.0.0");         // needs a far newer studio
    kit(dl,      "e", "jaytek_v1", "0.8.0", "", false);       // firmware file missing: half-copied
    fs::create_directories(dl / "f");                         // no kit.json at all

    auto kits = fwkits::scan(shipped.string(), true);
    const auto more = fwkits::scan(dl.string(), false);
    kits.insert(kits.end(), more.begin(), more.end());
    check(kits.size() == 4, "a folder missing its firmware or its kit.json is not a kit");

    fwkits::Kit k;
    check(fwkits::newestFor(kits, "jaytek_v1", "0.1.0", k) && k.version == "0.2.0",
          "a kit needing a newer studio is passed over");
    check(!k.shipped, "on a version tie the downloaded kit wins");
    check(!k.dashboard.empty() && k.firmware.find("fw.bin") != std::string::npos, "the kit's files are full paths");
    check(fwkits::newestFor(kits, "proteus_f7", "0.1.0", k) && k.version == "0.3.0", "each board gets its own kit");
    check(!fwkits::newestFor(kits, "nosuchboard", "0.1.0", k), "no kit for a board is 'none', not a guess");

    // WHAT CHANGED, IN WORDS. A kit carries every version's notes up to its own; the studio shows the
    // ones the ECU has not had — all of them when it skipped some, just the kit's own for a blank ECU.
    {
        const fs::path d = tmp / "notes" / "n";
        fs::create_directories(d);
        std::ofstream(d / "kit.json") << R"({"board":"jaytek_v1","version":"0.4.3","layout_hash":"abcd1234",
            "firmware":"fw.bin","meta":"m.meta","notes":[
              {"version":"0.4.1","changes":["one"]},{"version":"0.4.3","changes":["three a","three b"]},
              {"version":"0.4.2","changes":["two"]},{"version":"0.5.0","changes":["not in this kit"]}]})";
        std::ofstream(d / "fw.bin") << "x";
        std::ofstream(d / "m.meta") << "{}";
        const auto nk = fwkits::scan((tmp / "notes").string(), false);
        check(nk.size() == 1 && nk[0].notes.size() == 4 && nk[0].notes[0].version == "0.5.0",
              "a kit's notes are read, newest first");
        const auto since = nk.empty() ? std::vector<fwkits::Note>{} : fwkits::notesSince(nk[0], "0.4.1");
        check(since.size() == 2 && since[0].version == "0.4.3" && since[1].version == "0.4.2"
              && since[0].changes.size() == 2, "an ECU on 0.4.1 is shown 0.4.3 and 0.4.2, not its own");
        const auto blank = nk.empty() ? std::vector<fwkits::Note>{} : fwkits::notesSince(nk[0], "none");
        check(blank.size() == 1 && blank[0].version == "0.4.3", "a blank ECU is shown the kit's own version");
        const auto same = nk.empty() ? std::vector<fwkits::Note>{} : fwkits::notesSince(nk[0], "0.4.3");
        check(same.empty(), "an ECU already on the kit's version has nothing new");
    }
    check(fwkits::newestFor(kits, "jaytek_v1", "9.0.0", k) && k.version == "0.9.0",
          "a new enough studio is offered that kit");

    check(fwkits::isNewerThan("0.2.0", "0.1.0"), "0.2.0 is newer than the ECU's 0.1.0");
    check(!fwkits::isNewerThan("0.1.0", "0.1.0"), "the same version is not an update");
    check(!fwkits::isNewerThan("0.2.0", "garbage"), "an ECU version that is not a version offers nothing");

    // A META THAT NEEDS A NEWER STUDIO is refused, and says which studio. The shipped meta, which
    // needs 0.1.0, still loads for a 0.1.0 studio.
    {
        std::string body = "{\"meta\":{\"board\":\"jaytek_v1\",\"layout_hash\":\"abcd1234\",\"min_studio\":\"9.0.0\"}}";
        const uint32_t crc = crc32_ieee::compute(body.data(), int(body.size()));
        for (int i = 0; i < 4; ++i) body.push_back(char(crc >> (8 * i)));
        fs::create_directories(tmp);
        std::ofstream(tmp / "future.meta", std::ios::binary) << body;
        MetaModel::setStudioVersion("0.1.0");
        MetaModel m;
        check(!m.loadFile((tmp / "future.meta").string()) && m.needsStudio() == "9.0.0",
              "a meta needing studio 9.0.0 is refused by studio 0.1.0, and says so");
        // The shipped meta, from the firmware tree beside this repo. It used to be looked for at
        // ../../shared, which from build/ is not the firmware tree, so this check skipped itself.
        MetaModel shipped;
        std::string shippedPath;
        for (const char* c : { "../../../shared/tuneit-meta.json", "../../shared/tuneit-meta.json" })
            if (fs::exists(c)) { shippedPath = c; break; }
        check(!shippedPath.empty(), "the shipped meta is found (the firmware tree beside the studio)");
        check(shippedPath.empty() || (shipped.loadFile(shippedPath) && shipped.needsStudio().empty()),
              "the shipped meta loads for studio 0.1.0: its min_studio and meta_format are ones this studio reads");

        // A META IN A NEWER FORMAT is refused even when its min_studio does not ask for a newer studio:
        // the format is what the build checks, so it cannot be forgotten the way a version can.
        std::string fmt = "{\"meta\":{\"board\":\"jaytek_v1\",\"layout_hash\":\"abcd1234\",\"min_studio\":\"0.1.0\","
                          "\"meta_format\":" + std::to_string(MetaModel::kMetaFormat + 1) + "}}";
        const uint32_t crc2 = crc32_ieee::compute(fmt.data(), int(fmt.size()));
        for (int i = 0; i < 4; ++i) fmt.push_back(char(crc2 >> (8 * i)));
        std::ofstream(tmp / "newformat.meta", std::ios::binary) << fmt;
        MetaModel nf;
        check(!nf.loadFile((tmp / "newformat.meta").string()) && nf.needsStudio().rfind("newer than", 0) == 0,
              "a meta one format ahead is refused, even with min_studio 0.1.0");
        MetaModel::setStudioVersion("");
        MetaModel nf2;
        check(!nf2.loadFile((tmp / "newformat.meta").string()),
              "…and refused with no studio version set as well (tools and tests)");
    }

    fs::remove_all(tmp);
    std::printf("\n[firmware-kits] %s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
