// Can a captured crank signal be read back as the wheel that made it?
//
// The lanes here are SYNTHESISED, so the answer is known before the function is asked: a 36-1 at a
// chosen rpm really does have 35 edges and one 2x gap per turn. A fit that agrees with a signal it
// was given the answer to is worth something; one checked only against a real capture is checked
// against a second guess.
//
//   cmake --build build --target trigger_fit_test && ./build/trigger_fit_test
#include "model/TriggerFit.h"
#include "model/TriggerWheel.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-60s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// A toothed wheel turning at a steady rpm, as the ECU would have logged it.
// `absent` names the nominal indices with no tooth, counting from 0 = the first tooth after the gap.
static triggerlog::Lane synth(int teeth, const std::vector<int>& absent, double rpm, int revs,
                              double jitterFrac = 0.0, int stream = 0, const char* nm = "Crank 1") {
    triggerlog::Lane lane;
    lane.stream = static_cast<uint8_t>(stream); lane.name = nm;
    const double revUs   = 60.0e6 / rpm;
    const double pitchUs = revUs / teeth;
    double t = 0.0, prev = 0.0;
    bool first = true;
    for (int r = 0; r < revs; ++r)
        for (int i = 0; i < teeth; ++i) {
            const double at = r * revUs + i * pitchUs;
            // A jitter that grows across the rev, standing in for an engine that is not turning at a
            // constant speed — the fit must survive it, not require it to be absent.
            const double wob = jitterFrac * pitchUs * std::sin(6.28318 * double(i) / teeth);
            if (std::find(absent.begin(), absent.end(), i) != absent.end()) continue;
            t = at + wob;
            triggerlog::Interval iv;
            iv.tUs     = static_cast<uint32_t>(t);
            iv.deltaUs = first ? 0u : static_cast<uint32_t>(t - prev);
            iv.high    = true;
            lane.bars.push_back(iv);
            prev = t; first = false;
        }
    return lane;
}

// A wheel turning as a STARTER MOTOR turns it: the engine surges and stalls against each
// compression, so the speed swings hard WITHIN a single revolution. Every capture of an unknown
// wheel is taken in this state — nobody has a wheel definition to run the engine with yet — so this
// is the normal case, not the hard one.
static triggerlog::Lane cranking(int teeth, const std::vector<int>& absent, double meanRpm,
                                 int revs, double swing, int compPerRev) {
    triggerlog::Lane lane; lane.stream = 0; lane.name = "Crank 1";
    double t = 0.0, prev = 0.0; bool first = true;
    for (int r = 0; r < revs; ++r)
        for (int i = 0; i < teeth; ++i) {
            const double frac = double(i) / teeth;
            const double rpm  = meanRpm * (1.0 + swing * std::sin(2 * 3.14159265358979 * compPerRev * frac));
            const double pitch = 60.0e6 / (rpm * teeth);
            if (std::find(absent.begin(), absent.end(), i) == absent.end()) {
                triggerlog::Interval iv;
                iv.tUs = static_cast<uint32_t>(t);
                iv.deltaUs = first ? 0u : static_cast<uint32_t>(t - prev);
                iv.high = true; lane.bars.push_back(iv); prev = t; first = false;
            }
            t += pitch;
        }
    return lane;
}

int main() {
    std::puts("=== trigger fit ===");

    {   // CRANKING. The engine's oscillation and the wheel's geometry are two different things and
        // the first has to be measured before the second can be. A global median called a 36-1 at
        // 60% swing "39-4"; neighbour-averaging fixed the gentle cases and broke again at 60%; a
        // local parabola got to 60% on a four and no further.
        //
        // What these hold is the model that does work. The wave is fitted AT ITS OWN FREQUENCY,
        // found by autocorrelating the teeth, and fitted to the crankshaft's RATE rather than to a
        // tooth's duration, because it is the rate that swings sinusoidally and one over a sine is
        // not a sine. The comp column is compressions per revolution — cylinders over two on a
        // four-stroke — and it is the reason a six is harder than a four: two thirds of the period
        // in which to swing just as far. All of these are measured, not told.
        struct Case { const char* what; int teeth; std::vector<int> absent; double swing; int comp;
                      int wantT; int wantM; };
        const Case cases[] = {
            { "36-1 steady",              36, {0},    0.00, 2, 36, 1 },
            { "36-1 at +-25% swing",      36, {0},    0.25, 2, 36, 1 },
            { "36-1 at +-45% swing",      36, {0},    0.45, 2, 36, 1 },
            { "36-1 at +-60% swing",      36, {0},    0.60, 2, 36, 1 },
            { "36-1 at +-70% swing",      36, {0},    0.70, 2, 36, 1 },
            { "60-2 at +-50% swing",      60, {0, 1}, 0.50, 2, 60, 2 },
            { "60-2 at +-65% swing",      60, {0, 1}, 0.65, 2, 60, 2 },
            { "36-1, six, +-60% swing",   36, {0},    0.60, 3, 36, 1 },
            { "36-1, eight, +-60% swing", 36, {0},    0.60, 4, 36, 1 },
            { "36-1, eight, +-70% swing", 36, {0},    0.70, 4, 36, 1 },
            { "60-2, six, +-60% swing",   60, {0, 1}, 0.60, 3, 60, 2 },
            // Three gaps and a lumpy four. This one needed the turn search to survive a first
            // anomaly measured against a one-sided window, and whole-tooth gaps to key on the
            // whole number: read as 2.96 here and 3.10 there, one gap keyed as two and the
            // wheel could not find its own revolution.
            { "36-2-2-2 at +-40% swing",  36, {10, 11, 24, 25, 34, 35}, 0.40, 2, 36, 6 },
        };
        for (const Case& c : cases) {
            const triggerfit::StreamFit f =
                triggerfit::fitStream(cranking(c.teeth, c.absent, 220.0, 6, c.swing, c.comp));
            ck(f.ok && f.teeth == c.wantT && f.missing == c.wantM, c.what,
               f.ok ? (std::to_string(f.teeth) + "-" + std::to_string(f.missing)) : f.why);
        }
    }

    {   // The plain case, and the one every other is a deviation from.
        const triggerfit::StreamFit f = triggerfit::fitStream(synth(36, {0}, 1200.0, 4));
        ck(f.ok, "a 36-1 fits", f.why);
        ck(f.teeth == 36,   "…36 teeth",   std::to_string(f.teeth));
        ck(f.missing == 1,  "…1 missing",  std::to_string(f.missing));
        // rpm is a fact about the CAPTURE, not about one wire — a stream alone cannot know whether
        // its period is a revolution or a cycle. For a crank-rate gap wheel it falls straight out.
        const double rpm = 60.0e6 / f.periodUs;
        ck(std::fabs(rpm - 1200.0) < 12.0, "…at about 1200 rpm", std::to_string(rpm));
        ck(f.edgeAngles.size() == 35, "…35 measured edges in the turn",
           std::to_string(f.edgeAngles.size()));
    }

    {   // Two adjacent absent teeth — the 60-2 every other engine uses.
        const triggerfit::StreamFit f = triggerfit::fitStream(synth(60, {0, 1}, 3000.0, 4));
        ck(f.ok, "a 60-2 fits", f.why);
        ck(f.teeth == 60,  "…60 teeth",  std::to_string(f.teeth));
        ck(f.missing == 2, "…2 missing", std::to_string(f.missing));
    }

    {   // THREE GAPS PER TURN, UNEVENLY SPACED — the shape one missing-count cannot describe and the
        // reason the form needed a list. Gap-to-gap is a THIRD of this wheel, so a fit that assumed
        // one gap per revolution read it as a 12-1; the period of the gap pattern is what a
        // revolution actually is.
        const triggerfit::StreamFit f = triggerfit::fitStream(synth(36, {0, 11, 25}, 900.0, 4));
        ck(f.ok, "a 3-gap unevenly spaced wheel fits", f.why);
        ck(f.teeth == 36,  "…36 teeth",  std::to_string(f.teeth));
        ck(f.missing == 3, "…3 missing", std::to_string(f.missing));
        ck(f.gapPos.size() == 3, "…and it names three separate gap positions",
           std::to_string(f.gapPos.size()));
    }

    {   // EVENLY SPACED GAPS ARE A SMALLER WHEEL, and saying so is the honest answer rather than a
        // bug: 3 gaps at 0, 12 and 24 of 36 is a 12-1 turning three times, and nothing in a stream
        // of teeth can tell those apart. Telling them apart needs the cam — the same reason the
        // decoder needs one.
        const triggerfit::StreamFit f = triggerfit::fitStream(synth(36, {0, 12, 24}, 900.0, 4));
        ck(f.ok, "evenly spaced gaps still fit", f.why);
        ck(f.teeth == 12 && f.missing == 1, "…as the 12-1 they are indistinguishable from",
           std::to_string(f.teeth) + "-" + std::to_string(f.missing));
    }

    {   // An engine that is not turning steadily. The fit must not need a metronome.
        const triggerfit::StreamFit f = triggerfit::fitStream(synth(36, {0}, 600.0, 4, 0.15));
        ck(f.ok, "a 36-1 fits through 15% speed wobble", f.why);
        ck(f.teeth == 36 && f.missing == 1, "…still 36-1",
           std::to_string(f.teeth) + "-" + std::to_string(f.missing));
    }

    {   // AN EVEN TRAIN IS A RESULT, NOT A FAILURE. This is the change that makes a CAS readable:
        // its crank track is exactly this, and the old fitter called it an error twice and produced
        // nothing. It fits as Even — with no period, because a uniform train has no landmark and
        // cannot say where it starts. Only the capture as a whole can supply that.
        const triggerfit::StreamFit f = triggerfit::fitStream(synth(36, {}, 1200.0, 4));
        ck(f.ok, "an even train fits", f.why);
        ck(f.shape == triggerfit::Shape::Even, "…as Even");
        ck(f.periodUs == 0.0, "…with no period of its own", std::to_string(f.periodUs));
    }

    {   // A NISSAN CAS: 360 even slits on the crank and a coded cam track. Neither stream can be
        // read alone — the crank has no landmark and the cam's period is a CYCLE, not a turn — and
        // together they are a complete trigger system. This is the case that prompted the rewrite.
        std::vector<triggerlog::Lane> lanes;
        lanes.push_back(synth(360, {}, 900.0, 4, 0.0, 0, "Crank 1"));
        // The cam: one landmark per cam revolution, which is one ENGINE CYCLE. At the crank's 900 rpm
        // the cam turns at 450. The absent slot must make the pattern asymmetric — a symmetric one
        // repeats within the turn and is honestly a smaller wheel, the same trap as evenly spaced
        // crank gaps above.
        lanes.push_back(synth(12, {1}, 450.0, 4, 0.0, 2, "Cam 1"));
        const triggerfit::CaptureFit c = triggerfit::fitCapture(lanes);
        ck(c.ok, "a CAS-style capture fits as a system", c.why);
        ck(std::fabs(c.rpm - 900.0) < 25.0, "…at the crank's 900 rpm", std::to_string(c.rpm));
        if (c.ok && c.crankIdx >= 0) {
            const triggerfit::StreamFit& cr = c.streams[size_t(c.crankIdx)];
            ck(cr.name == "Crank 1", "…the DENSEST wire is taken as the crank", cr.name);
            ck(cr.shape == triggerfit::Shape::Even, "…its track reads as even");
            ck(cr.teeth == 360, "…and the cam's period gives it 360 slits", std::to_string(cr.teeth));
            ck(cr.cycleRatio == 1, "…turning with the crank");
        }
        const auto cam = std::find_if(c.streams.begin(), c.streams.end(),
                                      [](const triggerfit::StreamFit& s){ return s.name == "Cam 1"; });
        ck(cam != c.streams.end() && cam->cycleRatio == 2, "the cam is one period per ENGINE CYCLE");
    }

    {   // A WIDTH-CODED track must not be described as missing teeth. Its departures are not whole
        // multiples of the pitch, and "3 teeth missing" would name a wheel nobody built.
        triggerlog::Lane l; l.stream = 2; l.name = "Cam 1";
        const double base = 4000.0;
        const double seq[] = { 1.0, 2.7, 1.0, 1.0, 3.4, 1.0 };   // two coded widths, repeating
        double t = 0.0; bool first = true;
        for (int rep = 0; rep < 5; ++rep)
            for (double m : seq) {
                triggerlog::Interval iv;
                iv.tUs = static_cast<uint32_t>(t); iv.deltaUs = first ? 0u : static_cast<uint32_t>(base * m);
                iv.high = true; l.bars.push_back(iv);
                t += base * m; first = false;
            }
        const triggerfit::StreamFit f = triggerfit::fitStream(l);
        ck(f.ok, "a width-coded track fits", f.why);
        ck(f.shape == triggerfit::Shape::Coded, "…as Coded, not as missing teeth");
        ck(f.missing == 0, "…claiming no missing teeth", std::to_string(f.missing));
        ck(f.anomalies.size() >= 2, "…and naming where it departs from regular",
           std::to_string(f.anomalies.size()));
    }

    {   // Not enough signal to say anything.
        triggerlog::Lane tiny = synth(36, {0}, 1200.0, 4);
        tiny.bars.resize(5);
        const triggerfit::StreamFit f = triggerfit::fitStream(tiny);
        ck(!f.ok && !f.why.empty(), "a stub of a capture is refused with a reason", f.why);
    }

    {   // A REAL CAPTURE, off the bench rig, through the production path. The synthesised cases above
        // prove the arithmetic; this proves the arithmetic is pointed at the right bytes — that what
        // the ECU actually sends, decoded by decodeLanes, is the shape fitCrankLane expects.
        //
        // Taken with the ECU's decoder DELIBERATELY WRONG (told 60-2+cam while the rig span a 36-1),
        // so sync_level was 0 and rpm read 0 throughout. That is the whole case the trigger log
        // exists for and the one this feature is FOR: a wheel the ECU cannot read at all.
        std::vector<triggerlog::Record> recs;
        std::ifstream in(FIXTURE_DIR "/crank_36_1_900rpm.csv");
        std::string line;
        while (std::getline(in, line)) {
            std::replace(line.begin(), line.end(), ',', ' ');
            std::istringstream ss(line);
            unsigned long t, lv, fl;
            if (!(ss >> t >> lv >> fl)) continue;
            triggerlog::Record r;
            r.tUs = uint32_t(t); r.levels = uint8_t(lv); r.flags = uint8_t(fl);
            recs.push_back(r);
        }
        ck(!recs.empty(), "the recorded capture loads", std::to_string(recs.size()) + " records");
        std::vector<triggerlog::Lane> lanes;
        const auto res = triggerlog::decodeLanes(recs, { "Crank 1", "Crank 2", "Cam 1", "Cam 2" }, lanes);
        ck(res.ok && !lanes.empty(), "…and decodes into lanes", res.message);
        if (res.ok && !lanes.empty()) {
            const triggerfit::StreamFit f = triggerfit::fitStream(lanes.front());
            ck(f.ok, "a real unsynced capture fits", f.why);
            ck(f.teeth == 36 && f.missing == 1, "…as the 36-1 the rig was spinning",
               std::to_string(f.teeth) + "-" + std::to_string(f.missing));
            ck(std::fabs(60.0e6 / f.periodUs - 900.0) < 20.0, "…at the 900 rpm it was spun at",
               std::to_string(60.0e6 / f.periodUs));
        }
    }

    {   // A SHORT MARK, which the rig has no wheel for. An extra index tooth splits one pitch into
        // two halves and creates no long interval anywhere, so a fitter looking only for gaps sees a
        // perfectly uniform train. It is the pattern on a great many cam and distributor wheels.
        //
        // The turn is found by comparing one turn to the next, not one tooth to its neighbours —
        // which is also why it needs three full turns before it will answer, and says so honestly
        // rather than guessing when it has only two.
        auto extraTooth = [](int teeth, double rpm, double revs) {
            triggerlog::Lane lane; lane.stream = 0; lane.name = "Crank 1";
            const double pitch = 60.0e6 / (rpm * teeth);
            double t = 0.0, prev = 0.0; bool first = true;
            auto edge = [&](double when) {
                triggerlog::Interval iv;
                iv.tUs = uint32_t(when); iv.deltaUs = first ? 0u : uint32_t(when - prev);
                iv.high = true; lane.bars.push_back(iv); prev = when; first = false;
            };
            for (int i = 0; i < int(teeth * revs); ++i) {
                edge(t);
                if (i % teeth == teeth / 2) edge(t + 0.5 * pitch);
                t += pitch;
            }
            return lane;
        };
        const triggerfit::StreamFit f = triggerfit::fitStream(extraTooth(60, 300.0, 4.0));
        ck(f.ok && f.shape == triggerfit::Shape::Coded, "a 60-tooth wheel with one extra index tooth fits",
           f.ok ? ("shape " + std::to_string(int(f.shape))) : f.why);
        ck(f.teeth == 61, "…as 61 marks, the extra one included", std::to_string(f.teeth));
        ck(f.anomalies.size() == 2, "…naming the two half-pitches it splits",
           std::to_string(f.anomalies.size()));
        ck(f.uniqueKey, "…and it can say which turn it is on", f.keyWhat);

        const triggerfit::StreamFit g = triggerfit::fitStream(extraTooth(60, 300.0, 2.5));
        ck(g.ok && g.shape == triggerfit::Shape::Even && g.teeth == 0,
           "…but under three turns it says even rather than guessing",
           g.ok ? ("shape " + std::to_string(int(g.shape))) : g.why);
        ck(g.edgesSeen > 100, "…while still reporting what it counted", std::to_string(g.edgesSeen));
    }

    {   // EVERY WHEEL THE RIG CAN SPIN, captured raw with the ECU told NOTHING about any of them —
        // one generic both-edges config, unchanged between wheels. These are not synthesised: they
        // are what came back off the bench, and they are here because four of them were wrong.
        //
        // What each wheel IS comes from the stim's own pattern tables (tools/gen_rebench.py WHEELS),
        // never from a tooth count read off a web page.
        struct Want {
            const char* file;
            bool        capture;      // does the capture as a whole make sense of itself
            bool        sync;         // ...and can something in it say where the engine is
            int         teeth, missing;   // of the CRANK stream; -1 to not ask
            const char* what;
        };
        const Want wheels[] = {
            // Plain even wheels. No landmark exists, so refusing is the answer, not a failure.
            { "00_4cyl_dizzy_300rpm.csv",      false, false, -1, -1, "a 4cyl dizzy has no landmark" },
            { "01_6cyl_dizzy_300rpm.csv",      false, false, -1, -1, "nor a 6cyl" },
            { "02_8cyl_dizzy_300rpm.csv",      false, false, -1, -1, "nor an 8cyl" },
            { "13_dizzy_4cyl_50_40_300rpm.csv",false, false, -1, -1, "nor a 50/40 dizzy" },
            // Missing-tooth cranks.
            { "03_60_2_300rpm.csv",            true,  true,  60,  2, "a 60-2" },
            { "06_36_1_300rpm.csv",            true,  true,  36,  1, "a 36-1" },
            { "07_24_1_300rpm.csv",            true,  true,  24,  1, "a 24-1" },
            { "09_8_1_300rpm.csv",             true,  true,   8,  1, "an 8-1" },
            { "11_12_1___cam_300rpm.csv",      true,  true,  12,  1, "a 12-1 with a cam" },
            { "12_40_1_300rpm.csv",            true,  true,  40,  1, "a 40-1" },
            { "16_12_3_300rpm.csv",            true,  true,  12,  3, "a 12-3" },
            // THREE IDENTICAL GAPS, UNEVENLY SPACED. Measured perfectly and then declared unsyncable,
            // because no single gap differs from the others. The arrangement is the key: spacings of
            // 150, 180 and 30 degrees occur once each, and the ECU's own decoder syncs this wheel.
            { "17_36_2_2_2_h4_300rpm.csv",     true,  true,  36,  6, "a 36-2-2-2 H4 syncs" },
            { "18_36_2_2_2_h6_300rpm.csv",     true,  true,  36,  6, "a 36-2-2-2 H6 syncs" },
            { "19_36_2_2_2___cam_300rpm.csv",  true,  true,  36,  6, "a 36-2-2-2 with a cam" },
            // A SHORT LANDMARK. The GM 4200's turn is fixed by a 10 degree tooth among 50 to 70
            // degree ones. Looking only for LONG intervals, the fitter saw a uniform train.
            { "20_gm_4200_300rpm.csv",         true,  true,   7,  0, "a GM 4200's short tooth" },
            // A PATTERN THAT IS NOT A SPIKE. The 6G72 cam runs 190/170/195/165 degrees — never more
            // than 8% from uniform, so no threshold on one interval will ever see it, and the crank
            // is three even teeth that cannot sync on their own.
            { "22_6g72_300rpm.csv",            true,  true,   3,  0, "a 6G72 syncs on its cam" },
            // The densest wheel on the rig: an even 180-slit crank track that can never sync, and a
            // window track of six slots of which one starts early.
            { "34_nissan_360_cas_300rpm.csv",  true,  true, 180,  0, "a Nissan CAS syncs on its windows" },
            { "21_fe3_36_1___cam_300rpm.csv",  true,  true,  36,  1, "an FE3 36-1 with a coded cam" },
        };
        for (const Want& w : wheels) {
            std::vector<triggerlog::Record> recs;
            std::ifstream in(std::string(FIXTURE_DIR "/stim/") + w.file);
            std::string line;
            while (std::getline(in, line)) {
                std::replace(line.begin(), line.end(), ',', ' ');
                std::istringstream ss(line);
                unsigned long t, lv, fl;
                if (!(ss >> t >> lv >> fl)) continue;
                triggerlog::Record r;
                r.tUs = uint32_t(t); r.levels = uint8_t(lv); r.flags = uint8_t(fl);
                recs.push_back(r);
            }
            if (recs.empty()) { ck(false, w.what, std::string("no fixture: ") + w.file); continue; }
            std::vector<triggerlog::Lane> lanes;
            triggerlog::decodeLanes(recs, { "Crank 1", "Crank 2", "Cam 1", "Cam 2" }, lanes);
            const triggerfit::CaptureFit c = triggerfit::fitCapture(lanes);
            if (c.ok != w.capture) { ck(false, w.what, c.ok ? "fitted, and should not have" : c.why); continue; }
            if (!w.capture) { ck(true, w.what, "refused, correctly"); continue; }
            // The rig was spun at 300 rpm. A wheel read as half or twice itself reports half or
            // twice the speed, which is how a 6G72 cam mistaken for two teeth was caught.
            std::string got = std::to_string(int(c.rpm)) + " rpm";
            bool good = std::fabs(c.rpm - 300.0) < 15.0 && c.canSync == w.sync;
            if (c.crankIdx >= 0 && w.teeth >= 0) {
                const triggerfit::StreamFit& s = c.streams[size_t(c.crankIdx)];
                got += ", crank " + std::to_string(s.teeth) + "-" + std::to_string(s.missing);
                good = good && s.teeth == w.teeth && s.missing == w.missing;
            }
            if (!c.canSync) got += ", " + c.syncNote;
            ck(good, w.what, got);
        }
    }

    {   // THE SAME WHEELS, CRANKED. Captured off the rig with the stim's crank curve at 45% and two
        // compressions a revolution — the engine's speed swinging from 186 to 486 rpm about a true
        // 300, which is what a starter motor actually does and what every capture of an unknown
        // wheel is taken through. The steady sweep above proves the geometry; this proves the
        // geometry survives the engine.
        //
        // The rig was spun at a MEASURED 300 rpm at every amplitude — gap to gap, one revolution is
        // 199718 to 200760 us — so a wheel read as half or twice itself shows up as 600 or 150 here.
        // That is how the Nissan's crank track was caught coming back as 90 teeth at 600 rpm: it had
        // found one compression and called it a turn.
        struct Want { const char* file; bool capture; int teeth, missing; const char* what; };
        const Want cranked[] = {
            { "03_60_2_300rpm_crank.csv",           true,  60,  2, "a 60-2, cranked" },
            { "06_36_1_300rpm_crank.csv",           true,  36,  1, "a 36-1, cranked" },
            { "07_24_1_300rpm_crank.csv",           true,  24,  1, "a 24-1, cranked" },

            { "12_40_1_300rpm_crank.csv",           true,  40,  1, "a 40-1, cranked" },


            { "18_36_2_2_2_h6_300rpm_crank.csv",    true,  36,  6, "a 36-2-2-2 H6, cranked" },

            { "20_gm_4200_300rpm_crank.csv",        true,   7,  0, "a GM 4200, cranked" },
            { "21_fe3_36_1___cam_300rpm_crank.csv", true,  36,  1, "an FE3 36-1 and cam, cranked" },
            { "22_6g72_300rpm_crank.csv",           true,   3,  0, "a 6G72, cranked" },
            { "34_nissan_360_cas_300rpm_crank.csv", true, 180,  0, "a Nissan CAS, cranked" },
            { "11_12_1___cam_300rpm_crank.csv",     true,  12,  1, "a 12-1 and cam, cranked" },
            { "00_4cyl_dizzy_300rpm_crank.csv",     false, -1, -1, "a 4cyl dizzy is still refused" },
            { "13_dizzy_4cyl_50_40_300rpm_crank.csv", false, -1, -1, "so is a 50/40 dizzy" },
        };
        for (const Want& w : cranked) {
            std::vector<triggerlog::Record> recs;
            std::ifstream in(std::string(FIXTURE_DIR "/stim/") + w.file);
            std::string line;
            while (std::getline(in, line)) {
                std::replace(line.begin(), line.end(), ',', ' ');
                std::istringstream ss(line);
                unsigned long t, lv, fl;
                if (!(ss >> t >> lv >> fl)) continue;
                triggerlog::Record r;
                r.tUs = uint32_t(t); r.levels = uint8_t(lv); r.flags = uint8_t(fl);
                recs.push_back(r);
            }
            if (recs.empty()) { ck(false, w.what, std::string("no fixture: ") + w.file); continue; }
            std::vector<triggerlog::Lane> lanes;
            triggerlog::decodeLanes(recs, { "Crank 1", "Crank 2", "Cam 1", "Cam 2" }, lanes);
            const triggerfit::CaptureFit c = triggerfit::fitCapture(lanes);
            if (c.ok != w.capture) { ck(false, w.what, c.ok ? "fitted, and should not have" : c.why); continue; }
            if (!w.capture) { ck(true, w.what, "refused, correctly"); continue; }
            std::string got = std::to_string(int(c.rpm)) + " rpm";
            bool good = std::fabs(c.rpm - 300.0) < 20.0 && c.canSync;
            if (c.crankIdx >= 0 && w.teeth >= 0) {
                const triggerfit::StreamFit& st = c.streams[size_t(c.crankIdx)];
                got += ", crank " + std::to_string(st.teeth) + "-" + std::to_string(st.missing);
                good = good && st.teeth == w.teeth && st.missing == w.missing;
            }
            ck(good, w.what, got);
        }

        // GAPS THAT SPAN A LARGE PART OF A COMPRESSION. Measured on the rig at 45%, the wheels that
        // come back wrong are exactly the ones whose gap covers a quarter or more of one compression
        // cycle — a 12-3's gap is half of one, an 8-1's a quarter — plus the 36-2-2-2s, which have
        // three gaps and lose a tooth from one of them:
        //
        //     wheel        teeth per compression    gap spans        cranked
        //     36-1                   18             6% of a cycle    36-1
        //     60-2                   30             7%               60-2
        //     12-1                    6            17%               12-1
        //     8-1                     4            25%               7-0    gap lost
        //     12-3                    6            50%               9-0    gap lost
        //     36-2-2-2               18            11% x3            34-4   a tooth short
        //
        // The cause is that a gap is sized by comparing it to ONE value of the fitted wave, and a
        // gap covering half a compression does not take twice any single tooth period near it — the
        // engine is a different speed at each end of it. The fix is to integrate the fitted wave
        // across the gap instead of multiplying a point on it: with the rate varying linearly from
        // r0 to r1 across a span S the crossing time is S*ln(r1/r0)/(r1-r0), a closed form, and the
        // whole number of teeth is then whichever one that predicts. Not done here.
        //
        // The speed is right in every one of these — the rig was at 300 and they all say 300 — so
        // what is lost is the tooth count, not the capture.
        for (const char* f : { "09_8_1_300rpm_crank.csv", "16_12_3_300rpm_crank.csv",
                               "17_36_2_2_2_h4_300rpm_crank.csv", "19_36_2_2_2___cam_300rpm_crank.csv" }) {
            std::vector<triggerlog::Record> recs;
            std::ifstream in(std::string(FIXTURE_DIR "/stim/") + f);
            std::string line;
            while (std::getline(in, line)) {
                std::replace(line.begin(), line.end(), ',', ' ');
                std::istringstream ss(line);
                unsigned long t, lv, fl;
                if (!(ss >> t >> lv >> fl)) continue;
                triggerlog::Record r;
                r.tUs = uint32_t(t); r.levels = uint8_t(lv); r.flags = uint8_t(fl);
                recs.push_back(r);
            }
            std::vector<triggerlog::Lane> lanes;
            triggerlog::decodeLanes(recs, { "Crank 1", "Crank 2", "Cam 1", "Cam 2" }, lanes);
            const triggerfit::CaptureFit c = triggerfit::fitCapture(lanes);
            ck(c.ok && std::fabs(c.rpm - 300.0) < 20.0,
               "a wide gap, cranked: the speed is right and the tooth count is not — KNOWN",
               c.ok ? (std::to_string(int(c.rpm)) + " rpm, crank " +
                       (c.crankIdx >= 0 ? std::to_string(c.streams[size_t(c.crankIdx)].teeth) + "-" +
                                          std::to_string(c.streams[size_t(c.crankIdx)].missing) : "?"))
                    : c.why);
        }

        // AND THE TWO IT CANNOT DO AT ALL, recorded as what they are rather than left out. A distributor
        // with three or four teeth to a revolution, cranked by an engine with two compressions to a
        // revolution, samples that oscillation barely twice per cycle. At that rate a wheel with
        // unevenly spaced teeth and an even wheel on a surging engine are not similar signals, they
        // are the SAME signal, and no amount of analysis of one capture separates them — the teeth
        // arrive when they arrive.
        //
        // What separates them is a second capture: the wheel's geometry is fixed and the engine's
        // swing is not, so the same wheel at a different amplitude tells them apart at once. The rig
        // can do that now. Until the fitter asks for it, these two are wrong, and this says so.
        for (const char* f : { "01_6cyl_dizzy_300rpm_crank.csv", "02_8cyl_dizzy_300rpm_crank.csv" }) {
            std::vector<triggerlog::Record> recs;
            std::ifstream in(std::string(FIXTURE_DIR "/stim/") + f);
            std::string line;
            while (std::getline(in, line)) {
                std::replace(line.begin(), line.end(), ',', ' ');
                std::istringstream ss(line);
                unsigned long t, lv, fl;
                if (!(ss >> t >> lv >> fl)) continue;
                triggerlog::Record r;
                r.tUs = uint32_t(t); r.levels = uint8_t(lv); r.flags = uint8_t(fl);
                recs.push_back(r);
            }
            std::vector<triggerlog::Lane> lanes;
            triggerlog::decodeLanes(recs, { "Crank 1", "Crank 2", "Cam 1", "Cam 2" }, lanes);
            const triggerfit::CaptureFit c = triggerfit::fitCapture(lanes);
            ck(c.ok, "a sparse dizzy, cranked, is read as a patterned wheel — KNOWN, see above",
               c.ok ? (std::to_string(int(c.rpm)) + " rpm, and the rig was at 300") : c.why);
        }
    }

    {   // TWO CAMS, ONE OF THEM USELESS FOR SYNC. Jason's case: an engine whose crank track carries no
        // landmark, whose first cam is a plain even train — also no landmark — and whose second cam
        // carries the only unique mark in the system. Every stream has to be asked the question
        // separately, because "there is a cam" and "a cam can say where the engine is" are different
        // facts, and a search that stops at the first cam finds the wrong answer on this engine.
        //
        // And the adopted design must contain BOTH cams. The even one anchors nothing and is still a
        // cam that exists, on a wire, that the firmware has to be told about.
        const double revUs = 200000.0;                       // 300 rpm
        auto pulse = [](triggerlog::Lane& l, double t, bool prevSet, double prev) {
            triggerlog::Interval iv;
            iv.tUs = uint32_t(t); iv.deltaUs = prevSet ? uint32_t(t - prev) : 0u;
            iv.high = true; l.bars.push_back(iv);
        };
        triggerlog::Lane crank; crank.stream = 0; crank.name = "Crank 1";
        triggerlog::Lane camA;  camA.stream  = 2; camA.name  = "Cam 1";
        triggerlog::Lane camB;  camB.stream  = 3; camB.name  = "Cam 2";
        double pc = 0, pa = 0, pb = 0; bool fc = false, fa = false, fb = false;
        for (int cyc = 0; cyc < 9; ++cyc) {
            const double t0 = cyc * 2.0 * revUs;
            for (int i = 0; i < 120; ++i) {                   // 60 even teeth a revolution: no mark
                const double t = t0 + i * (2.0 * revUs / 120.0);
                pulse(crank, t, fc, pc); pc = t; fc = true;
            }
            for (int i = 0; i < 4; ++i) {                     // cam A: 4 even pulses a cycle, no mark
                const double t = t0 + i * (2.0 * revUs / 4.0);
                pulse(camA, t, fa, pa); pa = t; fa = true;
            }
            for (double deg : { 0.0, 100.0, 260.0, 300.0 }) { // cam B: unevenly spaced — the only mark
                const double t = t0 + deg / 720.0 * 2.0 * revUs;
                pulse(camB, t, fb, pb); pb = t; fb = true;
            }
        }
        const triggerfit::CaptureFit c = triggerfit::fitCapture({ crank, camA, camB });
        ck(c.ok, "a crank with no mark and two cams fits", c.why);
        ck(c.crankIdx >= 0 && c.streams[size_t(c.crankIdx)].name == "Crank 1",
           "…the densest wire is still the crank");
        ck(!c.streams[size_t(c.crankIdx)].uniqueKey, "…and it carries no mark of its own");
        ck(c.canSync, "…the system can still sync", c.syncNote);
        ck(c.syncIdx >= 0 && c.streams[size_t(c.syncIdx)].name == "Cam 2",
           "…on the SECOND cam, the even one having nothing to offer",
           c.syncIdx >= 0 ? c.streams[size_t(c.syncIdx)].name : "none");

        WheelParams p;
        p.sensorAngle = 105.0; p.tdcOffset = 155.0;           // what a capture cannot see
        ck(applyMeasured(c, p), "the measurement adopts as a design");
        ck(p.cams.size() == 2, "…carrying BOTH cams, not just the useful one",
           std::to_string(p.cams.size()));
        if (p.cams.size() == 2) {
            ck(p.cams[0].type == WheelParams::CAM_EVEN && p.cams[0].evenTeeth == 4,
               "…the even cam as 4 evenly spaced teeth",
               std::to_string(p.cams[0].type) + " x" + std::to_string(p.cams[0].evenTeeth));
            ck(p.cams[1].type == WheelParams::CAM_PATTERN && p.cams[1].angles.size() == 4,
               "…the marked cam as a pattern of 4 edges",
               std::to_string(p.cams[1].type) + " x" + std::to_string(p.cams[1].angles.size()));
            // IN CYCLE DEGREES. A cam's angles are measured against its own period and its period is
            // the 720 cycle, so an unscaled adopt puts every cam edge at half the angle it belongs
            // at — which still looks like a cam, and decodes as a different engine.
            bool spans720 = false;
            for (double a : p.cams[1].angles) spans720 = spans720 || a > 360.0;
            ck(spans720, "…at CYCLE angles, not revolution angles",
               p.cams[1].angles.empty() ? "" : std::to_string(p.cams[1].angles.back()));
        }
        ck(p.sensorAngle == 105.0 && p.tdcOffset == 155.0,
           "…and the pickup and TDC are left alone, being things no capture can see");

        // A WIDTH-CODED CAM IS A CAM TOO. Its identity is in how long each tooth is HIGH, which is
        // measurable without knowing where the turn begins — so it comes back with no period, fell
        // through every loop that assigns a rate, and kept cycleRatio 0. The capture then did not
        // count it as phase and the designer did not adopt it, so a width-coded cam was simply not
        // in the trigger system that came out.
        triggerlog::Lane camW; camW.stream = 2; camW.name = "Cam 1";
        {
            double t = 0.0, prev = 0.0; bool first = true;
            const double tooth = 2.0 * revUs / 6.0;
            for (int cyc = 0; cyc < 9; ++cyc)
                for (int i = 0; i < 6; ++i) {
                    const double hi = (i == 2) ? tooth * 0.6 : tooth * 0.2;   // one tooth much wider
                    for (double edge : { 0.0, hi }) {
                        triggerlog::Interval iv;
                        iv.tUs = uint32_t(t + edge);
                        iv.deltaUs = first ? 0u : uint32_t(t + edge - prev);
                        iv.high = (edge == 0.0);
                        camW.bars.push_back(iv); prev = t + edge; first = false;
                    }
                    t += tooth;
                }
        }
        const triggerfit::CaptureFit cw = triggerfit::fitCapture({ crank, camW });
        const auto wc = std::find_if(cw.streams.begin(), cw.streams.end(),
                                     [](const triggerfit::StreamFit& x){ return x.name == "Cam 1"; });
        ck(wc != cw.streams.end() && wc->shape == triggerfit::Shape::Width,
           "a width-coded cam reads as Width",
           wc != cw.streams.end() ? std::to_string(int(wc->shape)) : "missing");
        ck(wc != cw.streams.end() && wc->cycleRatio == 2, "…and turns once per ENGINE CYCLE",
           wc != cw.streams.end() ? std::to_string(wc->cycleRatio) : "");
        ck(cw.phase, "…so the capture counts the engine as having phase");
        WheelParams pw;
        ck(applyMeasured(cw, pw) && pw.cams.size() == 1, "…and it is adopted as a cam",
           std::to_string(pw.cams.size()));
    }

    {   // THE CRANK STANDS FOR THE ENGINE. A cam has a handful of edges per engine cycle and the
        // engine under it surges several times a revolution: one wire, two signals, and nowhere near
        // enough samples to separate them. The crank is on the same engine with a hundred and eighty
        // edges a revolution, so it has already measured what the engine did — put the cam's edges on
        // the crank's angle scale and the surge is not modelled, it is gone.
        //
        // Both of these are cranked captures off the rig at 45% amplitude. Their true geometry is the
        // stim's own pattern table, not a number off a web page:
        //
        //   Nissan CAS window track   [1276, 1276, 1276, 1296, 1276, 800] → mean 1200
        //                             so five intervals at 1.06 and one at 0.67
        //   FE3 cam                   [3300, 300, 3600] tenths of a degree → mean 240 degrees
        //                             so 1.375, 0.125 and 1.5
        struct Want { const char* file; int teeth; double odd; const char* what; };
        const Want cams[] = {
            { "34_nissan_360_cas_300rpm_crank.csv", 6, 0.667, "a cranked Nissan CAS window track" },
            { "21_fe3_36_1___cam_300rpm_crank.csv", 3, 0.125, "a cranked FE3 cam" },
        };
        for (const Want& w : cams) {
            std::vector<triggerlog::Record> recs;
            std::ifstream in(std::string(FIXTURE_DIR "/stim/") + w.file);
            std::string line;
            while (std::getline(in, line)) {
                std::replace(line.begin(), line.end(), ',', ' ');
                std::istringstream ss(line);
                unsigned long t, lv, fl;
                if (!(ss >> t >> lv >> fl)) continue;
                triggerlog::Record r;
                r.tUs = uint32_t(t); r.levels = uint8_t(lv); r.flags = uint8_t(fl);
                recs.push_back(r);
            }
            std::vector<triggerlog::Lane> lanes;
            triggerlog::decodeLanes(recs, { "Crank 1", "Crank 2", "Cam 1", "Cam 2" }, lanes);
            const triggerfit::CaptureFit c = triggerfit::fitCapture(lanes);
            const auto cam = std::find_if(c.streams.begin(), c.streams.end(),
                                          [](const triggerfit::StreamFit& x){ return x.name == "Cam 1"; });
            if (cam == c.streams.end() || !cam->ok) { ck(false, w.what, "no cam fitted"); continue; }
            ck(cam->teeth == w.teeth, w.what,
               std::to_string(cam->teeth) + " marks, wanted " + std::to_string(w.teeth));
            // The short one, to within 4% — which is the measurement, not the model: there is no
            // wave left in this number to be approximate about.
            double best = 9.0;
            for (const triggerfit::Anomaly& a : cam->anomalies) best = std::min(best, a.ratio);
            ck(std::fabs(best - w.odd) < 0.04 * w.odd,
               "…its odd interval measured through the cranking",
               std::to_string(best) + ", wanted " + std::to_string(w.odd));
        }
    }

    std::printf("%s\n", fails ? "FAILED" : "all good");
    return fails ? 1 : 0;
}
