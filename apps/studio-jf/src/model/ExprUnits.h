#pragma once
//
// ExprUnits — what a unit written on a literal MEANS, for both back ends at once.
//
// The grammar records the suffix on `1500ms` and stops there (see expr_ast::Node::unit), because a
// unit is only meaningful against the thing being compared: `age(trigger_teeth) < 1500ms` wants
// milliseconds because age() answers in milliseconds, and `clt > 95C` wants whatever unit the coolant
// channel is published in. Resolving that needs the meta, which the parser has no business knowing.
//
// It lives here rather than in either back end because there are TWO — ExprCompiler emits bytecode for
// the ECU, MathEvaluator answers the same expression in the studio — and a page that greys a row must
// agree with the firmware that runs it. Two copies of this rule is a silent-wrong-answer bug: the
// studio would say the fan is on and the ECU would leave it off, and nothing would report a fault.
//
// WHY IT MATTERS AT ALL. Without it a bare number silently means "whatever the other side is in", which
// is fine right up to the moment it isn't: a fuel pump's stop time is written in the template as a
// number that has to be milliseconds because age() is, and nothing anywhere says so. A literal that
// carries its unit is checkable; a literal that doesn't is a comment.
//
#include "MetaModel.h"
#include "ExprAst.h"

#include <string>

namespace expr_units {

// The unit a NODE's value is in, or empty when nothing declares one.
//
//   a channel          its telemetry units ("C", "s", "RPM")
//   a config setting   the field's units ("ms")
//   age(x)             ALWAYS milliseconds — the unit of the ANSWER, not of the channel asked about.
//                      trigger_teeth is a count with no unit at all, and its age is still in ms.
//   anything else      empty: arithmetic over mixed units is the author's business, not ours
//
// A blank answer is "not declared", never "dimensionless" — several channels ship no units yet, and
// refusing an expression because the meta is incomplete would be a worse failure than allowing it.
std::string unitOf(const MetaModel& meta, const expr_ast::Node& n);

// Fold a literal's unit into `target`, in place. Returns false with `err` set when the two name
// DIFFERENT QUANTITIES — volts against milliseconds — because there is no conversion and inventing one
// would be worse than the comparison being plainly wrong. An unrecognised unit is an error too: "5x"
// used to be a syntax error and should stay a refusal, not become a silent 5.
//
// A literal with NO unit is left exactly as written. That is the permissive default, and it is what
// every expression in the tune relies on today — but it is now a choice the author can override by
// writing the unit down, which is the whole point.
bool convertLiteral(expr_ast::Node& lit, const std::string& target, std::string& err);

// Walk a parsed expression and fold every unit-carrying literal into the unit of whatever it is being
// compared or combined with. Call once, after parse and before either back end walks the tree, so the
// compiler and the evaluator see the same numbers. Returns false with `err`/`errPos` set on a
// mismatch the author has to resolve.
bool resolve(const MetaModel& meta, expr_ast::Node& root, std::string& err, int& errPos);

} // namespace expr_units
