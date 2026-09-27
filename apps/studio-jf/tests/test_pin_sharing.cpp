// Can two cylinders name one coil?
//
// They have to be able to. A distributor runs every cylinder off ONE coil; wasted spark runs each
// companion pair off one. The picker offered every pin exactly once, so the moment cylinder 1 took
// IGN1 the option greyed out for everybody else — the second cylinder of a pair could not be given the
// coil it is physically wired to, and a twelve-cylinder distributor could name its coil once and then
// had eleven cylinders with nowhere to point.
//
// The firmware never had that rule: claim_one_ign returns quietly when the channel is already claimed
// (EventScheduler.cpp:135), because a shared coil is the wiring. So the app is the one that has to stop
// inventing a finer rule than the hardware has — but only where the hardware says so, which is why the
// picker DECLARES it: two sensors on one pin is still a conflict.
//
// WHAT MOVED: the firing model became fixed-channel — IGNk drives cylinder k, LSk drives cylinder k of
// a stage — so engine.cyl[N].ign_channel / inj_channel / ign_trailing_channel / inj_secondary_channel are
// gone and no cylinder names a coil any more. Nothing in definition/ declares a `shared: true` pool
// today, so the sharing branch (MetaModel::PickerSet::shared -> PinClaim::shareable) is dormant rather
// than dead: it is what a returning per-cylinder pin binding would switch back on. This test therefore
// pins the half the model can still express — a sensor pin is contended, and a pin claimed with no
// shared pool declared is never offered — plus the dormancy itself, so the day a pool declares
// `shared` the sharing checks below can come back with it.
//
//   cmake --build build --target pin_sharing_test && ./build/pin_sharing_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/MathEvaluator.h"
#include "model/SigilResolvers.h"
#include "surface/widgets/EnumPickerWidget.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-64s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());
    static ConfigSigilResolver configSigils;
    MathEvaluator::instance().registerResolver(&configSigils);

    c.setConfigValue("engine.cylinder_count", 4);

    std::puts("=== a sensor pin is contended; sharing is a pool declaration ===");

    // The declaration itself: no pool in the shipped meta says `shared`, so nothing the picker reports
    // may be offered twice. This is the guard on the dormant branch — if a pool starts declaring it, this
    // check is what tells you to restore the coil/injector sharing cases above it.
    {
        int declared = 0;
        for (const auto& [arrKey, ca] : m.configArrays())
            for (const auto& f : ca.fields)
                for (const auto& ps : f.pickerSets)
                    if (ps.shared) ++declared;
        ck(declared == 0, "no shipped pool declares itself shared (the branch is dormant)",
           std::to_string(declared) + " declared — restore the sharing cases");
    }

    // A SENSOR PIN IS NOT SHARED. Two sensors on one analog input is a genuine conflict — the firmware
    // assigns the callback unconditionally and one of them silently stops being read — so that claim
    // still greys the option out. This is the assertion that keeps `shared` a declaration rather than a
    // blanket relaxation.
    {
        c.setConfigValue("sensors.sensor[0].enabled", 1);
        c.setConfigValue("sensors.sensor[0].interface", 0);   // analog voltage
        c.setConfigValue("sensors.sensor[0].source", 0);      // AV1
        const auto claimed = EnumPickerWidget::claimedPoolPins("sensors.sensor[1].source");
        const auto it = claimed.find("AV1");
        ck(it != claimed.end(), "a pin another sensor holds is reported",
           it == claimed.end() ? "not reported" : it->second.holder);
        ck(it != claimed.end() && !it->second.shareable, "…and NOT shareable — it stays greyed out");
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "contended hardware is not offered; sharing stays a declaration",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
