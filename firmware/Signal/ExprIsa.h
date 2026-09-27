#pragma once
//
// ExprIsa — the expression VM's INSTRUCTION SET and structural validator.
//
// Split out of Expr.h so the STUDIO compiles against the same bytes the firmware
// executes. The studio needs the opcode table, the operand widths and the
// validator; it must not need the SignalBus, the generated signal catalog, or
// anything else that only exists on the MCU. Two copies of an opcode table is a
// silent-wrong-answer bug waiting to happen — the numbers are the file format.
//
// Include this to SPEAK the format (compiler, decompiler, validator).
// Include Expr.h to RUN it (the firmware's exec()).
//
#include <cstdint>
#include <cstring>

namespace expr {

// ---------------------------------------------------------------------------
// Instruction set
// ---------------------------------------------------------------------------
// One-byte opcode followed by 0..5 operand bytes, little-endian, byte-aligned.
// Opcode VALUES are part of the image format: append, never renumber, or every
// stored program silently means something else.
enum Op : uint8_t {
    OP_END        = 0,   // -                       terminate; result is the stack top

    // pushes
    OP_PUSH_SIG   = 1,   // u16 sel                 bus channel (0 = None, else SignalId+1)
    OP_PUSH_CFG   = 2,   // u24 off, u8 type        whole config field
    OP_PUSH_BCFG  = 3,   // u24 off, u8 type, u8 b  packed config field: b = lo(5) | width-1(3)
    OP_PUSH_F     = 4,   // i32                     literal x100 (0.01 resolution)
    OP_PUSH_I8    = 5,   // i8                      small integer literal, unscaled
    OP_PUSH_I16   = 6,   // i16                     integer literal, unscaled
    OP_PUSH_ZERO  = 7,   // -                       0
    OP_PUSH_ONE   = 8,   // -                       1

    // comparison   a b -> bool
    OP_GT = 9, OP_GE = 10, OP_LT = 11, OP_LE = 12, OP_EQ = 13, OP_NE = 14,

    // logical
    OP_AND = 15,         // a b -> bool
    OP_OR  = 16,         // a b -> bool
    OP_NOT = 17,         // a   -> bool

    // arithmetic   a b -> value
    OP_ADD = 18, OP_SUB = 19, OP_MUL = 20, OP_DIV = 21,

    // helpers — every one a pure function of now, which is why they are cheap
    OP_MIN    = 22,      // a b     -> value
    OP_MAX    = 23,      // a b     -> value
    OP_ABS    = 24,      // a       -> value
    OP_CLAMP  = 25,      // x lo hi -> value
    OP_SELECT = 26,      // c a b   -> value   (c ? a : b)
    OP_BIT    = 27,      // a n     -> bool    (bit n of a; status words compare as words)
    OP_INTERP = 28,      // u16 id: x -> value (a table in the tune, read at the x on the stack)
    OP_AGE    = 29,      // u16 sel: -> value  (ms since the channel was last written)
    // A TABLE, READ THE WAY THE FIRMWARE READS IT. No stack operand, because a table already knows
    // where its coordinates come from: its axes name bus channels. OP_INTERP is the same table with X
    // supplied instead — one registry, two questions ("what does it say now" vs "what does it say at
    // this value"), which is exactly the difference between a table and a curve in this firmware.
    OP_TABLE  = 30,      // u16 id: -> value   (table at its own configured axes)

    // A LITERAL THE x100 ONE CANNOT HOLD. OP_PUSH_F stores an i32 of hundredths, so anything finer
    // than 0.01 rounds — and a value under 0.005 rounds to NOTHING. That is not a rounding error, it
    // is a different program: the compiler emits the raw->engineering conversion after every scaled
    // setting as `* <its scale>`, and every 0.001-scaled field in this ECU (the output slots'
    // param_a..d, the PID gains, several tables) therefore compiled to `* 0`. A fuel pump's
    // "uptime_s < prime_s" became "uptime_s < 0" — false for ever, so the pump never ran and nothing
    // anywhere reported a fault, because multiplying by zero is a perfectly valid program.
    //
    // So: a real float32. Appended rather than replacing OP_PUSH_F, because the compact form is
    // exact for the hundredths that most literals are and one byte cheaper per operand — and because
    // the image format is append-only. The compiler picks the narrow one when it is EXACT and this
    // one otherwise, so no existing program changes a byte.
    OP_PUSH_F32 = 31,    // f32                 IEEE-754 literal, exactly as written

    OP__COUNT
};

// Config field types for PUSH_CFG / PUSH_BCFG. Values are part of the image
// format — append only.
enum CfgType : uint8_t {
    CT_U8 = 0, CT_S8 = 1, CT_U16 = 2, CT_S16 = 3,
    CT_U32 = 4, CT_S32 = 5, CT_F32 = 6,
    CT__COUNT
};

inline uint8_t cfg_type_size(uint8_t t) {
    switch (t) {
        case CT_U8: case CT_S8:  return 1;
        case CT_U16: case CT_S16: return 2;
        case CT_U32: case CT_S32: case CT_F32: return 4;
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
// Stack depth 16 (64 bytes of frame) — 8 covers anything hand-written, and the
// extra 8 cost nothing worth counting. Enforced identically by the compiler and
// by validate() so a program that runs on the bench cannot fail in the field.
static constexpr uint8_t  STACK_MAX = 16;
// Per-site program block. 64 bytes covers three clauses with arithmetic and a
// packed config read with room to spare (design doc, "Sizing the program block").
static constexpr uint16_t PROGRAM_MAX = 64;

// Threshold quantum: literals are stored x100, so half a stored count is the
// natural epsilon for EQ/NE on a live float. Same value cond::eval used.
static constexpr float EQ_EPS = 0.005f;

// How many operand bytes follow an opcode. The single source of truth for the
// instruction widths — validate() and exec() both use it, so the two can never
// disagree about where the next instruction starts. Unknown opcodes report 0;
// validate() rejects them and exec() bails on them.
inline uint16_t operand_bytes_(uint8_t op) {
    switch (op) {
        case OP_PUSH_SIG: case OP_AGE: case OP_INTERP: case OP_TABLE: case OP_PUSH_I16: return 2;
        case OP_PUSH_CFG:  return 4;
        case OP_PUSH_BCFG: return 5;
        case OP_PUSH_F:    return 4;
        case OP_PUSH_F32:  return 4;
        case OP_PUSH_I8:   return 1;
        default:           return 0;
    }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
// The M7 must never execute unvalidated bytecode: a program can arrive from an
// SD card or a partial write, so the studio having checked it is not evidence.
// Run this ONCE per config load, not per evaluation.
//
// What it can prove: opcodes exist, operands are present and in range, config
// reads land inside the image, bit ranges fit their field, curve ids exist, the
// stack never underflows or overflows, the program terminates inside the block,
// and exactly one value is left at END.
//
// What it cannot prove: that a baked offset points at the field the author
// MEANT. With no descriptor table an in-range wrong offset reads a neighbouring
// field. That is the client's job and the same trust the tune image already
// relies on everywhere else.
//
// Note it does NOT check alignment: the config is #pragma pack(1), so a
// perfectly correct offset is frequently unaligned. Reads go through memcpy for
// exactly that reason — an unaligned VLDR would fault the M7.
enum class Invalid : uint8_t {
    None = 0,
    Truncated,        // operand bytes run past the end of the program
    UnknownOp,
    BadType,          // config type code not in CfgType
    OffsetOutOfRange, // offset + size > cfg_size
    BadBitRange,      // width 0, or lo+width past the top of the field
    BadSignal,        // selector past the end of the catalog
    BadCurve,         // curve id >= curve_count
    StackUnderflow,
    StackOverflow,
    NotTerminated,    // ran off the end of the block without an END
    BadResultCount,   // END with other than exactly one value on the stack
};

// `len` is the usable program length (PROGRAM_MAX for an in-config block).
// `sig_count` is the size of the signal catalog (SIG_COUNT on the firmware side, the meta's
// signal map size in the studio) — passed in rather than included, so this header stays free of
// the generated catalog and the studio can validate a program before it ever reaches an ECU.
inline Invalid validate(const uint8_t* p, uint16_t len, uint32_t cfg_size,
                        uint16_t curve_count, uint16_t sig_count) {
    int depth = 0;
    uint16_t i = 0;
    while (i < len) {
        const uint8_t op = p[i++];
        // Same width table exec() uses, so the two decoders cannot disagree
        // about where the next instruction starts.
        if ((uint16_t)(i + operand_bytes_(op)) > len) return Invalid::Truncated;

        switch (op) {
            case OP_END:
                if (depth != 1) return Invalid::BadResultCount;
                return Invalid::None;

            case OP_PUSH_SIG:
            case OP_AGE: {
                uint16_t sel; memcpy(&sel, p + i, 2); i += 2;
                // sel 0 = None (legal, evaluates untrustworthy); else SignalId+1.
                if (sel > sig_count) return Invalid::BadSignal;
                if (depth >= STACK_MAX) return Invalid::StackOverflow;
                depth++;
                break;
            }

            case OP_PUSH_CFG:
            case OP_PUSH_BCFG: {
                const uint16_t n = operand_bytes_(op);
                const uint32_t off = (uint32_t)p[i] | ((uint32_t)p[i+1] << 8) |
                                     ((uint32_t)p[i+2] << 16);
                const uint8_t type = p[i+3];
                const uint8_t sz = cfg_type_size(type);
                if (sz == 0) return Invalid::BadType;
                if (off + sz > cfg_size) return Invalid::OffsetOutOfRange;
                if (op == OP_PUSH_BCFG) {
                    const uint8_t bits  = p[i+4];
                    const uint8_t lo    = bits & 0x1F;
                    const uint8_t width = (uint8_t)((bits >> 5) & 0x07) + 1;   // 1..8
                    if (type == CT_F32) return Invalid::BadType;               // no bits in a float
                    if (lo + width > sz * 8) return Invalid::BadBitRange;
                }
                i += n;
                if (depth >= STACK_MAX) return Invalid::StackOverflow;
                depth++;
                break;
            }

            case OP_PUSH_F:
            case OP_PUSH_F32: {
                i += 4;
                if (depth >= STACK_MAX) return Invalid::StackOverflow;
                depth++;
                break;
            }
            case OP_PUSH_I16: {
                i += 2;
                if (depth >= STACK_MAX) return Invalid::StackOverflow;
                depth++;
                break;
            }
            case OP_PUSH_I8: {
                i += 1;
                if (depth >= STACK_MAX) return Invalid::StackOverflow;
                depth++;
                break;
            }
            case OP_PUSH_ZERO:
            case OP_PUSH_ONE:
                if (depth >= STACK_MAX) return Invalid::StackOverflow;
                depth++;
                break;

            case OP_INTERP: {
                uint16_t id; memcpy(&id, p + i, 2); i += 2;
                if (id >= curve_count) return Invalid::BadCurve;
                if (depth < 1) return Invalid::StackUnderflow;   // x -> value
                break;
            }

            case OP_TABLE: {
                uint16_t id; memcpy(&id, p + i, 2); i += 2;
                if (id >= curve_count) return Invalid::BadCurve;
                if (++depth > STACK_MAX) return Invalid::StackOverflow;   // -> value
                break;
            }

            // unary: one in, one out
            case OP_NOT: case OP_ABS:
                if (depth < 1) return Invalid::StackUnderflow;
                break;

            // binary: two in, one out
            case OP_GT: case OP_GE: case OP_LT: case OP_LE: case OP_EQ: case OP_NE:
            case OP_AND: case OP_OR:
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
            case OP_MIN: case OP_MAX: case OP_BIT:
                if (depth < 2) return Invalid::StackUnderflow;
                depth--;
                break;

            // ternary: three in, one out
            case OP_CLAMP: case OP_SELECT:
                if (depth < 3) return Invalid::StackUnderflow;
                depth -= 2;
                break;

            default:
                return Invalid::UnknownOp;
        }
    }
    return Invalid::NotTerminated;
}

// An all-zero block is a valid EMPTY program: OP_END with nothing on the stack.
// That is the common case (most sites carry no expression), so it gets its own
// answer rather than tripping BadResultCount.
inline bool is_empty(const uint8_t* p, uint16_t len) {
    return len == 0 || p[0] == OP_END;
}

} // namespace expr
