#include "OutputTemplate.h"

#include "Cache.h"
#include "MetaModel.h"

#include <cstdio>

namespace {
const char* kParamField[4] = { "param_a", "param_b", "param_c", "param_d" };
}

std::vector<OutputTemplate> outputTemplatesFromMeta(const jf::JJson& root) {
    std::vector<OutputTemplate> out;
    for (const jf::JJson& tv : root["output_templates"].arr()) {
        OutputTemplate t;
        t.id      = tv["id"].str();
        t.name    = tv["name"].str();
        t.blurb   = tv["blurb"].str();
        t.detail  = tv["detail"].str();
        t.onWhen  = tv["on_when"].str();
        t.offWhen = tv["off_when"].str();
        t.freqExpr = tv["freq_expr"].str();
        t.kind        = tv["kind"].number<int>(1);
        t.valueSource = tv["value_source"].number<int>(2);
        t.onInvalid   = tv["on_invalid"].number<int>(0);
        t.fixedPct    = tv["fixed_pct"].number<int>(100);
        t.pwmFreqHz   = tv["pwm_freq_hz"].number<int>(0);
        t.minOnMs     = tv["min_on_ms"].number<int>(0);
        t.minOffMs    = tv["min_off_ms"].number<int>(0);
        t.maxOnMs     = tv["max_on_ms"].number<int>(0);
        t.rearmMs     = tv["rearm_ms"].number<int>(0);
        t.maxOnFrom   = tv["max_on_from"].str();
        t.rearmFrom   = tv["rearm_from"].str();
        t.dutyTable   = tv["duty_table"].str();
        for (const jf::JJson& cv : tv["candidates"].arr())
            t.candidates.push_back({ cv["signal"].str(), cv["role"].number<int>(0) });
        for (const jf::JJson& pv : tv["params"].arr()) {
            OutputTemplate::Param p;
            p.name   = pv["name"].str();
            p.label  = pv["label"].str();
            p.units  = pv["units"].str();
            p.help   = pv["help"].str();
            p.def    = pv["default"].number(0.0);
            p.min    = pv["min"].number(0.0);
            p.max    = pv["max"].number(0.0);
            p.digits = pv["digits"].number<int>(0);
            t.params.push_back(std::move(p));
        }
        t.optionsLabel = tv["options_label"].str();
        t.optionsLead = tv["options_lead"].str();
        for (const jf::JJson& ov : tv["options"].arr()) {
            OutputTemplate::Option o;
            o.name  = ov["name"].str();
            o.label = ov["label"].str();
            o.help  = ov["help"].str();
            o.input = ov["input"].str();
            o.term  = ov["term"].str();
            o.join  = ov["join"].str();
            if (o.join != "and" && o.join != "or") o.join = "and";   // a template that says nothing
            o.def   = ov["default"].number<int>(1) != 0;
            t.options.push_back(std::move(o));
        }
        for (const jf::JJson& iv : tv["inputs"].arr()) t.inputs.push_back(iv.str());
        // Only the first four parameters can be stored — the slot has four fields. A template asking
        // for more is a schema mistake, and dropping the tail silently would compile a condition that
        // reads a parameter nobody can set.
        if (t.params.size() > 4) t.params.resize(4);
        out.push_back(std::move(t));
    }
    return out;
}

TemplateChoice templateDefaultChoice(const OutputTemplate& t) {
    TemplateChoice c;
    for (const OutputTemplate::Option& o : t.options) { c.on.push_back(o.def); c.join.push_back(o.join); }
    return c;
}

std::vector<TemplateChoice> templateChoiceCandidates(const OutputTemplate& t) {
    const size_t n = t.options.size() > 4 ? 4 : t.options.size();
    std::vector<TemplateChoice> out{ templateDefaultChoice(t) };
    for (uint32_t mask = 0; mask < (1u << n); ++mask) {
        // WHICH CONNECTIVES CAN BE SEEN. join[i] only reaches the program when option i is ticked and
        // something is ticked before it, so those are the only bits worth enumerating — otherwise the
        // same program is offered under several choices and recognition picks one arbitrarily.
        std::vector<size_t> live;
        bool any = false;
        for (size_t k = 0; k < n; ++k) {
            const bool on = ((mask >> k) & 1u) != 0;
            if (on && any) live.push_back(k);
            if (on) any = true;
        }
        for (uint32_t jm = 0; jm < (1u << live.size()); ++jm) {
            TemplateChoice c;
            for (size_t k = 0; k < n; ++k) {
                c.on.push_back(((mask >> k) & 1u) != 0);
                c.join.push_back(t.options[k].join);
            }
            for (size_t b = 0; b < live.size(); ++b)
                c.join[live[b]] = ((jm >> b) & 1u) ? "or" : "and";
            if (!(c == out.front())) out.push_back(std::move(c));
        }
    }
    return out;
}

std::string templateOptionGroup(const OutputTemplate& t, const TemplateChoice& chosen) {
    // LEFT TO RIGHT, BRACKETED AS IT GOES. "a or b and c" becomes "((a or b) and c)" — the reading
    // order is the grouping, which is the only rule a person can hold in their head while ticking
    // boxes. It also means the studio never has to agree with the compiler about precedence.
    std::string acc;
    for (size_t i = 0; i < t.options.size(); ++i) {
        if (!chosen.ticked(i)) continue;
        const std::string& term = t.options[i].term;
        if (acc.empty()) { acc = term; continue; }
        const std::string j = (i < chosen.join.size() && chosen.join[i] == "or") ? " or " : " and ";
        acc = "(" + acc + j + term + ")";
    }
    // NOTHING TICKED IS NOT AN EMPTY BRACKET. The lead goes with the group, so declining every option
    // leaves the condition as the part every car shares — not "… and ()", which does not compile, and
    // not "… and (0)", which is a starter that can never crank.
    if (acc.empty()) return {};
    // A SINGLE TERM NEEDS NO BRACKET; anything assembled above already carries its own.
    return t.optionsLead + acc;
}

std::vector<std::string> templateInputs(const OutputTemplate& t, const TemplateChoice& chosen) {
    std::vector<std::string> ids = t.inputs;
    for (size_t i = 0; i < t.options.size(); ++i) {
        if (!chosen.ticked(i)) continue;
        const std::string& in = t.options[i].input;
        if (in.empty()) continue;
        bool dup = false;
        for (const std::string& have : ids) if (have == in) { dup = true; break; }
        if (!dup) ids.push_back(in);
    }
    return ids;
}

std::string templateSource(const OutputTemplate& t, const std::string& src,
                           const std::string& slotPath, const TemplateChoice& chosen) {
    std::string out;
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] != '{') { out += src[i]; continue; }
        const size_t close = src.find('}', i);
        if (close == std::string::npos) { out += src[i]; continue; }
        const std::string token = src.substr(i + 1, close - i - 1);
        // THE OPTION GROUP, in the same pass as the parameters: it is a {token} like any other, and
        // resolving it anywhere else would mean two places that know how a condition is assembled.
        if (token == "@options") { out += templateOptionGroup(t, chosen); i = close; continue; }
        const int idx = t.indexOf(token);
        // An unknown token is left as it was written rather than quietly deleted: the compiler will
        // refuse it by name, which is a far better failure than a condition that silently means
        // something else.
        if (idx < 0) { out += src.substr(i, close - i + 1); }
        else         { out += "[#" + slotPath + "." + kParamField[idx] + "]"; }
        i = close;
    }
    return out;
}

TemplateWrites templateWrites(const OutputTemplate& t, const std::string& slotPath,
                              const std::vector<double>& paramValues,
                              const TemplateChoice& chosen) {
    TemplateWrites w;
    // ENGINEERING IN, RAW OUT — once, here, so no caller has to know.
    //
    // A template speaks in the units a person does: 3 seconds of prime, 100 % duty. The config stores
    // every field SCALED (param_a at 0.001, fixed_x10 at 0.1) and Cache::setConfigValue writes raw. The
    // engineering number went straight in, so a 3-second prime stored 3 where 3000 was meant: the
    // Template Numbers panel read it back as 0.003, and the compiled condition — which applies the
    // field's scale on the ECU — was a thousand times short, so the pump primed for three milliseconds.
    // Each field's OWN scale, never a constant: they differ per field, and a template cannot know them.
    auto put = [&](const std::string& field, double eng) {
        const std::string path = slotPath + "." + field;
        const double sc = Cache::instance().configScale(path);
        w.values.push_back({ path, (sc != 0.0 && sc != 1.0) ? eng / sc : eng });
    };

    // MAKE IT A GENERIC OUTPUT. A row the wizard has just described is a row the user asked for, and every
    // field on the page is gated on its function being Generic, so leaving it None made Apply look like it
    // had done nothing: the template written, the conditions compiled, and the whole panel still grey. The
    // wizard is the one path that knows the user meant it. (outputs.output[i] IS pin i — there is no pin to
    // choose; the row the wizard was opened on is the pin.)
    put("function",       3);                   // Generic
    put("kind",           t.kind);
    put("value_source",   t.valueSource);
    put("on_invalid",     t.onInvalid);
    put("fixed_x10",      t.fixedPct);          // per cent, as a person says it (put() scales)
    if (t.pwmFreqHz) put("pwm_freq_hz", t.pwmFreqHz);
    put("freq_source",    t.freqExpr.empty() ? 0 : 2);   // Fixed, or Expression
    put("min_on_ms",      t.minOnMs);
    put("min_off_ms",     t.minOffMs);
    // THE TABLE IT READS, resolved from the name the template gives to the id the field stores —
    // through the meta's own option list, which is the registry, so the template cannot be pointed at
    // a table this firmware does not have without the resolution simply failing.
    if (!t.dutyTable.empty()) {
        const MetaModel* m = Cache::instance().meta();
        const std::vector<std::string> ids =
            m ? m->enumOptionIds(slotPath + ".duty_table_sel") : std::vector<std::string>{};
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == t.dutyTable) { put("duty_table_sel", static_cast<double>(i)); break; }
    }

    // THE CHANNELS IT DELIVERS. A slot whose value comes off the bus names them here; the ids are
    // resolved through the meta's signal map, the same table the picker on the page uses, so a template
    // naming a channel this firmware does not have writes no candidate rather than a wrong one.
    // n_cand is written from what RESOLVED, not from what was asked for.
    if (!t.candidates.empty()) {
        const MetaModel* m = Cache::instance().meta();
        int n = 0;
        for (const OutputTemplate::Candidate& c : t.candidates) {
            if (!m || n >= 4) break;
            const auto it = m->signalMap().find(c.signal);
            if (it == m->signalMap().end()) continue;
            put("cand[" + std::to_string(n) + "].sig",  static_cast<double>(it->second));
            put("cand[" + std::to_string(n) + "].role", static_cast<double>(c.role));
            ++n;
        }
        put("n_cand", static_cast<double>(n));
    }

    // A PARAMETER MAY DRIVE A TIMING rather than a condition — a starter's crank limit is the gate's
    // max-on, which is where the safety actually lives, and it would be a poor wizard that asked for
    // it and then wrote it nowhere. Named in seconds; the fields are milliseconds.
    double maxOn = t.maxOnMs, rearm = t.rearmMs;
    for (size_t i = 0; i < t.params.size() && i < paramValues.size(); ++i) {
        if (!t.maxOnFrom.empty() && t.params[i].name == t.maxOnFrom) maxOn = paramValues[i] * 1000.0;
        if (!t.rearmFrom.empty() && t.params[i].name == t.rearmFrom) rearm = paramValues[i] * 1000.0;
    }
    put("max_on_ms", maxOn);
    put("rearm_ms",  rearm);

    for (size_t i = 0; i < 4; ++i)
        put(kParamField[i], i < paramValues.size() ? paramValues[i] : 0.0);

    w.name      = t.name;
    w.onSource  = templateSource(t, t.onWhen,  slotPath, chosen);
    w.offSource = templateSource(t, t.offWhen, slotPath, chosen);
    if (!t.freqExpr.empty()) w.freqSource = templateSource(t, t.freqExpr, slotPath, chosen);
    return w;
}
