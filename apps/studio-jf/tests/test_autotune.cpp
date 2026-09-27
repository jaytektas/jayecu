// The autotuner's arithmetic, checked without a window, an ECU or a log file.
//
// Everything the feature is worth rests here: which cell a reading is credited to once transport delay
// is accounted for, whether a correction the ECU is already applying is folded in or silently lost,
// how much of a cell's change one record is allowed to buy, and whether a filter that exists to reject
// a tip-in actually sees the tip-in rather than the calm 300 ms afterwards.
//
//   cmake --build build --target test_autotune && ./build/test_autotune
#include "model/Autotune.h"
#include "model/MetaModel.h"
#include "model/TsIniImporter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace autotune;

static int failures = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  %s  %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : ("   - " + detail).c_str());
    if (!ok) ++failures;
}
static void near(double got, double want, double tol, const std::string& what) {
    char d[96];
    std::snprintf(d, sizeof d, "got %.4f, want %.4f", got, want);
    check(std::fabs(got - want) <= tol, what, d);
}

// A grid coarse enough to reason about by hand: 3 rpm bins x 3 load bins.
static Engine makeEngine() {
    Engine e;
    e.setGrid({ 1000.0, 2000.0, 3000.0 }, { 40.0, 60.0, 80.0 });
    e.setBase({ 50.0, 55.0, 60.0,
                65.0, 70.0, 75.0,
                80.0, 85.0, 90.0 });
    DelayTable flat;                                  // no transport delay unless a test asks for one
    flat.xb = { 0.0, 10000.0 }; flat.yb = { 0.0, 500.0 }; flat.cells = { 0, 0, 0, 0 };
    e.setDelay(flat);
    return e;
}

static Sample at(uint32_t ms, double rpm, double load, double lambda,
                 double target = 1.0, double ego = 1.0) {
    Sample s;
    s.ms = ms; s.x = rpm; s.y = load;
    s.lambda = lambda; s.target = target; s.ego = ego; s.ok = true;
    return s;
}

int main(int argc, char** argv) {
    std::printf("=== a lean reading asks for fuel, in its own cell ===\n");
    {
        Engine e = makeEngine();
        e.setSettings({ Resistance::Easy, 50.0, 50.0 });
        // Dead on the 2000 rpm / 60 kPa breakpoint, 5 % lean.
        for (int i = 0; i < 4; ++i) e.add(at(1000 + i * 30, 2000.0, 60.0, 1.05));
        near(e.changePct(1, 1), 5.0, 0.01, "5 % lean proposes +5 % on the cell it was measured in");
        check(e.changePct(0, 0) == 0.0 && e.changePct(2, 2) == 0.0,
              "…and no other cell is touched");
        near(e.proposed(1, 1), 70.0 * 1.05, 0.01, "the proposed value is the base cell scaled");
        // The sign is the whole feature: rich must REMOVE fuel, and a test that only ever ran lean
        // would pass with the division upside down.
        Engine r = makeEngine();
        r.setSettings({ Resistance::Easy, 50.0, 50.0 });
        for (int i = 0; i < 4; ++i) r.add(at(1000 + i * 30, 2000.0, 60.0, 0.90));
        near(r.changePct(1, 1), -10.0, 0.01, "10 % rich proposes -10 %");
    }

    std::printf("\n=== a correction the ECU is already applying is FOLDED IN, not lost ===\n");
    {
        Engine e = makeEngine();
        e.setSettings({ Resistance::Easy, 50.0, 50.0 });
        // The mixture is exactly on target — but only because the ECU is adding 8 %. The table is
        // still 8 % wrong, and an autotuner that reads only lambda would call this cell finished.
        for (int i = 0; i < 4; ++i) e.add(at(1000 + i * 30, 2000.0, 60.0, 1.00, 1.00, 1.08));
        near(e.changePct(1, 1), 8.0, 0.01, "on target with 8 % of trim behind it still proposes +8 %");
        Engine o = makeEngine();
        o.setSettings({ Resistance::Easy, 50.0, 50.0 });
        for (int i = 0; i < 4; ++i) o.add(at(1000 + i * 30, 2000.0, 60.0, 1.00));
        check(o.changePct(1, 1) == 0.0, "…and with the trims off, on target means leave it alone");
    }

    std::printf("\n=== the target that counts is the one that was COMMANDED, not the one now ===\n");
    {
        Engine e = makeEngine();
        e.setSettings({ Resistance::Easy, 50.0, 50.0 });
        DelayTable d; d.xb = { 0.0, 10000.0 }; d.yb = { 0.0, 500.0 }; d.cells = { 200, 200, 200, 200 };
        e.setDelay(d);
        // 0.85 target while the gas was made, then the target moves to 1.00 before the reading lands.
        // Judged against the new target this reads 18 % lean and would pour fuel into a power cell.
        for (uint32_t t = 0; t <= 200; t += 25) e.add(at(1000 + t, 2000.0, 60.0, 0.85, 0.85));
        for (uint32_t t = 225; t <= 400; t += 25) e.add(at(1000 + t, 2000.0, 60.0, 0.85, 1.00));
        near(e.changePct(1, 1), 0.0, 0.01, "a mixture on its OWN target proposes no change");
    }

    std::printf("\n=== transport delay credits the cell that MADE the gas ===\n");
    {
        Engine e = makeEngine();
        e.setSettings({ Resistance::Easy, 50.0, 50.0 });
        DelayTable d; d.xb = { 0.0, 10000.0 }; d.yb = { 0.0, 500.0 }; d.cells = { 300, 300, 300, 300 };
        e.setDelay(d);
        // Sit at 1000/40 long enough to fill the ring, then jump to 3000/80. The lean gas arriving
        // just after the jump was made at the OLD point.
        //
        // The sensor is not lit for the first stretch, which is both realistic and the point: those
        // records carry no judgement, but they are still the truthful answer to "where was the engine
        // 300 ms ago", so the ring must keep them or the jump has nothing to look back at.
        for (uint32_t t = 0; t < 600; t += 30) {
            Sample cold = at(1000 + t, 1000.0, 40.0, 0.0);
            cold.ok = false;
            e.add(cold);
        }
        for (uint32_t t = 600; t < 780; t += 30) e.add(at(1000 + t, 3000.0, 80.0, 1.06));
        near(e.changePct(0, 0), 6.0, 0.01, "the correction lands where the engine WAS");
        check(e.changePct(2, 2) == 0.0, "…and not where it now is");
    }

    std::printf("\n=== a reading between two cells belongs to both, in proportion ===\n");
    {
        Engine e = makeEngine();
        e.setSettings({ Resistance::Easy, 50.0, 50.0 });
        // A quarter of the way from 1000 to 2000 rpm, on the 40 kPa row.
        for (int i = 0; i < 8; ++i) e.add(at(1000 + i * 30, 1250.0, 40.0, 1.04));
        near(e.weight(0, 0) / e.weight(1, 0), 3.0, 0.01,
             "a quarter of the way along gives the near cell three times the weight");
        near(e.changePct(0, 0), 4.0, 0.01, "both cells still propose the full error…");
        near(e.changePct(1, 0), 4.0, 0.01, "…because weight buys confidence, not a smaller number");
    }

    std::printf("\n=== resistance is how much evidence a cell needs to move the whole way ===\n");
    {
        Engine e = makeEngine();
        e.setSettings({ Resistance::Normal, 50.0, 50.0 });   // full weight = 4 records
        e.add(at(1000, 2000.0, 60.0, 1.10));
        near(e.changePct(1, 1), 2.5, 0.01, "one record of four buys a quarter of the change");
        for (int i = 1; i < 4; ++i) e.add(at(1000 + i * 30, 2000.0, 60.0, 1.10));
        near(e.changePct(1, 1), 10.0, 0.01, "…four buys all of it");
        for (int i = 4; i < 12; ++i) e.add(at(1000 + i * 30, 2000.0, 60.0, 1.10));
        near(e.changePct(1, 1), 10.0, 0.01, "…and more does not overshoot");

        Engine h = makeEngine();
        h.setSettings({ Resistance::Hard, 50.0, 50.0 });
        for (int i = 0; i < 4; ++i) h.add(at(1000 + i * 30, 2000.0, 60.0, 1.10));
        near(h.changePct(1, 1), 10.0 * 4.0 / 12.0, 0.01, "Hard wants the same thing said three times over");
    }

    std::printf("\n=== authority: whichever limit binds first ===\n");
    {
        Engine e = makeEngine();
        e.setSettings({ Resistance::Easy, 6.0, 100.0 });     // 6 % ceiling
        for (int i = 0; i < 4; ++i) e.add(at(1000 + i * 30, 2000.0, 60.0, 1.40));
        near(e.changePct(1, 1), 6.0, 0.01, "a 40 % error is held to the percentage limit");

        Engine a = makeEngine();
        a.setSettings({ Resistance::Easy, 100.0, 3.5 });     // 3.5 VE points on a cell of 70
        for (int i = 0; i < 4; ++i) a.add(at(1000 + i * 30, 2000.0, 60.0, 1.40));
        near(a.changePct(1, 1), 5.0, 0.01, "…and the absolute limit binds where it is tighter");
        near(a.proposed(1, 1), 73.5, 0.01, "3.5 points on a cell of 70");
    }

    std::printf("\n=== a filter sees the instant the gas was MADE ===\n");
    {
        // This is the whole reason the filters are evaluated against the production sample. The tip-in
        // happens, and 300 ms later — when its over-fuelled exhaust reaches the sensor — the throttle
        // has stopped moving and dTPS reads zero. A filter tested against the arriving record would
        // wave it through and file accel-enrichment error in an rpm x load cell.
        Engine e = makeEngine();
        e.setSettings({ Resistance::Easy, 50.0, 50.0 });
        DelayTable d; d.xb = { 0.0, 10000.0 }; d.yb = { 0.0, 500.0 }; d.cells = { 300, 300, 300, 300 };
        e.setDelay(d);
        e.setFilters({ Filter{ "dTPS", "delta_tps", Filter::Op::Above, 50.0, true } });

        auto s = [&](uint32_t ms, double dtps, double lambda) {
            Sample x = at(1000 + ms, 2000.0, 60.0, lambda);
            x.filt = { dtps };
            return x;
        };
        for (uint32_t t = 0; t <= 570; t += 30) e.add(s(t, 0.0, 1.00));      // calm, on target
        for (uint32_t t = 600; t <= 660; t += 30) e.add(s(t, 400.0, 1.00));  // the throttle moving
        for (uint32_t t = 690; t <= 870; t += 30) e.add(s(t, 0.0, 1.00));    // calm again long before…
        for (uint32_t t = 900; t <= 960; t += 30) e.add(s(t, 0.0, 0.80));    // …the tip-in's gas lands

        const Stats st = e.stats();
        check(st.filtered > 0, "the tip-in's gas is rejected", "filtered=" + std::to_string(st.filtered));
        check(st.activeFilter == "dTPS", "…and the panel can say which filter did it", st.activeFilter);
        check(e.changePct(1, 1) == 0.0, "…so nothing accel enrichment caused is baked into the map");
    }

    std::printf("\n=== the accountability numbers ===\n");
    {
        Engine e = makeEngine();
        e.setSettings({ Resistance::Easy, 50.0, 50.0 });
        e.setFilters({ Filter{ "Minimum RPM", "rpm", Filter::Op::Below, 500.0, true } });
        auto s = [&](uint32_t ms, double rpm, double lambda) {
            Sample x = at(1000 + ms, rpm, 60.0, lambda);
            x.filt = { rpm };
            return x;
        };
        for (int i = 0; i < 10; ++i) e.add(s(i * 30, 2000.0, 1.05));      // used
        for (int i = 10; i < 14; ++i) e.add(s(i * 30, 300.0, 1.05));      // stalling: filtered
        Sample bad = s(500, 2000.0, 1.05); bad.ok = false; e.add(bad);    // no reading at all

        const Stats st = e.stats();
        check(st.total == 15, "every record offered is counted", std::to_string(st.total));
        check(st.used == 10, "…the ones that were used", std::to_string(st.used));
        check(st.filtered == 5, "…and the ones that were not", std::to_string(st.filtered));
        check(st.used + st.filtered == st.total, "the three add up");
        check(st.altered >= 1, "cells altered is reported", std::to_string(st.altered));
        check(st.cells == 9, "…out of the whole grid", std::to_string(st.cells));
        near(st.maxChange, 5.0, 0.01, "max cell change");
        check(st.activeFilter == "lambda not readable",
              "the last rejection is what the panel shows as active", st.activeFilter);
    }

    std::printf("\n=== a proposal means nothing against a different grid ===\n");
    {
        Engine e = makeEngine();
        e.setSettings({ Resistance::Easy, 50.0, 50.0 });
        for (int i = 0; i < 4; ++i) e.add(at(1000 + i * 30, 2000.0, 60.0, 1.05));
        check(e.hasProposal(), "there is a proposal to lose");
        e.setGrid({ 1000.0, 2000.0, 3000.0 }, { 40.0, 60.0, 80.0 });
        check(!e.hasProposal(), "re-stating the grid throws the accumulated evidence away");
        check(e.stats().total == 0, "…and the counters with it");
    }

    // ---- THE CONTRACT THE PANEL READS -------------------------------------------------------------
    // Everything above is arithmetic; none of it runs unless the loaded definition says which channel
    // is the mixture, which is the target and which report the correction already being applied. Both
    // kinds of definition have to produce that, in one shape, or the panel has a special case in it.
    const auto checkContract = [](const MetaModel& m, const std::string& who) {
        const MetaModel::Autotune& a = m.autotune();
        check(a.valid(), who + ": states an autotune contract");
        check(!a.table.empty() && m.resolveTable(a.table).valid,
              who + ": …naming a table that resolves", a.table);
        check(!a.targetTable.empty() || !a.targetChannel.empty(),
              who + ": …and a target, as a map or a channel");
        check(!a.egoChannels.empty(),
              who + ": …and which channels carry the correction already applied",
              std::to_string(a.egoChannels.size()) + " channel(s)");
        check(!a.filters.empty(), who + ": …and what disqualifies a reading",
              std::to_string(a.filters.size()) + " filter(s)");
        for (const auto& f : a.filters)
            check(!f.name.empty() && !f.channel.empty(),
                  who + ": every filter names a channel and something to call it", f.name);
    };

    std::printf("\n=== the definition states the contract: native ===\n");
    {
        MetaModel m;
        if (!m.loadFile(REAL_META)) check(false, "the shipped meta loads", REAL_META);
        else                        checkContract(m, "native");
    }
    if (argc > 1) {
        std::printf("\n=== …and so does a TunerStudio ini ===\n");
        std::string err;
        auto j = TsIniImporter::importFile(argv[1], &err);
        if (!j) check(false, "the ini imports", err);
        else {
            // A .meta is the JSON body plus a CRC32 footer, and loadFile is the only door in — so the
            // import is written the way the studio writes it and read back the way the studio reads it.
            const std::string tmp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                                  + "/autotune_import.meta";
            check(TsIniImporter::writeMetaFile(*j, tmp), "the import writes a .meta");
            MetaModel m;
            check(m.loadFile(tmp), "…and the studio loads it back");
            checkContract(m, "imported");
            std::remove(tmp.c_str());
        }
    }

    std::printf("\n%s\n", failures ? (std::to_string(failures) + " FAILED").c_str() : "ALL PASSED");
    return failures ? 1 : 0;
}
