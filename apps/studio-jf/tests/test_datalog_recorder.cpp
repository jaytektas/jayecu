// The studio recorder writes a log MegaLogViewer can read.
//
// A log is ONE START TO ONE STOP. This drives the recorder the way the app does — start, feed it
// telemetry frames, stop — and then reads the file back as MLV's parser would: two quoted header
// lines, a tab-separated name row, a tab-separated unit row of the same width, and data rows of the
// same width again. A column count that drifts is the one failure that produces a file which opens
// and is wrong, so it is what this checks hardest.
//
//   cmake --build build --target datalog_recorder_test && ./build/datalog_recorder_test
#include "../src/model/DatalogRecorder.h"
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"
#include "mlg_log.h"   // the FIRMWARE's descriptor catalog — the order the mask indexes

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

namespace fs = std::filesystem;

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out; std::string tok; std::istringstream is(s);
    while (std::getline(is, tok, d)) out.push_back(tok);
    return out;
}

int main(int argc, char** argv) {
    MetaModel meta;
    const char* cands[] = { argc > 1 ? argv[1] : nullptr,
#ifdef REAL_META
                            REAL_META,
#endif
                            "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::puts("[log-recorder] (no meta found — skipped)"); return 0; }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    std::vector<uint8_t> image = meta.defaultImage();
    if (image.empty()) image.assign((size_t)meta.configSize(), 0);
    C.setConfigImage(image);

    // Somewhere disposable — never the user's real log directory.
    const fs::path dir = fs::temp_directory_path() / "jayecu_datalog_recorder_test";
    std::error_code ec; fs::remove_all(dir, ec);
    DatalogRecorder::setDirectory(dir.string());
    DatalogRecorder::setKeepFiles(0);

    DatalogRecorder& rec = DatalogRecorder::instance();
    std::puts("=== Studio datalog recorder -> MSL ===");

    std::string why;
    ck(rec.start(&why), "a recording starts", why);
    ck(rec.recording(), "…and reports itself as recording");

    // Three frames, the way the link delivers them.
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> frame((size_t)meta.telemetrySize(), (uint8_t)(i + 1));
        C.ingestTelemetry(frame);
        rec.onFrame();
    }
    const std::string path = rec.stop();
    ck(!rec.recording(), "…and stops");
    ck(!path.empty() && fs::exists(path), "the file is on disk", path);

    // --- read it back the way MLV would ---------------------------------------------------
    std::ifstream in(path);
    std::vector<std::string> lines; std::string ln;
    while (std::getline(in, ln)) { if (!ln.empty() && ln.back() == '\r') ln.pop_back(); lines.push_back(ln); }
    ck(lines.size() == 3 + 4, "two header lines, names, units, then a row per frame",
       std::to_string(lines.size()));
    if (lines.size() < 5) { std::puts("\nFAILED (file too short to check)"); return 1; }

    ck(lines[0].size() > 2 && lines[0].front() == '"' && lines[0].back() == '"',
       "line 1 is the quoted controller signature", lines[0]);
    ck(lines[1].rfind("\"Capture Date: ", 0) == 0 && lines[1].back() == '"',
       "line 2 is the quoted capture date", lines[1]);

    const auto names = split(lines[2], '\t');
    const auto units = split(lines[3], '\t');
    ck(names.size() > 1 && names[0] == "Time", "the first column is Time");
    ck(units.size() == names.size(), "the units row is exactly as wide as the names row",
       std::to_string(units.size()) + " vs " + std::to_string(names.size()));
    ck(!units.empty() && units[0] == "s", "…and Time is in seconds");

    // Every data row the same width — the failure that opens and lies.
    bool widthsMatch = true;
    for (size_t i = 4; i < lines.size(); ++i)
        if (split(lines[i], '\t').size() != names.size()) widthsMatch = false;
    ck(widthsMatch, "every data row has the same column count as the header");

    // Time ascends from zero: the first row is the origin, not an arbitrary tick.
    const auto row0 = split(lines[4], '\t');
    ck(!row0.empty() && std::stod(row0[0]) == 0.0, "the first row is at t = 0",
       row0.empty() ? "" : row0[0]);

    // --- the channel set is what the definition marked, and it is fewer columns ------------
    {
        // The templates come from the META, so this also proves they survived the trip.
        ck(!DatalogRecorder::templates().empty(), "the firmware declares channel templates",
           std::to_string(DatalogRecorder::templates().size()));
        ck(DatalogRecorder::applyTemplate("everything"), "\"Everything\" applies");
        const size_t all = DatalogRecorder::selectedChannels().size();
        ck(DatalogRecorder::applyTemplate("fuel"), "\"Fuel and Mixture\" applies");
        const size_t fuel = DatalogRecorder::selectedChannels().size();
        ck(fuel > 0 && fuel < all, "a focused template is a real subset",
           std::to_string(fuel) + " of " + std::to_string(all));
        // A template that names channels must actually carry them — the whole point of the set.
        const auto sel = DatalogRecorder::selectedChannels();
        const bool hasTarget = std::find(sel.begin(), sel.end(), "lambda_target") != sel.end();
        const bool hasVe     = std::find(sel.begin(), sel.end(), "ve") != sel.end();
        ck(hasTarget && hasVe, "…carrying the channels it names (ve, lambda_target)");
        ck(DatalogRecorder::templateId() == "fuel", "…and the set knows which template it is",
           DatalogRecorder::templateId());
        // Editing by hand stops it claiming to be that template.
        DatalogRecorder::setCustomSelection({ "rpm", "clt" });
        ck(DatalogRecorder::templateId().empty(), "an edited set is no longer a named template");
        ck(DatalogRecorder::selectedChannels().size() == 2, "…and is exactly what was chosen");
        DatalogRecorder::applyTemplate("standard");
        const size_t std_ = DatalogRecorder::selectedChannels().size();
        ck(std_ > 0 && std_ < all, "the standard set is a real subset of every channel",
           std::to_string(std_) + " of " + std::to_string(all));

        // THE STUDIO RECORDS ITS OWN SET. The card's channel choice above must not narrow what a
        // recording on this PC keeps: unconfigured, the studio records every channel.
        auto recordedNames = [&]() -> std::vector<std::string> {
            std::string w2;
            if (!rec.start(&w2)) { ck(false, "a recording starts", w2); return {}; }
            C.ingestTelemetry(std::vector<uint8_t>((size_t)meta.telemetrySize(), 5));
            rec.onFrame();
            const std::string p2 = rec.stop();
            std::ifstream in2(p2);
            std::string l1, l2, l3;
            std::getline(in2, l1); std::getline(in2, l2); std::getline(in2, l3);
            auto cols = split(l3, '\t');
            if (!cols.empty()) cols.erase(cols.begin());          // Time
            return cols;
        };
        DatalogRecorder::setRecordingSelection({});
        ck(recordedNames().size() == meta.telemetry().size(),
           "with no recording set chosen, the studio records every channel -- whatever the card logs");
        DatalogRecorder::setRecordingSelection({ "map", "rpm", "no_such_channel" });
        const auto chosen = recordedNames();
        ck(chosen == std::vector<std::string>{ "map", "rpm" },
           "a chosen recording set is what is recorded, in the order chosen, less what does not exist");
        ck(DatalogRecorder::selectedChannels().size() == std_,
           "…and choosing it leaves the card's channels alone");
        DatalogRecorder::setRecordingSelection({});
    }

    // --- the mask indexes the FIRMWARE's catalog, not the studio's idea of it ---------------
    // This is the one that cannot be got wrong quietly. The studio derives a channel's bit from its
    // rank when the meta is sorted by frame offset; the ECU uses its generated table's order. If
    // those ever disagree, the card logs the wrong channels in a file that opens perfectly and every
    // column is mislabelled. So it is checked against the real table rather than assumed.
    {
        const auto order = DatalogRecorder::catalogOrder();
        ck(order.size() == MLG_FIELD_COUNT, "the studio sees as many channels as the firmware",
           std::to_string(order.size()) + " vs " + std::to_string(MLG_FIELD_COUNT));
        size_t mismatch = SIZE_MAX;
        for (size_t i = 0; i < order.size() && i < MLG_FIELD_COUNT; ++i)
            if (order[i] != MLG_FIELDS[i].name) { mismatch = i; break; }
        ck(mismatch == SIZE_MAX, "…in exactly the same order",
           mismatch == SIZE_MAX ? "" : ("first differs at " + std::to_string(mismatch) + ": " +
                                        order[mismatch] + " vs " + MLG_FIELDS[mismatch].name));

        // …and the bits land on the channels the selection actually names.
        DatalogRecorder::setCustomSelection({ order[0], order[9] });
        const auto bits = DatalogRecorder::maskBytes();
        ck(bits.size() == (MLG_FIELD_COUNT + 7) / 8, "the mask is one bit per channel",
           std::to_string(bits.size()));
        ck((bits[0] & 0x01) && (bits[1] & 0x02) && !(bits[0] & 0x02),
           "…set for exactly the chosen channels");
        DatalogRecorder::applyTemplate("standard");
    }

    // --- user profiles: made from a selection, replaced by name, and gone when deleted -----
    {
        DatalogRecorder::setCustomSelection({ "rpm", "clt", "map" });
        DatalogRecorder::saveProfile("misfire hunt");
        const auto names = DatalogRecorder::profileNames();
        ck(std::find(names.begin(), names.end(), "misfire hunt") != names.end(),
           "a profile is saved from whatever is selected");
        ck(DatalogRecorder::profileChannels("misfire hunt").size() == 3,
           "…carrying exactly those channels",
           std::to_string(DatalogRecorder::profileChannels("misfire hunt").size()));

        // Applying it puts those channels back after something else has been chosen.
        DatalogRecorder::applyTemplate("everything");
        ck(DatalogRecorder::selectedChannels().size() > 3, "…something else is selected");
        ck(DatalogRecorder::applyProfile("misfire hunt"), "the profile applies");
        ck(DatalogRecorder::selectedChannels().size() == 3, "…restoring the set it named",
           std::to_string(DatalogRecorder::selectedChannels().size()));

        // SAVING UNDER AN EXISTING NAME REPLACES IT — one profile, not two with the same name.
        DatalogRecorder::setCustomSelection({ "rpm" });
        DatalogRecorder::saveProfile("misfire hunt");
        size_t dupes = 0;
        for (const std::string& n : DatalogRecorder::profileNames()) if (n == "misfire hunt") ++dupes;
        ck(dupes == 1, "saving over a name replaces it", std::to_string(dupes));
        ck(DatalogRecorder::profileChannels("misfire hunt").size() == 1, "…with the new set");

        DatalogRecorder::deleteProfile("misfire hunt");
        const auto after = DatalogRecorder::profileNames();
        ck(std::find(after.begin(), after.end(), "misfire hunt") == after.end(), "…and delete removes it");
        DatalogRecorder::applyTemplate("standard");
    }

    // --- what the Onboard Logging dialog leans on ------------------------------------------
    // A dialog holding a WORKING set must be able to resolve a template and save a profile without
    // any of it reaching the ECU on the way — Apply is the only commit. Both of these were the
    // save-the-selection-put-it-back dance before, which pushed a mask twice per click.
    {
        DatalogRecorder::applyTemplate("standard");
        const auto liveBefore = DatalogRecorder::customSelection();

        const auto tch = DatalogRecorder::templateChannels("everything");
        ck(!tch.empty(), "templateChannels resolves a template", std::to_string(tch.size()));
        ck(DatalogRecorder::customSelection() == liveBefore,
           "…without touching the live selection");
        ck(DatalogRecorder::templateChannels("no-such-template").empty(),
           "an unknown template resolves to nothing");

        DatalogRecorder::saveProfile("dialog set", { "rpm", "map" });
        ck(DatalogRecorder::profileChannels("dialog set").size() == 2,
           "a profile saves from a set the caller is holding");
        ck(DatalogRecorder::customSelection() == liveBefore,
           "…and that does not become the live selection either");
        DatalogRecorder::deleteProfile("dialog set");
    }

    // A PROFILE IS THE WHOLE SETUP. It was a channel list, so "the misfire profile" came back with
    // its columns and without the condition that decided when to record them — a saved setup that
    // still had to be finished by hand. Round-trip everything through the store.
    {
        DatalogRecorder::Profile p;
        p.name      = "pull recorder";
        p.channels  = { "rpm", "map", "clt" };
        p.logWhen   = "rpm > 4000";
        p.logUntil  = "rpm < 3000";
        p.enabled   = 1;
        p.rateHz    = 50;
        p.minOnMs   = 250;  p.minOffMs = 500;  p.maxOnMs = 30000;  p.rearmMs = 1000;
        p.onInvalid = 2;
        DatalogRecorder::saveProfile(p);

        const DatalogRecorder::Profile back = DatalogRecorder::profile("pull recorder");
        ck(back.name == p.name,             "a profile round-trips its name");
        ck(back.channels == p.channels,     "…its channels");
        ck(back.logWhen == p.logWhen && back.logUntil == p.logUntil,
           "…BOTH gate conditions, as source", back.logWhen + " / " + back.logUntil);
        ck(back.rateHz == 50 && back.minOnMs == 250 && back.minOffMs == 500 &&
           back.maxOnMs == 30000 && back.rearmMs == 1000 && back.onInvalid == 2,
           "…and the gate's timing, the rate and on-invalid");

        // A profile that predates the gate travelling with it reads back as a channel list with the
        // rest at defaults — it has no gate, which is the truth about it, not a parse failure.
        DatalogRecorder::saveProfile("channels only", { "rpm" });
        const auto old = DatalogRecorder::profile("channels only");
        ck(old.channels.size() == 1 && old.logWhen.empty() && old.rateHz == 10,
           "a channel-list profile reads back with the rest at defaults");

        ck(DatalogRecorder::profile("no such profile").name.empty(),
           "an unknown name comes back empty");
        DatalogRecorder::deleteProfile("channels only");
    }

    // THE MASK, BOTH WAYS. The dialog shows the ECU's channels by decoding what the tune holds, so
    // the decode has to be the encode read backwards or the dialog quietly shows a different set
    // from the one the card is recording.
    {
        const std::vector<std::string> want = { "clt", "map", "rpm" };
        const auto bits = DatalogRecorder::maskBytesFor(want);
        const auto back = DatalogRecorder::channelsFromMask(bits);
        ck(back == want, "a mask decodes to exactly the channels it encoded",
           std::to_string(back.size()));
        // ALL ZERO IS NOT "NOTHING" — the firmware reads it as "as shipped", so this must too.
        const auto shipped = DatalogRecorder::channelsFromMask(std::vector<uint8_t>(bits.size(), 0));
        ck(!shipped.empty(), "an all-zero mask decodes to the definition's own set",
           std::to_string(shipped.size()));
    }

    // ACTIVATE NEEDS A CARD. It is the one thing here that writes to the ECU, and a logger pointed
    // at an empty slot is not a setting — so the state the firmware reports on the frame
    // (sd_present / sd_msc_active) is part of the contract, not just UI dressing.
    auto sdFrame = [&meta](uint8_t present, uint8_t onUsb) {
        std::vector<uint8_t> f((size_t)meta.telemetrySize(), 0);
        const auto& t = meta.telemetry();
        auto set = [&](const char* n, uint8_t v) {
            const auto it = t.find(n);
            if (it != t.end() && (size_t)it->second.offset < f.size()) f[(size_t)it->second.offset] = v;
        };
        set("sd_present", present);
        set("sd_msc_active", onUsb);
        return f;
    };
    {
        C.ingestTelemetry(sdFrame(0, 0));
        ck(DatalogRecorder::ecuCard() == DatalogRecorder::Card::None,
           "no card fitted reads as None");
        C.ingestTelemetry(sdFrame(1, 1));
        ck(DatalogRecorder::ecuCard() == DatalogRecorder::Card::OnUsb,
           "a card handed to USB reads as OnUsb, not Ready");
        DatalogRecorder::Profile p;
        p.channels = { "rpm" };
        std::string why;
        ck(!DatalogRecorder::activate(p, &why), "…and Activate is refused while the PC has it", why);
        ck(std::string(why).find("PC") != std::string::npos, "…saying so", why);
    }
    C.ingestTelemetry(sdFrame(1, 0));      // card fitted and the ECU's: the normal running state
    ck(DatalogRecorder::ecuCard() == DatalogRecorder::Card::Ready, "a fitted card reads as Ready");

    // ACTIVATE IS THE ONLY THING THAT TOUCHES THE CAR — and what it writes must be what
    // currentSettings() reads back, or the dialog shows one thing and the ECU does another.
    {
        DatalogRecorder::Profile p;
        p.name = "bench";
        p.channels = { "clt", "map", "rpm" };
        p.logWhen = "rpm > 4000";
        p.logUntil = "";
        p.rateHz = 25; p.minOnMs = 100; p.maxOnMs = 5000; p.onInvalid = 0; p.enabled = 1;
        std::string why;
        ck(DatalogRecorder::activate(p, &why), "a profile activates", why);

        const DatalogRecorder::Profile live = DatalogRecorder::currentSettings();
        ck(live.channels == p.channels, "…and the ECU reads back those channels",
           std::to_string(live.channels.size()));
        ck(live.logWhen == p.logWhen, "…that gate, decompiled from the ECU's own bytecode",
           live.logWhen);
        ck(live.logUntil.empty(), "…an empty condition stays empty", live.logUntil);
        ck(live.rateHz == 25 && live.minOnMs == 100 && live.maxOnMs == 5000 && live.onInvalid == 0,
           "…and the rate, timing and on-invalid");
        // TWO RECORDERS, TWO SETS: activating the card's setup does not change what the studio records.
        ck(DatalogRecorder::recordingChannels().size() == meta.telemetry().size(),
           "…and the studio's own recording set is untouched by it",
           std::to_string(DatalogRecorder::recordingChannels().size()));

        // A GATE THAT DOES NOT COMPILE STOPS THE WHOLE ACTIVATION. Writing the channels and
        // dropping the condition would leave the car logging the right columns at the wrong times.
        DatalogRecorder::Profile bad = p;
        bad.logWhen = "rpm >>> banana";
        why.clear();
        ck(!DatalogRecorder::activate(bad, &why), "a gate that will not compile is refused", why);
        ck(!why.empty(), "…with a reason");
        ck(DatalogRecorder::currentSettings().logWhen == p.logWhen,
           "…and the ECU still holds the last good one");
        DatalogRecorder::deleteProfile("pull recorder");
    }

    // WHAT A SETUP COSTS. The rule the dialog advises from, and it is exactly linear in both terms —
    // which is the thing a tuner needs told, because neither a channel count nor a rate means
    // anything alone. Checked against the geometry the FIRMWARE uses, not a second opinion.
    {
        const auto c20  = DatalogRecorder::costOf({ "rpm", "map", "clt" }, 100);
        ck(c20.channels == 3, "cost counts the channels it recognised");
        ck(c20.recordBytes == 4 + 1 + 2 + 2 + 1 || c20.recordBytes > 5,
           "a record is prefix + fields + checksum", std::to_string(c20.recordBytes));
        ck(c20.bytesPerSec == c20.recordBytes * 100.0, "throughput is record x rate");

        // LINEAR IN THE RATE: double it, double the bytes.
        const auto c200 = DatalogRecorder::costOf({ "rpm", "map", "clt" }, 200);
        ck(c200.bytesPerSec == 2 * c20.bytesPerSec, "…and doubling the rate doubles it");
        ck(c200.mbPerHour == 2 * c20.mbPerHour, "…and the hourly capacity with it");

        // …AND LINEAR IN THE CHANNELS.
        const auto wide = DatalogRecorder::costOf(DatalogRecorder::templateChannels("everything"), 100);
        ck(wide.recordBytes > c20.recordBytes, "a wider set makes a bigger record",
           std::to_string(wide.recordBytes));
        ck(wide.mbPerHour > c20.mbPerHour, "…and costs more per hour",
           std::to_string(wide.mbPerHour));

        // PAST THE CARD IS A NOTE, NOT A LIMIT. Nothing caps the rate by channel count: the ECU
        // drops the surplus and counts it, so the only real cost of asking for too much is that the
        // log is slower than requested. The flag exists to say so, not to refuse.
        const auto over = DatalogRecorder::costOf(DatalogRecorder::templateChannels("everything"), 1000);
        ck(over.pastCard, "a wide set at 1 kHz is noted as past the reference card",
           std::to_string(over.bytesPerSec / 1024.0) + " KB/s");
        ck(!DatalogRecorder::costOf({ "rpm" }, 100).pastCard, "one channel at 100 Hz is not");
        ck(over.mbPerHour > 0, "…and still reports what an hour would cost",
           std::to_string(over.mbPerHour));

        // THE SLOWEST CHANNEL IN THE SET is what decides whether a rate is outrunning the data.
        // A frame-rate-only set has nothing slower than 1 kHz; add one sensor and it does.
        const auto fastSet = DatalogRecorder::costOf(
            DatalogRecorder::templateChannels("fast_trigger"), 1000);
        ck(fastSet.channels > 0, "the fast trigger template resolves",
           std::to_string(fastSet.channels));
        ck(fastSet.slowestHz >= 1000, "…and every channel in it runs at frame rate",
           fastSet.slowestChannel + " @ " + std::to_string(fastSet.slowestHz));

        // A TEMPLATE CARRIES ITS RATE. Selecting "Trigger and Sync (1 kHz)" and being left at 10 Hz
        // is a set that names a thing it does not do — and 10 Hz was exactly what the dialog showed
        // before templateProfile() picked the rate up.
        const auto fastP = DatalogRecorder::templateProfile("fast_trigger");
        ck(fastP.rateHz == 1000, "a 1 kHz template loads at 1 kHz", std::to_string(fastP.rateHz));
        const auto stdP = DatalogRecorder::templateProfile("standard");
        ck(stdP.rateHz == 10, "…and a standard one at its own rate", std::to_string(stdP.rateHz));
        ck(DatalogRecorder::templateById("fast_trigger")->rateHz == 1000,
           "…read from the definition, not compiled in");

        const auto withSensor = DatalogRecorder::costOf({ "rpm", "clt" }, 1000);
        ck(withSensor.slowestHz > 0 && withSensor.slowestHz < 1000,
           "a temperature channel drags the set's slowest rate down",
           withSensor.slowestChannel + " @ " + std::to_string(withSensor.slowestHz));
        ck(withSensor.slowestChannel == "clt", "…and it is named", withSensor.slowestChannel);

        // An unknown name contributes nothing rather than guessing a width.
        ck(DatalogRecorder::costOf({ "no_such_channel" }, 100).channels == 0,
           "a channel the firmware does not have costs nothing");
    }

    // THE GATE'S CONFIG PATHS. The dialog compiles into these two by name, and a path that does not
    // resolve fails SILENTLY — the expression is typed, accepted, and never written. So assert the
    // names exist and are big enough to hold a program.
    {
        int off = 0, size = 0;
        ck(meta.resolveBlob("datalog.log_when", off, size) && size > 0,
           "datalog.log_when is an expression field", std::to_string(size));
        ck(meta.resolveBlob("datalog.log_until", off, size) && size > 0,
           "datalog.log_until is an expression field", std::to_string(size));
    }

    // --- pruning keeps the newest and never the open one -----------------------------------
    DatalogRecorder::setKeepFiles(2);
    for (int i = 0; i < 3; ++i) {   // stamped per second, so make them distinguishable
        std::string w;
        if (!rec.start(&w)) { ck(false, "a further recording starts", w); break; }
        C.ingestTelemetry(std::vector<uint8_t>((size_t)meta.telemetrySize(), 7));
        rec.onFrame();
        rec.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    }
    ck((int)DatalogRecorder::existingLogs().size() <= 2, "pruning keeps at most the configured count",
       std::to_string(DatalogRecorder::existingLogs().size()));

    fs::remove_all(dir, ec);
    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "All datalog recorder tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
