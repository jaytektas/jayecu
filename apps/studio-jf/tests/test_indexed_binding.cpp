// resolveIndexed: name[@expr] -> name[<idx>], with the tail (idx<0) resolving BLANK.
// Repro for "engine.cyl[@engine.firing_order[6].cyl - 1].bank" showing a value on an unused slot.
//   cmake --build build --target indexed_binding_test && ./build/indexed_binding_test
#include "../src/model/MathEvaluator.h"
#include "../src/model/ISigilResolver.h"
#include <cstdio>
#include <map>
#include <string>

static int fails = 0;
static void ck(const std::string& got, const std::string& want, const char* what) {
    const bool ok = got == want;
    std::printf("  %-52s %s   got=[%s] want=[%s]\n", what, ok ? "PASS" : "FAIL", got.c_str(), want.c_str());
    if (!ok) ++fails;
}

// '#' config resolver — returns the mapped value, or a sentinel for an unmapped path so we SEE a miss.
struct Cfg : ISigilResolver {
    std::map<std::string, double> v;
    char   sigil() const override { return '#'; }
    double resolveSigil(const std::string& n) const override {
        auto it = v.find(n); return it == v.end() ? -777.0 : it->second;
    }
    bool provides(const std::string& n) const override { return v.count(n) != 0; }
    std::vector<std::string> available() const override {
        std::vector<std::string> o; for (auto& [k, x] : v) { (void)x; o.push_back(k); } return o;
    }
};
static Cfg g_cfg;

int main() {
    auto& E = MathEvaluator::instance();
    E.registerResolver(&g_cfg);
    std::puts("=== resolveIndexed: tail (idx<0) must resolve blank ===");

    // A used position: firing_order[0].cyl = 1 -> [@ 1 - 1] = index 0.
    g_cfg.v["engine.firing_order[0].cyl"] = 1;
    ck(E.resolveIndexed("engine.cyl[@engine.firing_order[0].cyl - 1].bank"),
       "engine.cyl[0].bank", "used slot (cyl 1) -> cyl[0]");

    // The unused tail: firing_order[6].cyl = 0 -> [@ 0 - 1] = index -1 -> BLANK.
    g_cfg.v["engine.firing_order[6].cyl"] = 0;
    ck(E.resolveIndexed("engine.cyl[@engine.firing_order[6].cyl - 1].bank"),
       "", "tail slot (cyl 0) -> blank");

    // Same, position 11 (row 12).
    g_cfg.v["engine.firing_order[11].cyl"] = 0;
    ck(E.resolveIndexed("engine.cyl[@engine.firing_order[11].cyl - 1].bank"),
       "", "tail slot pos 11 -> blank");

    // Direct evaluate of the inner, to see what the index computes to.
    std::printf("  inner eval firing_order[6].cyl - 1 = %g (want -1)\n",
                E.evaluate("engine.firing_order[6].cyl - 1"));
    std::printf("  inner eval firing_order[6].cyl     = %g (want 0)\n",
                E.evaluate("engine.firing_order[6].cyl"));

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "resolveIndexed blanks the tail",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
