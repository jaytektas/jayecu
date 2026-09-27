#include "MathEvaluator.h"
#include <j/core/Log.h>
#include "ExprAst.h"
#include "ExprUnits.h"
#include "Cache.h"
#include "Perf.h"

static std::function<int(int)>& canFrames_();   // defined below, beside the other installed hooks

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stack>

MathEvaluator& MathEvaluator::instance() {
    static MathEvaluator inst;
    return inst;
}

void MathEvaluator::registerResolver(ISigilResolver* resolver) {
    if (resolver) m_resolvers[resolver->sigil()] = resolver;
    invalidateOwners();          // a new resolver may claim tokens the memo recorded as unowned
}

double MathEvaluator::evaluate(const std::string& expression) const {
    PERF_SCOPE("MathEvaluator::evaluate");
    PERF_COUNT("expression evaluations");
    if (expression.empty()) return 0.0;
    // AN INDEXED SUBSCRIPT IS RESOLVED HERE, ONCE, FOR EVERYTHING THAT EVALUATES.
    //
    // A binding went through bindPath(), which resolves "[@expr]" before use. A CONDITION did not: it
    // was handed to this function verbatim, and resolveSigil() dispatches on the leading sigil without
    // ever looking at the subscript — so "[#outputs.output[@[#pc.output_sel]].enabled] == 1" asked the
    // config resolver for a path containing a bracketed expression, which locates nothing and reads 0.
    // Every gated control on a page indexed by a selector was therefore permanently disabled: the
    // Output Setup page's whole Driving, Timing, When and Value half, greyed with no way to reach it.
    //
    // This is the one front door for studio-side expressions, so it is the one place the resolution
    // belongs. The find() costs nothing on the expressions that have no subscript, which is nearly all
    // of them, and resolveIndexed's own recursion terminates because a subscript cannot contain "[@".
    if (expression.find("[@") != std::string::npos) {
        const std::string resolved = resolveIndexed(expression);
        if (resolved != expression) return resolved.empty() ? 0.0 : evaluate(resolved);
    }
    // A single bracketed sigil (e.g. "[@Name]") is resolved directly — no need to tokenize.
    if (expression.front() == '[' && expression.back() == ']' &&
        expression.find(']', 1) == expression.size() - 1) {
        return resolveSigil(expression);
    }
    // ONE front end: the same parser ExprCompiler uses. This class is now a BACK END — it walks
    // the shared AST resolving every sigil, including the studio-only ones the firmware VM cannot
    // address ([@widget.prop], [%app.state]). See ExprAst.h.
    //
    // Parsed trees are cached per expression STRING: a binding or a condition is a constant that
    // gets evaluated every frame for every control carrying it, so re-parsing it each time is work
    // done over and over for an answer that cannot change. Only the VALUES change.
    auto it = m_astCache.find(expression);
    if (it == m_astCache.end()) {
        if (m_astCache.size() > 4096) m_astCache.clear();
        std::string err;
        int pos = -1;
        auto ast = expr_ast::parse(expression, err, pos);
        // THE SAME UNIT FOLD THE COMPILER DOES, through the same helper — a row greyed by this
        // evaluator and a gate run by the firmware must agree about what "1500ms" is. Cached with the
        // tree, so it costs nothing per frame. A unit error leaves the numbers as written rather than
        // taking the UI down: a visibility expression that refuses to answer is a blank page, and the
        // compiler is where an author is told about it.
        if (ast)
            if (const MetaModel* m = Cache::instance().meta()) {
                std::string uerr;
                int upos = -1;
                (void)expr_units::resolve(*m, *ast, uerr, upos);
            }
        it = m_astCache.emplace(expression, std::move(ast)).first;
        PERF_COUNT("expression parses");
    }
    return it->second ? evalNode(*it->second) : 0.0;
}

// Walk the shared AST. Anything the language can express and the studio can answer is answered
// here; a firmware-only function (age/interp reads MCU state) evaluates to 0 rather than throwing,
// because a visibility expression must never take the UI down.
double MathEvaluator::evalNode(const expr_ast::Node& n) const {
    using expr_ast::Kind;
    switch (n.kind) {
        case Kind::Number: return n.num;
        case Kind::Ident:  return resolveSigil(n.text);
        case Kind::Sigil:  return resolveSigil(std::string("[") + n.sigil + n.text + "]");

        case Kind::Unary: {
            const double a = evalNode(*n.kids[0]);
            return n.text == "-" ? -a : (a == 0.0 ? 1.0 : 0.0);
        }
        case Kind::Binary: {
            const double a = evalNode(*n.kids[0]);
            const double b = evalNode(*n.kids[1]);
            const std::string& o = n.text;
            if (o == "+")  return a + b;
            if (o == "-")  return a - b;
            if (o == "*")  return a * b;
            if (o == "/")  return b == 0.0 ? 0.0 : a / b;
            if (o == "^")  return std::pow(a, b);
            if (o == ">")  return a >  b ? 1.0 : 0.0;
            if (o == ">=") return a >= b ? 1.0 : 0.0;
            if (o == "<")  return a <  b ? 1.0 : 0.0;
            if (o == "<=") return a <= b ? 1.0 : 0.0;
            if (o == "==") return a == b ? 1.0 : 0.0;
            if (o == "!=") return a != b ? 1.0 : 0.0;
            if (o == "&&") return (a != 0.0 && b != 0.0) ? 1.0 : 0.0;
            if (o == "||") return (a != 0.0 || b != 0.0) ? 1.0 : 0.0;
            return 0.0;
        }
        case Kind::Call: {
            const std::string& f = n.text;
            auto arg = [&](size_t k) { return k < n.kids.size() ? evalNode(*n.kids[k]) : 0.0; };
            if (f == "sin")  return std::sin(arg(0));
            if (f == "cos")  return std::cos(arg(0));
            if (f == "sqrt") return std::sqrt(arg(0));
            if (f == "abs")  return std::fabs(arg(0));
            if (f == "min")  return std::min(arg(0), arg(1));
            if (f == "max")  return std::max(arg(0), arg(1));
            // INTEGER ARITHMETIC, for pages that have to place something in a repeating group — which
            // coil a cylinder shares, which bank it is on. Everything here is a double, so a page had no
            // way to say "the whole part" or "the remainder" and had to be told the answer instead.
            // mod() takes its sign from the DIVISOR (Python's rule, not C's), so a negative dividend
            // wraps into the group rather than landing outside it.
            if (f == "floor") return std::floor(arg(0));
            if (f == "mod") {
                const double b = arg(1);
                return (b == 0.0) ? 0.0 : arg(0) - b * std::floor(arg(0) / b);
            }
            if (f == "select") return arg(0) != 0.0 ? arg(1) : arg(2);
            if (f == "clamp") {
                const double x = arg(0), lo = arg(1), hi = arg(2);
                return x < lo ? lo : (x > hi ? hi : x);
            }
            if (f == "bit") {
                const long long w = (long long)arg(0);
                const int k = (int)arg(1);
                return (k >= 0 && k < 32 && ((w >> k) & 1)) ? 1.0 : 0.0;
            }
            // HOW MANY FRAMES THIS BUS CARRIES — the one question a page cannot ask in path
            // arithmetic, because the answer is a walk over ninety-six array elements.
            if (f == "canframes") return canFrames_() ? double(canFrames_()((int)arg(0))) : 0.0;
            if (f == "lerp") {
                const double x = arg(0), x0 = arg(1), y0 = arg(2), x1 = arg(3), y1 = arg(4);
                const double range = x1 - x0;
                return std::fabs(range) < 1e-6 ? y0 : y0 + ((x - x0) / range) * (y1 - y0);
            }
            return 0.0;      // age()/interp() read ECU state — not answerable on the host
        }
    }
    return 0.0;
}

// The element a template's "[*]" means, for as long as a widget is evaluating. See the declaration.
static std::string& elemCtx_() { static std::string s; return s; }
const std::string& MathEvaluator::elementContext() { return elemCtx_(); }
MathEvaluator::ElementScope::ElementScope(std::string key) : m_prev(elemCtx_()) { elemCtx_() = std::move(key); }
MathEvaluator::ElementScope::~ElementScope() { elemCtx_() = std::move(m_prev); }

// "[*]" -> the element in scope. Applied to every path the evaluator resolves, whatever expression it
// came from, which is the whole point of doing it here.
// See the declaration for why this is one function and not two.
std::string MathEvaluator::elementKey(const std::string& context) {
    // The LAST bracket pair is the subscript: a sigil wrapper puts a bracket at the front
    // ("[#trigger.streams[0]]") and taking the first one yields the wrapper, not the element.
    const size_t lb = context.rfind('[');
    if (lb == std::string::npos) return context;                    // a bare key ("clt")
    const size_t rb = context.find(']', lb);
    return context.substr(lb + 1, rb == std::string::npos ? std::string::npos : rb - lb - 1);
}

// id -> primary telemetry channel, for "$*". Installed by the app from the meta; empty until then.
static std::function<std::string(const std::string&)>& primarySig_() {
    static std::function<std::string(const std::string&)> f; return f;
}
void MathEvaluator::setPrimarySignalResolver(std::function<std::string(const std::string&)> fn) {
    primarySig_() = std::move(fn);
}

// id -> the RAW channel its input publishes, for "$~". Installed by the app; empty until then.
static std::function<std::string(const std::string&)>& rawSig_() {
    static std::function<std::string(const std::string&)> f; return f;
}
void MathEvaluator::setRawSignalResolver(std::function<std::string(const std::string&)> fn) {
    rawSig_() = std::move(fn);
}

// bus -> how many frames the tune has on it, for canframes(). Installed by the app; until then the
// count is zero, which leaves a page saying "nothing committed yet" rather than locking a control
// nobody can unlock.
static std::function<int(int)>& canFrames_() {
    static std::function<int(int)> f; return f;
}
void MathEvaluator::setCanFrameCounter(std::function<int(int bus)> fn) { canFrames_() = std::move(fn); }

std::string MathEvaluator::resolveTemplate(const std::string& text, const std::string& context) {
    if (context.empty()) return text;
    std::string out = text;
    // "[*]" -> the element's config KEY (an array subscript: sensor[*] -> sensor[clt]).
    const std::string key = "[" + elementKey(context) + "]";
    for (size_t at = out.find("[*]"); at != std::string::npos; at = out.find("[*]", at + key.size()))
        out.replace(at, 3, key);
    // "$*" -> the element's PRIMARY telemetry channel ($* -> $boost_kpa). A star right after a sigil is
    // unambiguous — a sigil is never a multiply operand — so this never touches an arithmetic '*'. Only
    // the '*' is replaced; the '$' stays. Falls back to the config key when no map is installed (id==sig).
    if (out.find("$*") != std::string::npos) {
        const std::string k = elementKey(context);
        const std::string sig = primarySig_() ? primarySig_()(k) : k;
        const std::string repl = sig.empty() ? k : sig;
        for (size_t at = out.find("$*"); at != std::string::npos; at = out.find("$*", at + 1 + repl.size()))
            out.replace(at + 1, 1, repl);
    }
    // "$~" -> the element's RAW INPUT channel ($~ -> $hw_av3), from its interface and pin. An element
    // whose input is unassigned has none, and the token is then left alone — which reads as an
    // unresolved binding rather than silently borrowing some other channel's number.
    if (out.find("$~") != std::string::npos) {
        const std::string k = elementKey(context);
        const std::string sig = rawSig_() ? rawSig_()(k) : std::string();
        if (!sig.empty())
            for (size_t at = out.find("$~"); at != std::string::npos; at = out.find("$~", at + 1 + sig.size()))
                out.replace(at + 1, 1, sig);
    }
    return out;
}

// See the declaration. "[@" is only an index operator in subscript position (after a name / ']'); a
// bare "[@name]" is the widget sigil and is skipped. The tail is resolved recursively so a path may
// carry more than one indexed subscript.
std::string MathEvaluator::resolveIndexed(const std::string& text) const {
    for (size_t at = text.find("[@"); at != std::string::npos; at = text.find("[@", at + 2)) {
        const char prev = at > 0 ? text[at - 1] : '\0';
        if (!(std::isalnum((unsigned char)prev) || prev == '_' || prev == ']'))
            continue;                                   // operand-position '@' sigil — leave for the evaluator
        int depth = 0; size_t close = std::string::npos;
        for (size_t i = at; i < text.size(); ++i) {
            if (text[i] == '[') ++depth;
            else if (text[i] == ']' && --depth == 0) { close = i; break; }
        }
        if (close == std::string::npos) break;          // unbalanced — leave it; downstream reports it unresolved
        const long idx = std::lround(evaluate(text.substr(at + 2, close - (at + 2))));
        if (idx < 0) return {};                          // unused slot / bad read -> blank, unbound cell
        return text.substr(0, at) + "[" + std::to_string(idx) + "]" +
               resolveIndexed(text.substr(close + 1));
    }
    return text;
}
// See the declaration. Same walk as resolveIndexed, but each "[@…]" subscript becomes "[0]" (a
// representative element) instead of being evaluated — a concrete path for field metadata that does
// not depend on WHICH element the index picks.
std::string MathEvaluator::fieldPath(const std::string& text) const {
    for (size_t at = text.find("[@"); at != std::string::npos; at = text.find("[@", at + 2)) {
        const char prev = at > 0 ? text[at - 1] : '\0';
        if (!(std::isalnum((unsigned char)prev) || prev == '_' || prev == ']'))
            continue;                                   // operand-position '@' sigil — leave for the evaluator
        int depth = 0; size_t close = std::string::npos;
        for (size_t i = at; i < text.size(); ++i) {
            if (text[i] == '[') ++depth;
            else if (text[i] == ']' && --depth == 0) { close = i; break; }
        }
        if (close == std::string::npos) break;          // unbalanced — leave it
        return text.substr(0, at) + "[0]" + fieldPath(text.substr(close + 1));
    }
    return text;
}

static std::string applyElem_(const std::string& path) {
    return MathEvaluator::resolveTemplate(path, elemCtx_());
}

double MathEvaluator::resolveSigil(const std::string& raw) const {
    const std::string token = applyElem_(raw);
    if (token.empty()) return 0.0;
    // Bracketed sigil: [<sigil><name>]  → route by the leading sigil char.
    if (token.size() >= 4 && token.front() == '[' && token.back() == ']') {
        const std::string inner = token.substr(1, token.size() - 2);
        auto it = m_resolvers.find(inner[0]);
        if (it != m_resolvers.end()) return it->second->resolveSigil(inner.substr(1));
        return 0.0;
    }
    // Bare token: prefer the resolver that OWNS it, else fall back to the first non-zero answer.
    //
    // BOTH ANSWERS ARE REMEMBERED. Ownership cannot change from one frame to the next — only values can
    // — and the fallback is expensive: it asks every resolver, including the widget one, which walks
    // every placed widget to answer. Unmemoised, a page carrying a few dozen tokens nobody claims spent
    // most of its frame re-discovering that nobody claims them.
    if (const auto hit = m_ownerCache.find(token); hit != m_ownerCache.end())
        return hit->second ? hit->second->resolveSigil(token) : 0.0;

    for (const auto& [ch, r] : m_resolvers)
        if (r->provides(token)) { m_ownerCache[token] = r; return r->resolveSigil(token); }

    PERF_SCOPE("resolve: unowned-token fallback");
    PERF_COUNT("unowned token fallbacks");
    for (const auto& [ch, r] : m_resolvers) {
        const double v = r->resolveSigil(token);
        if (v != 0.0) { m_ownerCache[token] = r; return v; }
    }
    // NOBODY. Remembered as such, and said once — a binding that resolves to nothing is a page bug
    // (a channel that was renamed, a token that is not a channel at all), and it should be findable
    // rather than merely slow.
    m_ownerCache[token] = nullptr;
    JLOGC("model.expr", jf::JLogLevel::Debug)
        << "no resolver owns \"" << token << "\" — it will read as 0";
    return 0.0;
}

// ---- Metadata passthrough: [<sigil><name>] → the owning resolver's getter --------------------
std::string MathEvaluator::metaFor(const std::string& token,
                                   std::string (ISigilResolver::*getter)(const std::string&) const) const {
    if (token.size() < 4 || token.front() != '[' || token.back() != ']') return {};
    const std::string inner = token.substr(1, token.size() - 2);
    auto it = m_resolvers.find(inner[0]);
    if (it == m_resolvers.end()) return {};
    return (it->second->*getter)(inner.substr(1));
}
std::string MathEvaluator::getUnit(const std::string& t)       const { return metaFor(t, &ISigilResolver::getUnit); }
std::string MathEvaluator::getQuantity(const std::string& t)   const { return metaFor(t, &ISigilResolver::getQuantity); }
std::string MathEvaluator::getLabel(const std::string& t)      const { return metaFor(t, &ISigilResolver::getLabel); }
std::string MathEvaluator::getCategory(const std::string& t)   const { return metaFor(t, &ISigilResolver::getCategory); }
std::string MathEvaluator::getVisibility(const std::string& t) const { return metaFor(t, &ISigilResolver::getVisibility); }
std::string MathEvaluator::getFormula(const std::string& t)    const { return metaFor(t, &ISigilResolver::getFormula); }





