// ONE expression language, two back-ends — and they must agree.
//
// The design doc's rule is "one grammar, one editor, one validator, one decompiler". For a while
// there were two parsers: MathEvaluator's (visibility/data-source) and ExprCompiler's (firmware
// gates). They had already drifted — `and` worked in one and not the other, `clamp` in one and
// `lerp` in the other, and `a == b > c` parsed at different precedences. Both now share
// ExprAst; this test is what keeps them sharing it.
//
//   cmake --build build --target expr_unified_test && ./build/expr_unified_test
#include "model/ExprAst.h"
#include "model/ExprCompiler.h"
#include "model/MathEvaluator.h"
#include "model/MetaModel.h"
#include "model/Cache.h"
#include "model/ISigilResolver.h"

#include "Signal/Expr.h"
#include "signal_ids.h"

#include <cmath>
#include <map>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-62s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// A stand-in for the telemetry resolver: the host back end reads channels through whatever
// resolver owns them, so a controlled map is the right stub — this test is about the language and
// the two back-ends agreeing, not about Cache plumbing.
struct TestChannels : ISigilResolver {
    std::map<std::string, double> v;
    char sigil() const override { return '$'; }
    double resolveSigil(const std::string& n) const override {
        auto it = v.find(n);
        return it == v.end() ? 0.0 : it->second;
    }
    bool provides(const std::string& n) const override { return v.count(n) != 0; }
    std::vector<std::string> available() const override {
        std::vector<std::string> o;
        for (const auto& [k, x] : v) { (void)x; o.push_back(k); }
        return o;
    }
};
static TestChannels g_chans;

static bool parses(const std::string& src, std::string* err = nullptr) {
    std::string e; int pos = -1;
    auto n = expr_ast::parse(src, e, pos);
    if (err) *err = e;
    return n != nullptr;
}

int main(int argc, char** argv) {
    std::puts("=== One grammar, two back-ends ===");

    std::puts("\n-- the grammar is ONE grammar: both spellings, all functions, one precedence --");
    ck(parses("rpm > 2500 and map > 50"),  "`and` parses");
    ck(parses("rpm > 2500 && map > 50"),   "`&&` parses");
    ck(parses("not (rpm > 2500)"),         "`not` parses");
    ck(parses("!(rpm > 2500)"),            "`!` parses");
    ck(parses("clamp(rpm, 0, 8000) > 5"),  "clamp() parses (was firmware-only)");
    ck(parses("lerp(rpm, 0, 0, 8000, 100) > 5"), "lerp() parses (was host-only)");
    ck(parses("sqrt(map) > 5"),            "sqrt() parses (was host-only)");
    ck(parses("age(map) < 500"),           "age() parses (was firmware-only)");
    ck(parses("2 ^ 3 > 5"),                "`^` parses");
    ck(parses("[@widget.rowcount] > 0"),   "the studio-only @ sigil parses");
    ck(parses("[%app.connected] == 1"),    "the studio-only % sigil parses");
    ck(parses("electronic_throttle.etb[0].enabled == 1"),
       "a bare config path with a subscript is ONE identifier");

    {
        // The precedence that used to differ between the two parsers. C's rule: equality is LOOSER
        // than relational, so this groups as `1 == (2 > 3)` = `1 == 0` = false, NOT `(1 == 2) > 3`.
        std::string e; int p = -1;
        auto n = expr_ast::parse("1 == 2 > 3", e, p);
        ck(n && n->kind == expr_ast::Kind::Binary && n->text == "==",
           "equality binds LOOSER than relational (C precedence)",
           n ? n->text : e);
    }

    std::puts("\n-- errors name the offending token, with a caret position --");
    {
        std::string e;
        ck(!parses("rpm > ", &e), "truncated expression rejected", e);
        ck(!parses("min(rpm)", &e), "wrong arity rejected", e);
        ck(e.find("min") != std::string::npos && e.find("2") != std::string::npos,
           "…and the message says which function and how many args", e);
        ck(!parses("rpm @@ 3", &e), "junk rejected", e);
    }

    std::puts("\n-- unparse round-trips through the SHARED renderer --");
    for (const char* src : {"rpm > 2500 and (map > 50 or tps > 80)",
                            "not (rpm > 4000)",
                            "map - (rpm - 100) > 0",
                            "1 == 2 > 3",
                            "2 ^ 3 ^ 2 > 5",
                            "clamp(rpm, 0, 8000) > 5",
                            "[@a.b] > 0 and [%c.d] == 1"}) {
        std::string e; int p = -1;
        auto a = expr_ast::parse(src, e, p);
        if (!a) { ck(false, std::string("parse: ") + src, e); continue; }
        const std::string text = expr_ast::unparse(*a);
        auto b = expr_ast::parse(text, e, p);
        const bool same = b && expr_ast::unparse(*b) == text;
        ck(same, std::string("round trip: ") + src, text);
    }

    // The rest needs a meta.
    MetaModel meta;
    const char* cands[] = { argc > 1 ? argv[1] : nullptr,
                            "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    if (!loaded) {
        std::puts("\n[expr-unified] (no meta — back-end agreement checks skipped)");
        std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All unified-grammar tests passed",
                    fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    C.setConfigImage(std::vector<uint8_t>((size_t)meta.configSize(), 0));
    MathEvaluator::instance().registerResolver(&g_chans);

    std::puts("\n-- a back-end refuses what it cannot run, BY NAME, not by parse failure --");
    {
        // Studio-only address spaces compile-refuse with an explanation. The expression is valid;
        // it just cannot run on an ECU. That distinction is the whole point of one grammar.
        auto r = ExprCompiler::compile("[@ve_table.rowcount] > 0", meta, (uint32_t)meta.configSize());
        ck(!r.ok, "a widget-state sigil does not compile for the ECU");
        ck(r.error.find("studio state") != std::string::npos ||
           r.error.find("cannot read") != std::string::npos,
           "…and the error explains WHY, rather than 'syntax error'", r.error);

        r = ExprCompiler::compile("sqrt(map) > 5", meta, (uint32_t)meta.configSize());
        ck(!r.ok, "a host-only function does not compile for the ECU");
        ck(r.error.find("sqrt") != std::string::npos,
           "…and the error names the function", r.error);

        r = ExprCompiler::compile("2 ^ 3 > 5", meta, (uint32_t)meta.configSize());
        ck(!r.ok && r.error.find("^") != std::string::npos,
           "…same for the host-only operator", r.error);

        // And the converse: firmware-only functions still PARSE on the host (one grammar), they
        // simply evaluate to 0 there rather than taking the UI down.
        ck(parses("age(map) < 500"), "a firmware-only function still parses on the host");
        ck(MathEvaluator::instance().evaluate("age(map)") == 0.0,
           "…and evaluates to 0 in the studio rather than throwing");
    }

    std::puts("\n-- the two back-ends AGREE on everything both can run --");
    {
        // Drive real values through both: the host evaluator (via the telemetry resolver reading
        // the Cache) and the firmware VM (executing compiled bytecode against a SignalBus).
        struct Case { const char* src; double rpm, map, tps; };
        const Case cases[] = {
            {"rpm > 2500",                              3000, 40, 85},
            {"rpm > 2500 and map > 50",                 3000, 40, 85},
            {"rpm > 2500 and (map > 50 or tps > 80)",   3000, 40, 85},
            {"(rpm > 2500 or map > 50) and tps > 80",   1000, 60, 85},
            {"not (rpm > 4000)",                        3000, 40, 85},
            {"map + 20 > 50",                           3000, 40, 85},
            {"map * 2 == 80",                           3000, 40, 85},
            {"rpm / 1000 >= 3",                         3000, 40, 85},
            {"min(rpm, 100) == 100",                    3000, 40, 85},
            {"max(map, tps) == 85",                     3000, 40, 85},
            {"clamp(rpm, 0, 2000) == 2000",             3000, 40, 85},
            {"select(tps > 80, 1, 0) == 1",             3000, 40, 85},
            {"abs(0 - map) == 40",                      3000, 40, 85},
            {"1 == 2 > 3",                              3000, 40, 85},
            {"rpm > 2500 == tps > 80",                  3000, 40, 85},
        };
        for (const Case& c : cases) {
            // host side: the resolver answers the channels
            g_chans.v = {{"rpm", c.rpm}, {"map", c.map}, {"tps", c.tps}};
            const double host = MathEvaluator::instance().evaluate(c.src);

            // firmware side: compile, then run on the VM with the same values
            const auto r = ExprCompiler::compile(c.src, meta, (uint32_t)meta.configSize());
            if (!r.ok) { ck(false, std::string("compile: ") + c.src, r.error); continue; }
            SignalBus bus;
            bus.set(SIG_RPM, (float)c.rpm, true, 1000);
            bus.set(SIG_MAP, (float)c.map, true, 1000);
            bus.set(SIG_TPS, (float)c.tps, true, 1000);
            const std::vector<uint8_t>& img = C.configImage();
            expr::Ctx ctx;
            ctx.bus = &bus; ctx.cfg = img.data(); ctx.cfg_size = (uint32_t)img.size(); ctx.now_ms = 1000;
            const bool fw = expr::eval_bool(r.code.data(), (uint16_t)r.code.size(), ctx, true);

            const bool agree = (host != 0.0) == fw;
            ck(agree, std::string("agree: ") + c.src,
               "host=" + std::to_string(host) + " firmware=" + (fw ? "true" : "false"));
        }
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All unified-grammar tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
