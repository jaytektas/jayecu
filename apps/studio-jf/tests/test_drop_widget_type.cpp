// What does dragging a binding out of the dictionary DROP?
//
// The type comes from Cache::widgetTypeFor, which asks what the field IS. A field that names a hardware
// channel by index is a LIST — the board's own pool — and dropping a spin box on one asks the user to know
// that "3" means IGN4. Four of them had no control hint at all in the schema, so they fell through every
// classification and landed on "configedit": the cylinder's ignition, trailing-coil, injector and secondary
// injector channels. Each now names the pool the firmware claims it through.
//
// WHAT MOVED: those four fields are gone — the fixed-channel firing model binds IGNk to cylinder k
// and LSk to cylinder k of a stage, so no cylinder names a channel any more. The two pools a tune still
// picks from are the trigger stream's capture input and a sensor's source pin, and the property under
// test is unchanged: a field that names hardware by index must drop as the board's list, never as a
// spin box asking the user to know that "3" means IGN4.
//
//   cmake --build build --target drop_widget_type_test && ./build/drop_widget_type_test
#include "model/Cache.h"
#include "model/MetaModel.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m); c.setConfigImage(m.defaultImage());

    std::puts("=== a channel field drops as its picker, not as a number ===");
    struct Case { const char* path; const char* want; const char* first; };
    const Case cases[] = {
        { "trigger.streams[0].capture_index",    "enum", "VR1"  },   // capture inputs
        { "sensors.sensor[0].source",            "enum", "AV1"  },   // analog pool (set 0 = the AV ifaces)
    };
    for (const Case& k : cases) {
        const std::string got = c.widgetTypeFor(k.path);
        ck(got == k.want, std::string(k.path) + " drops as " + k.want, "got " + got);
        std::string by; std::vector<MetaModel::PickerSet> sets;
        const bool has = m.fieldPicker(k.path, by, sets);
        ck(has && !sets.empty() && !sets[0].options.empty(), "…with the board's own pool behind it");
        if (has && !sets.empty() && !sets[0].options.empty()) {
            // "None" heads a channel list (the 255 sentinel the field's default holds); the pool follows.
            const auto& lab = sets[0].options;
            const auto& val = sets[0].values;
            const bool sentinel = !lab.empty() && lab.front() == "None" && !val.empty() && val.front() == 255;
            const std::string firstPin = sentinel ? lab[1] : lab[0];
            ck(firstPin == k.first, std::string("…starting at ") + k.first, firstPin);
        }
    }

    std::puts("\n=== and the classifications either side of it still hold ===");
    ck(c.widgetTypeFor("ignition.ign_table")            == "table",      "a table drops as a table");
    ck(c.widgetTypeFor("trigger.streams[0].cell[].v")   == "array1d",    "a run drops as the strip");
    ck(c.widgetTypeFor("ignition.max_adv_deg")          == "configedit", "a plain scalar is still a spin box");

    // A "None" option (the 255 sentinel) and the parallel `pins` flags came in with the cylinder channel
    // fields: an option that names no hardware must not be counted as a claim on any. No shipped pool
    // carries either today, so what is pinned is the INVARIANT — wherever pins are emitted they stay
    // parallel to options, and any 255 is not a pin — which stays true when a pool brings them back.
    {
        int withPins = 0, parallel = 0, sentinels = 0, sentinelPins = 0;
        for (const auto& [arrKey, ca] : m.configArrays())
            for (const auto& f : ca.fields)
                for (const auto& ps : f.pickerSets) {
                    if (ps.pins.empty()) continue;
                    ++withPins;
                    if (ps.pins.size() == ps.options.size()) ++parallel;
                    for (size_t i = 0; i < ps.values.size() && i < ps.pins.size(); ++i)
                        if (ps.values[i] == 255) { ++sentinels; if (ps.pins[i]) ++sentinelPins; }
                }
        ck(withPins == parallel, "every option that says whether it is a pin says it for all of them",
           std::to_string(parallel) + " of " + std::to_string(withPins) + " sets");
        ck(sentinelPins == 0, "no 255 sentinel is counted as a pin",
           std::to_string(sentinels) + " sentinel(s) seen");
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "every channel field offers the board's list",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
