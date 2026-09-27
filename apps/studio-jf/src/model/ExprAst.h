#pragma once
//
// ExprAst — THE expression front end. One tokenizer, one grammar, one parser, for every
// expression the studio understands.
//
// There is exactly one language. What differs is where it RUNS, and that is a back-end choice:
//
//   host      MathEvaluator walks this AST resolving every sigil, including the ones that exist
//             only in the studio ([@widget.prop], [%app.state]). Used for visibility, enable and
//             data-source expressions, evaluated per frame.
//   firmware  ExprCompiler walks this AST and emits bytecode for the ECU's stack VM
//             (firmware/Signal/ExprIsa.h). It can address bus channels and config settings; a
//             studio-only sigil is refused with a message that says WHY, not a parse error.
//
// This is what docs/expression-evaluator-design.md means by "one grammar, one editor, one
// validator, one decompiler". Two parsers is how you end up with `and` working in one place and
// not the other, `clamp` in one and `lerp` in the other, and `a == b > c` quietly meaning two
// different things — all of which had happened before this was unified.
//
// PRECEDENCE is C's, which is what the studio's expressions have always used: equality binds
// LOOSER than the relational operators. Anything already saved in a layout keeps its meaning.
//
//   ||                       1   (also spelled `or`)
//   &&                       2   (also spelled `and`)
//   == !=                    3
//   < <= > >=                4
//   + -                      5
//   * /                      6
//   ^                        7   right-associative, host only
//   ! -   (unary)            8   (`!` also spelled `not`)
//
#include <memory>
#include <string>
#include <vector>

namespace expr_ast {

enum class Kind {
    Number,     // 2500, 12.34, 1500ms, 95C — a literal may carry the UNIT it was written in
    Ident,      // a bare name: rpm, electronic_throttle.etb[0].enabled
    Sigil,      // [$rpm] [#module.field] [@widget.prop] [%app.state]
    Unary,      // text = "!" or "-"
    Binary,     // text = "&&" "||" "==" "!=" "<" "<=" ">" ">=" "+" "-" "*" "/" "^"
    Call,       // text = function name, kids = arguments
};

struct Node {
    Kind        kind = Kind::Number;
    double      num  = 0;      // Number
    // THE UNIT THE LITERAL WAS WRITTEN IN, verbatim and unresolved ("ms", "C", "s"), empty when the
    // number was bare. The GRAMMAR only records it; what it means is the back end's business, because
    // a unit is only meaningful against the thing being compared — and both back ends have to reach
    // the same answer, so neither may invent its own rule. See exprUnitConvert().
    std::string unit;          // Number
    std::string text;          // Ident/Sigil body, operator symbol, or function name
    char        sigil = 0;     // '$' '#' '@' '%' for Sigil, else 0
    int         pos = 0;       // byte offset in the source, for the editor's caret
    std::vector<std::unique_ptr<Node>> kids;
};

using NodePtr = std::unique_ptr<Node>;

// Every function the language has, and where it can run. A back-end that cannot serve one says so
// by name ("sqrt() is evaluated in the studio and has no ECU instruction") instead of failing to
// parse, which is the difference between a language with two dialects and one language with two
// back-ends.
struct FnInfo {
    const char* name;
    int         arity;
    bool        host;        // MathEvaluator can evaluate it
    bool        firmware;    // ExprCompiler can emit it
};
const std::vector<FnInfo>& functions();
const FnInfo* functionInfo(const std::string& lowerName);

// Parse `source`. Returns null on failure with `error` set (and `errPos` = byte offset, or -1).
// An empty/whitespace-only source parses to null with NO error — callers treat that as "nothing
// specified" (always visible / always armed), which is a meaning, not a failure.
NodePtr parse(const std::string& source, std::string& error, int& errPos);

// Render an AST back to source. Parentheses appear exactly where they change meaning.
std::string unparse(const Node& n);

} // namespace expr_ast
