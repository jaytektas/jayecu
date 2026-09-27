// [Datalog] from a TunerStudio-format definition becomes the channels the studio's recorder logs by
// default. Without it an imported ECU had no "worth logging" set, and a recording fell back to every
// output channel -- hundreds of columns where the ECU's own definition names a few dozen.
//
//   cmake --build build --target ts_datalog_test && ./build/ts_datalog_test

#include "model/TsIniImporter.h"
#include "model/MetaModel.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  %s  %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : ("   - " + detail).c_str());
    if (!ok) ++fails;
}

// This test's own definition: four channels, three of them named in [Datalog] -- one with a trailing
// {condition}, one spelled with no spaces -- plus an entry for a channel the definition does not have.
static const char* kIni = R"INI(
[TunerStudio]
   signature = "bench-ecu datalog fixture 0.1"

[Constants]
   pageSize = 64
   page = 1
   idleTarget = scalar, U16, 0, "RPM", 1, 0, 0, 3000, 0

[OutputChannels]
   ochBlockSize = 16
   engineSpeed = scalar, U16, 0, "RPM", 1, 0
   coolant     = scalar, S16, 2, "deg C", 0.1, 0
   throttle    = scalar, S16, 4, "%", 0.01, 0
   boardTemp   = scalar, S16, 6, "deg C", 0.1, 0

[Datalog]
   ; channel       label          type    format
   entry = engineSpeed, "Engine Speed", int,   "%d"
   entry = coolant,     "Coolant",      float, "%.1f"
   entry=throttle,"Throttle",float,"%.2f", { throttle > 0 }
   entry = notAChannel, "Missing",      float, "%.1f"
)INI";

int main() {
    std::printf("=== ts datalog import ===\n");
    const jf::JJson root = TsIniImporter::importText(kIni);
    const jf::JJson& tel = root["telemetry"];

    ck(tel["engineSpeed"]["datalog"].boolean(false), "a [Datalog] entry marks its channel");
    ck(tel["coolant"]["datalog"].boolean(false),     "…every one of them");
    ck(tel["throttle"]["datalog"].boolean(false),    "…including one with a {condition} and no spaces");
    ck(!tel["boardTemp"]["datalog"].boolean(false),  "a channel [Datalog] does not name stays unmarked");
    ck(!tel["notAChannel"].isObject(),               "an entry for a missing channel invents nothing");

    const std::string path = std::string(BUILD_TMP) + "/ts_datalog_test.meta";
    ck(TsIniImporter::writeMetaFile(root, path), "the import writes a .meta");
    MetaModel m;
    ck(m.loadFile(path), "the imported definition loads");
    int flagged = 0;
    for (const auto& [name, f] : m.telemetry()) if (f.datalog) ++flagged;
    ck(flagged == 3, "the loaded definition carries exactly the three logged channels",
       std::to_string(flagged));

    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
