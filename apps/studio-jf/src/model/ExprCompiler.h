#pragma once
//
// ExprCompiler — expression source text <-> firmware bytecode.
//
// The client half of the expression evaluator (docs/expression-evaluator-design.md).
// The tuner writes readable source; this compiles it against the CURRENT layout's meta,
// baking signal selectors, config offsets and bit ranges straight into the instructions.
// The firmware then carries no names and no descriptor table — it just runs the program.
//
//     rpm > 2500 and (map > 50 or tps > 80)
//        -> PUSH_SIG rpm, PUSH_F 250000, GT, PUSH_SIG map, ... OR, AND, END
//
// Opcodes come from firmware/Signal/ExprIsa.h — the SAME header the firmware executes, so
// the two cannot drift. A second opcode table here would be a silent-wrong-answer bug.
//
// Bytecode is layout-bound: an offset baked for one layout addresses a different field in
// another. So bytecode belongs in the config IMAGE and the SOURCE belongs in the tune;
// migrating a tune is a recompile, never a transplant (see the design doc).
//
// GRAMMAR (deliberately the same shape the studio already uses for enable/visibility
// conditions, so a tuner meets one language, not two):
//
//   or  ||            lowest precedence
//   and &&
//   not !             unary
//   == != < <= > >=
//   + -
//   * /
//   unary -
//   ( )  functions  literals  channels  settings
//
//   channels   rpm          or  [$rpm]        a bus signal, by its catalog name
//   settings   [#sensors.sensor[clt].diag_op_max]   a config field, by meta path
//              [#sensors.sensor[clt].diag_enable.raw_min]  ...or one of its named bit groups
//   functions  min max abs clamp select bit interp age
//
#include "MetaModel.h"
#include "../../../../firmware/Signal/ExprIsa.h"

#include <cstdint>
#include <string>
#include <vector>

class ExprCompiler {
public:
    struct Result {
        bool                 ok = false;
        std::string          error;         // human-readable, names the token that failed
        int                  errPos = -1;   // byte offset into the source, for the editor caret
        std::vector<uint8_t> code;          // bytecode, terminated by OP_END
    };

    // Compile `source` against `meta`. `cfgSize` is the config image size (for the validator's
    // range check); `blockSize` is the field's program capacity from the meta (`length`).
    // An empty/whitespace-only source compiles to a single OP_END — the "always armed" program.
    static Result compile(const std::string& source, const MetaModel& meta,
                          uint32_t cfgSize, uint16_t blockSize = expr::PROGRAM_MAX);

    // Bytecode -> readable source. Used when an ECU is read cold with no project file: the
    // program still shows as text, provided the meta for that image's layout_hash is in the
    // library. Round-trips MEANING, not characters — spacing is normalised and parentheses may
    // appear where the author did not type them.
    static std::string decompile(const uint8_t* code, uint16_t len, const MetaModel& meta);

    // Every name the editor can offer: bus channels plus the config paths reachable as settings.

    // Reverse-lookup a baked config offset to its meta path, and optionally the field's
    // display scale (the decompiler needs it to recognise the conversion the compiler emits).
    static std::string configPathAt(const MetaModel& meta, int offset, double* scaleOut = nullptr);

private:
    struct Parser;
};
