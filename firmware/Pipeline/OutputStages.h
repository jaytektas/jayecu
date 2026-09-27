#pragma once
//
// Output-side pipeline stages — the bus->world mirror of the input pipeline, running on the
// SAME pipe::Pipeline engine.  See docs/polymorphic-pipeline-architecture.md:
//
//   Output: bus -> [ arbitrate -> encode -> emit ] -> world
//
// The mirror of the input side:
//   - arbitrate (bus -> one value)   mirrors  Acquire (world -> one raw), but with the one
//     genuine asymmetry: FAN-IN.  Several producers (a controller, a limiter, an override) want
//     the same actuator, so arbitrate reduces N candidate bus signals to one by policy.
//   - encode    (eng value -> command)  mirrors  Decode (raw -> eng value).
//   - emit      (-> hardware)            mirrors  Publish (-> bus).
//
// PUBLISH-INVALID MIRROR: if no arbitration candidate is valid (every source has gone dark on
// the bus / expired its TTL), the value is marked invalid and emit drives the FAIL-SAFE command
// — the output learns a source died from the same validity the input side broadcasts.
//
#include "Pipeline.h"
#include "../Engine/TableEval.h"   // a slot's value may come from its own duty map

namespace pipe {

enum ArbPolicy : uint8_t { ARB_PRIORITY = 0, ARB_MIN = 1, ARB_MAX = 2 };

struct ArbitrateCfg { uint16_t sig[4]; uint8_t n; uint8_t policy; float fallback; };
struct EncodeCfg    { float scale; float offset; float lo; float hi; };   // command = clamp(v*scale+offset)
using  OutSink      = void (*)(void* ctx, float command, bool valid);
// `observe` is where the emitted command is RECORDED, for the channel that says what this output is
// doing. Without it the only place the number exists is inside the sink, and an output nobody can
// watch cannot be tuned, tested on a bench, or believed. Null when nothing is watching.
struct EmitCfg      { OutSink sink; void* ctx; float failsafe; float* observe = nullptr; };

// --- Roled-candidate arbitration (the production model) -----------------------------------
// A candidate signal carries a ROLE; the stage composes a fixed precedence (the difference
// between "a knob" and "a knob a safety limiter can clamp + an override can veto"):
//   PRIMARY  : the control request. >1 primary -> a sub-policy combines them.
//   LIMIT    : a hard cap. The result is clamped DOWN to the lowest valid limit.
//   OVERRIDE : a veto. The first valid override REPLACES everything (rev-cut, manual, safety).
enum CandRole : uint8_t { ROLE_PRIMARY = 0, ROLE_LIMIT = 1, ROLE_OVERRIDE = 2 };
enum PrimaryPolicy : uint8_t { PRIM_FIRST = 0, PRIM_AVG = 1, PRIM_MIN = 2, PRIM_MAX = 3 };
struct RoledCandidate { uint16_t sig; uint8_t role; };
struct ArbitrateRoledCfg {
    const RoledCandidate* cand;   // array of candidates (OutputManager packs these from live config)
    uint8_t n;
    uint8_t primary_policy;       // PrimaryPolicy (used when >1 primary is valid)
    float   failsafe;             // emitted when NO candidate is valid (dead bus -> safe state)
    // THE SLOT'S GATE, owned by OutputManager and read here — nullptr means "no condition, always
    // on", which is every output that predates the feature. The stage stays pure: the latch, the
    // minimum on/off times and the starter's maximum-on live on the manager, because the expression
    // VM has no state by design and this stage runs from a const cfg.
    const uint8_t* gate = nullptr;
    // What a closed gate emits. Not the failsafe: those are different questions — failsafe is "the
    // software has stopped having an opinion", this is "the answer is no".
    float   off_value = 0.0f;
};

// Arbitrate: read up to n candidate bus signals (primary + overrides/limits) and reduce to one
// value.  PRIORITY = first valid wins (ordered candidates); MIN/MAX = the extreme of the valid
// ones (a safety limiter clamps via MIN).  No valid candidate -> ctx.valid=false + fallback.
inline void arbitrate(Ctx& c, const void* cfg, State&) {
    const auto* a = static_cast<const ArbitrateCfg*>(cfg);
    bool have = false; float acc = 0.0f;
    for (uint8_t k = 0; k < a->n && k < 4; k++) {
        const SignalId s = static_cast<SignalId>(a->sig[k]);
        if (!c.bus || !c.bus->valid(s)) continue;
        const float v = c.bus->get(s);
        if (!have) { acc = v; have = true; if (a->policy == ARB_PRIORITY) break; }
        else if (a->policy == ARB_MIN) { if (v < acc) acc = v; }
        else if (a->policy == ARB_MAX) { if (v > acc) acc = v; }
    }
    c.value = have ? acc : a->fallback;
    c.valid = have;
}

// --- Where the VALUE comes from, when it is not the candidates ------------------------------
// A slot's value used to be arbitrated bus signals and nothing else, which says nothing about the
// loads a car actually modulates: a variable-speed fan, a PWM fuel pump, a speed-sensitive steering
// pump. Those are a MAP — duty against one or two channels — or a constant. Both are stages here
// rather than a branch inside arbitration, so the pipeline stays a list of pure steps and the gate
// (which decides WHEN) is untouched by what decides HOW MUCH.
struct SourceCfg {
    const tbl::TableDesc* table = nullptr;   // Table source: interpolated against its own axes
    float                 fixed = 0.0f;      // Fixed source: a constant
    const uint8_t*        gate  = nullptr;   // same gate the arbitration stage honours
    float                 off_value = 0.0f;
    // EXPRESSION source: the slot's duty program, and the number it last returned. The VM is not run
    // in a pipeline stage — a stage has no Ctx to give it and no business owning one — so the manager
    // evaluates it alongside the conditions and leaves the answer here. `value_ok` false means the
    // program could not be answered, and the failsafe stands in, exactly as a dead candidate does.
    const float*          computed  = nullptr;
    const bool*           value_ok  = nullptr;
    float                 failsafe  = 0.0f;
};

inline void value_from_table(Ctx& c, const void* cfg, State&) {
    const auto* s = static_cast<const SourceCfg*>(cfg);
    if (s->gate && !*s->gate) { c.value = s->off_value; c.valid = true; return; }
    c.value = (s->table && c.bus) ? tbl::table_eval(*s->table, *c.bus) : s->off_value;
    c.valid = true;
}

inline void value_fixed(Ctx& c, const void* cfg, State&) {
    const auto* s = static_cast<const SourceCfg*>(cfg);
    c.value = (s->gate && !*s->gate) ? s->off_value : s->fixed;
    c.valid = true;
}

// The value the slot's duty EXPRESSION computed this frame (see SourceCfg::computed).
inline void value_from_expr(Ctx& c, const void* cfg, State&) {
    const auto* s = static_cast<const SourceCfg*>(cfg);
    if (s->gate && !*s->gate)                   { c.value = s->off_value; c.valid = true; return; }
    const bool ok = s->computed && s->value_ok && *s->value_ok;
    c.value = ok ? *s->computed : s->failsafe;
    c.valid = true;                              // like arbitration: always emit something defined
}

// Roled arbitration: PRIMARY (sub-policy) -> LIMIT (clamp down) -> OVERRIDE (veto).  ALWAYS
// leaves ctx.valid=true: if NO candidate is valid the failsafe is substituted and passes
// downstream, so a dead bus / crashed module forces the actuator to a defined safe state.
inline void arbitrate_roled(Ctx& c, const void* cfg, State&) {
    const auto* a = static_cast<const ArbitrateRoledCfg*>(cfg);
    // A CLOSED GATE SHORT-CIRCUITS. No candidate is read, so a slot that is off costs nothing and —
    // more to the point — cannot be pushed on by a producer: "when" is decided before "how much".
    if (a->gate && !*a->gate) { c.value = a->off_value; c.valid = true; return; }
    bool  any = false;
    float value = a->failsafe;

    // 1. PRIMARY baseline — combine the valid primaries by sub-policy.
    bool have_p = false; float acc = 0.0f; uint8_t np = 0;
    for (uint8_t k = 0; k < a->n; k++) {
        if (a->cand[k].role != ROLE_PRIMARY) continue;
        const SignalId s = static_cast<SignalId>(a->cand[k].sig);
        if (!c.bus || !c.bus->valid(s)) continue;
        const float v = c.bus->get(s); any = true;
        if (!have_p) { acc = v; have_p = true; np = 1; if (a->primary_policy == PRIM_FIRST) break; }
        else if (a->primary_policy == PRIM_MIN) { if (v < acc) acc = v; }
        else if (a->primary_policy == PRIM_MAX) { if (v > acc) acc = v; }
        else                                    { acc += v; np++; }      // PRIM_AVG
    }
    if (have_p) value = (a->primary_policy == PRIM_AVG) ? acc / static_cast<float>(np) : acc;

    // 2. LIMIT — clamp the result down to the lowest valid limit.
    for (uint8_t k = 0; k < a->n; k++) {
        if (a->cand[k].role != ROLE_LIMIT) continue;
        const SignalId s = static_cast<SignalId>(a->cand[k].sig);
        if (!c.bus || !c.bus->valid(s)) continue;
        const float lim = c.bus->get(s); any = true;
        if (lim < value) value = lim;
    }

    // 3. OVERRIDE — first valid override vetoes everything.
    for (uint8_t k = 0; k < a->n; k++) {
        if (a->cand[k].role != ROLE_OVERRIDE) continue;
        const SignalId s = static_cast<SignalId>(a->cand[k].sig);
        if (!c.bus || !c.bus->valid(s)) continue;
        value = c.bus->get(s); any = true; break;
    }

    c.value = any ? value : a->failsafe;   // nothing valid -> safe state
    c.valid = true;                        // always emit (failsafe is a valid command)
}

// Encode: engineering value -> actuator command, clamped to the actuator's range.  The mirror
// of decode_linear (a curve-encode could join it later, as decode_curve did on the input side).
inline void encode_linear(Ctx& c, const void* cfg, State&) {
    const auto* e = static_cast<const EncodeCfg*>(cfg);
    float v = c.value * e->scale + e->offset;
    if (v < e->lo) v = e->lo; else if (v > e->hi) v = e->hi;
    c.value = v;
}

// Emit: drive the actuator through a sink (a SoftPwm / digital-pin driver in firmware; a capture
// in tests).  On an invalid arbitration result, emit the fail-safe command.
inline void emit_sink(Ctx& c, const void* cfg, State&) {
    const auto* e = static_cast<const EmitCfg*>(cfg);
    const float out = c.valid ? c.value : e->failsafe;
    if (e->observe) *e->observe = out;       // what was COMMANDED, fail-safe included
    if (e->sink) e->sink(e->ctx, out, c.valid);
}

} // namespace pipe
