#pragma once
//
// Expr — the pure expression VM.
//
// A stack machine that answers ONE question about NOW: given the signal bus and
// the config image, what is this expression worth? It is the successor to the
// four inline `cond::` precondition slots, which could only express a flat
// sum-of-products with no precedence control.
//
// Division of labour (see docs/expression-evaluator-design.md):
//   client (studio)  parses source text, resolves names, bakes config offsets
//                    and bit ranges from the meta, emits bytecode, validates it
//   firmware (here)  validates the bytecode structurally, then executes it
// The firmware carries NO descriptor table and NO names: every config access is
// an offset the compiler baked from the meta for this exact layout_hash.
//
// PURE, by design and permanently:
//   * no writes — the VM returns a value, the caller decides what to do with it
//   * no state — no timers, no latches, no memory of the previous evaluation.
//     "Held true for 5 s" belongs to the calling site (like `diag_delay_ms`),
//     not to this grammar; a stateful operator would need a guaranteed
//     evaluation cadence and defined reset points, which is a separate design.
//   * no clock read — `now_ms` is passed in, so AGE is still a pure function of
//     its inputs and a test can drive time by hand.
//   * bounded — no loops, no calls, no recursion. Worst-case execution time is
//     a constant you can state: MAX_LEN dispatches.
//
// Both a boolean site and a future numeric site are served without change: the
// boolean site takes `truthy()` (result != 0), a numeric site takes `value`.
//
#include "ExprIsa.h"                    // the instruction set, shared with the studio compiler
#include "SignalBus.h"
#include "../../generated/signal_ids.h"
#include <cstdint>
#include <cstring>

namespace expr {

// ---------------------------------------------------------------------------
// Execution context
// ---------------------------------------------------------------------------
// A calibration curve lookup, supplied by the caller so the kernel stays free of
// table-engine dependencies. Return false if the curve cannot be evaluated; the
// result is then invalid (see "validity" below). Null is legal when no program
// uses OP_INTERP — validate() rejects programs that reference curves the site
// did not offer, so a null hook here can never be reached by a validated
// program with curve_count == 0.
using CurveFn = bool (*)(uint16_t id, float x, float& out, void* user);
// The same registry read at the table's OWN axes — no coordinate from the program, because the table
// already knows where its coordinates come from. Null is legal when no program uses OP_TABLE.
using TableFn = bool (*)(uint16_t id, float& out, void* user);

struct Ctx {
    const SignalBus* bus      = nullptr;
    const uint8_t*   cfg      = nullptr;   // base of the config image
    uint32_t         cfg_size = 0;         // sizeof(EcuConfig)
    uint32_t         now_ms   = 0;         // passed in, never read from a clock
    CurveFn          curve    = nullptr;
    TableFn          table    = nullptr;
    void*            curve_user = nullptr;
    uint16_t         curve_count = 0;      // ids 0..curve_count-1 are valid (ONE registry, both ops)
};

// ---------------------------------------------------------------------------
// Validity ("fail-to-false"), and why it is per-operand rather than whole-program
// ---------------------------------------------------------------------------
// A None, out-of-range or stale signal cannot be allowed to satisfy a gate — a
// dead sensor must not arm a fault, and its own validity DTC carries the
// fail-safe instead. That was cond::eval's rule and it is kept.
//
// It is tracked PER STACK SLOT, not as a single "program failed" flag, because
// whole-program strictness would break the useful case: with MAP dead,
// `map > 50 OR tps > 80` should still arm on throttle alone, exactly as it does
// today. So an untrustworthy value is marked and travels:
//
//   arithmetic     propagates      (a sum involving junk is junk)
//   comparison     propagates      (an untrustworthy operand yields an
//                                   untrustworthy FALSE, never a true)
//   AND / OR       coerce to false and produce a CLEAN result, so OR can rescue
//                  a live clause and AND still fails to false
//   NOT            propagates      — deliberately asymmetric with AND/OR. If NOT
//                  coerced, `NOT(dead > 5)` would evaluate TRUE, which is the
//                  one trap that catches the person writing a safety gate.
//
// A boolean site treats an untrustworthy result as false; a numeric site treats
// it as "no answer" and falls back.
struct Value {
    float value = 0.0f;
    bool  ok    = true;    // false = untrustworthy, treat as false
};

struct Result {
    Value    v;
    bool     ran = false;  // false = the program was rejected before execution
    // Convenience for a boolean site: untrustworthy or non-zero-less => false.
    bool truthy() const { return ran && v.ok && v.value != 0.0f; }
};

// Defined at the bottom under "internals"; declared here because exec() uses them.
inline bool read_signal_(const Ctx& ctx, uint16_t sel, float& out);
inline bool read_cfg_(const Ctx& ctx, uint32_t off, uint8_t type, float& out);
inline bool read_cfg_word_(const Ctx& ctx, uint32_t off, uint8_t type, uint32_t& out);

// validate() lives in ExprIsa.h (shared with the studio). This is the firmware's spelling of it:
// the signal catalog is a compile-time constant here, so callers should not have to pass it.
inline Invalid validate(const uint8_t* p, uint16_t len, uint32_t cfg_size,
                        uint16_t curve_count = 0) {
    return validate(p, len, cfg_size, curve_count, static_cast<uint16_t>(SIG_COUNT));
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
// Assumes validate() has already passed for this (program, cfg_size, curves).
// It still bounds-checks the stack — the cost is trivial beside a wrong answer
// on a gate, and it keeps a hand-built program in a test from corrupting the
// frame.
inline Result exec(const uint8_t* p, uint16_t len, const Ctx& ctx) {
    Result r;
    Value st[STACK_MAX];
    uint8_t sp = 0;

    const auto push = [&](float v, bool ok) -> bool {
        if (sp >= STACK_MAX) return false;
        st[sp].value = v; st[sp].ok = ok; sp++;
        return true;
    };

    uint16_t i = 0;
    while (i < len) {
        const uint8_t op = p[i++];
        // Operand bytes must lie inside the program. validate() proves this for
        // any program it passed, but exec must not DEPEND on having been
        // validated: bytes can arrive from an SD card or a partial write, and a
        // truncated instruction here would read past the block. One comparison
        // per operand fetch, against a class of memory bug in firmware that
        // runs on a car — an easy trade.
        const uint16_t need = operand_bytes_(op);
        if ((uint16_t)(i + need) > len) return r;
        switch (op) {
            case OP_END:
                if (sp != 1) return r;             // ran stays false
                r.v = st[0];
                r.ran = true;
                return r;

            case OP_PUSH_SIG: {
                uint16_t sel; memcpy(&sel, p + i, 2); i += 2;
                float v = 0.0f;
                const bool ok = read_signal_(ctx, sel, v);
                if (!push(v, ok)) return r;
                break;
            }

            case OP_AGE: {
                uint16_t sel; memcpy(&sel, p + i, 2); i += 2;
                if (sel == 0 || sel > SIG_COUNT || !ctx.bus) { if (!push(0, false)) return r; break; }
                const SignalId s = static_cast<SignalId>(sel - 1);
                // Age is meaningful precisely WHEN a channel is stale — that is
                // the point of asking — so this does not require validity. A
                // channel never written reads as maximally stale (now_ms - 0),
                // which is the honest answer, so there is no special case.
                if (!push(static_cast<float>(ctx.bus->age_ms(s, ctx.now_ms)), true)) return r;
                break;
            }

            case OP_PUSH_CFG: {
                const uint32_t off = (uint32_t)p[i] | ((uint32_t)p[i+1] << 8) |
                                     ((uint32_t)p[i+2] << 16);
                const uint8_t type = p[i+3];
                i += 4;
                float v = 0.0f;
                const bool ok = read_cfg_(ctx, off, type, v);
                if (!push(v, ok)) return r;
                break;
            }

            case OP_PUSH_BCFG: {
                const uint32_t off = (uint32_t)p[i] | ((uint32_t)p[i+1] << 8) |
                                     ((uint32_t)p[i+2] << 16);
                const uint8_t type = p[i+3];
                const uint8_t bits = p[i+4];
                i += 5;
                uint32_t word = 0;
                if (!read_cfg_word_(ctx, off, type, word)) { if (!push(0, false)) return r; break; }
                const uint8_t lo    = bits & 0x1F;
                const uint8_t width = (uint8_t)((bits >> 5) & 0x07) + 1;
                const uint32_t mask = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
                if (!push(static_cast<float>((word >> lo) & mask), true)) return r;
                break;
            }

            case OP_PUSH_F: {
                int32_t c; memcpy(&c, p + i, 4); i += 4;
                if (!push(static_cast<float>(c) * 0.01f, true)) return r;
                break;
            }
            // The literal the hundredths form cannot hold — a field scale of 0.001, most often.
            // memcpy for the same reason every other read here uses it: the block is byte-aligned.
            case OP_PUSH_F32: {
                float c; memcpy(&c, p + i, 4); i += 4;
                if (!push(c, true)) return r;
                break;
            }
            case OP_PUSH_I16: {
                int16_t c; memcpy(&c, p + i, 2); i += 2;
                if (!push(static_cast<float>(c), true)) return r;
                break;
            }
            case OP_PUSH_I8: {
                const int8_t c = static_cast<int8_t>(p[i]); i += 1;
                if (!push(static_cast<float>(c), true)) return r;
                break;
            }
            case OP_PUSH_ZERO: if (!push(0.0f, true)) return r; break;
            case OP_PUSH_ONE:  if (!push(1.0f, true)) return r; break;

            case OP_INTERP: {
                uint16_t id; memcpy(&id, p + i, 2); i += 2;
                if (sp < 1) return r;
                Value& x = st[sp - 1];
                float out = 0.0f;
                const bool ok = x.ok && ctx.curve &&
                                ctx.curve(id, x.value, out, ctx.curve_user);
                x.value = ok ? out : 0.0f;
                x.ok    = ok;
                break;
            }

            case OP_TABLE: {
                uint16_t id; memcpy(&id, p + i, 2); i += 2;
                float out = 0.0f;
                const bool ok = ctx.table && ctx.table(id, out, ctx.curve_user);
                // A table that cannot be read pushes an INVALID zero rather than a plausible one: the
                // per-operand validity is the whole reason a dead axis cannot quietly become a gate's
                // reason to fire.
                if (!push(ok ? out : 0.0f, ok)) return r;
                break;
            }

            case OP_NOT: {
                if (sp < 1) return r;
                Value& a = st[sp - 1];
                // Propagates: an untrustworthy operand must not become TRUE.
                a.value = (a.ok && a.value == 0.0f) ? 1.0f : 0.0f;
                break;
            }
            case OP_ABS: {
                if (sp < 1) return r;
                Value& a = st[sp - 1];
                a.value = a.value < 0.0f ? -a.value : a.value;
                break;
            }

            case OP_CLAMP: case OP_SELECT: {
                if (sp < 3) return r;
                const Value c = st[sp - 3], a = st[sp - 2], b = st[sp - 1];
                sp -= 2;
                Value& out = st[sp - 1];
                if (op == OP_CLAMP) {
                    float v = c.value;
                    if (v < a.value) v = a.value;
                    if (v > b.value) v = b.value;
                    out.value = v;
                    out.ok = c.ok && a.ok && b.ok;
                } else {
                    // SELECT coerces its CONDITION like AND/OR do (untrustworthy
                    // condition takes the false branch) but carries the chosen
                    // branch's trustworthiness through.
                    const bool take_a = c.ok && c.value != 0.0f;
                    out = take_a ? a : b;
                }
                break;
            }

            default: {
                // All remaining opcodes are binary.
                if (sp < 2) return r;
                const Value b = st[sp - 1];
                sp--;
                Value& a = st[sp - 1];
                const bool both = a.ok && b.ok;
                switch (op) {
                    case OP_GT: a.value = (a.value >  b.value) ? 1.f : 0.f; a.ok = both; break;
                    case OP_GE: a.value = (a.value >= b.value) ? 1.f : 0.f; a.ok = both; break;
                    case OP_LT: a.value = (a.value <  b.value) ? 1.f : 0.f; a.ok = both; break;
                    case OP_LE: a.value = (a.value <= b.value) ? 1.f : 0.f; a.ok = both; break;
                    case OP_EQ: {
                        const float d = a.value - b.value;
                        a.value = (d < EQ_EPS && -d < EQ_EPS) ? 1.f : 0.f; a.ok = both; break;
                    }
                    case OP_NE: {
                        const float d = a.value - b.value;
                        a.value = (d >= EQ_EPS || -d >= EQ_EPS) ? 1.f : 0.f; a.ok = both; break;
                    }
                    // AND/OR coerce and CLEAN: an untrustworthy clause reads as
                    // false, so OR can still be rescued by a live one.
                    case OP_AND: {
                        const bool av = a.ok && a.value != 0.0f;
                        const bool bv = b.ok && b.value != 0.0f;
                        a.value = (av && bv) ? 1.f : 0.f; a.ok = true; break;
                    }
                    case OP_OR: {
                        const bool av = a.ok && a.value != 0.0f;
                        const bool bv = b.ok && b.value != 0.0f;
                        a.value = (av || bv) ? 1.f : 0.f; a.ok = true; break;
                    }
                    case OP_ADD: a.value += b.value; a.ok = both; break;
                    case OP_SUB: a.value -= b.value; a.ok = both; break;
                    case OP_MUL: a.value *= b.value; a.ok = both; break;
                    case OP_DIV:
                        // Divide by zero is untrustworthy, not a trap value: the
                        // gate fails to false rather than propagating an inf.
                        if (b.value == 0.0f) { a.value = 0.f; a.ok = false; }
                        else { a.value /= b.value; a.ok = both; }
                        break;
                    case OP_MIN: a.value = (a.value < b.value) ? a.value : b.value; a.ok = both; break;
                    case OP_MAX: a.value = (a.value > b.value) ? a.value : b.value; a.ok = both; break;
                    case OP_BIT: {
                        const uint32_t word = (uint32_t)(int64_t)a.value;
                        const int32_t  n    = (int32_t)b.value;
                        if (n < 0 || n > 31) { a.value = 0.f; a.ok = false; }
                        else { a.value = ((word >> n) & 1u) ? 1.f : 0.f; a.ok = both; }
                        break;
                    }
                    default:
                        return r;    // unknown opcode: validated programs cannot reach this
                }
                break;
            }
        }
    }
    return r;   // ran off the end without END — ran stays false
}

// Convenience for a boolean site (a DTC gate, an arm condition, an output rule).
// `on_invalid` is what a program that could not run is worth: always TRUE for a
// gate, because a broken expression must not silently switch detection off.
inline bool eval_bool(const uint8_t* p, uint16_t len, const Ctx& ctx, bool on_invalid) {
    if (is_empty(p, len)) return true;          // no expression = always armed
    const Result r = exec(p, len, ctx);
    return r.ran ? r.truthy() : on_invalid;
}

// ---------------------------------------------------------------------------
// internals
// ---------------------------------------------------------------------------

// Read a bus channel as a float honouring its declared type. False (fail-to-
// false) when None, out of range, or not currently valid.
inline bool read_signal_(const Ctx& ctx, uint16_t sel, float& out) {
    if (sel == 0 || !ctx.bus) return false;                  // 0 = None
    const SignalId s = static_cast<SignalId>(sel - 1);
    if (s >= SIG_COUNT || !ctx.bus->valid(s)) return false;
    switch (SIGNAL_TYPES[s]) {
        case SIG_T_U32: out = static_cast<float>(ctx.bus->get_u32(s)); break;
        case SIG_T_I32: out = static_cast<float>(ctx.bus->get_i32(s)); break;
        default:        out = ctx.bus->get(s);                         break;
    }
    return true;
}

// Read a config field. ALWAYS via memcpy: the config image is #pragma pack(1),
// so a correct offset is routinely unaligned and a direct load would fault the
// M7 (and silently mis-read elsewhere).
inline bool read_cfg_(const Ctx& ctx, uint32_t off, uint8_t type, float& out) {
    const uint8_t sz = cfg_type_size(type);
    if (!ctx.cfg || sz == 0 || off + sz > ctx.cfg_size) return false;
    const uint8_t* q = ctx.cfg + off;
    switch (type) {
        case CT_U8:  { uint8_t  v; memcpy(&v, q, 1); out = (float)v; break; }
        case CT_S8:  { int8_t   v; memcpy(&v, q, 1); out = (float)v; break; }
        case CT_U16: { uint16_t v; memcpy(&v, q, 2); out = (float)v; break; }
        case CT_S16: { int16_t  v; memcpy(&v, q, 2); out = (float)v; break; }
        case CT_U32: { uint32_t v; memcpy(&v, q, 4); out = (float)v; break; }
        case CT_S32: { int32_t  v; memcpy(&v, q, 4); out = (float)v; break; }
        case CT_F32: { float    v; memcpy(&v, q, 4); out = v;        break; }
        default: return false;
    }
    return true;
}

// Read a config field as a RAW BIT PATTERN, for bit-group extraction.
//
// Deliberately separate from read_cfg_ rather than casting its float result: a
// u32/s32 field larger than 2^24 does not survive a round trip through float,
// so extracting bits from the float would silently return the wrong group. A
// bit group is a small unsigned field whatever its storage type's signedness,
// so this reads the pattern and never sign-extends.
inline bool read_cfg_word_(const Ctx& ctx, uint32_t off, uint8_t type, uint32_t& out) {
    const uint8_t sz = cfg_type_size(type);
    if (!ctx.cfg || sz == 0 || type == CT_F32 || off + sz > ctx.cfg_size) return false;
    uint32_t v = 0;
    memcpy(&v, ctx.cfg + off, sz);   // little-endian target; zero-extends by construction
    out = v;
    return true;
}

} // namespace expr
