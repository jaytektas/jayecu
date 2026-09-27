#include "ExprUnits.h"
#include "UnitManager.h"

#include <cctype>

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The unit id UnitManager knows, from what the author typed. Matched case-insensitively for the same
// reason substUnits does it: nobody types "V" for volts and "ms" for milliseconds with equal care, and
// "5v" is not a different threshold from "5V". Empty when nothing in the tables matches.
std::string canonicalUnit(const std::string& written) {
    if (written.empty()) return {};
    UnitManager& um = UnitManager::instance();
    if (!um.findQuantityForUnit(written).empty()) return written;
    const std::string want = lower(written);
    for (const std::string& qid : um.quantities())
        for (const auto& cand : um.getQuantity(qid).units)
            if (lower(cand.id) == want) return cand.id;
    return {};
}

} // namespace

namespace expr_units {

std::string unitOf(const MetaModel& meta, const expr_ast::Node& n) {
    using expr_ast::Kind;
    switch (n.kind) {
        case Kind::Number:
            return n.unit;
        case Kind::Call: {
            // age() answers in milliseconds whatever it was asked about — the unit of the ANSWER, not
            // of the operand. trigger_teeth is a bare count with no unit and its age is still in ms,
            // which is exactly the case a naive "inherit from the argument" rule would get wrong.
            const std::string fn = lower(n.text);
            if (fn == "age") return "ms";
            return {};
        }
        case Kind::Ident:
        case Kind::Sigil: {
            // A channel, or a config setting. Both are looked up the way the compiler looks them up,
            // so a name that resolves for one resolves for the other.
            const bool cfg = (n.kind == Kind::Sigil && n.sigil == '#');
            const std::string name = n.text;
            if (!cfg) {
                const auto& t = meta.telemetry();
                const auto it = t.find(name);
                if (it != t.end()) return it->second.units;
                if (n.kind == Kind::Sigil && n.sigil == '$') return {};
            }
            const MetaModel::Location L = meta.locate(name);
            if (L.valid()) return L.units;
            return {};
        }
        default:
            return {};
    }
}

bool convertLiteral(expr_ast::Node& lit, const std::string& target, std::string& err) {
    if (lit.kind != expr_ast::Kind::Number || lit.unit.empty()) return true;   // bare: as written
    const std::string from = canonicalUnit(lit.unit);
    if (from.empty()) {
        err = "unknown unit '" + lit.unit + "'";
        return false;
    }
    // Nothing to convert INTO. The other side declares no unit (plenty of channels still ship none),
    // so the honest move is to take the number as written rather than guess at a conversion.
    const std::string to = canonicalUnit(target);
    if (to.empty()) { lit.unit.clear(); return true; }

    UnitManager& um = UnitManager::instance();
    if (um.findQuantityForUnit(from) != um.findQuantityForUnit(to)) {
        err = "'" + lit.unit + "' and '" + target + "' are different quantities — there is no "
              "conversion between them";
        return false;
    }
    lit.num = um.convert(lit.num, from, to);
    lit.unit.clear();                       // folded: the number is now in the target's unit
    return true;
}

namespace {

bool isComparison(const std::string& op) {
    return op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=";
}

// TOP-DOWN, and that order is the whole of it. Folding bottom-up let a literal reach the "a unit that
// survived every fold" clause below on its own, one call before its parent comparison ever looked at
// it — so every unit was quietly cleared and every conversion silently skipped. The comparison must
// have its say about a child before the child is asked to account for itself.
bool walk(const MetaModel& meta, expr_ast::Node& n, std::string& err, int& errPos) {
    using expr_ast::Kind;

    // A COMPARISON is where a unit earns its keep, and it is also the only place the target is
    // unambiguous: one side is the thing, the other is the threshold. Arithmetic (`rpm / 4`) is left
    // alone deliberately — the unit of a quotient is a question this language does not try to answer,
    // and pretending otherwise would refuse expressions that are perfectly clear to their author.
    if (n.kind == Kind::Binary && isComparison(n.text) && n.kids.size() == 2) {
        expr_ast::Node& a = *n.kids[0];
        expr_ast::Node& b = *n.kids[1];
        // Each literal folds into the OTHER side. Both sides being literals is a constant comparison
        // and needs no help from us.
        if (b.kind == Kind::Number && !b.unit.empty()) {
            if (!convertLiteral(b, unitOf(meta, a), err)) { errPos = b.pos; return false; }
        }
        if (a.kind == Kind::Number && !a.unit.empty()) {
            if (!convertLiteral(a, unitOf(meta, b), err)) { errPos = a.pos; return false; }
        }
    }
    for (auto& kid : n.kids)
        if (kid && !walk(meta, *kid, err, errPos)) return false;

    // A unit that survived the fold above is on a literal that is not being compared with anything —
    // "1500ms + rpm", say. A suffix nobody recognises is refused, because "5x" was a syntax error
    // before this existed and quietly becoming a plain 5 would be worse. A REAL unit with nothing to
    // convert against keeps its number and loses the suffix, which is what substUnits has always done
    // for the colour rules: there is no conversion to apply, and refusing would reject an expression
    // whose author was simply writing down what the number was.
    if (n.kind == Kind::Number && !n.unit.empty()) {
        if (canonicalUnit(n.unit).empty()) { err = "unknown unit '" + n.unit + "'"; errPos = n.pos; return false; }
        n.unit.clear();
    }
    return true;
}

} // namespace

bool resolve(const MetaModel& meta, expr_ast::Node& root, std::string& err, int& errPos) {
    return walk(meta, root, err, errPos);
}

} // namespace expr_units
