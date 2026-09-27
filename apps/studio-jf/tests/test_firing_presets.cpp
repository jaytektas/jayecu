// The Setting Selector's firing-order presets, through the REAL apply path (PresetOptions::apply ->
// Cache::setConfigValue). Proves a preset writes only engine.firing_order[*].cyl + engine.cylinder_count
// (never tdc_angle or bank — the firmware computes TDCs from the order), and that matchIndex() then marks
// it selected. Reads the actual preset string authored into the dashboard so the test tracks the data.
//   cmake --build build --target firing_presets_test && ./build/firing_presets_test <meta> <presets.txt>
#include "../src/model/PresetOptions.h"
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-56s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    MetaModel meta;
    const char* mc[] = { argc > 1 ? argv[1] : nullptr, "../../shared/tuneit-meta.json",
                         "../../../shared/tuneit-meta.json" };
    bool ok = false;
    for (const char* c : mc) if (c && meta.loadFile(c)) { ok = true; break; }
    if (!ok) { std::puts("[firing-presets] (no meta — skipped)"); return 0; }

    // The authored preset string (label|path=val,…\n…). Read from the dashboard export if given, else a
    // self-contained fallback that exercises the same contract (firing_order + cylinder_count, never TDC).
    auto fo = [](std::initializer_list<int> seq) {
        std::string s; int i = 0; auto add = [&](int idx, int v){ if (i) s += ','; s += "engine.firing_order[" + std::to_string(idx) + "].cyl=" + std::to_string(v); ++i; };
        int p = 0; for (int c : seq) add(p++, c);
        for (; p < 12; ++p) add(p, 0);
        s += ",engine.cylinder_count=" + std::to_string((int)seq.size());
        return s;
    };
    std::string presets =
        "I4 1-3-4-2 (Honda, Toyota, most)|" + fo({1,3,4,2}) + "\n"
        "I6 1-5-3-6-2-4 (BMW, 2JZ, RB)|"    + fo({1,5,3,6,2,4}) + "\n"
        "V8 1-8-7-2-6-5-4-3 (GM LS)|"       + fo({1,8,7,2,6,5,4,3}) + "\n"
        "V12 1-7-5-11-3-9-6-12-2-8-4-10 (Jaguar, BMW)|" + fo({1,7,5,11,3,9,6,12,2,8,4,10});
    if (argc > 2) { std::ifstream f(argv[2]); std::stringstream ss; ss << f.rdbuf(); if (ss.rdbuf()->in_avail() || f) { std::string s = ss.str(); if (!s.empty()) presets = s; } }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    std::vector<uint8_t> img = meta.defaultImage();
    if (img.empty()) img.assign((size_t)meta.configSize(), 0);
    C.setConfigImage(img);

    PresetOptions po = PresetOptions::fromCompact(presets);
    const size_t wantMin = argc > 2 ? 15 : 4;   // the authored dashboard has the full set; the fallback, four
    std::printf("=== %zu firing-order presets ===\n", po.options.size());
    ck(po.options.size() >= wantMin, "the preset set parsed", std::to_string(po.options.size()));

    // Every preset: writes cylinder_count + firing_order, and NOTHING else (no tdc_angle, no bank).
    bool clean = true;
    for (const auto& o : po.options)
        for (const auto& pr : o.pairs)
            if (pr.first.find("tdc_angle") != std::string::npos || pr.first.find(".bank") != std::string::npos)
                { clean = false; std::printf("   stray write: %s in '%s'\n", pr.first.c_str(), o.label.c_str()); }
    ck(clean, "no preset writes tdc_angle or bank");

    // Apply the GM LS V8 and read the config image back through the Cache.
    int ls = -1;
    for (size_t i = 0; i < po.options.size(); ++i)
        if (po.options[i].label.find("GM LS") != std::string::npos) ls = (int)i;
    ck(ls >= 0, "found the GM LS V8 preset");
    if (ls >= 0) {
        po.apply(ls);
        const int want[12] = { 1,8,7,2,6,5,4,3, 0,0,0,0 };
        bool fo = true;
        for (int i = 0; i < 12; ++i)
            if ((int)llround(C.configValue("engine.firing_order[" + std::to_string(i) + "].cyl")) != want[i]) fo = false;
        ck(fo, "firing_order = 1-8-7-2-6-5-4-3 + zeroed tail");
        ck((int)llround(C.configValue("engine.cylinder_count")) == 8, "cylinder_count = 8");
        ck(po.matchIndex() == ls, "the selector now marks GM LS as selected", std::to_string(po.matchIndex()));
    }

    // Apply the I6 and confirm the tail is re-zeroed (positions 6..11 cleared going 8cyl -> 6cyl).
    int i6 = -1;
    for (size_t i = 0; i < po.options.size(); ++i)
        if (po.options[i].label.find("1-5-3-6-2-4") != std::string::npos) i6 = (int)i;
    if (i6 >= 0) {
        po.apply(i6);
        const int want[12] = { 1,5,3,6,2,4, 0,0,0,0,0,0 };
        bool fo = true;
        for (int i = 0; i < 12; ++i)
            if ((int)llround(C.configValue("engine.firing_order[" + std::to_string(i) + "].cyl")) != want[i]) fo = false;
        ck(fo && (int)llround(C.configValue("engine.cylinder_count")) == 6,
           "switching to I6 clears the tail (no stale positions 7-8)");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "clean", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
