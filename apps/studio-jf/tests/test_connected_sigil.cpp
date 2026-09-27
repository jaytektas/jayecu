// [%connected] — the studio's link-live flag — and the "editable offline OR stopped" panel condition.
//
// Two bugs met here: the flag was declared but never set (always read 0), and the expression builder's
// field tree had no App State category, so "[%connected]" read as an unknown field. This proves the
// sigil tracks the flag, the builder tree now exposes it, and the panel enableCondition behaves across
// offline / running / cranking / stopped.
//
//   cmake --build build --target connected_sigil_test && ./build/connected_sigil_test
#include "../src/model/MathEvaluator.h"
#include "../src/model/SigilResolvers.h"
#include "../src/model/ISigilResolver.h"
#include "../src/ui/DictionaryTree.h"
#include "../src/model/MetaModel.h"

#include <cstdio>
#include <map>
#include <string>
#include <algorithm>

static int fails = 0;
static void ck(bool ok, const std::string& what) {
    std::printf("  %-62s %s\n", what.c_str(), ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

// A '$' channel so the condition can read engine_state (0 STOPPED / 1 CRANKING / 2 RUNNING).
struct TestChannels : ISigilResolver {
    std::map<std::string, double> v;
    char sigil() const override { return '$'; }
    double resolveSigil(const std::string& n) const override {
        auto it = v.find(n); return it == v.end() ? 0.0 : it->second;
    }
    bool provides(const std::string& n) const override { return v.count(n) != 0; }
    std::vector<std::string> available() const override {
        std::vector<std::string> o; for (const auto& [k, x] : v) { (void)x; o.push_back(k); } return o;
    }
};
static TestChannels g_chans;
static AppStateSigilResolver g_app;

int main() {
    auto& E = MathEvaluator::instance();
    E.registerResolver(&g_app);
    E.registerResolver(&g_chans);

    std::puts("=== [%connected] + editable-offline-or-stopped condition ===");

    // The resolver exposes 'connected' — which is what buildSigilTree lists.
    auto av = AppStateSigilResolver{}.available();
    ck(std::find(av.begin(), av.end(), "connected") != av.end(), "resolver exposes 'connected'");

    // The sigil tracks the flag (was the dead one).
    AppStateSigilResolver::connected = false;
    ck(E.evaluate("[%connected]") == 0.0, "flag false -> [%connected] == 0");
    AppStateSigilResolver::connected = true;
    ck(E.evaluate("[%connected]") == 1.0, "flag true  -> [%connected] == 1");

    // THE panel enableCondition: editable when offline OR (online AND stopped).
    const char* cond = "![%connected] || [$engine_state] == 0";

    AppStateSigilResolver::connected = false; g_chans.v["engine_state"] = 2.0;   // offline, last state RUNNING
    ck(E.evaluate(cond) != 0.0, "offline -> editable (even if last telemetry was RUNNING)");

    AppStateSigilResolver::connected = true;  g_chans.v["engine_state"] = 2.0;   // online, RUNNING
    ck(E.evaluate(cond) == 0.0, "online + running  -> disabled");
    g_chans.v["engine_state"] = 1.0;                                             // online, CRANKING
    ck(E.evaluate(cond) == 0.0, "online + cranking -> disabled");
    g_chans.v["engine_state"] = 0.0;                                             // online, STOPPED
    ck(E.evaluate(cond) != 0.0, "online + stopped  -> editable");

    // The 'unknown field' fix: buildSigilTree now carries an App State group with [%connected].
    MetaModel meta;   // empty is fine — the App State category is meta-independent
    JTreeViewNode tree = buildSigilTree(meta, {});
    bool listed = false;
    for (const auto& cat : tree.children)
        if (cat.label == "App State")
            for (const auto& n : cat.children)
                if (n.userData == "[%connected]") listed = true;
    ck(listed, "buildSigilTree exposes [%connected] under \"App State\" (no longer unknown)");

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "[%connected] tracks the link and the condition holds",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
