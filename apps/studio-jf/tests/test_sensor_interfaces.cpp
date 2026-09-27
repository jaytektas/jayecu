// WHICH INTERFACES a sensor may be read through — and whether anything enforced it.
//
// The catalogue has always declared it: a coolant-flow switch is [digital, analog_voltage], a
// flex-fuel sender is [digital_freq] and nothing else, and codegen ORs that into
// SensorDescriptor::interface_mask. The meta never published it, so every picker in the studio
// offered the WHOLE interface enum for every sensor and the declaration constrained nothing. Pick a
// wrong one and the firmware builds a pipeline over a pin the sensor is not wired to — a switch on
// Pulse Width reads a duty cycle off a digital line and simply stops reading — with no error and no
// hint in the list of why.
//
// The meta now carries element_interfaces (ids, per element, `locked:` collapsing it to one) and
// MetaModel::allowedOptionIds answers it for a bound path. Everything here goes through that one
// public door, against the real definition and the real default tune.
//
//   cmake --build build --target sensor_interfaces_test && ./build/sensor_interfaces_test
#include "model/MetaModel.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) { if (!s.empty()) s += ", "; s += x; }
    return s.empty() ? "(none)" : s;
}
static bool has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}
static std::string ipath(const std::string& sensor) {
    return "sensors.sensor[" + sensor + "].interface";
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }

    std::puts("=== sensor interfaces: what an input may actually be read through ===");

    // 0 — the meta has to carry it at all. This is the whole gap: the firmware knew, the studio did not.
    const auto& ca = m.configArrays().at("sensors.sensor");
    {
        ck(int(ca.elementInterfaces.size()) == ca.count,
           "every element publishes its interface capability",
           std::to_string(ca.elementInterfaces.size()) + " of " + std::to_string(ca.count));
        int silent = 0;
        for (const auto& v : ca.elementInterfaces) if (v.empty()) ++silent;
        ck(silent == 0, "no sensor is left unconstrained by an empty declaration",
           std::to_string(silent) + " with none");
    }

    // 1 — EVERY sensor can come off the CAN bus. The firmware binds a device signal to the sensor's
    //     CHANNEL and never asks what kind of sensor it is, so a catalogue list read as a permission
    //     list refused a CAN coolant switch, a CAN flex sender, a CAN anything — all of them real
    //     parts. This is the assertion that would have caught the first version of this rule.
    {
        int noCan = 0; std::string first;
        for (int i = 0; i < ca.count && i < int(ca.elementInterfaces.size()); ++i) {
            if (ca.elementIds[i] == "battery" || ca.elementIds[i] == "ecu_temp"
                || ca.elementIds[i] == "map") continue;                 // locked by the catalogue
            if (!has(ca.elementInterfaces[i], "can_device")) {
                if (!noCan) first = ca.elementIds[i] + ": " + join(ca.elementInterfaces[i]);
                ++noCan;
            }
        }
        ck(noCan == 0, "every sensor that is not LOCKED can be read off CAN", first);
    }

    // 2 — a switch is not only a switch-shaped part. The one that started this: coolant_flow_sw is
    //     declared [digital, analog_voltage], and that is what it usually IS, not all it may be.
    {
        const auto a = m.allowedOptionIds(ipath("coolant_flow_sw"));
        ck(has(a, "digital") && has(a, "analog_voltage"), "coolant_flow_sw keeps its natural reads", join(a));
        ck(has(a, "can_device") && has(a, "sent") && has(a, "pulse_width") && has(a, "digital_freq"),
           "…and offers CAN, SENT, pulse and frequency besides", join(a));
    }

    // 3 — the TWO that are genuinely impossible, and the only two the firmware cannot build for an
    //     arbitrary sensor. `digital` has no decode at all — the pin level IS the value — so a
    //     temperature read that way publishes 0 or 1 degrees; and there are exactly three on-board
    //     acquire functions, none of them a coolant probe.
    {
        const auto a = m.allowedOptionIds(ipath("clt"));
        ck(!has(a, "digital"), "clt cannot be read as a digital level — there is no decode", join(a));
        ck(!has(a, "on_board"), "…nor as an on-board source, which is three functions and no more", join(a));
        ck(has(a, "can_device") && has(a, "sent"), "…while CAN and SENT are both buildable", join(a));
    }

    // 4 — `locked:` still COLLAPSES the list. The battery is on a dedicated divider (AV12); the
    //     firmware's active_iface() ignores the tune's selector for it entirely, so offering a choice
    //     would be a control that does nothing.
    {
        const auto a = m.allowedOptionIds(ipath("battery"));
        ck(a.size() == 1 && a[0] == "analog_voltage",
           "battery is locked to its board divider — one entry, not a menu", join(a));
        const auto e = m.allowedOptionIds(ipath("ecu_temp"));
        ck(e.size() == 1 && e[0] == "on_board", "ecu_temp is locked on-board", join(e));
    }

    // 5 — addressed by INDEX, not by id. Both spellings reach the same element, or a templated page
    //     (which resolves [*] to a number) would be unfiltered while the named page was filtered.
    {
        const int idx = int(std::find(ca.elementIds.begin(), ca.elementIds.end(), std::string("battery"))
                            - ca.elementIds.begin());
        const auto a = m.allowedOptionIds("sensors.sensor[" + std::to_string(idx) + "].interface");
        ck(a.size() == 1 && a[0] == "analog_voltage",
           "the numeric path answers the same as the named one", join(a));
    }

    // 6 — EVERY OTHER ENUM is untouched. The rule is keyed on the set id because an option id is
    //     unique only within its own set, and a rule about one set silently applying to another is the
    //     bug that deleted "Switch" from the interface list in the first place.
    {
        ck(m.allowedOptionIds("sensors.sensor[aux_1].type").empty(),
           "a sensor TYPE enum is not constrained by this rule");
        ck(m.allowedOptionIds("sensors.sensor[clt].enabled").empty(),
           "a non-enum field answers no constraint");
        ck(m.allowedOptionIds("engine.firing_order").empty(),
           "a top-level scalar answers no constraint");
    }

    // 7 — THE INVARIANT. Each element's default interface must be inside its own declared list, or
    //     the sensor ships configured as something the catalogue says it cannot be, and the picker
    //     would have to keep offering an entry it is meant to hide.
    {
        const MetaModel::ArrayField* fld = nullptr;
        for (const auto& f : ca.fields) if (f.name == "interface") fld = &f;
        ck(fld != nullptr, "the interface field is in the meta");
        int bad = 0; std::string first;
        for (int i = 0; fld && i < ca.count && i < int(ca.elementInterfaces.size()); ++i) {
            const int d = int(MetaModel::fieldDefault(*fld, i, 0.0));
            const std::string id = (d >= 0 && d < int(fld->optionIds.size())) ? fld->optionIds[d] : "";
            if (!has(ca.elementInterfaces[i], id)) {
                if (!bad) first = ca.elementIds[i] + " defaults to " + (id.empty() ? "?" : id)
                                  + ", declares " + join(ca.elementInterfaces[i]);
                ++bad;
            }
        }
        ck(bad == 0, "every sensor's DEFAULT interface is one it declares", first);
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "OK", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
