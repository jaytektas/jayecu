// What does the field picker show you?
//
// Two things went wrong in one dialog. The list it was built from came from meta.config(), which is the
// FLAT scalars and the tables — everything inside a struct array (every cylinder's TDC angle, all 122
// sensors, all 42 outputs, every trigger stream) lives in configArrays() and was simply not offered.
// Then the list it did have was FLATTENED into one level per module, so 2854 rows sat under a handful of
// headings with nothing saying which sensor or which cylinder any of them belonged to.
//
// The dictionary has built the right shape all along — module ▸ Values / Tables / arrays ▸ element ▸
// field — so the picker shows THAT, from the same builder. This test is about the tree: it contains the
// fields, it is nested rather than flat, and every path it offers can be located. The last one is the
// half that fails quietly: a path a picker offers but the Cache cannot resolve is recorded into a preset
// that then writes nowhere and never matches.
//
//   cmake --build build --target config_paths_test && ./build/config_paths_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "ui/DictionaryTree.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-64s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// Depth of the deepest leaf, and the path from the root to a named binding — how a reader would say
// "engine ▸ Per-Cylinder Map ▸ Cylinder 1 ▸ TDC Angle".
static void walk(const JTreeViewNode& n, const std::string& trail, int depth,
                 const std::function<void(const JTreeViewNode&, const std::string&, int)>& fn) {
    const std::string here = trail.empty() ? n.label : trail + " > " + n.label;
    fn(n, here, depth);
    for (const auto& c : n.children) walk(c, here, depth + 1, fn);
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    const JTreeViewNode tree = buildConfigTree(m);

    int leaves = 0, unresolvedFields = 0, elementRows = 0, bitRows = 0, maxDepth = 0;
    std::string firstBad, tdcTrail, sensorTrail;
    walk(tree, {}, 0, [&](const JTreeViewNode& n, const std::string& trail, int depth) {
        if (depth > maxDepth) maxDepth = depth;
        if (n.userData.empty()) return;
        ++leaves;
        if (n.userData == "engine.cyl[0].tdc_angle")      tdcTrail = trail;
        if (n.userData == "sensors.sensor[clt].source")   sensorTrail = trail;
        if (c.isConfig(n.userData) || m.locate(n.userData).valid()) return;
        if (!n.userData.empty() && n.userData.back() == ']') { ++elementRows; return; }
        if (m.bitGroupOf(n.userData)) { ++bitRows; return; }
        if (firstBad.empty()) firstBad = n.userData + "  (" + trail + ")";
        ++unresolvedFields;
    });

    std::puts("=== the field picker's tree ===");

    // The field that was reported missing — and WHERE it is, which is the other half of the complaint.
    ck(!tdcTrail.empty(), "a cylinder's TDC angle is in the tree");
    if (!tdcTrail.empty()) std::printf("      %s\n", tdcTrail.c_str());
    ck(!sensorTrail.empty(), "a sensor's field is there, under its own element");
    if (!sensorTrail.empty()) std::printf("      %s\n", sensorTrail.c_str());

    // NESTED, not flattened. The old picker was Sources ▸ module ▸ leaf: three levels, everything at the
    // bottom. An array field needs module ▸ array ▸ element ▸ field to say what it belongs to.
    ck(maxDepth >= 4, "the tree is nested deeply enough to say what a field belongs to",
       "deepest " + std::to_string(maxDepth));

    // Enough leaves to be the whole config, not a corner of it.
    ck(leaves > 3000, "it offers the whole config", std::to_string(leaves) + " bindings");

    // EVERY FIELD ROW RESOLVES. The tree also carries rows that are not fields — an element row
    // ("engine.cyl[0]") exists to be dropped on a viewport, and a bit-group row addresses bits inside a
    // field — so those are counted apart. The preset dialog declines anything that does not locate, which
    // is what stops a pair being recorded that writes nowhere.
    ck(unresolvedFields == 0, "every FIELD the tree offers can be located",
       std::to_string(unresolvedFields) + " cannot, e.g. " + firstBad);
    std::printf("      (%d element rows and %d bit-group rows are not fields and are declined)\n",
                elementRows, bitRows);

    // A BIT GROUP INSIDE AN ARRAY ELEMENT IS A REAL FIELD. locate() found the group and then looked its
    // parent up in the flat scalar map, so every one of these resolved to nothing — a checkbox that read
    // 0 for ever and wrote nowhere, on every sensor, output and trigger stream the dictionary offers.
    {
        const std::string p = "sensors.sensor[0].flags.invert";
        const MetaModel::Location L = m.locate(p);
        ck(L.valid(), "a bit group inside an array element resolves", p);
        ck(L.bits.packed(), "…as a BIT RANGE, not the whole byte");

        // It reads and writes the bits it names, and leaves the rest of the byte alone.
        const std::string parent = "sensors.sensor[0].flags";
        c.setConfigValue(parent, 0);
        c.setConfigValue(p, 1);
        ck(c.configValue(p) == 1.0, "…and what is written is what is read back",
           std::to_string(c.configValue(p)));
        const double whole = c.configValue(parent);
        ck(whole != 0.0, "…in the parent byte", std::to_string(whole));
        c.setConfigValue(p, 0);
        ck(c.configValue(parent) == 0.0, "…and clearing it clears only those bits",
           std::to_string(c.configValue(parent)));
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "the picker shows the config as a tree, and every leaf resolves",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
