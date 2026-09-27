// Every personality an output slot can be given COMPILES, against the meta it ships in.
//
// A template writes two conditions as source and the studio compiles them at apply time. A typo in one
// — a channel that was renamed, a function that does not exist, a parameter token nobody declared —
// would otherwise be found by the first person who picked it, at which point the wizard writes one
// condition and not the other and the slot is half set up. Cheap to check here: compile all of them.
//
//   cmake --build build --target output_templates_test && ./build/output_templates_test

#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"
#include "../src/model/ExprCompiler.h"
#include "../src/model/OutputTemplate.h"

#include <j/config/Json.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
// "neutral+clutch" / "neutral" / "none" — which options a case had ticked, so a failure says which of
// the four programs it was.
static std::string _choiceTag(const OutputTemplate& t, const TemplateChoice& c) {
    std::string s;
    for (size_t i = 0; i < t.options.size(); ++i) {
        if (!c.ticked(i)) continue;
        if (!s.empty()) s += " " + (i < c.join.size() ? c.join[i] : std::string("and")) + " ";
        s += t.options[i].name;
    }
    return s.empty() ? "none" : s;
}
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("[templates] %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  — " + detail).c_str());
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    MetaModel meta;
    const char* cands[] = { argc > 1 ? argv[1] : nullptr, META_PATH };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::printf("[templates] FAIL: no meta at %s\n", META_PATH); return 1; }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    C.setConfigImage(meta.defaultImage());

    const std::vector<OutputTemplate> ts = outputTemplatesFromMeta(jf::JJson::parseFile(meta.path()));
    check(!ts.empty(), "the firmware ships output templates", std::to_string(ts.size()));

    // Slot 7 stands for all of them: a template's conditions reference the slot's OWN parameters, so
    // the paths differ per slot but the shape does not.
    const std::string slot = "outputs.output[7]";
    // Resolved BEFORE the check call: argument evaluation order is unspecified, so a size read inside
    // the call can print the value the resolver had not yet written (it said "0" and passed).
    int off = 0, size = 0;
    const bool blobOk = meta.resolveBlob(slot + ".on_expr", off, size);
    check(blobOk && size > 0, "a slot has a program block to compile into", std::to_string(size));

    // THE CATALOG IS SPARSE. Signal ids are append-only and permanent, so the number of NAMED signals
    // is smaller than the highest id — and a program's selector must be checked against the latter.
    // Bounding on the count is what made every channel added since the catalog last had no gaps
    // (ac_request, start_sw, uptime_s, trigger_teeth) compile into a program the firmware would reject.
    check(meta.signalCount() > static_cast<int>(meta.signalMap().size()),
          "the signal catalog is sparse, and its SIZE is the upper id",
          std::to_string(meta.signalMap().size()) + " names, ids to " +
              std::to_string(meta.signalCount() - 1));

    for (const OutputTemplate& t : ts) {
        check(t.params.size() <= 4, t.name + ": asks for no more than the four parameters a slot has",
              std::to_string(t.params.size()));

        // EVERY CHOICE ITS OPTIONS ALLOW, not just the shipped one — the tick sets and the
        // connectives. A box the user can clear is a different program, and "the defaults compile"
        // says nothing about the starter somebody sets up with no clutch switch, which is the
        // configuration the option exists for. This is the same list recognition searches, so a
        // choice that cannot compile is a choice the wizard could offer and then fail to write.
        const std::vector<TemplateChoice> choices = templateChoiceCandidates(t);
        if (!t.options.empty())
            check(choices.front() == templateDefaultChoice(t),
                  t.name + ": the shipped choice is the first one recognition tries");

        // An option must name a real sensor and contribute a real term, or it is a tick box that
        // silently does nothing — the template equivalent of a condition naming a dead channel.
        for (const OutputTemplate::Option& o : t.options) {
            check(!o.term.empty(), t.name + ": its option '" + o.name + "' contributes a term");
            check(!o.label.empty(), t.name + ": its option '" + o.name + "' has a label to tick");
            check(o.join == "and" || o.join == "or",
                  t.name + ": its option '" + o.name + "' says how it joins", o.join);
            // THE POLARITY THE WIZARD EDITS is the sensor's own invert bit. A path that does not
            // resolve is a tick box writing nowhere — the bug the packed-field fix was about.
            if (o.input.empty()) continue;
            check(meta.locate("sensors.sensor[" + o.input + "].flags.invert").valid(),
                  t.name + ": its option '" + o.name + "' has an invert flag to edit");
            if (o.input.empty()) continue;
            const std::string ip = "sensors.sensor[" + o.input + "].enabled";
            check(meta.locate(ip).valid(),
                  t.name + ": its option '" + o.name + "' reads a real sensor", ip);
        }
        // A CANDIDATE MUST NAME A REAL CHANNEL. A template whose value comes off the bus resolves its
        // names through the meta's signal map at apply time and writes nothing for one it cannot find —
        // so a renamed channel would leave the slot enabled, pinned, and delivering NOTHING, with no
        // error anywhere. That is a worse failure than a condition that will not compile, because the
        // wizard still looks like it worked.
        for (const OutputTemplate::Candidate& c : t.candidates) {
            check(meta.signalMap().count(c.signal) > 0,
                  t.name + ": its candidate names a real channel", c.signal);
            check(c.role >= 0 && c.role <= 2,
                  t.name + ": its candidate has a role a slot understands", std::to_string(c.role));
        }
        // …and a slot told to read candidates needs at least one, or it is enabled and silent.
        if (t.valueSource == 0)
            check(!t.candidates.empty(), t.name + ": reads candidates, so it names one");
        if (!t.candidates.empty())
            check(t.valueSource == 0,
                  t.name + ": names candidates, so its value source reads them",
                  std::to_string(t.valueSource));

        // A TOKEN THAT IS NEVER SUBSTITUTED is an option list nothing uses: the terms would be
        // declared, offered, ticked, and then dropped on the floor.
        if (!t.options.empty())
            check(t.onWhen.find(kOptionsToken) != std::string::npos ||
                  t.offWhen.find(kOptionsToken) != std::string::npos,
                  t.name + ": declares options and has a {@options} token to put them in");

      for (const TemplateChoice& choice : choices) {
        const std::string tag = t.options.empty() ? std::string()
                                                  : " [" + _choiceTag(t, choice) + "]";
        for (const char* which : { "on", "off" }) {
            const std::string src = templateSource(t, (std::string(which) == "on") ? t.onWhen : t.offWhen,
                                                   slot, choice);
            // NO TOKEN LEFT BEHIND. templateSource leaves an unknown "{token}" as written rather than
            // deleting it, so this catches a condition naming a parameter the template never declared —
            // which would otherwise compile as garbage or not at all.
            check(src.find('{') == std::string::npos,
                  t.name + tag + ": every {token} in its " + which + " condition is a declared parameter", src);

            ExprCompiler::Result r = ExprCompiler::compile(src, meta,
                                                           static_cast<uint32_t>(C.configImage().size()),
                                                           static_cast<uint16_t>(size));
            check(r.ok, t.name + tag + ": its " + which + " condition compiles", r.ok ? src : r.error);
            check(r.code.size() <= static_cast<size_t>(size),
                  t.name + tag + ": …and fits the slot's program block",
                  std::to_string(r.code.size()) + " of " + std::to_string(size) + " bytes");
        }
      }

        // A TABLE-DRIVEN template must leave the slot reading a real table. value_source 1 with no
        // table named is a slot at its failsafe: applied, apparently configured, and never running.
        if (t.valueSource == 1) {
            const TemplateWrites w = templateWrites(t, slot, std::vector<double>(4, 1.0),
                                                    templateDefaultChoice(t));
            double sel = -1;
            for (const auto& v : w.values) if (v.first == slot + ".duty_table_sel") sel = v.second;
            const std::vector<std::string> ids = meta.enumOptionIds(slot + ".duty_table_sel");
            check(sel >= 0 && sel < static_cast<double>(ids.size()),
                  t.name + ": its duty comes from a table it actually names",
                  t.dutyTable + " -> id " + std::to_string(static_cast<int>(sel)));
        }

        // The inputs it names must be real sensors — a template offering to wire something the
        // catalogue does not have is a wizard row that can never be satisfied.
        for (const std::string& sid : t.inputs) {
            const std::string p = "sensors.sensor[" + sid + "].enabled";
            check(meta.locate(p).valid(), t.name + ": its input '" + sid + "' is a real sensor", p);
        }
        // AN UNTICKED OPTION NAMES NOTHING. The wired/not-wired list is built from this, so an option
        // whose switch stayed in it after being declined is a warning about a decision the user made.
        if (!t.options.empty()) {
            const std::vector<std::string> none =
                templateInputs(t, TemplateChoice{ std::vector<bool>(t.options.size(), false), {} });
            bool leaked = false;
            for (const OutputTemplate::Option& o : t.options)
                for (const std::string& sid : none) if (!o.input.empty() && sid == o.input) leaked = true;
            check(!leaked, t.name + ": declining every option names none of their switches");
            const std::vector<std::string> all =
                templateInputs(t, TemplateChoice{ std::vector<bool>(t.options.size(), true), {} });
            check(all.size() == t.inputs.size() + t.options.size(),
                  t.name + ": …and ticking them all names every one",
                  std::to_string(all.size()));
        }
    }

    // APPLY MUST LEAVE THE ROW USABLE. Every generic field on the output's page is gated on its function
    // being Generic, so a wizard that writes the whole template and leaves the row None reads as a wizard
    // that did nothing: the panel stays grey. It is the one path that knows for certain the user meant
    // this pin to run as a generic output.
    if (!ts.empty()) {
        const TemplateWrites w = templateWrites(ts.front(), slot, std::vector<double>(4, 1.0),
                                                templateDefaultChoice(ts.front()));
        bool enables = false, has_kind = false;
        for (const auto& v : w.values) {
            if (v.first == slot + ".function" && v.second == 3.0) enables = true;   // Generic
            if (v.first == slot + ".kind") has_kind = true;
        }
        check(enables,  "applying a template makes its row a GENERIC output");

        // WHAT IT ACTUALLY STORES. A template's numbers are engineering — 3 seconds of prime, 100 %
        // duty — and the config is scaled, so the wizard writing the engineering number straight in
        // stored a prime of 3 raw where 3000 was meant. On the page that read back as 0.003; on the
        // ECU, where the compiled condition applies the field's scale, it was a three-millisecond
        // prime. Pinned as RAW, since that is what setConfigValue writes and what the ECU reads.
        for (const OutputTemplate& t2 : ts) {
            const TemplateWrites w2 = templateWrites(t2, slot, std::vector<double>(t2.params.size(), 3.0),
                                                     templateDefaultChoice(t2));
            for (const auto& [p2, v2] : w2.values) {
                const double sc = C.configScale(p2);
                if (sc == 1.0 || sc == 0.0) continue;               // unscaled fields say nothing here
                double eng = 0.0;
                if (p2 == slot + ".fixed_x10") eng = t2.fixedPct;
                else if (p2.size() > 8 && p2.compare(p2.size() - 8, 8, ".param_a") == 0) {
                    if (t2.params.empty()) continue;               // asks for nothing; param_a stays 0
                    eng = 3.0;
                }
                else continue;                                     // param_b..d default to 0
                check(std::abs(v2 - eng / sc) < 1e-6,
                      t2.name + ": " + p2.substr(p2.rfind('.') + 1) + " is stored RAW",
                      std::to_string(v2) + " for " + std::to_string(eng) + " at scale " +
                          std::to_string(sc));
            }
        }
        check(has_kind, "…and still writes the rest of it");
        check(!w.name.empty() && !w.onSource.empty(),
              "…and gives it a name and a condition to run on", w.name + " / " + w.onSource);
    }

    // THE TACHOMETER IS A GENERIC OUTPUT, and its signal is the CARRIER: the template computes a frequency,
    // so the slot's Frequency From must become Expression and the program must compile into freq_expr.
    {
        const auto it = std::find_if(ts.begin(), ts.end(), [](const OutputTemplate& t) { return t.id == "tachometer"; });
        check(it != ts.end(), "the firmware ships a tachometer template");
        if (it != ts.end()) {
            const TemplateWrites w = templateWrites(*it, slot, { 2.0, 60.0 }, templateDefaultChoice(*it));
            double fs = -1.0;
            for (const auto& v : w.values) if (v.first == slot + ".freq_source") fs = v.second;
            check(fs == 2.0, "…it sets Frequency From to Expression", std::to_string(fs));
            int foff = 0, fsize = 0;
            const bool blob = meta.resolveBlob(slot + ".freq_expr", foff, fsize);
            const ExprCompiler::Result r = ExprCompiler::compile(w.freqSource, meta,
                                                                 uint32_t(C.configImage().size()), uint16_t(fsize));
            check(blob && r.ok, "…and its carrier compiles: " + w.freqSource, r.ok ? "" : r.error);
        }
    }

    std::printf("\n[templates] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
