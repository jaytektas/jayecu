// THE AUTOTUNER'S GRID MUST STAY THE TABLE'S GRID.
//
// Everything the panel holds is addressed by CELL INDEX: the weights, the accumulated error, the base
// it clamps against. So a breakpoint that moves, or a bin that is inserted, makes every record it has
// collected describe a place that no longer exists — and an Apply would then write the proposal into
// cells it was never measured in. Silently, across the whole map, with an undo step that says
// "Auto Tune" and nothing about what it actually did.
//
// The panel snapshots the bins when the tab opens. The question this pins is what happens afterwards,
// and there are three ways it can go wrong: a cell edit (must NOT throw the proposal away), an axis
// edit that reaches a signal (must), and an axis edit that reaches NO signal — tiWriteBins announces
// nothing at all — which is why Start and Apply check for themselves rather than trusting the hook.
//
//   cmake --build build --target autotune_grid_test && ./build/autotune_grid_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "ui/AutotunePanel.h"

#include <j/core/SceneGraph.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// A TELEMETRY FRAME, BUILT THE WAY THE ECU BUILDS ONE. The panel reads live channels through the
// Cache, so the only honest way to give it a driving engine without one is to hand the Cache a frame:
// values encoded at the offsets and scales the definition declares, decoded back by the same code the
// link uses. Anything else would be testing a shortcut instead of the path that runs.
static std::vector<uint8_t> frameOf(const MetaModel& m, const std::map<std::string, double>& vals) {
    std::vector<uint8_t> f(static_cast<size_t>(m.telemetrySize()), 0);
    for (const auto& [name, v] : vals) {
        const auto it = m.telemetry().find(name);
        if (it == m.telemetry().end()) { std::printf("  (no such channel: %s)\n", name.c_str()); continue; }
        const MetaModel::TelemField& t = it->second;
        const double raw = t.scale != 0.0 ? v / t.scale : v;
        uint8_t* p = f.data() + t.offset;
        if (t.datatype == "F32") { const float x = static_cast<float>(v); std::memcpy(p, &x, 4); continue; }
        long long q = static_cast<long long>(std::llround(raw));
        for (int b = 0; b < t.size; ++b) p[b] = static_cast<uint8_t>((q >> (8 * b)) & 0xFF);
    }
    return f;
}

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  %s  %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : ("   - " + detail).c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    // THE DELAY IS NOT WHAT THIS TEST IS ABOUT (test_autotune.cpp owns that), and a lookback of 300 ms
    // against samples fed in a tight loop would reject every one of them for want of history. Zero it
    // in the image, so a record is credited to the cell the engine is in and the grid is the subject.
    {
        const TableImage d = c.resolveTable("lambda.ltft_delay_table");
        if (d.valid)
            for (int r = 0; r < c.tiLiveN(d, 1); ++r)
                for (int cc = 0; cc < c.tiLiveN(d, 0); ++cc) c.tiSetCell(d, cc, r, 0, 0.0);
    }

    jf::JSceneGraph g;
    AutotunePanel p(g);
    p.attach();

    const std::string path = p.tablePath();
    ck(!path.empty(), "the panel resolved a table to tune", path);
    ck(p.gridMatchesTable(), "…and its grid is that table's grid");

    const TableImage t = c.resolveTable(path);
    const int nx = c.tiLiveN(t, 0), ny = c.tiLiveN(t, 1);
    ck(p.engine().cols() == nx && p.engine().rows() == ny,
       "…bin for bin", std::to_string(p.engine().cols()) + "x" + std::to_string(p.engine().rows()));

    // ---- a CELL edit leaves the grid alone -----------------------------------------------------
    {
        const double was = c.tiCell(t, 0, 0, 0);
        c.tiSetCell(t, 0, 0, 0, was + 5.0);
        p.onTableEdited();
        ck(p.gridMatchesTable(), "a cell edit does not move the grid");
        ck(p.engine().base(0, 0) == c.tiCell(t, 0, 0, 0),
           "…and the base the proposal is clamped against follows it",
           std::to_string(p.engine().base(0, 0)));
        c.tiSetCell(t, 0, 0, 0, was);
        p.onTableEdited();
    }

    // ---- an AXIS edit does not -----------------------------------------------------------------
    // tiWriteBins emits nothing, so this is exactly the edit that reaches no hook. The panel must
    // notice by ASKING, not by being told.
    {
        std::vector<double> bins = c.tiBins(t, 0);
        const double was = bins[1];
        ck(bins.size() >= 3, "the table has bins to move");
        bins[1] = was + 137.0;                       // a breakpoint moved, the COUNT unchanged
        c.tiWriteBins(t, 0, bins);
        ck(!p.gridMatchesTable(),
           "a moved breakpoint is detected even with the bin count unchanged",
           "the count-only check this replaces would have passed here");

        p.onTableEdited();
        ck(p.gridMatchesTable(), "…and the panel re-seeds itself from the table");
        ck(p.engine().xBins()[1] == was + 137.0, "…onto the bins that are actually there now");

        // Put it back, the same way a person would.
        bins[1] = was;
        c.tiWriteBins(t, 0, bins);
        p.onTableEdited();
        ck(p.gridMatchesTable() && p.engine().xBins()[1] == was, "…and back again");
    }

    // ---- a real run, so the Apply checks below are about something --------------------------------
    // Without a proposal every Apply assertion passes for the wrong reason: nothing is written because
    // there is nothing to write. So the panel is given a driving engine — 6 % lean of a commanded 1.00
    // at a fixed point — and the proposal it produces is checked before anything is done to it.
    const double RPM = 2000.0, LOAD = 60.0;
    auto drive = [&](double lambda, int n) {
        for (int i = 0; i < n; ++i) {
            c.ingestTelemetry(frameOf(m, { { "rpm", RPM }, { "fuel_load", LOAD },
                                           { "lambda_1", lambda }, { "lambda_target", 1.00 },
                                           { "fuel_corr_stft", 1.0 }, { "fuel_corr_ltft", 1.0 },
                                           { "clt", 90.0 }, { "tps", 20.0 }, { "tps_rate", 0.0 },
                                           { "battery", 13.5 }, { "fuel_cut", 0.0 } }));
            p.onFrame();
        }
    };

    std::printf("\n=== it learns from a frame, not just from a rig ===\n");
    p.start();
    drive(1.06, 20);
    p.stop();
    const autotune::Stats st = p.engine().stats();
    ck(st.used > 0, "records were used", std::to_string(st.used) + " of " + std::to_string(st.total));
    ck(st.altered > 0, "…and cells altered", std::to_string(st.altered));
    ck(std::fabs(st.maxChange - 6.0) < 0.05, "6 % lean of target proposes +6 %",
       std::to_string(st.maxChange));

    std::printf("\n=== Apply refuses to write through a mapping that no longer holds ===\n");
    // The gate has to hold even when nothing told the panel anything, because that is the case with no
    // symptom: the cells would be written, the map would look plausible, and the only evidence would be
    // a fuel table quietly wrong in every cell the run had visited.
    {
        std::vector<double> bins = c.tiBins(t, 1);
        std::vector<double> before;
        for (int r = 0; r < ny; ++r)
            for (int cc = 0; cc < nx; ++cc) before.push_back(c.tiCell(t, cc, r, 0));

        bins[1] += 11.0;
        c.tiWriteBins(t, 1, bins);                   // moved, and NOBODY is told
        ck(!p.gridMatchesTable(), "the grid is stale and no signal said so");
        p.applyProposal();                           // must write nothing at all

        bool untouched = true;
        for (int r = 0, k = 0; r < ny; ++r)
            for (int cc = 0; cc < nx; ++cc, ++k)
                if (c.tiCell(t, cc, r, 0) != before[k]) untouched = false;
        ck(untouched, "Apply against a moved grid writes NOTHING");
        ck(p.statusText().find("axes changed") != std::string::npos,
           "…and says why, rather than looking like it worked", p.statusText());
        ck(!p.engine().hasProposal(), "…and the proposal measured against the old grid is gone");

        bins[1] -= 11.0;
        c.tiWriteBins(t, 1, bins);
        p.onTableEdited();
    }

    std::printf("\n=== …and writes the map when the grid DOES hold ===\n");
    // The other half of the same claim: a guard that refuses everything would pass every check above.
    {
        p.start();
        drive(1.06, 20);
        p.stop();
        const double was = c.tiCell(t, p.engine().lastCol() < 0 ? 0 : p.engine().lastCol(),
                                       p.engine().lastRow() < 0 ? 0 : p.engine().lastRow(), 0);
        const int lc = p.engine().lastCol() < 0 ? 0 : p.engine().lastCol();
        const int lr = p.engine().lastRow() < 0 ? 0 : p.engine().lastRow();
        ck(p.engine().hasProposal(), "there is a proposal to apply");
        p.applyProposal();
        const double now = c.tiCell(t, lc, lr, 0);
        ck(std::fabs(now - was * 1.06) < 0.05, "the live cell moved by the proposed 6 %",
           std::to_string(was) + " -> " + std::to_string(now));
        ck(!p.engine().hasProposal(), "…and the evidence is spent, so a second Apply cannot re-apply it");
    }

    std::printf("\n=== a stacked map is tuned on the plane the engine is BURNING ===\n");
    // VE's third axis is ethanol. The autotuner read and wrote plane 0 whatever the engine was on, so
    // on a flex tune above E0 it measured one fuel and corrected another — silently, because plane 0
    // is a perfectly good-looking map.
    {
        Cache::WriteGuard wg(c);                       // enabling an axis is structural
        c.tiSetEnabled(t, 2, true);
        ck(c.tiLiveN(t, 2) > 1, "the table has a live third axis to be on the wrong side of",
           std::to_string(c.tiLiveN(t, 2)) + " plane(s)");

        const std::vector<double> zb = c.tiBins(t, 2);
        const double e85 = zb.size() > 1 ? zb[1] : 100.0;      // the SECOND plane's own breakpoint
        auto driveOn = [&](double ethanol, double lambda, int n) {
            for (int i = 0; i < n; ++i) {
                c.ingestTelemetry(frameOf(m, { { "rpm", RPM }, { "fuel_load", LOAD },
                                               { "flex_ethanol", ethanol },
                                               { "lambda_1", lambda }, { "lambda_target", 1.00 },
                                               { "fuel_corr_stft", 1.0 }, { "fuel_corr_ltft", 1.0 },
                                               { "clt", 90.0 }, { "tps", 20.0 }, { "tps_rate", 0.0 },
                                               { "battery", 13.5 }, { "fuel_cut", 0.0 } }));
                p.onFrame();
            }
        };

        // Sit on the SECOND plane and learn there.
        c.ingestTelemetry(frameOf(m, { { "flex_ethanol", e85 } }));
        p.start();
        ck(p.livePlane() == 1, "the run is about the plane the engine is on",
           "plane " + std::to_string(p.livePlane()));
        // BOTH PLANES SNAPSHOTTED, so "the other one did not move" is a comparison rather than a hope.
        std::vector<double> was0, was1;
        for (int rr = 0; rr < ny; ++rr)
            for (int cc = 0; cc < nx; ++cc) {
                was0.push_back(c.tiCell(t, cc, rr, 0));
                was1.push_back(c.tiCell(t, cc, rr, 1));
            }
        driveOn(e85, 1.06, 20);
        p.stop();
        ck(p.engine().hasProposal(), "…and it learned something there");
        p.applyProposal();

        int moved0 = 0, moved1 = 0;
        for (int rr = 0, k = 0; rr < ny; ++rr)
            for (int cc = 0; cc < nx; ++cc, ++k) {
                if (c.tiCell(t, cc, rr, 0) != was0[k]) ++moved0;
                if (c.tiCell(t, cc, rr, 1) != was1[k]) ++moved1;
            }
        ck(moved1 > 0, "the E85 plane was corrected", std::to_string(moved1) + " cell(s)");
        ck(moved0 == 0, "…and the E0 plane was not touched at all",
           std::to_string(moved0) + " cell(s) moved");

        // MOVING TO ANOTHER FUEL ENDS THE RUN rather than carrying the evidence across.
        p.start();
        driveOn(e85, 1.06, 10);
        ck(p.engine().hasProposal(), "a run is under way on the E85 plane");
        driveOn(zb.empty() ? 0.0 : zb[0], 1.06, 1);    // the tank is now E0
        ck(!p.recording(), "changing fuel stops the run");
        ck(!p.engine().hasProposal(), "…and discards what was measured on the other fuel");
        ck(p.statusText().find("plane changed") != std::string::npos,
           "…and says so", p.statusText());

        c.tiSetEnabled(t, 2, false);
    }

    std::printf("\n%s\n", fails ? (std::to_string(fails) + " FAILED").c_str() : "ALL PASSED");
    return fails ? 1 : 0;
}
