// A physical pin can only belong to one sensor. The picker asks claimedPoolPins() which pins other
// ENABLED sensors already hold, and greys those — naming the owner — instead of offering a pin that will
// make the firmware raise P1650.
//
// The subtleties are the whole point, so they are what this pins:
//   - keyed by board pin NAME, not raw value: AV1 and DIG1 are both pool index 0
//   - a DISABLED sensor holds nothing
//   - your OWN pin is never reported (it must stay pickable, and a pre-existing clash stay visible)
//   - each sibling resolves through ITS OWN interface's set, so mixed interfaces compare on common ground
//
//   ./build/pin_conflict_test [meta.json]

#include "../src/surface/widgets/EnumPickerWidget.h"
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"
#include "../src/model/TuneFile.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& note = {}) {
    std::printf("[pin] %-62s %s%s\n", what, ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

static std::string elemOf(const MetaModel& m, const char* id) { return std::string("sensors.sensor[") + id + "]"; }

int main(int argc, char** argv) {
    MetaModel meta;
    const char* cands[] = { argc > 1 ? argv[1] : nullptr, REAL_META };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    // NOT a skip. The two relative candidates this used to guess with resolved only from the build
    // directory, and anywhere else the test printed "skipped" and returned SUCCESS — a green result
    // for a run that checked nothing.
    if (!loaded) { std::printf("[pin] cannot load %s\n", REAL_META); return 1; }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    C.setConfigImage(std::vector<uint8_t>(size_t(meta.configSize()), 0));

    // CLT takes AV3 (analog interface = 1, pin value 2 == "AV3" in the analog set).
    const std::string clt = elemOf(meta, "clt"), iat = elemOf(meta, "iat");
    C.setConfigValue(clt + ".enabled", 1);
    C.setConfigValue(clt + ".interface", 1);
    C.setConfigValue(clt + ".source", 2);

    auto claimed = EnumPickerWidget::claimedPoolPins(iat + ".source");
    check(claimed.count("AV3") == 1, "another enabled sensor's pin is reported as taken");
    if (claimed.count("AV3"))
        check(claimed["AV3"].holder.find("Coolant") != std::string::npos,
              "…and it names the sensor that holds it");
    check(claimed.count("AV4") == 0, "a pin nobody holds is free");

    // Our OWN pin is never reported — it has to stay pickable, and a pre-existing clash stay visible.
    auto mine = EnumPickerWidget::claimedPoolPins(clt + ".source");
    check(mine.count("AV3") == 0, "your own pin is not reported against you");

    // A DISABLED sensor holds nothing.
    C.setConfigValue(clt + ".enabled", 0);
    claimed = EnumPickerWidget::claimedPoolPins(iat + ".source");
    check(claimed.count("AV3") == 0, "a disabled sensor releases its pin");

    // Value-identity is not enough: pool index 0 is AV1 for an analog sensor. A sibling holding index 0
    // must claim the NAME its own interface gives it, not merely "index 0".
    C.setConfigValue(clt + ".enabled", 1);
    C.setConfigValue(clt + ".source", 0);
    claimed = EnumPickerWidget::claimedPoolPins(iat + ".source");
    check(claimed.count("AV1") == 1 && claimed.count("AV3") == 0,
          "the claim is by pin NAME (index 0 -> AV1), not by raw value");

    // ---- and the pool-wide half: a holder in a DIFFERENT array ---------------------------------
    // A sensor's Frequency Input and a trigger stream's Capture Input are separate arrays over the
    // same physical DIG pins. This is the case the sibling-scoped version could not see at all.
    {
        const auto& arrays = meta.configArrays();
        const auto ti = arrays.find("trigger.streams");
        if (ti == arrays.end()) {
            std::printf("[pin] (no trigger.streams array in this meta — cross-array case skipped)\n");
        } else {
            // Find which option value the trigger picker gives to a DIG pin, then have a sensor take
            // that same pin as a frequency input and check the trigger picker sees it held.
            // ONE field, `source`, whose LIST is chosen by the interface. `source_freq` is a TS-ini
            // suffix for the same byte and is not a path — writing it wrote nothing, the gate had
            // already reset `source` to -1, and the check was failing against a sensor that held no
            // pin at all rather than against the code under test. Set the interface FIRST, then the
            // pin: the gate restores the default whenever the list changes underneath it.
            C.setConfigValue(clt + ".enabled", 1);
            C.setConfigValue(clt + ".interface", 3);      // frequency
            C.setConfigValue(clt + ".source", 0);         // DIG1, in the frequency list
            auto x = EnumPickerWidget::claimedPoolPins("trigger.streams[0].capture_index");
            check(x.count("DIG1") == 1,
                  "a pin held in ANOTHER array is reported as taken");
            if (x.count("DIG1"))
                check(x["DIG1"].holder.find("Coolant") != std::string::npos,
                      "…and it names the holder, which is not a sibling");

            // The TUNE FILE addresses an array element the same way, and a struct-array key that does
            // not `locate()` is silently dropped on load (TuneFile reports it "unmapped"). An array
            // with no element_ids is keyed by INDEX, and that is the only form there is for it.
            check(meta.locate("trigger.streams[2].enabled").kind == MetaModel::Location::Kind::Scalar,
                  "a numeric-key struct-array field locates (a tune stores it this way)");
            check(meta.locate("trigger.streams[2].capture_index").kind == MetaModel::Location::Kind::Scalar,
                  "…and so does its pin field");

            // AND THE OTHER DIRECTION, which is the one a user meets first: the TRIGGER holds the pin
            // and a SENSOR is being pointed at it. Cam Intake B1 on DIG1, then open start_sw's digital
            // input list — DIG1 must come back held. The trigger pool counts VR1/VR2 first, so DIG1 is
            // capture index 2 there and 0 in the sensor's list: the claim can only ever be keyed by the
            // pin's NAME, never by the stored number.
            C.setConfigValue(clt + ".enabled", 0);        // clear the previous holder
            C.setConfigValue("trigger.streams[2].enabled", 1);
            C.setConfigValue("trigger.streams[2].capture_index", 2);   // DIG1
            const std::string sw = "sensors.sensor[start_sw]";
            C.setConfigValue(sw + ".enabled", 1);
            C.setConfigValue(sw + ".interface", 4);       // digital level
            auto y = EnumPickerWidget::claimedPoolPins(sw + ".source");
            check(y.count("DIG1") == 1,
                  "a pin held by a TRIGGER STREAM is reported as taken to a sensor");
            if (y.count("DIG1"))
                check(y["DIG1"].holder.find("Cam Intake B1") != std::string::npos,
                      "…and names the stream by its role, not its index", y["DIG1"].holder);
        }
    }

    // ---- the TUNE ROUND TRIP for a struct-array field addressed by INDEX -----------------------
    // The claim scan only sees what the config image holds, so a field that does not survive a tune
    // load is a pin nothing reports as taken however correct the scan is. Everything above proves the
    // scan; this proves the field is there for it to read.
    {
        MigrationReport rep;
        std::vector<uint8_t> img = meta.defaultImage();
        const std::string doc =
            std::string("{\"format\":\"jayecu-tune/2\",\"layout_hash\":\"") + meta.layoutHash() +
            "\",\"scalars\":{},\"tables\":{},\"axes\":{},\"expressions\":{},\"arrays\":{"
            "\"trigger.streams[2].enabled\":1,"
            "\"trigger.streams[2].capture_index\":\"DIG1\","
            "\"sensors.sensor[start_sw].enabled\":1}}";
        const std::vector<uint8_t> js(doc.begin(), doc.end());
        const std::vector<uint8_t> out = TuneFile::deserialise(js, meta, rep);
        check(!out.empty(), "the tune document parses");
        if (!out.empty()) {
            C.setConfigImage(out);
            check(C.configValue("sensors.sensor[start_sw].enabled") == 1.0,
                  "an id-keyed array field survives the round trip");
            check(C.configValue("trigger.streams[2].enabled") == 1.0,
                  "an INDEX-keyed array field survives it too");
            check(C.configValue("trigger.streams[2].capture_index") == 2.0,
                  "…and its pin, stored by NAME, resolves back to the pool index");
        }
        for (const auto& u : rep.unmapped)  std::printf("[pin]   unmapped:  %s\n", u.c_str());
        for (const auto& u : rep.unresolved) std::printf("[pin]   unresolved: %s\n", u.c_str());
    }

    // Wiring map: a board resource resolves to its connector terminal + wire colour (drives the wiring
    // widget). AV2 lands on CN3-26 with a G/W wire per the board yaml's connector colours.
    {
        std::string pin, col;
        check(meta.wiringTrim("AV2", pin, col), "a board resource has a connector-map entry");
        check(pin == "CN3-26", "…its external connector pin (AV2 -> CN3-26)");
        check(col == "G/W",    "…its wire colour code (AV2 -> G/W)");
        std::string p2, c2;
        check(!meta.wiringTrim("NOT_A_PIN", p2, c2), "an unknown resource has no wiring entry");
        // AV2's terminal is on CN3 (pin says so), and CN3's shell is the blue 776231-1.
        auto ci = meta.connectors().find("CN3");
        check(ci != meta.connectors().end(), "the physical connector is in the meta");
        if (ci != meta.connectors().end())
            check(ci->second.color == "blue", "…with its shell colour (CN3 -> blue)");
    }

    std::printf("\n[pin] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
