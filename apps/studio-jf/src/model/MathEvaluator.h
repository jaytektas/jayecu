#pragma once

#include "ExprAst.h"
#include "ISigilResolver.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// MathEvaluator — expression engine ported from omnidyno (Qt-free). Parses an arithmetic/logic
// expression (tokenize -> shunting-yard -> postfix), resolving each bracketed sigil token
// ([#name] / [$name] / [@name]) back to its registered provider by the leading sigil char.
// Singleton; providers register themselves once at startup.
//
//   MathEvaluator::instance().evaluate("[$rpm] > 4000 && [#etb.enabled] == 1")
//
// Supported: + - * / ^, comparisons (== != > >= < <=), logic (&& ||), unary !, parens, and the
// functions sin cos sqrt abs min max floor(x) mod(a,b) select(cond,a,b) clamp(x,lo,hi) bit(w,k)
// lerp(x,x0,y0,x1,y1). `RAW` yields rawValue. mod() takes its sign from the divisor.
class MathEvaluator {
public:
    static MathEvaluator& instance();

    void   registerResolver(ISigilResolver* resolver);
    // ---- TEMPLATE ELEMENT SCOPE ---------------------------------------------------------------
    // A template page names its array element as "[*]" — trigger.streams[*].slots — and the viewport
    // showing it supplies which element. Resolution belongs HERE, at the one point a path becomes a
    // value, and not in each caller: it was applied to the signalName property only, so a template
    // page's BINDING resolved and its condition did not, and every expression property added later
    // (rowCount, ranges, a data source) would have needed its own copy of the same substitution.
    //
    // Scoped rather than passed, because the evaluator is reached through a dozen call sites that have
    // no business knowing about templates. A widget declares which element it is about for the duration
    // of its own evaluation; outside a scope there is no placeholder to resolve and nothing changes.
    struct ElementScope {
        explicit ElementScope(std::string key);
        ~ElementScope();
        ElementScope(const ElementScope&) = delete;
        ElementScope& operator=(const ElementScope&) = delete;
    private:
        std::string m_prev;
    };
    static const std::string& elementContext();

    // "$*" -> the element's PRIMARY telemetry channel. "[*]" splices the element's config KEY (an array
    // subscript); a sensor's telemetry channel is a FLAT name that is usually the same string but not
    // always (boost_pressure's channel is boost_kpa, flex_fuel's is ethanol). This hook maps a config key
    // to that channel — installed by the app from the meta's element_signals — so "$*" resolves correctly
    // for every sensor. Unset (or returning the key unchanged) leaves "$*" as "$<key>".
    static void setPrimarySignalResolver(std::function<std::string(const std::string& key)> fn);
    // "$~" — the element's RAW INPUT channel, the counterpart to "$*". A sensor page has to show both:
    // the number arriving on the pin and what the calibration makes of it, because a calibration is a
    // claim about the relationship between the two and it cannot be checked while only one end is on
    // screen. Resolved by the app, which knows a sensor's interface and pin (MetaModel::hwPoolSignals).
    static void setRawSignalResolver(std::function<std::string(const std::string& key)> fn);

    // canframes(bus) — HOW MANY FRAMES A BUS CARRIES. A page needs it to say "this bus has been
    // committed": its bit rate is editable while nothing is on it and locked once something is,
    // because every frame on a bus was written for the rate the bus was at, and changing it under
    // them breaks all of them at once with no error anywhere — a bus at the wrong rate never ACKs,
    // which reads as nothing being plugged in.
    //
    // A HOOK, not knowledge: the evaluator cannot walk a config array and has no idea what a frame
    // is. The app installs the counter, exactly as it installs the two signal resolvers above.
    static void setCanFrameCounter(std::function<int(int bus)> fn);

    // "[*]" -> the element named by `context`, in any text that carries a path. THE substitution — the
    // evaluator applies it with the scope in force, CanvasWidget with a widget's own element, and the
    // expression builder with the page's, so a preview resolves exactly what the page will.
    static std::string resolveTemplate(const std::string& text, const std::string& context);

    // "name[@<expr>]" -> "name[<idx>]": evaluate <expr> and splice its integer result as a LITERAL
    // subscript. The read-a-value sibling of resolveTemplate's "[*]" (which splices THIS widget's
    // element) — [@expr] splices an index COMPUTED from config, so a grid can bind a whole column to a
    // per-element field of whichever element ANOTHER array selects. e.g. firing_order[i].cyl is the
    // 1-based cylinder that fires i-th and cyl[] is 0-based, so "cyl[@engine.firing_order[0].cyl - 1]
    // .tdc_angle" points a cell at whichever cylinder fires first (the -1 lives in the binding). It is
    // a PATH rewrite resolved when a binding is (re)resolved, NOT a per-frame read: the address is
    // computed once and every consumer (read/write/meta/bounds) sees the same literal path.
    //
    // "[@" is the index operator only in SUBSCRIPT position — right after a name or a closing ']'. A
    // bare "[@name]" at operand position is the '@' widget sigil (SigilResolvers) and passes through;
    // the char before "[@" tells them apart. The inner may hold its own subscript (firing_order[0]),
    // so the closing ']' is matched by DEPTH. A negative/out-of-range result (an unused slot reads 0 ->
    // -1) yields "": the caller reads an empty path as an unbound, blank cell — a firing position no
    // cylinder fills.
    std::string resolveIndexed(const std::string& text) const;

    // The FIELD a "name[@<expr>]" binding names, as a concrete path — every "[@…]" subscript pinned to
    // element 0 instead of being evaluated. A field's metadata (decimals, unit, range) is the SAME for
    // every element, so this answers "what field is this?" even when the live index is unresolved. On an
    // unused firing-order slot resolveIndexed() returns "" (blank value, correct) — but the cell must
    // still format itself as a Bank (0 decimals -> "0"), not fall back to a generic "0.00". Same depth-
    // aware subscript rules as resolveIndexed; text with no "[@…]" comes back unchanged.
    std::string fieldPath(const std::string& text) const;

    // The ELEMENT KEY inside a context, whatever form the context arrived in. The studio's own tools
    // produce more than one: the expression builder yields "[#trigger.streams[0]]", a picker a bare
    // path "trigger.streams[0]", a hand-typed id just "clt". All three name the same element and all
    // three must give "0" / "clt".
    //
    // It existed twice, and the two disagreed — one took the FIRST bracket and one the LAST. On a
    // builder-produced context the first-bracket copy returned "#trigger.streams[0", so bindings
    // resolved (they used the other copy) while every expression silently did not. A page that mostly
    // worked, with the widgets driven by expressions quietly empty.
    static std::string elementKey(const std::string& context);

    double resolveSigil(const std::string& token) const;
    double evaluate(const std::string& expression) const;


    std::string getUnit(const std::string& token) const;
    std::string getQuantity(const std::string& token) const;
    std::string getLabel(const std::string& token) const;
    std::string getCategory(const std::string& token) const;
    std::string getVisibility(const std::string& token) const;
    std::string getFormula(const std::string& token) const;


    // Parsed form, kept per expression STRING. A binding or a condition is a constant that gets
    // evaluated every frame for every control that carries it, so parsing it each time is work done
    // over and over for an answer that cannot change. Only the VALUES change, and those are read at
    // evaluation. Cleared if it ever grows unreasonable, so a generated expression storm cannot leak.
    mutable std::unordered_map<std::string, expr_ast::NodePtr> m_astCache;
    // Walk the SHARED AST (ExprAst.h) resolving sigils. This class is the HOST back end of the one
    // expression language; ExprCompiler is the firmware back end. There is no second grammar.
    double evalNode(const expr_ast::Node& n) const;

private:
    MathEvaluator() = default;

    std::string metaFor(const std::string& token,
                        std::string (ISigilResolver::*getter)(const std::string&) const) const;

    std::unordered_map<char, ISigilResolver*> m_resolvers;

    // WHICH RESOLVER OWNS A BARE TOKEN — asked once, not every frame.
    //
    // A token nobody claims falls back to "ask everyone and take the first non-zero answer", and one of
    // those askings is the widget resolver, whose lookup walks every placed widget (its own provides()
    // says so: ~4ms). Thirty-four unclaimed tokens on a page therefore cost 400ms A FRAME — the studio
    // ran at 2fps and its telemetry poll, which is serviced by the same loop, fell to 2Hz with it.
    //
    // Ownership is a property of the token, the loaded meta and the placed widgets — not of the values,
    // which is all that changes between frames. So it is remembered: the resolver that claimed it, or
    // null for "nobody, do not walk again". Cleared when the resolver set changes (see registerResolver)
    // and when the meta does (Cache::setMeta), which are the only two things that can change the answer.
    mutable std::unordered_map<std::string, ISigilResolver*> m_ownerCache;

public:
    // Drop the ownership memo — a new meta, or a resolver gaining/losing what it can answer for.
    void invalidateOwners() const { m_ownerCache.clear(); }
};
