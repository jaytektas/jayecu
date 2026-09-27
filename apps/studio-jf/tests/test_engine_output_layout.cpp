// THE STUDIO LAYS OUT THE COIL AND INJECTOR ROWS — what it writes, and when.
//
// outputs.output[i] IS physical output i. The firmware fires what the rows say and allocates nothing, so
// this is the only place a standard wiring exists. Pinned here:
//   layout   — coil-on-plug / wasted spark (by cylinder number, one coil per companion pair, a Chevy V8
//              included) / distributor / rotary; injector blocks per stage, Bank and Multi-Point values; a
//              Generic output's pin is never taken.
//   when     — an edit to cylinder count / ignition mode / stages lays the rows out; an edit to the firing
//              order does not; a hand-edited pin survives an unrelated edit.
//
//   cmake --build build --target engine_output_layout_test && ./build/engine_output_layout_test

#include "model/Cache.h"
#include "model/EngineOutputLayout.h"
#include "model/MetaModel.h"

#include <cstdio>
#include <string>

using namespace engine_outputs;

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[outlay] %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

static Engine piston(std::vector<int> order, Coils coils) {
    Engine e;
    e.cylinders = int(order.size());
    e.coils = coils;
    e.firingOrder = order;
    e.banks.assign(order.size(), 1);
    e.tdcDeg10.assign(order.size(), 0);
    e.stages = 1;
    e.modes[0] = Sequential;
    return e;
}

static std::string coilList(const std::vector<Row>& rows) {
    std::string s;
    for (const Row& r : rows)
        if (r.function == Ignition) s += "IGN" + std::to_string(r.row + 1) + "=" + std::to_string(r.cylinder) + " ";
    return s;
}

int main() {
    const std::vector<bool> none(kIgnRows + kLsRows, false);

    // ---- coils ------------------------------------------------------------------------------------
    ck(coilList(layout(piston({1, 3, 4, 2}, Coils::CoilOnPlug), true, false, none)) == "IGN1=1 IGN2=2 IGN3=3 IGN4=4 ",
       "coil-on-plug: IGN n = cylinder n",
       coilList(layout(piston({1, 3, 4, 2}, Coils::CoilOnPlug), true, false, none)));
    ck(coilList(layout(piston({1, 3, 4, 2}, Coils::Wasted), true, false, none)) == "IGN1=1 IGN2=2 ",
       "wasted spark 1-3-4-2: pairs 1+4 and 2+3 -> IGN1 = 1, IGN2 = 2");
    ck(coilList(layout(piston({1, 8, 4, 3, 6, 5, 7, 2}, Coils::Wasted), true, false, none))
           == "IGN1=1 IGN2=2 IGN3=4 IGN4=5 ",
       "wasted spark Chevy V8: 1+6, 2+3, 4+7, 5+8 -> 1, 2, 4, 5",
       coilList(layout(piston({1, 8, 4, 3, 6, 5, 7, 2}, Coils::Wasted), true, false, none)));
    ck(coilList(layout(piston({1, 5, 3, 6, 2, 4}, Coils::Wasted), true, false, none)) == "IGN1=1 IGN2=2 IGN3=3 ",
       "wasted spark 1-5-3-6-2-4: 1+6, 2+5, 3+4 -> 1, 2, 3");
    ck(coilList(layout(piston({0, 0, 0, 0}, Coils::Wasted), true, false, none)) == "IGN1=1 IGN2=2 IGN3=3 IGN4=4 ",
       "wasted spark with no firing order yet: a coil each, never none");
    ck(coilList(layout(piston({1, 3, 4, 2}, Coils::Distributor), true, false, none)) == "IGN1=13 ",
       "distributor: IGN1 = All");
    {
        Engine r = piston({1, 2, 3, 4, 5, 6}, Coils::CoilOnPlug);
        r.rotary = true;
        const std::vector<Row> rows = layout(r, true, true, none);
        ck(coilList(rows) == "IGN1=1 IGN2=2 IGN5=1 IGN6=2 ", "rotary: leading IGN r, trailing IGN 4+r = rotor r",
           coilList(rows));
        int inj = 0;
        for (const Row& x : rows) if (x.function == Injector) ++inj;
        ck(inj == 2, "rotary: one injector per rotor", std::to_string(inj));
    }

    // ---- injectors --------------------------------------------------------------------------------
    {
        Engine e = piston({1, 3, 4, 2}, Coils::Wasted);
        e.stages = 2; e.modes[1] = Bank; e.injectors[1] = 2;
        e.banks = {1, 1, 2, 2};
        const std::vector<Row> rows = layout(e, false, true, none);
        std::string s;
        for (const Row& r : rows)
            s += "LS" + std::to_string(r.row - kLsBase + 1) + "=" + std::to_string(r.cylinder) + "/s" + std::to_string(r.stage + 1) + " ";
        ck(s == "LS1=1/s1 LS2=2/s1 LS3=3/s1 LS4=4/s1 LS5=14/s2 LS6=15/s2 ",
           "stage 1 sequential LS1-4, stage 2 bank LS5 = Bank 1, LS6 = Bank 2", s);
        e.modes[1] = MultiPoint;
        const std::vector<Row> mp = layout(e, false, true, none);
        ck(mp.size() == 6 && mp[4].cylinder == CylAll && mp[5].cylinder == CylAll, "multi-point injectors are All");
    }
    {
        std::vector<bool> generic = none;
        generic[kLsBase + 2] = true;                          // a fan already on LS3
        const std::vector<Row> rows = layout(piston({1, 3, 4, 2}, Coils::Wasted), false, true, generic);
        bool ls3 = false; int cyl3 = -1;
        for (const Row& r : rows) { if (r.row == kLsBase + 2) ls3 = true; if (r.cylinder == 3) cyl3 = r.row; }
        ck(!ls3, "a Generic output's pin is never taken");
        ck(cyl3 == kLsBase + 4, "…the injector it displaced takes the next free pin (LS5)", std::to_string(cyl3));
    }

    // ---- when: through the Cache, as an edit on a page does it -------------------------------------
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());
    install(c);
    auto fn  = [&c](int i) { return int(c.configValue("outputs.output[" + std::to_string(i) + "].function")); };
    auto cyl = [&c](int i) { return int(c.configValue("outputs.output[" + std::to_string(i) + "].cylinder")); };

    c.setConfigValue("engine.num_inj_stages", 1);
    c.setConfigValue("engine.ign_mode", 2);                   // coil-on-plug
    const int order[4] = {1, 3, 4, 2};
    for (int i = 0; i < 4; ++i) c.setConfigValue("engine.firing_order[" + std::to_string(i) + "].cyl", order[i]);
    c.setConfigValue("engine.cylinder_count", 2);             // the default is 4: an edit that changes nothing is no edit
    c.setConfigValue("engine.cylinder_count", 4);
    ck(fn(0) == Ignition && cyl(0) == 1 && fn(3) == Ignition && cyl(3) == 4 && fn(4) == None,
       "a cylinder-count edit lays out IGN1-4 = cylinders 1-4");
    ck(fn(kLsBase) == Injector && cyl(kLsBase + 3) == 4 && fn(kLsBase + 4) == None,
       "…and LS1-4 = cylinders 1-4");

    c.setConfigValue("engine.ign_mode", 1);                   // wasted spark
    ck(fn(0) == Ignition && cyl(0) == 1 && fn(1) == Ignition && cyl(1) == 2 && fn(2) == None && fn(3) == None,
       "an ignition-mode edit re-lays the coils: IGN1 = 1, IGN2 = 2, IGN3/4 cleared");

    // A hand edit, then an edit that decides nothing about outputs.
    c.setConfigValue("outputs.output[1].cylinder", 3);
    c.setConfigValue("engine.firing_order[1].cyl", 2);
    c.setConfigValue("engine.firing_order[3].cyl", 3);
    ck(cyl(1) == 3, "a firing-order edit moves no output, and a hand-edited pin stays as edited");

    c.setConfigValue("outputs.output[" + std::to_string(kLsBase + 5) + "].function", Generic);   // fan on LS6
    c.setConfigValue("engine.cylinder_count", 6);
    ck(fn(kLsBase + 5) == Generic, "a cylinder-count edit leaves a Generic output's pin alone");
    ck(fn(kLsBase + 6) == Injector && cyl(kLsBase + 6) == 6, "…cylinder 6's injector goes to LS7 instead");

    // ---- the Cylinder picker offers what the engine and the row's stage can use ------------------------
    {
        auto opts = [&c](int row) {
            std::string s;
            for (int v : cylinderOptions(c, "outputs.output[" + std::to_string(row) + "].cylinder"))
                s += std::to_string(v) + " ";
            return s;
        };
        // Six cylinders now (from the section above), wasted spark.
        ck(opts(0) == "1 2 3 4 5 6 ", "a coil row offers the six cylinders", opts(0));
        ck(opts(kLsBase) == "1 2 3 4 5 6 ", "a sequential injector row offers the six cylinders", opts(kLsBase));
        ck(opts(kLsBase + 5).empty(), "a Generic row has no cylinder list", opts(kLsBase + 5));
        ck(cylinderOptions(c, "engine.cylinder_count").empty(), "…nor does any other field");
        c.setConfigValue("engine.inj_stage[0].num_outputs", 2);   // a grouped stage with no count lays out none
        c.setConfigValue("engine.inj_stage[0].mode", Bank);
        c.setConfigValue("engine.cyl[3].bank", 2);
        ck(opts(kLsBase) == "14 15 ", "a Bank stage's injector offers Bank 1 and Bank 2", opts(kLsBase));
        c.setConfigValue("engine.inj_stage[0].mode", MultiPoint);
        ck(opts(kLsBase) == "13 ", "a Multi-Point stage's injector offers All", opts(kLsBase));
        c.setConfigValue("engine.ign_mode", 0);
        ck(opts(0) == "13 ", "a distributor coil offers All", opts(0));
    }

    std::printf(fails ? "\n[outlay] %d FAILED\n" : "\n[outlay] all passed\n", fails);
    return fails ? 1 : 0;
}
