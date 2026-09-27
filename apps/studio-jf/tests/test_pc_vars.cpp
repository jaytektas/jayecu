// Host variables ("pc.<name>") come from two places and must stay apart: the DEFINITION (the meta's
// "pcVars", from the schema's pc_vars) and the PROJECT (the pcvars.json side-car the dictionary edits).
// The shipped dashboard binds to the definition's in hundreds of places; they used to live only in a
// side-car nothing installed, so on a user's machine every chooser read a bare 0.
#include "../src/model/MetaModel.h"
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) ++fails;
}
static const MetaModel::PcVar* find(const MetaModel& m, const std::string& n) {
    for (const auto& v : m.pcVars()) if (v.name == n) return &v;
    return nullptr;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }

    // The meta declares the dashboard's variables, defaults and option lists included.
    for (const char* n : {"diag_view", "test_count", "test_on_ms", "test_off_ms", "knock_noise_cyl",
                          "etb_autotune_rule"}) {
        ck(find(m, n) != nullptr, std::string("the meta declares pc.") + n);
        ck(m.config().count(std::string("pc.") + n) != 0, std::string("pc.") + n + " resolves as a path");
        ck(m.isDefinitionPcVar(n), std::string(n) + " is the definition's");
    }
    const auto* tc = find(m, "test_count");
    ck(tc && tc->hasDef && tc->defV == 3.0, "test_count keeps its default of 3");
    const auto* rule = find(m, "etb_autotune_rule");
    ck(rule && rule->kind == "enum" && rule->options.size() == 5, "the autotune rule is a 5-option enum");
    ck(m.projectPcVars().empty(), "a fresh meta has no project variables");

    // The project adds its own; a name the definition owns is ignored, not duplicated or renamed.
    MetaModel::PcVar mine; mine.name = "my_sel"; mine.maxV = 3;
    MetaModel::PcVar clash; clash.name = "test_count"; clash.maxV = 999;
    m.applyPcVars({mine, clash});
    ck(m.projectPcVars().size() == 1 && m.projectPcVars()[0].name == "my_sel",
       "the project keeps only its own variable");
    ck(m.config().count("pc.my_sel") != 0, "the project's variable resolves");
    ck(find(m, "test_count") && find(m, "test_count")->maxV == 200.0, "the definition's declaration wins");
    ck(m.pcVars().size() == 7, "the live set is definition + project");

    // Removing everything the project had leaves the definition's in place.
    m.applyPcVars({});
    ck(m.config().count("pc.my_sel") == 0, "a removed project variable stops resolving");
    ck(m.config().count("pc.diag_view") != 0, "the definition's survive an empty project set");

    // A meta reload starts the project over (the studio re-applies its side-car afterwards).
    m.applyPcVars({mine});
    ck(m.loadFile(REAL_META), "reload");
    ck(m.projectPcVars().empty() && m.config().count("pc.my_sel") == 0, "a reload drops project variables");
    ck(m.config().count("pc.diag_view") != 0, "a reload keeps the definition's");

    std::printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
