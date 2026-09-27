// The trigger wheel library: shipped wheels come from the ECU's meta, user wheels persist on disk.
//
// These used to be a hardcoded list inside this binary, which meant the wheels a studio offered were
// a property of the build rather than of the firmware connected — a wheel could exist in one studio
// and not another while both talked to the same ECU. And "Save to library" was emitted into a
// callback nothing connected, so an authored wheel was built and dropped.
#include "../src/model/TriggerWheel.h"
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    std::puts("=== trigger wheel library (meta-shipped + user-saved) ===");

    std::puts("\n-- the shipped library is parsed out of a meta document --");
    {
        const std::string doc = R"({"trigger_wheels":[
            {"name":"60-2","sync":"CRANK","cam_edge":-1,"streams":[
                {"rate":0,"kind":"gap","slots":60,"ratio":3,"gaps":[0]}]},
            {"name":"Nissan 360 CAS","sync":"PHASE","cam_edge":0,"streams":[
                {"rate":0,"kind":"gap","slots":180,"ratio":0,"gaps":[]},
                {"rate":1,"kind":"seq","cell":[1276,1276,1276,1296,1276,800]}]}]})";
        const std::vector<Wheel> w = wheelsFromMeta(jf::JJson::parse(doc));
        ck(w.size() == 2, "both wheels decoded", std::to_string(w.size()));
        ck(w[0].name == "60-2" && w[0].streams.size() == 1, "gap wheel: name + one stream");
        ck(w[0].streams[0].slots == 60 && w[0].streams[0].ratio == 3, "gap slots/ratio survive");
        ck(w[0].streams[0].cell == std::vector<int>{0}, "gap position survives");
        // The even wheel: ratio 0 with NO gap list. Reading that back as "one missing tooth" is the
        // bug that cost a whole evening on the firmware side; it must round-trip as truly even.
        ck(w[1].streams[0].ratio == 0 && w[1].streams[0].cell.empty(),
           "an EVEN track stays even — ratio 0, no gaps");
        ck(w[1].streams[1].cell.size() == 6 && w[1].streams[1].cell[5] == 800,
           "the sequence cell survives, short interval included");
        ck(w[1].sync == "PHASE", "sync level survives");
    }

    std::puts("\n-- cam_edge is a wheel-level shorthand that lands on the cam streams --");
    {
        const std::string doc = R"({"trigger_wheels":[{"name":"x","sync":"PHASE","cam_edge":2,
            "streams":[{"rate":0,"kind":"gap","slots":36,"ratio":2,"gaps":[0]},
                       {"rate":1,"kind":"width","width_min":0,"width_max":7200,"width_target":0}]}]})";
        const std::vector<Wheel> w = wheelsFromMeta(jf::JJson::parse(doc));
        ck(!w.empty() && w[0].streams[1].edge == 2, "cam stream takes the edge");
        ck(!w.empty() && w[0].streams[0].edge == 0, "the crank stream is left alone");
    }

    std::puts("\n-- a user wheel saves, reloads, and updates in place --");
    {
        // Point HOME at a scratch dir so the real library is never touched.
        const std::string tmp = "/tmp/jayecu-wheel-test-" + std::to_string(::getpid());
        std::filesystem::create_directories(tmp);
        setenv("HOME", tmp.c_str(), 1);

        ck(loadUserWheels().empty(), "no user wheels to begin with");

        Wheel w; w.name = "My CAS"; w.sync = "PHASE"; w.tdcOffset = 155.0;
        w.streams.push_back(gapStream(0, 180, 0, {}));
        w.streams.push_back(seqStream(1, {1276, 1276, 1276, 1296, 1276, 800}));
        ck(saveUserWheel(w), "saved");

        std::vector<Wheel> back = loadUserWheels();
        ck(back.size() == 1, "one wheel comes back", std::to_string(back.size()));
        ck(!back.empty() && back[0].name == "My CAS", "by name");
        ck(!back.empty() && back[0].tdcOffset == 155.0, "TDC offset survives the round trip");
        ck(!back.empty() && back[0].streams.size() == 2 &&
           back[0].streams[1].cell.size() == 6, "both streams and the cell survive");
        ck(!back.empty() && back[0].streams[0].ratio == 0 && back[0].streams[0].cell.empty(),
           "an even track is still even after a round trip");

        // Re-saving an edited wheel of the same name REPLACES it. Appending would grow the library
        // by one every time the user pressed save, which is how a library becomes unusable.
        w.tdcOffset = 10.0;
        ck(saveUserWheel(w), "re-saved after an edit");
        back = loadUserWheels();
        ck(back.size() == 1, "still one wheel, not two", std::to_string(back.size()));
        ck(!back.empty() && back[0].tdcOffset == 10.0, "and it holds the NEW value");

        Wheel other; other.name = "Another"; other.streams.push_back(gapStream(0, 36, 2, {0}));
        ck(saveUserWheel(other) && loadUserWheels().size() == 2, "a different name appends");

        std::filesystem::remove_all(tmp);
    }

    std::puts("\n-- a missing or unreadable store is empty, not a crash --");
    {
        const std::string tmp = "/tmp/jayecu-wheel-empty-" + std::to_string(::getpid());
        std::filesystem::create_directories(tmp);
        setenv("HOME", tmp.c_str(), 1);
        ck(loadUserWheels().empty(), "no file yet -> empty library");
        std::filesystem::remove_all(tmp);
    }

    std::puts("\n-- export and import move wheels between machines --");
    {
        const std::string tmp = "/tmp/jayecu-wheel-io-" + std::to_string(::getpid());
        std::filesystem::create_directories(tmp);
        setenv("HOME", tmp.c_str(), 1);

        Wheel a; a.name = "Shared CAS"; a.sync = "PHASE";
        a.streams.push_back(gapStream(0, 180, 0, {}));
        a.streams.push_back(seqStream(1, {1276, 1276, 1276, 1296, 1276, 800}));
        const std::string file = tmp + "/wheels.json";
        ck(exportWheels(file, { a }), "exported one wheel to a file");

        const std::vector<Wheel> read = readWheelFile(file);
        ck(read.size() == 1 && read[0].name == "Shared CAS", "and it reads back");
        ck(read.size() == 1 && read[0].streams.size() == 2 &&
           read[0].streams[1].cell[5] == 800, "with its streams intact");

        // Importing MERGES: a wheel someone sends you must not wipe your own library.
        Wheel mine; mine.name = "Mine"; mine.streams.push_back(gapStream(0, 36, 2, {0}));
        saveUserWheel(mine);
        ck(importWheels(file) == 1, "import reports one wheel");
        const std::vector<Wheel> lib = loadUserWheels();
        ck(lib.size() == 2, "the imported wheel joined the existing one",
           std::to_string(lib.size()));
        bool haveBoth = false;
        for (const Wheel& w : lib) if (w.name == "Mine") haveBoth = true;
        ck(haveBoth, "and the user's own wheel survived the import");

        // Re-importing the same file updates in place instead of duplicating.
        ck(importWheels(file) == 1 && loadUserWheels().size() == 2,
           "re-importing updates, it does not duplicate");

        ck(importWheels(tmp + "/nope.json") == -1, "an unreadable file reports -1, not a crash");
        std::filesystem::remove_all(tmp);
    }

    std::puts("\n-- an even fine track decodes to an EVEN wheel, not '180 missing 1' --");
    {
        // Straight from the meta as shipped: ratio 0 with no gap list. The designer form must show
        // an even 180-slit track; showing "missing 1" is the same wrong reading that put the
        // firmware one tooth short per revolution.
        const std::string doc = R"({"trigger_wheels":[{"name":"Nissan 360 CAS","sync":"PHASE",
            "cam_edge":0,"streams":[{"rate":0,"kind":"gap","slots":180,"ratio":0,"gaps":[]},
            {"rate":1,"kind":"seq","cell":[1276,1276,1276,1296,1276,800]}]}]})";
        const std::vector<Wheel> w = wheelsFromMeta(jf::JJson::parse(doc));
        ck(w.size() == 1, "decoded");
        const WheelParams p = decodeWheel(w[0]);
        ck(p.crankType == WheelParams::CRANK_EVEN, "classified EVEN, not MISSING",
           "crankType=" + std::to_string(p.crankType));
        ck(p.evenTeeth == 180, "180 even teeth", std::to_string(p.evenTeeth));
    }

    std::puts("\n-- every trigger the decoder supports survives a designer round trip --");
    {
        // buildWheel(decodeWheel(w)) must be lossless for EVERY shape, or the designer silently
        // degrades a wheel the moment it is opened. Each of these exercises a primitive the form
        // could not express before: multiple gaps, a crank sequence, a cam width window, and a cam
        // edge pattern.
        struct Case { const char* what; Wheel w; };
        std::vector<Case> cases;

        Wheel multi; multi.name = "36-2-2-2"; multi.sync = "CRANK";
        multi.streams.push_back(gapStream(0, 36, 3, {0, 1, 14}));
        cases.push_back({ "three gap positions", multi });

        Wheel odd; odd.name = "odd-fire"; odd.sync = "CRANK";
        odd.streams.push_back(seqStream(0, {1350, 2250}));
        cases.push_back({ "crank sequence", odd });

        Wheel pulse; pulse.name = "60-2 + cam"; pulse.sync = "PHASE";
        pulse.streams.push_back(gapStream(0, 60, 3, {0}));
        pulse.streams.push_back(widStream(1, 100, 250, 1200));
        cases.push_back({ "cam width window", pulse });

        Wheel pat; pat.name = "CAS"; pat.sync = "PHASE";
        pat.streams.push_back(gapStream(0, 180, 0, {}));
        pat.streams.push_back(seqStream(1, {1276, 1276, 1276, 1296, 1276, 800}));
        cases.push_back({ "cam edge pattern + even crank", pat });

        for (const Case& c : cases) {
            const Wheel out = buildWheel(decodeWheel(c.w));
            bool same = out.streams.size() == c.w.streams.size();
            for (size_t i = 0; same && i < out.streams.size(); ++i) {
                const WheelStream &a = c.w.streams[i], &b = out.streams[i];
                same = a.prim == b.prim && a.rate == b.rate && a.slots == b.slots &&
                       a.ratio == b.ratio && a.cell == b.cell &&
                       (a.prim != 2 || (a.wMin == b.wMin && a.wMax == b.wMax && a.wTgt == b.wTgt));
            }
            ck(same, std::string("round trip preserves ") + c.what);
        }
    }

    std::puts("\n-- the editor reaches every decoder capability --");
    {
        // Each of these is a decode primitive or per-stream setting the form could not express, so a
        // wheel using it could not be authored and one loaded from the library lost it on save.
        Wheel w; w.name = "everything"; w.sync = "PHASE";
        WheelStream cs = widStream(0, 100, 250, 1200);   // WIDTH on the CRANK, not just a cam
        cs.edge = 1;                                     // FALLING — the form only offered Rising/Both
        cs.windowPct = 12;                               // match tolerance, previously hardcoded 25
        w.streams.push_back(cs);
        WheelStream cam = seqStream(1, {1800, 1800, 1800, 1800});
        cam.edge = 1;
        w.streams.push_back(cam);

        const WheelParams p = decodeWheel(w);
        ck(p.crankType == WheelParams::CRANK_WIDTH, "a WIDTH crank decodes as such",
           std::to_string(p.crankType));
        ck(p.crankEdge == 1, "the crank's FALLING edge survives", std::to_string(p.crankEdge));
        ck(p.windowPct == 12, "the match tolerance survives", std::to_string(p.windowPct));
        ck(p.cams.size() == 1 && p.cams[0].edge == 1, "a cam's FALLING edge survives");

        const Wheel back = buildWheel(p);
        ck(back.streams.size() == 2, "both streams rebuilt", std::to_string(back.streams.size()));
        ck(!back.streams.empty() && back.streams[0].prim == 2, "crank stays a WIDTH stream");
        ck(!back.streams.empty() && back.streams[0].edge == 1, "and keeps its edge");
        ck(!back.streams.empty() && back.streams[0].windowPct == 12, "and its tolerance");
    }

    std::puts("\n-- deleting removes one of the user's own, and only those --");
    {
        const std::string tmp = "/tmp/jayecu-wheel-del-" + std::to_string(::getpid());
        std::filesystem::create_directories(tmp);
        setenv("HOME", tmp.c_str(), 1);

        Wheel a; a.name = "Keep";   a.streams.push_back(gapStream(0, 36, 2, {0}));
        Wheel b; b.name = "Delete"; b.streams.push_back(gapStream(0, 60, 3, {0}));
        saveUserWheel(a); saveUserWheel(b);
        ck(loadUserWheels().size() == 2, "two to begin with");

        ck(deleteUserWheel("Delete"), "delete reports success");
        const std::vector<Wheel> left = loadUserWheels();
        ck(left.size() == 1 && left[0].name == "Keep", "the other one survived",
           std::to_string(left.size()));

        // A shipped wheel is not in the user store, so deleting it must fail rather than appear to
        // work and have the wheel reappear on the next meta load.
        ck(!deleteUserWheel("60-2"), "a wheel that is not the user's own is refused");
        ck(loadUserWheels().size() == 1, "and nothing else was removed");
        std::filesystem::remove_all(tmp);
    }

    // WHICH SLOT a cam is applied to. The designer's "which cam" list writes into WheelStream::role, which
    // is a SLOT of trigger.streams[] — 2 Intake B1, 3 Exhaust B1 — and it wrote the 1-based DISPLAY role
    // instead, so the first cam of every designer-authored wheel landed on Cam Exhaust B1 and a fourth
    // exceeded the count and was dropped without a word.
    std::puts("\n-- a cam is applied to the slot it names --");
    {
        Wheel w;
        w.name = "2JZ-ish";
        w.streams.push_back(gapStream(0, 36, 3, { 0 }));      // crank
        w.streams.push_back(widStream(1, 0, 7200, 3650));     // one cam, role unset

        auto slotsOf = [](const Wheel& x) {
            std::string on;
            for (const auto& pr : wheelPairs(x))
                if (pr.first.find(".enabled") != std::string::npos && pr.second != 0.0)
                    on += pr.first.substr(16, 1);            // trigger.streams[N]
            return on;
        };
        ck(slotsOf(w) == "02", "an unplaced cam goes to the first CAM slot, 2", "slots " + slotsOf(w));

        // The round trip a designer apply takes, with the cam's role set as the list's first item does.
        WheelParams p = decodeWheel(w);
        ck(p.cams.size() == 1, "the cam decodes as a cam, not as a second crank",
           std::to_string(p.cams.size()) + " cams, hasCrank2=" + std::to_string(p.hasCrank2));
        p.cams[0].role = kSlotCamBase + 0;                   // "Intake bank 1"
        ck(slotsOf(buildWheel(p)) == "02", "…and choosing 'Intake bank 1' keeps it there, not one past it",
           "slots " + slotsOf(buildWheel(p)));
        p.cams[0].role = kSlotCamBase + 3;                   // "Exhaust bank 2" — the last slot there is
        ck(slotsOf(buildWheel(p)) == "05", "…the last cam slot is reachable and inside the count",
           "slots " + slotsOf(buildWheel(p)));
    }

    // A wheel states its angles in DEGREES; the config stores tenths. Applying one has to convert, and
    // for a long time nothing did: wheelPairs emitted 155.0 on a comment claiming the Cache multiplied by
    // ten, Cache::setConfigValue takes RAW and writes bytes, and a 2JZ's 155° trigger offset went into the
    // ECU as raw 155 — read back, and acted on, as 15.5°. A tenth of the right base timing.
    std::puts("\n-- applying a wheel converts engineering units into raw bytes --");
    {
        MetaModel m;
        if (!m.loadFile(REAL_META)) {
            std::puts("  (no meta — skipped)");
        } else {
            Cache& c = Cache::instance();
            c.setMeta(&m);
            c.setConfigImage(m.defaultImage());

            Wheel w;
            w.name = "2JZ 36-2 +Rear Cam";
            w.tdcOffset   = 155.0;              // degrees, as the library states it
            w.sensorAngle = 105.0;
            w.streams.push_back(gapStream(0, 36, 3, { 6 }));

            const int n = applyWheelToConfig(w, c);
            ck(n > 0, "applying writes settings", std::to_string(n));
            const double raw = c.readRaw(m.locate("trigger.trigger_offset_btdc"));
            ck(raw == 1550, "155 degrees is stored as raw 1550, not 155",
               "raw " + std::to_string(raw) + " = " + std::to_string(raw * 0.1) + " deg");
            ck(c.readRaw(m.locate("trigger.sensor_angle")) == 1050,
               "…and the pickup angle likewise");
            // An unscaled field must pass through untouched — the conversion is per field, not blanket.
            ck(c.readRaw(m.locate("trigger.streams[0].slots")) == 36,
               "a scale-1.0 field is unchanged by the conversion",
               std::to_string(c.readRaw(m.locate("trigger.streams[0].slots"))));
            // And the round trip: read back in engineering units and the wheel is the one applied.
            const Wheel back = wheelFromConfig([&c](const std::string& p) {
                return c.configValue(p) * c.configScale(p);
            });
            ck(back.tdcOffset == 155.0, "reading the config back gives the degrees the wheel stated",
               std::to_string(back.tdcOffset));
        }
    }

    std::puts("\n-- a wheel REPLACES the cam-phase setup rather than inheriting it --");
    {
        MetaModel m;
        if (!m.loadFile(REAL_META)) {
            std::puts("  (no meta — skipped)");
        } else {
            Cache& c = Cache::instance();
            c.setMeta(&m);
            c.setConfigImage(m.defaultImage());

            // Put a PHASED cam in the ECU first, as a previously-applied VVT wheel would have.
            c.writeRaw(m.locate("trigger.streams[2].phased"), 1);
            c.writeRaw(m.locate("trigger.streams[2].phase_authority"), 600);
            c.writeRaw(m.locate("trigger.streams[2].phase_allowance"), 400);

            // Now apply a wheel whose cam is FIXED. wheelPairs() emits every slot so that applying
            // a wheel replaces the trigger setup; these fields used not to be emitted at all, so
            // the new fixed cam kept the old phaser's 60 degree band — which the firmware's own
            // comment calls blinding the check, and it is silent.
            Wheel w;
            w.name = "fixed-cam wheel over a phased config";
            w.streams.push_back(gapStream(0, 36, 2, { 0 }));
            w.streams.push_back(widStream(1, 600, 1100, 1000));
            w.streams.back().role = 2;
            applyWheelToConfig(w, c);

            ck(c.readRaw(m.locate("trigger.streams[2].phased")) == 0,
               "a fixed-cam wheel clears the previous wheel's phased flag",
               std::to_string(c.readRaw(m.locate("trigger.streams[2].phased"))));
            ck(c.readRaw(m.locate("trigger.streams[2].phase_allowance")) == 0,
               "…and its mechanical allowance",
               std::to_string(c.readRaw(m.locate("trigger.streams[2].phase_allowance"))));

            // A wheel that IS phased states it, and it survives the trip to the ECU and back.
            Wheel v;
            v.name = "phased";
            v.streams.push_back(gapStream(0, 60, 3, { 0 }));
            v.streams.push_back(widStream(1, 600, 1100, 1000));
            v.streams.back().role = 2;
            v.streams.back().phased = 1;
            v.streams.back().phaseAuthority = 900;
            v.streams.back().nominalAngle = 1000;
            applyWheelToConfig(v, c);
            const Wheel back = wheelFromConfig([&c](const std::string& p) {
                return c.configValue(p) * c.configScale(p);
            });
            const WheelStream* cam = nullptr;
            for (const WheelStream& st : back.streams) if (st.rate == 1) { cam = &st; break; }
            ck(cam && cam->phased == 1, "a phased cam round-trips through the ECU config");
            // ANGLES IN 0.1 DEGREES MUST NOT BE HANDED OVER AS DEGREES. Every scale-0.1 field
            // used to be written ten times too large and then silently clamped: this wheel's
            // 60-110 deg cam window became 600.0-720.0 deg, which no real cam pulse can match, and
            // its 100.0 deg VVT reference became 720.0. The studio could not correctly apply a
            // WIDTH-cam wheel at all.
            ck(c.readRaw(m.locate("trigger.streams[2].width_min")) == 600,
               "a 60.0 deg cam window floor is written as 60.0, not 600.0",
               std::to_string(c.readRaw(m.locate("trigger.streams[2].width_min"))));
            ck(c.readRaw(m.locate("trigger.streams[2].width_max")) == 1100,
               "…and the ceiling is not clamped at the field maximum",
               std::to_string(c.readRaw(m.locate("trigger.streams[2].width_max"))));
            ck(c.readRaw(m.locate("trigger.streams[2].nominal_angle")) == 1000,
               "…nor the VVT reference",
               std::to_string(c.readRaw(m.locate("trigger.streams[2].nominal_angle"))));
            ck(c.readRaw(m.locate("trigger.streams[2].phase_authority")) == 900,
               "…nor the phaser authority",
               std::to_string(c.readRaw(m.locate("trigger.streams[2].phase_authority"))));
            ck(cam && cam->nominalAngle == 1000, "the VVT reference round-trips in 0.1 deg units",
               cam ? std::to_string(cam->nominalAngle) : "no cam");
            ck(cam && cam->phaseAuthority == 900, "…carrying its authority",
               cam ? std::to_string(cam->phaseAuthority) : "no cam");
        }
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All trigger-library tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
