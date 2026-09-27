// EVERY CONTROL ON A PAGE, ASKED THE TWO QUESTIONS THAT MATTER.
//
// A page is data — bindings and conditions in a document — so "I built the page" says nothing about
// whether a single control on it works. The Output Setup page shipped with a name box that could not
// be typed into, a wizard that left the slot disabled, and a Driving panel gated on a condition:
// three separate failures, each found by hand, one at a time, by the person trying to use it. All
// three were answerable without a mouse.
//
// For each bound control this asks:
//   1. does its binding resolve to a real field? (an unresolvable path reads 0 and writes nowhere)
//   2. with the slot enabled, is the control ENABLED? (a gate that never opens is a dead panel)
// and for the enable conditions, that they are false when they should be — a gate that is always
// open is not a gate.
//
//   cmake --build build --target page_controls_test
//   STUDIO_ECU_DIR=<the studio's per-ECU folder> ./build/page_controls_test
// The pages and the pcvars.json side-car live in that folder (written by the studio for a connected
// ECU), so without STUDIO_ECU_DIR there is nothing to check and the test skips.

#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/MathEvaluator.h"
#include "model/SigilResolvers.h"
#include "surface/PanelModel.h"

#include <j/config/Json.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[page] %-64s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// Every element in a saved page, including the ones nested inside panels — whose children are stored
// as a JSON STRING, not as a nested array, so a plain tree walk finds the panels and stops there. That
// is how a first version of this test passed while looking at 13 of the page's 134 controls.
static void collect(const jf::JJson& node, std::vector<jf::JJson>& out) {
    if (node.isArray()) { for (const auto& e : node.arr()) collect(e, out); return; }
    if (!node.isObject()) return;
    if (node["type"].isString()) out.push_back(node);
    for (const auto& [key, v] : node.obj()) {
        (void)key;
        if (v.isArray() || v.isObject()) { collect(v, out); continue; }
        if (v.isString() && v.str().size() > 2 && v.str()[0] == '[' && v.str()[1] == '{') {
            const auto parsed = jf::JJson::tryParse(v.str());
            if (parsed) collect(*parsed, out);
        }
    }
}

int main() {
    const char* ecuDir = std::getenv("STUDIO_ECU_DIR");
    if (!ecuDir || !*ecuDir) {
        std::printf("[page] STUDIO_ECU_DIR not set — no saved pages to check, skipped\n");
        return 0;
    }
    const std::string PCVARS_PATH = std::string(ecuDir) + "/pcvars.json";
    const std::string DOC_PATH    = std::string(ecuDir) + "/dashboard.gui";
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());
    static ConfigSigilResolver configSigils;
    MathEvaluator::instance().registerResolver(&configSigils);

    // THE HOST-SIDE VARIABLES THE PAGE INDEXES BY. pc.output_sel is not in the generated meta — it is a
    // per-ECU side-car the studio writes (pcvars.json), and every binding on this page is subscripted by
    // it. Without them the page's paths resolve to nothing, which is a fair description of what the user
    // sees if the side-car is ever missing.
    if (const auto pcv = jf::JJson::tryParseFile(PCVARS_PATH)) {
        std::vector<MetaModel::PcVar> vars;
        for (const jf::JJson& v : (*pcv)["pcVars"].arr()) {
            MetaModel::PcVar pv;
            pv.name = v["name"].str();
            if (pv.name.empty()) continue;
            pv.datatype = v.contains("datatype") ? v["datatype"].str() : std::string("F32");
            pv.scale = v["scale"].number(1.0);
            pv.minV = v["min"].number(); pv.maxV = v["max"].number();
            pv.units = v["units"].str(); pv.label = v["label"].str(); pv.kind = v["kind"].str();
            for (const jf::JJson& o : v["options"].arr()) pv.options.push_back(o.str());
            vars.push_back(pv);
        }
        m.applyPcVars(vars);
        c.setConfigImage(m.defaultImage());
        // The host block is ALLOCATED, not part of the tune image — the studio does this on connect, so
        // a test that skips it has a selector that reads 0 whatever it writes. (Which is how the first
        // run of this test blamed the app for its own missing setup.)
        c.initSegments();
    }
    ck(m.config().count("pc.output_sel") != 0, "the page's slot selector (pc.output_sel) exists");

    const auto doc = jf::JJson::tryParseFile(DOC_PATH);
    if (!doc) { std::printf("[page] no document at %s — skipped\n", DOC_PATH.c_str()); return 0; }
    // EVERY PAGE THAT DRIVES A SLOT, not just the first one that broke. The carrier moved to its own
    // page, which means a second page's worth of bindings and gates indexed by the same selector —
    // and a page nobody drives is exactly how the first one shipped unusable.
    int fails_all = 0;
    // Each page with what it is REQUIRED to have. A slot page is gated throughout — every control on it
    // answers to the slot being enabled and to which source is in force — while a generic table page
    // gates nothing, because nothing about a table is conditional: it is cells, two axes and a name.
    // Demanding gates of it would be demanding a condition somebody would then have to invent.
    // A TEMPLATE PAGE IS WRITTEN WITH "[*]" AND MEANS NOTHING WITHOUT AN ELEMENT. The viewport that
    // shows one supplies it; this test is the viewport, so it has to supply one too. Checking the raw
    // body resolved nothing — "outputs.output[*].enabled" is not a field and no gate on it can ever
    // open — and every assertion here would have failed for the template rather than for the page.
    struct PageReq { const char* path; size_t minEls; bool gated; const char* element; };
    // ONE OUTPUT'S OWN PAGE, not the old single "Output Setup" behind a slot picker. Outputs are a
    // template now — one body, a page per slot, each gated on that slot's enable — so the path names
    // an output and the Frequency page hangs off it rather than off whatever a dropdown pointed at.
    for (const PageReq& req : {
             // Rows are pins: outputs.output[3] IS IGN4, and its page is named so.
             PageReq{"Configuration/Electrical/Outputs/IGN4",               20, true,  "outputs.output[3]"},
             PageReq{"Configuration/Electrical/Outputs/IGN4/Frequency",     10, true,  "outputs.output[3]"},
             // GATED NOW, and not by a slot switch: a generic table's optional Y axis is a real code
             // path — TableEval collapses a disabled axis to index 0 without ever fetching its source
             // — so `table_1_y_src` greys while `table_1_y_en` is off. It is here as the page whose
             // gate answers to something OTHER than an output slot, which is what the satisfiability
             // check below has to cope with.
             PageReq{"Configuration/Generic Tables/Generic Table 1",         8, true,  ""} }) {
    const char* PAGE = req.path;
    fails = 0;
    std::printf("\n[page] %s\n", PAGE);
    const jf::JJson& page = (*doc)["panelLibrary"][PAGE];
    if (!page.isObject()) { std::printf("[page] page not in the document — FAILED\n"); return 1; }

    // Stand in for the viewport: for the duration of this page, "[*]" is this element.
    MathEvaluator::ElementScope elemScope(req.element);

    std::vector<jf::JJson> els;
    collect(page, els);
    ck(els.size() >= req.minEls, "the page has its controls", std::to_string(els.size()) + " elements");

    // THE SLOT THE PAGE IS SHOWING. pc.output_sel is a host-side variable, so it lives in the config
    // image like any other field; the page's every binding is indexed by it.
    // THE SLOT THE PAGE IS SHOWING is now the ELEMENT the viewport supplies, not a host-side
    // selector: outputs are a template, so "[*]" is whatever element is in scope. pc.output_sel is
    // gone with the picker, and writing it here disabled one slot while the page read another —
    // which is why every gate looked "always open".
    const int SLOT = 3;
    const std::string slot = "outputs.output[" + std::to_string(SLOT) + "]";
    c.setConfigValue(slot + ".function", 0);

    // 1. Every binding resolves to a real field.
    int bound = 0, unresolved = 0;
    std::string firstBad;
    for (const auto& e : els) {
        const std::string sig = e["props"]["signalName"].str();
        if (sig.empty() || sig[0] == '[') continue;             // channels/expressions, not config paths
        if (e["type"].str() == "wizard") continue;              // names a SLOT, not a field in one
        // A live readout names a TELEMETRY CHANNEL, unsigilled — "rpm", not "[$rpm]". It is a real
        // binding to a real thing; it is simply not a config field, and asking the config map about it
        // is the test's confusion rather than the page's.
        if (m.telemetry().count(sig)) continue;
        ++bound;
        const std::string concrete = MathEvaluator::instance().resolveIndexed(
            MathEvaluator::resolveTemplate(sig, MathEvaluator::elementContext()));
        int off = 0, size = 0;
        const bool ok = m.locate(concrete).valid() || m.resolveBlob(concrete, off, size);
        if (!ok) { ++unresolved; if (firstBad.empty()) firstBad = sig + " -> " + concrete; }
    }
    ck(unresolved == 0, "every config binding on the page resolves to a field",
       std::to_string(unresolved) + " of " + std::to_string(bound) + " unresolved: " + firstBad);

    // 2. The gates. With the slot DISABLED they must be shut; with it ENABLED they must open — a panel
    //    that never ungreys is exactly as useless as one that is always live.
    std::set<std::string> gates;
    for (const auto& e : els) {
        const std::string en = e["props"]["enableCondition"].str();
        if (!en.empty()) gates.insert(en);
    }
    ck(req.gated == !gates.empty(), "the page gates its controls (or has nothing to gate)",
       std::to_string(gates.size()) + " distinct conditions");

    // A GATE MUST BE ABLE TO OPEN. Not "opens when the slot is enabled" — several are compound
    // ("enabled AND value_source == 0"), and it is no fault of theirs that one term is not enough. The
    // invariant is that the whole condition is SATISFIABLE: set each field it names to a value meeting
    // the comparison it is written against, and the gate must go true. A gate that stays shut then is
    // either naming a field that does not exist or comparing against a value the field cannot hold,
    // and both of those are a dead panel on somebody's screen.
    int stuck_off = 0, stuck_on = 0;
    std::string firstStuck;
    for (const std::string& g : gates) {
        c.setConfigValue(slot + ".function", 0);
        const bool offNow = MathEvaluator::instance().evaluate(g) != 0.0;

        // Satisfy every "[#path] <op> <number>" term the condition is built from.
        for (size_t at = g.find("[#"); at != std::string::npos; at = g.find("[#", at + 2)) {
            int depth = 0; size_t close = std::string::npos;
            for (size_t k = at; k < g.size(); ++k) {
                if (g[k] == '[') ++depth;
                else if (g[k] == ']' && --depth == 0) { close = k; break; }
            }
            if (close == std::string::npos) break;
            const std::string path = MathEvaluator::instance().resolveIndexed(
                MathEvaluator::resolveTemplate(g.substr(at + 2, close - at - 2),
                                               MathEvaluator::elementContext()));
            size_t q = g.find_first_not_of(' ', close + 1);
            if (q == std::string::npos) break;
            std::string op;
            while (q < g.size() && std::string("=<>!").find(g[q]) != std::string::npos) op += g[q++];
            q = g.find_first_not_of(' ', q);
            if (op.empty() || q == std::string::npos ||
                !(std::isdigit((unsigned char)g[q]) || g[q] == '-')) continue;
            const double n = std::atof(g.c_str() + q);
            // "!=" is satisfied by anything else — a row's function gate reads `function != 0`.
            c.setConfigValue(path, (op == ">" || op == "!=") ? n + 1 : (op == "<") ? n - 1 : n);
        }
        const bool onNow = MathEvaluator::instance().evaluate(g) != 0.0;
        if (!onNow) { ++stuck_off; if (firstStuck.empty()) firstStuck = g; }
        // Only a gate that NAMES the slot switch is asked to answer to it. The carrier page's gates ask
        // which source is in force, which is a fair question about a slot nobody has switched on yet —
        // its whole node is hidden unless the slot is a PWM one, and that is the gate it answers to.
        if (offNow && onNow && g.find(".function") != std::string::npos) ++stuck_on;
    }

    ck(stuck_off == 0, "…and every gate can be SATISFIED",
       std::to_string(stuck_off) + " never open: " + firstStuck);
    ck(stuck_on == 0, "…and is shut while the slot is disabled", std::to_string(stuck_on) + " always open");

    // 3. A CONTROL MUST HAVE SOMETHING TO OFFER. An enum with no options and a picker with no list are
    //    the same thing on screen: a box that opens onto nothing. The output candidates arrived exactly
    //    that way — `sig` names a bus channel and the meta carried no hint that it did, so the page
    //    drew an empty dropdown and a candidate could not be chosen at all.
    int empty_choice = 0;
    std::string firstEmpty;
    for (const auto& e : els) {
        const std::string t = e["type"].str();
        if (t != "enum" && t != "combobox") continue;
        const std::string sig = e["props"]["signalName"].str();
        if (sig.empty() || sig[0] == '[') continue;
        const std::string path = MathEvaluator::instance().resolveIndexed(
            MathEvaluator::resolveTemplate(sig, MathEvaluator::elementContext()));
        // Either it is a picker (a signal/sensor selector, whose list is the catalogue) or it declares
        // its own options. Neither = nothing to choose.
        const bool picker  = m.isSignalField(path);
        const bool options = !m.enumOptions(path).empty();
        if (!picker && !options) { ++empty_choice; if (firstEmpty.empty()) firstEmpty = t + " " + path; }
    }
    ck(empty_choice == 0, "every dropdown offers something to choose",
       std::to_string(empty_choice) + " empty: " + firstEmpty);

    fails_all += fails;
    }
    std::printf(fails_all ? "\n[page] %d FAILED\n" : "\n[page] all passed\n", fails_all);
    return fails_all ? 1 : 0;
}
