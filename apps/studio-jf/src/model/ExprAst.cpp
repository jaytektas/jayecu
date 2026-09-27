#include "ExprAst.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace expr_ast {

const std::vector<FnInfo>& functions() {
    // host-only entries are studio maths with no ECU instruction; firmware-only ones read state
    // that exists on the MCU. Everything else is available in both, which is most of it.
    static const std::vector<FnInfo> kFns = {
        {"abs",    1, true,  true },
        {"min",    2, true,  true },
        {"max",    2, true,  true },
        {"select", 3, true,  true },
        {"clamp",  3, true,  true },
        {"bit",    2, true,  true },
        // INTEGER ARITHMETIC. Everything here is a double, so a page had no way to say "the whole
        // part" or "the remainder" and anything placing a value in a repeating group — which bank a
        // driver belongs to, which coil a cylinder shares — had to be told the answer instead.
        // HOST-ONLY for now: adding them to the firmware means an ExprIsa opcode each, and nothing on
        // the ECU asks these questions yet. The parser is shared, so they have to be declared here or
        // an expression using them does not parse at all — and a failed parse evaluates to 0, which
        // reads as a real answer. That is exactly how this was found.
        {"floor",  1, true,  false},
        {"mod",    2, true,  false},
        {"sin",    1, true,  false},
        {"cos",    1, true,  false},
        {"sqrt",   1, true,  false},
        {"lerp",   5, true,  false},
        // HOW MANY GENERIC CAN FRAMES A BUS CARRIES. Host-only and it must stay so: the ECU never
        // asks, and the question is about the TUNE the studio is editing rather than about anything
        // happening on the MCU. The CAN Setup page gates its Bitrate row on canframes(n) == 0, which
        // is the difference between a bus that has committed to a rate and one that has not.
        {"canframes", 1, true, false},
        {"age",    1, false, true },   // ms since a channel was last written — MCU state
        {"interp", 2, false, true },   // a table in the tune, read at an x YOU supply
        // A TABLE READ THE WAY THE FIRMWARE READS IT — no x, because a table already knows where its
        // coordinates come from: its axes name bus channels. Same registry as interp(); the difference
        // is only which question is being asked ("what does it say now" vs "what does it say at x").
        {"table",  1, false, true },
    };
    return kFns;
}

const FnInfo* functionInfo(const std::string& lowerName) {
    for (const FnInfo& f : functions())
        if (lowerName == f.name) return &f;
    return nullptr;
}

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

struct Tok {
    enum Kind { End, Num, Ident, Sigil, Punct } kind = End;
    std::string text;
    std::string unit;          // Num: an identifier written flush against the digits ("1500ms")
    double      num = 0;
    char        sigil = 0;
    int         pos = 0;
};

struct Lexer {
    const std::string& s;
    size_t i = 0;
    std::string error;
    int errPos = -1;

    explicit Lexer(const std::string& src) : s(src) {}

    Tok next() {
        while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
        Tok t;
        t.pos = (int)i;
        if (i >= s.size()) return t;
        const char c = s[i];

        if (c == '[') {                                   // [<sigil><body>]
            const size_t open = i++;
            if (i >= s.size() || !std::strchr("$#@%", s[i])) {
                error = "expected $, #, @ or % after '['"; errPos = (int)open; return t;
            }
            t.sigil = s[i++];
            int depth = 0;
            while (i < s.size() && (s[i] != ']' || depth > 0)) {
                if (s[i] == '[') depth++;
                if (s[i] == ']') depth--;
                t.text += s[i++];
            }
            if (i >= s.size()) { error = "unterminated '['"; errPos = (int)open; return t; }
            i++;                                          // closing ]
            t.kind = Tok::Sigil;
            return t;
        }
        if (std::isdigit((unsigned char)c) ||
            (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1]))) {
            size_t j = i;
            while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '.')) j++;
            t.kind = Tok::Num;
            t.text = s.substr(i, j - i);
            t.num  = std::atof(t.text.c_str());
            // A UNIT WRITTEN FLUSH AGAINST THE DIGITS is part of the literal: "1500ms", "95C", "5V".
            // Flush only — "1500 ms" is two tokens and stays a syntax error, because a space is how a
            // person separates a number from the next thing rather than qualifying it. The grammar
            // does not judge whether the suffix names a real unit; that needs the meta, which the
            // parser has no business knowing about.
            if (j < s.size() && (std::isalpha((unsigned char)s[j]) || s[j] == '%')) {
                size_t k = j;
                while (k < s.size() && (std::isalnum((unsigned char)s[k]) || s[k] == '_' || s[k] == '%')) k++;
                t.unit = s.substr(j, k - j);
                j = k;
            }
            i = j;
            return t;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            // A bare name may be a whole config path: dots AND balanced subscripts are part of it,
            // so `electronic_throttle.etb[0].enabled` is ONE identifier and not name/[0]/.field.
            size_t j = i;
            while (j < s.size()) {
                const char d = s[j];
                if (std::isalnum((unsigned char)d) || d == '_' || d == '.') { j++; continue; }
                if (d == '[') {
                    int depth = 0; size_t k = j;
                    for (; k < s.size(); k++) {
                        if (s[k] == '[') depth++;
                        else if (s[k] == ']' && --depth == 0) { k++; break; }
                    }
                    if (depth != 0) break;                // unterminated — end the name here
                    j = k; continue;
                }
                break;
            }
            t.kind = Tok::Ident;
            t.text = s.substr(i, j - i);
            i = j;
            return t;
        }
        static const char* kTwo[] = {">=", "<=", "==", "!=", "&&", "||"};
        for (const char* p : kTwo) {
            if (s.compare(i, 2, p) == 0) { t.kind = Tok::Punct; t.text = p; i += 2; return t; }
        }
        if (std::strchr("><+-*/^(),!", c)) {
            t.kind = Tok::Punct; t.text = std::string(1, c); i++; return t;
        }
        error = std::string("unexpected character '") + c + "'";
        errPos = (int)i;
        return t;
    }
};

struct Parser {
    Lexer lex;
    Tok   tok;
    std::string error;
    int   errPos = -1;

    explicit Parser(const std::string& src) : lex(src) { advance(); }

    void advance() {
        tok = lex.next();
        if (!lex.error.empty() && error.empty()) { error = lex.error; errPos = lex.errPos; }
    }
    NodePtr fail(const std::string& msg, int pos) {
        if (error.empty()) { error = msg; errPos = pos; }
        return nullptr;
    }
    bool isPunct(const char* p) const { return tok.kind == Tok::Punct && tok.text == p; }
    bool isWord(const char* w) const { return tok.kind == Tok::Ident && lower(tok.text) == w; }

    static NodePtr mk(Kind k, int pos) {
        auto n = std::make_unique<Node>();
        n->kind = k; n->pos = pos;
        return n;
    }
    static NodePtr binary(const std::string& op, NodePtr a, NodePtr b) {
        auto n = mk(Kind::Binary, a ? a->pos : 0);
        n->text = op;
        n->kids.push_back(std::move(a));
        n->kids.push_back(std::move(b));
        return n;
    }

    // --- precedence climbing; each level is one rung of the table in the header ---
    NodePtr parseOr() {
        auto a = parseAnd();
        if (!a) return nullptr;
        while (isPunct("||") || isWord("or")) {
            advance();
            auto b = parseAnd();
            if (!b) return nullptr;
            a = binary("||", std::move(a), std::move(b));
        }
        return a;
    }
    NodePtr parseAnd() {
        auto a = parseEquality();
        if (!a) return nullptr;
        while (isPunct("&&") || isWord("and")) {
            advance();
            auto b = parseEquality();
            if (!b) return nullptr;
            a = binary("&&", std::move(a), std::move(b));
        }
        return a;
    }
    NodePtr parseEquality() {
        auto a = parseRelational();
        if (!a) return nullptr;
        while (isPunct("==") || isPunct("!=")) {
            const std::string op = tok.text;
            advance();
            auto b = parseRelational();
            if (!b) return nullptr;
            a = binary(op, std::move(a), std::move(b));
        }
        return a;
    }
    NodePtr parseRelational() {
        auto a = parseAdd();
        if (!a) return nullptr;
        while (isPunct("<") || isPunct("<=") || isPunct(">") || isPunct(">=")) {
            const std::string op = tok.text;
            advance();
            auto b = parseAdd();
            if (!b) return nullptr;
            a = binary(op, std::move(a), std::move(b));
        }
        return a;
    }
    NodePtr parseAdd() {
        auto a = parseMul();
        if (!a) return nullptr;
        while (isPunct("+") || isPunct("-")) {
            const std::string op = tok.text;
            advance();
            auto b = parseMul();
            if (!b) return nullptr;
            a = binary(op, std::move(a), std::move(b));
        }
        return a;
    }
    NodePtr parseMul() {
        auto a = parsePow();
        if (!a) return nullptr;
        while (isPunct("*") || isPunct("/")) {
            const std::string op = tok.text;
            advance();
            auto b = parsePow();
            if (!b) return nullptr;
            a = binary(op, std::move(a), std::move(b));
        }
        return a;
    }
    NodePtr parsePow() {
        auto a = parseUnary();
        if (!a) return nullptr;
        if (isPunct("^")) {                    // right-associative
            advance();
            auto b = parsePow();
            if (!b) return nullptr;
            a = binary("^", std::move(a), std::move(b));
        }
        return a;
    }
    NodePtr parseUnary() {
        if (isPunct("!") || isWord("not") || isPunct("-")) {
            const std::string op = isPunct("-") ? "-" : "!";
            const int pos = tok.pos;
            advance();
            auto a = parseUnary();
            if (!a) return nullptr;
            auto n = mk(Kind::Unary, pos);
            n->text = op;
            n->kids.push_back(std::move(a));
            return n;
        }
        return parsePrimary();
    }

    NodePtr parsePrimary() {
        const int pos = tok.pos;
        if (tok.kind == Tok::End) return fail("unexpected end of expression", pos);

        if (isPunct("(")) {
            advance();
            auto a = parseOr();
            if (!a) return nullptr;
            if (!isPunct(")")) return fail("expected ')'", tok.pos);
            advance();
            return a;
        }
        if (tok.kind == Tok::Num) {
            auto n = mk(Kind::Number, pos);
            n->num  = tok.num;
            n->unit = tok.unit;
            advance();
            return n;
        }
        if (tok.kind == Tok::Sigil) {
            auto n = mk(Kind::Sigil, pos);
            n->sigil = tok.sigil;
            n->text  = tok.text;
            advance();
            return n;
        }
        if (tok.kind == Tok::Ident) {
            const std::string name = tok.text;
            const std::string lname = lower(name);

            if (lname == "true" || lname == "false") {
                auto n = mk(Kind::Number, pos);
                n->num = (lname == "true") ? 1.0 : 0.0;
                advance();
                return n;
            }
            if (const FnInfo* fn = functionInfo(lname)) {
                advance();
                if (!isPunct("(")) return fail("expected '(' after " + lname, pos);
                advance();
                auto n = mk(Kind::Call, pos);
                n->text = lname;
                if (!isPunct(")")) {
                    for (;;) {
                        auto a = parseOr();
                        if (!a) return nullptr;
                        n->kids.push_back(std::move(a));
                        if (isPunct(",")) { advance(); continue; }
                        break;
                    }
                }
                if (!isPunct(")")) return fail("expected ')' closing " + lname, tok.pos);
                advance();
                if ((int)n->kids.size() != fn->arity)
                    return fail(lname + "() takes " + std::to_string(fn->arity) + " argument" +
                                (fn->arity == 1 ? "" : "s") + ", got " +
                                std::to_string(n->kids.size()), pos);
                return n;
            }
            auto n = mk(Kind::Ident, pos);
            n->text = name;
            advance();
            return n;
        }
        return fail("unexpected '" + tok.text + "'", pos);
    }
};

int precOf(const Node& n) {
    if (n.kind == Kind::Unary)  return 8;
    if (n.kind != Kind::Binary) return 100;
    const std::string& o = n.text;
    if (o == "||") return 1;
    if (o == "&&") return 2;
    if (o == "==" || o == "!=") return 3;
    if (o == "<" || o == "<=" || o == ">" || o == ">=") return 4;
    if (o == "+" || o == "-") return 5;
    if (o == "*" || o == "/") return 6;
    if (o == "^") return 7;
    return 100;
}

std::string wrap(const Node& n, int need) {
    const std::string t = unparse(n);
    return precOf(n) < need ? "(" + t + ")" : t;
}

std::string trimNum(double v) {
    char buf[40];
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
        return buf;
    }
    snprintf(buf, sizeof(buf), "%.4f", v);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

} // namespace

NodePtr parse(const std::string& source, std::string& error, int& errPos) {
    error.clear();
    errPos = -1;
    if (source.find_first_not_of(" \t\r\n") == std::string::npos)
        return nullptr;                       // nothing specified — a meaning, not a failure

    Parser p(source);
    auto n = p.parseOr();
    if (!n || !p.error.empty()) {
        error  = p.error.empty() ? "invalid expression" : p.error;
        errPos = p.errPos;
        return nullptr;
    }
    if (p.tok.kind != Tok::End) {
        error  = "unexpected '" + p.tok.text + "' after the expression";
        errPos = p.tok.pos;
        return nullptr;
    }
    return n;
}

std::string unparse(const Node& n) {
    switch (n.kind) {
        case Kind::Number: return trimNum(n.num) + n.unit;   // the unit is part of the literal
        case Kind::Ident:  return n.text;
        case Kind::Sigil:  return std::string("[") + n.sigil + n.text + "]";
        case Kind::Unary:
            // `not` reads better than `!` in a gate, and is what the firmware decompiler emits.
            // Unary binds TIGHTER than comparison (C precedence, rung 8), so `not` over a
            // comparison MUST keep its parentheses: `not rpm > 4000` reads as `(not rpm) > 4000`,
            // which is a different question.
            return (n.text == "!") ? ("not " + wrap(*n.kids[0], 8))
                                   : ("-" + wrap(*n.kids[0], 8));
        case Kind::Call: {
            std::string out = n.text + "(";
            for (size_t k = 0; k < n.kids.size(); k++) {
                if (k) out += ", ";
                out += unparse(*n.kids[k]);
            }
            return out + ")";
        }
        case Kind::Binary: {
            const int pr = precOf(n);
            std::string op = n.text;
            if (op == "&&") op = "and";
            else if (op == "||") op = "or";
            // Left-associative: the RIGHT operand needs parentheses at equal precedence too, or
            // `a - (b - c)` comes back as `a - b - c`, which is a different number.
            const bool rightAssoc = (n.text == "^");
            return wrap(*n.kids[0], rightAssoc ? pr + 1 : pr) + " " + op + " " +
                   wrap(*n.kids[1], rightAssoc ? pr : pr + 1);
        }
    }
    return {};
}

} // namespace expr_ast
