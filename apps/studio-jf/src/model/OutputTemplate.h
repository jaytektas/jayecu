#pragma once
//
// OutputTemplate — what an output slot can BE, read from the ECU's own meta.
//
// The slots are deliberately generic; the firmware has never heard of a fuel pump. That is what makes
// four pumps and eight fans twelve slots rather than a limit somebody chose, and it is also, on its
// own, a blank expression editor. So the conventions ship as DATA in the schema
// (definition/ecu.schema.yaml `output_templates`) and arrive in the meta: the templates a studio offers
// are the ones the connected firmware can actually run, not whatever this build was compiled with.
//
// A template writes the two conditions, the timings, the fail direction and the kind, and asks only for
// the numbers that vary. Those numbers land in the slot's OWN param_a..d fields and the conditions
// REFERENCE them — "{prime_s}" becomes "[#outputs.output[7].param_a]" — for a reason worth stating: a
// fuel pump then compiles to identical bytecode whatever its prime time, so the studio still recognises
// the template after the numbers are changed, and after a tune has been through an ECU where only
// bytecode comes back.
//
#include <j/config/Json.h>   // jf::JJson — the templates ride in the meta

#include <string>
#include <vector>

struct OutputTemplate {
    struct Param {
        std::string name;      // the {token} the conditions substitute
        std::string label;     // what the wizard calls it
        std::string units;
        std::string help;
        double      def = 0.0, min = 0.0, max = 0.0;
        int         digits = 0;
    };

    // AN OPTIONAL CLAUSE — a term some cars have and others do not.
    //
    // A neutral switch is not a number, so it cannot be a parameter; and hard-wired into the condition
    // it makes the template useless to anybody without that switch, because a condition naming an
    // unwired channel reads it as FALSE and so answers no for ever. So it is a tick box: the ticked
    // ones are joined and substituted for the "{@options}" token, and ticking none removes the term
    // altogether rather than leaving a clause nothing can satisfy.
    struct Option {
        std::string name;      // stable id, for nothing but readability in the schema
        std::string label;     // what the wizard's tick box says
        std::string help;
        std::string input;     // the sensor this clause reads — an `inputs` entry while it is ticked
        std::string term;      // the condition fragment it contributes
        std::string join;      // "and" / "or": how it attaches to the ticked options BEFORE it
        bool        def = true;
    };

    // A value taken off the BUS: a module publishes a duty and this slot delivers it. Named rather
    // than numbered — the wizard resolves the name to a signal id through the meta, so a template
    // cannot point a slot at a channel this firmware does not have.
    struct Candidate {
        std::string signal;    // bus channel id, e.g. "wastegate_duty"
        int         role = 0;  // 0 Primary (requests), 1 Limit (clamps down), 2 Override (replaces)
    };

    std::string id, name, blurb, detail;
    std::string onWhen, offWhen;        // source, with {param} and {@options} tokens
    // THE CARRIER, COMPUTED — a template whose signal IS a frequency (a tachometer: rpm * ppr / 60).
    // Empty leaves the slot on its fixed frequency; set, it is compiled into freq_expr and the slot's
    // Frequency From becomes Expression.
    std::string freqExpr;
    std::vector<Param>       params;
    std::vector<Option>      options;
    std::vector<Candidate>   candidates;
    std::vector<std::string> inputs;    // sensor ids the conditions ALWAYS read, offered for wiring
    // What goes in front of the assembled group. Empty is legal — a template whose token already sits
    // inside a bracket wants none.
    std::string optionsLead;
    std::string optionsLabel;           // the heading the tick boxes sit under

    // The fixed part of the personality — everything the template decides for you.
    int  kind = 1, valueSource = 2, onInvalid = 0;
    int  fixedPct = 100, pwmFreqHz = 0;
    int  minOnMs = 0, minOffMs = 0, maxOnMs = 0, rearmMs = 0;
    // A parameter may drive a timing instead of appearing in a condition (a starter's crank limit is
    // the gate's max-on, not part of any expression). These name which one, in seconds.
    std::string maxOnFrom, rearmFrom;
    // WHICH TABLE a table-driven template leaves the slot reading, by registry NAME
    // ("generic_tables_table_1"). A slot naming none falls to its failsafe, so a template that says
    // "your duty comes from a table" and points at nothing configures a fan that never runs.
    std::string dutyTable;

    // The param index (0..3 -> param_a..d) for a token, or -1.
    int indexOf(const std::string& token) const {
        for (size_t i = 0; i < params.size(); ++i)
            if (params[i].name == token) return static_cast<int>(i);
        return -1;
    }

};

// WHICH OPTIONS, AND HOW THEY COMBINE — the whole of what the user chose, in one value.
//
// The connectives are part of the choice, not of the template: "in neutral or clutch down" and
// "in neutral and clutch down" are both reasonable cars. `join[i]` attaches option i to everything
// ticked before it, so join[first ticked] is meaningless by construction.
struct TemplateChoice {
    std::vector<bool>        on;
    std::vector<std::string> join;      // "and" / "or", per option

    bool operator==(const TemplateChoice& o) const { return on == o.on && join == o.join; }
    bool ticked(size_t i) const { return i < on.size() && on[i]; }
};

// What the wizard offers before anybody touches it.
TemplateChoice templateDefaultChoice(const OutputTemplate& t);

// Every choice that could have produced a stored program, DEFAULT FIRST — what recognition searches.
// Only the connectives of TICKED options vary, so a choice is not counted twice for a connective
// nobody can see: with three options that is 14 candidates rather than 8 x 4.
std::vector<TemplateChoice> templateChoiceCandidates(const OutputTemplate& t);

// The token an assembled option group replaces. Inside braces so it shares the parameter
// substitution pass, with a sigil so it can never collide with a parameter name.
inline constexpr const char* kOptionsToken = "{@options}";

// The ticked terms as the text they substitute to: assembled LEFT TO RIGHT with explicit brackets, so
// the listed order is the meaning and no reader has to know the language's precedence. Empty when
// nothing is ticked, which is what makes the whole clause disappear rather than become "and ()".
std::string templateOptionGroup(const OutputTemplate& t, const TemplateChoice& chosen);

// The sensors a template's conditions read GIVEN a choice: its always-on `inputs` plus the input of
// every ticked option. An option left unticked contributes no clause, so its switch is not something
// the user needs to be warned about.
std::vector<std::string> templateInputs(const OutputTemplate& t, const TemplateChoice& chosen);

// The shipped list, in schema order. Empty if the meta carries none (an older firmware).
std::vector<OutputTemplate> outputTemplatesFromMeta(const jf::JJson& root);

// One template's condition SOURCE for a given slot: every "{token}" replaced by the config path of the
// slot's own parameter. Compiled by the caller (ExprCompiler) — this only ever produces text, so the
// same string is what the expression editor shows when somebody opens it afterwards.
std::string templateSource(const OutputTemplate& t, const std::string& src,
                           const std::string& slotPath, const TemplateChoice& chosen);

// Everything applying a template writes, as (path, value) pairs plus the two compiled conditions.
// Kept as data so the wizard can SHOW it before it writes it: a template that hides what it did is a
// template you cannot take over from.
struct TemplateWrites {
    std::vector<std::pair<std::string, double>> values;   // scalars, in engineering units
    std::string name;                                     // the slot's name field
    std::string onSource, offSource;                      // ready to compile
    std::string freqSource;                               // …and the carrier, when the template computes one
};
TemplateWrites templateWrites(const OutputTemplate& t, const std::string& slotPath,
                              const std::vector<double>& paramValues,
                              const TemplateChoice& chosen);
