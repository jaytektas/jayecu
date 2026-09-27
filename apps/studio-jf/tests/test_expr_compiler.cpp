// The studio's expression compiler, cross-checked against the FIRMWARE'S OWN VM.
//
// This is the join between the two halves of the evaluator: the studio compiles source text
// to bytecode, and the ECU executes it. A test that only checked the compiler against itself
// would pass happily while the firmware read the bytes differently — so every program here is
// compiled by ExprCompiler and then RUN by expr::exec() from firmware/Signal/Expr.h, the same
// header the M7 compiles. Agreement is the property under test, not either half alone.
#include "model/ExprCompiler.h"
#include "model/MetaModel.h"
#include "model/ExprAst.h"
#include "Signal/Expr.h"          // the firmware's executor + SignalBus
#include "signal_ids.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-64s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

static MetaModel  g_meta;
static SignalBus  g_bus;
static std::vector<uint8_t> g_cfg;

// Compile, then RUN on the firmware VM. Returns the boolean a DTC gate would see.
static bool runSrc(const std::string& src, bool* compiled = nullptr, std::string* err = nullptr) {
    const auto r = ExprCompiler::compile(src, g_meta, (uint32_t)g_cfg.size());
    if (compiled) *compiled = r.ok;
    if (err) *err = r.error;
    if (!r.ok) return false;
    expr::Ctx c;
    c.bus = &g_bus;
    c.cfg = g_cfg.data();
    c.cfg_size = (uint32_t)g_cfg.size();
    c.now_ms = 10000;
    // TABLES, STUBBED TO THEIR OWN ID. What is under test here is the JOIN: that a table NAME compiles
    // to the id the VM will look up. A hook returning the id itself makes that visible in the result —
    // "table(generic_tables_table_1) == 159" is the compiler and the VM agreeing about which table that
    // is. What a real table interpolates to is the firmware's own test (tests/test_expr_vm.cpp), run
    // against real config; dragging g_config in here would test that instead of this.
    c.table       = [](uint16_t id, float& out, void*) { out = (float)id; return true; };
    c.curve       = [](uint16_t id, float x, float& out, void*) { out = (float)id + x; return true; };
    c.curve_user  = &g_bus;
    c.curve_count = 4096;
    return expr::eval_bool(r.code.data(), (uint16_t)r.code.size(), c, /*on_invalid=*/true);
}

static bool compiles(const std::string& src, std::string* err = nullptr) {
    const auto r = ExprCompiler::compile(src, g_meta, (uint32_t)g_cfg.size());
    if (err) *err = r.error;
    return r.ok;
}

int main(int argc, char** argv) {
    const char* cands[] = { argc > 1 ? argv[1] : nullptr, META_PATH,
                            "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : cands) if (c && g_meta.loadFile(c)) { loaded = true; break; }
    // NOT A SKIP. A test that cannot find the thing it tests has not passed — and this one returned 0
    // when it could not, which under ctest is indistinguishable from every case below succeeding.
    if (!loaded) { std::printf("[expr] FAIL: no meta at %s\n", META_PATH); return 1; }
    if (g_meta.signalMap().empty()) { std::puts("[expr] (meta has no signals — skipped)"); return 0; }

    g_cfg.assign((size_t)g_meta.configSize(), 0);
    std::puts("=== Expression compiler (studio) x VM (firmware) ===");

    // --- the live values every case below reads ---
    g_bus.set(SIG_RPM, 3000.0f, true, 10000);
    g_bus.set(SIG_MAP, 40.0f,   true, 10000);
    g_bus.set(SIG_TPS, 85.0f,   true, 10000);
    g_bus.set(SIG_CLT, 90.0f,   true, 10000);

    std::puts("\n-- grammar and precedence --");
    ck(runSrc("rpm > 2500"), "rpm > 2500");
    ck(!runSrc("rpm > 4000"), "rpm > 4000 is false");
    ck(runSrc("rpm > 2500 and (map > 50 or tps > 80)"),
       "rpm > 2500 and (map > 50 or tps > 80)");
    // The SAME three tests, grouped the other way, must differ — the whole reason for the VM.
    ck(!runSrc("(rpm > 4000 and map > 50) or tps > 90"),
       "(rpm > 4000 and map > 50) or tps > 90 is false");
    ck(runSrc("rpm > 4000 or tps > 80"), "or binds looser than and");
    ck(runSrc("not (rpm > 4000)"), "not (rpm > 4000)");
    ck(runSrc("rpm > 2500 && tps > 80"), "&& spelled with symbols");
    ck(runSrc("rpm > 4000 || tps > 80"), "|| spelled with symbols");
    ck(runSrc("!(rpm > 4000)"), "! spelled with a symbol");

    std::puts("\n-- arithmetic --");
    ck(runSrc("map + 20 > 50"), "map + 20 > 50");
    ck(runSrc("map * 2 == 80"), "map * 2 == 80");
    ck(runSrc("rpm / 1000 >= 3"), "rpm / 1000 >= 3");
    ck(runSrc("-map < 0"), "unary minus");
    ck(runSrc("rpm - 500 > map * 10"), "mixed precedence: rpm - 500 > map * 10");
    ck(runSrc("clt > 89.5"), "fractional literal (x100 fixed point)");

    std::puts("\n-- functions --");
    ck(runSrc("min(rpm, 100) == 100"), "min");
    ck(runSrc("max(rpm, 100) == 3000"), "max");
    ck(runSrc("abs(0 - map) == 40"), "abs");
    ck(runSrc("clamp(rpm, 0, 2000) == 2000"), "clamp");
    ck(runSrc("select(tps > 80, 1, 0) == 1"), "select");
    ck(runSrc("bit(6, 1)"), "bit(6, 1) — bit 1 of 0b110");
    ck(!runSrc("bit(6, 0)"), "bit(6, 0) is clear");
    ck(runSrc("age(map) < 1000"), "age(channel)");

    std::puts("\n-- tables, by name --");
    {
        // A TABLE IS NAMED, not numbered. The id is the table's place in the schema's own order, so a
        // program written against a number would silently mean a different table the day a table is
        // added — and nobody reading "interp(37, clt)" could say which one it was to begin with.
        const int gid = g_meta.tableId("generic_tables_table_1");
        ck(gid >= 0, "the meta carries the table registry the VM switches over",
           std::to_string(g_meta.tableRegistry().size()) + " tables");
        ck(runSrc("table(generic_tables_table_1) == " + std::to_string(gid)),
           "table(<name>) reads that table at its own axes");
        ck(runSrc("interp(generic_tables_table_1, 0) == " + std::to_string(gid)),
           "interp(<name>, x) reads the same table at an x you supply");
        bool compiled = true; std::string err;
        runSrc("table(no_such_table_here)", &compiled, &err);
        ck(!compiled, "a table this firmware does not have is a compile ERROR, not an id", err);
        // The generic pool is the point of the feature: eight tables an expression can read by name.
        ck(g_meta.tableId("generic_tables_table_8") >= 0, "the whole generic pool is nameable");
    }

    std::puts("\n-- sigil forms (the studio's existing spelling) --");
    ck(runSrc("[$rpm] > 2500"), "[$rpm] > 2500");
    ck(runSrc("[$rpm] > 2500 and [$tps] > 80"), "two bracketed channels");

    std::puts("\n-- config settings, by meta path --");
    {
        // Write a real setting into the image, then gate on it. This is the case the old
        // four-slot encoding could not do AT ALL: it compared a channel to a constant only.
        const std::string path = "sensors.sensor[clt].diag_op_max";
        const MetaModel::Location L = g_meta.locate(path);
        if (L.valid()) {
            const int16_t raw = 500;                       // stored raw; scale applies below
            std::memcpy(&g_cfg[(size_t)L.offset], &raw, sizeof(raw));
            const double eng = raw * L.scale;
            ck(runSrc("clt > [#" + path + "] - " + std::to_string((int)(eng - 10))),
               "clt > [#sensors.sensor[clt].diag_op_max] - k",
               "eng=" + std::to_string(eng));
            ck(runSrc("[#" + path + "] == " + std::to_string(eng)),
               "a setting reads back in ENGINEERING units (scale applied)");
        } else {
            ck(false, "resolve sensors.sensor[clt].diag_op_max");
        }
    }
    {
        // A packed bit group — a sensor's per-check enable flag. One bit out of a byte,
        // addressed by name, baked as PUSH_BCFG.
        const std::string grp = "sensors.sensor[clt].diag_enable.raw_min";
        if (g_meta.bitGroupOf(grp)) {
            const int off = g_meta.locate("sensors.sensor[clt].diag_enable").offset;
            g_cfg[(size_t)off] = 0x01;                    // bit 0 set
            ck(runSrc("[#" + grp + "]"), "a 1-bit group reads as true when set");
            g_cfg[(size_t)off] = 0x02;                    // bit 0 clear, neighbour set
            ck(!runSrc("[#" + grp + "]"), "…and false when clear, unaffected by its neighbour");
        } else {
            ck(false, "meta exposes bit groups");
        }
    }

    std::puts("\n-- fail-to-false: a dead channel cannot arm a gate --");
    {
        SignalBus fresh;                                   // nothing published at all
        SignalBus saved = g_bus;
        g_bus = fresh;
        ck(!runSrc("map > 50"), "a never-written channel reads false");
        ck(!runSrc("not (map > 50)"), "…and NOT does not invert it into true");
        g_bus = saved;
        g_bus.set(SIG_MAP, 40.0f, true, 10000);
        ck(runSrc("map > 50 or tps > 80"), "a live clause still carries an OR");
    }

    std::puts("\n-- errors are reported, not silently mis-compiled --");
    {
        std::string err;
        ck(!compiles("rpm > ", &err), "truncated expression rejected", err);
        ck(!compiles("nosuchchannel > 5", &err), "unknown channel rejected", err);
        ck(err.find("nosuchchannel") != std::string::npos, "…and the message names it", err);
        ck(!compiles("[#no.such.setting] > 5", &err), "unknown setting rejected", err);
        ck(!compiles("rpm > 2500)", &err), "unbalanced paren rejected", err);
        ck(!compiles("(rpm > 2500", &err), "unclosed paren rejected", err);
        ck(!compiles("rpm 2500", &err), "two operands with no operator rejected", err);
        ck(!compiles("min(rpm)", &err), "wrong arity rejected", err);
        ck(!compiles("rpm > 2500 @ 3", &err), "junk character rejected", err);
    }

    std::puts("\n-- an empty expression is the always-armed program --");
    {
        const auto r = ExprCompiler::compile("   ", g_meta, (uint32_t)g_cfg.size());
        ck(r.ok && r.code.size() == 1 && r.code[0] == expr::OP_END, "blank source -> single OP_END");
        expr::Ctx c; c.bus = &g_bus; c.cfg = g_cfg.data(); c.cfg_size = (uint32_t)g_cfg.size();
        ck(expr::eval_bool(r.code.data(), 1, c, true), "…and the firmware reads it as always armed");
    }

    std::puts("\n-- size: a realistic gate fits the 64-byte block --");
    {
        const auto r = ExprCompiler::compile(
            "map > 50 and rpm > 2500 and clt > 60 and tps > 10", g_meta, (uint32_t)g_cfg.size());
        ck(r.ok && r.code.size() <= expr::PROGRAM_MAX,
           "four-clause gate fits", std::to_string(r.code.size()) + " bytes");
        std::printf("       (four-clause gate: %zu bytes of %u)\n",
                    r.code.size(), (unsigned)expr::PROGRAM_MAX);
        const auto big = ExprCompiler::compile(
            "map > 1 and map > 2 and map > 3 and map > 4 and map > 5 and map > 6 and "
            "map > 7 and map > 8 and map > 9 and map > 10 and map > 11 and map > 12",
            g_meta, (uint32_t)g_cfg.size());
        ck(!big.ok, "an over-long program is REFUSED, not truncated", big.error);
    }

    std::puts("\n-- HYSTERESIS without state: the site publishes its output, the gate reads it --");
    {
        // The VM is pure — it cannot remember last frame. But hysteresis does not actually need the
        // VM to remember anything: it needs the CURRENT OUTPUT as an input. A site that publishes
        // its own state to the bus makes the rule expressible with no stateful opcode at all:
        //
        //     clt > 95 or (fan_on and clt > 90)
        //       off -> on  above 95
        //       on  -> off below 90
        //
        // `etb_state_1` stands in here for whatever channel the site publishes; the shape is what
        // matters. The memory lives in the bus, where it is inspectable, not inside the expression.
        const std::string src = "clt > 95 or (etb_state_1 and clt > 90)";
        const auto r = ExprCompiler::compile(src, g_meta, (uint32_t)g_cfg.size());
        ck(r.ok, "a hysteresis rule compiles", r.error);
        if (r.ok) {
            auto run = [&](float clt, bool state) {
                SignalBus b;
                b.set(SIG_CLT, clt, true, 1000);
                b.set(SIG_ETB_STATE_1, state ? 1.0f : 0.0f, true, 1000);
                expr::Ctx c;
                c.bus = &b; c.cfg = g_cfg.data(); c.cfg_size = (uint32_t)g_cfg.size(); c.now_ms = 1000;
                return expr::eval_bool(r.code.data(), (uint16_t)r.code.size(), c, true);
            };
            // Rising: nothing happens until 95, and it does NOT come on at 92 from cold.
            ck(!run(80.f, false), "cold and off -> stays off");
            ck(!run(92.f, false), "…still off between the thresholds while OFF");
            ck( run(96.f, false), "…switches on above the upper threshold");
            // Falling: once on, it HOLDS through the band — that is the hysteresis.
            ck( run(92.f, true),  "on and in the band -> HOLDS on");
            ck( run(91.f, true),  "…still holding just above the lower threshold");
            ck(!run(89.f, true),  "…and releases below the lower threshold");
            // The band is what stops the chatter a single threshold would produce.
            ck(run(92.f, true) != run(92.f, false),
               "the SAME temperature gives a different answer by state — no chatter at one line");
        }
    }

    std::puts("\n-- round trip: compile -> decompile -> recompile -> identical bytes --");
    {
        const char* srcs[] = {
            "rpm > 2500",
            "rpm > 2500 and (map > 50 or tps > 80)",
            "(rpm > 2500 or map > 50) and tps > 80",
            "not (rpm > 4000)",
            "map + 20 > 50",
            "map - (rpm - 100) > 0",
            "min(rpm, 100) == 100",
            "clamp(rpm, 0, 2000) == 2000",
            "select(tps > 80, 1, 0) == 1",
            "abs(0 - map) == 40",
            "bit(6, 1)",
            "age(map) < 1000",
            "clt > 89.5",
        };
        for (const char* src : srcs) {
            const auto a = ExprCompiler::compile(src, g_meta, (uint32_t)g_cfg.size());
            if (!a.ok) { ck(false, std::string("compile: ") + src, a.error); continue; }
            const std::string text = ExprCompiler::decompile(a.code.data(),
                                                             (uint16_t)a.code.size(), g_meta);
            const auto b = ExprCompiler::compile(text, g_meta, (uint32_t)g_cfg.size());
            const bool same = b.ok && b.code == a.code;
            ck(same, std::string("round trip: ") + src,
               same ? "" : ("-> \"" + text + "\"" + (b.ok ? " (bytes differ)" : " " + b.error)));
        }
    }
    {
        // Grouping must survive the round trip — this is exactly where a decompiler that
        // forgets parentheses turns one gate into a different one.
        const auto a = ExprCompiler::compile("rpm > 2500 and (map > 50 or tps > 80)",
                                             g_meta, (uint32_t)g_cfg.size());
        const std::string text = ExprCompiler::decompile(a.code.data(),
                                                         (uint16_t)a.code.size(), g_meta);
        ck(text.find('(') != std::string::npos, "decompile keeps the parentheses", text);
        std::printf("       (decompiled: %s)\n", text.c_str());
    }
    {
        // A SCALED setting must round-trip. The compiler emits an implicit `* scale` after a
        // scaled field (config is stored raw, the bus is in engineering units), and a decompiler
        // that rendered that multiply as source would produce an expression which, recompiled,
        // applies the scale TWICE — a silently different gate, not a visibly broken one.
        int done = 0;
        for (const auto& [path, f] : g_meta.config()) {
            if (f.scale == 1.0 || f.scale == 0.0 || f.datatype == "ASCII" || f.datatype == "EXPR")
                continue;
            const std::string src = "[#" + path + "] > 5";
            const auto a = ExprCompiler::compile(src, g_meta, (uint32_t)g_cfg.size());
            if (!a.ok) continue;
            const std::string text = ExprCompiler::decompile(a.code.data(),
                                                             (uint16_t)a.code.size(), g_meta);
            const auto b = ExprCompiler::compile(text, g_meta, (uint32_t)g_cfg.size());
            ck(b.ok && b.code == a.code, "scaled setting round-trips: " + path,
               "-> \"" + text + "\"");
            ck(text.find('*') == std::string::npos,
               "…with no stray scale multiply in the source", text);
            ++done;
        }
        ck(done > 0, "the meta actually has a scaled field to test");
        std::printf("       (%d scaled field%s round-tripped)\n", done, done == 1 ? "" : "s");
    }
    {
        // A SCALE FINER THAN A HUNDREDTH. This is the case the loop above used to stop short of: it
        // checked the first three scaled fields it met and broke, and the 0.001 ones sort later.
        //
        // OP_PUSH_F stores an i32 of HUNDREDTHS, so 0.001 encoded as llround(0.1) = 0 and every
        // program reading such a field multiplied it by nothing. On the bench that was a fuel pump
        // whose "uptime_s < prime_s" had become "uptime_s < 0" — false for ever, no error raised,
        // because multiplying by zero is a perfectly valid program. Asserted on the BYTES, not on
        // the round-trip, so it fails at the point the wrong number is written.
        int checked = 0;
        for (const auto& [path, f] : g_meta.config()) {
            if (f.datatype == "ASCII" || f.datatype == "EXPR") continue;
            if (f.scale == 0.0 || std::llround(f.scale * 100.0) != 0 || f.scale == 1.0) continue;
            const auto a = ExprCompiler::compile("[#" + path + "] > 5", g_meta, (uint32_t)g_cfg.size());
            if (!a.ok) continue;
            // Find the literal the compiler emitted for the scale and read it back as the VM would.
            double emitted = -1.0;
            for (size_t i = 0; i + 5 <= a.code.size(); ++i) {
                if (a.code[i] == expr::OP_PUSH_F32 && a.code[i + 5] == expr::OP_MUL) {
                    float v; std::memcpy(&v, a.code.data() + i + 1, 4); emitted = v; break;
                }
                if (a.code[i] == expr::OP_PUSH_F && a.code[i + 5] == expr::OP_MUL) {
                    int32_t v; std::memcpy(&v, a.code.data() + i + 1, 4); emitted = v / 100.0; break;
                }
            }
            ck(std::abs(emitted - f.scale) < 1e-9,
               "a scale finer than 0.01 survives the literal: " + path,
               "scale " + std::to_string(f.scale) + " emitted as " + std::to_string(emitted));
            ++checked;
        }
        ck(checked > 0, "the meta actually has a sub-hundredth scale to test");
        std::printf("       (%d sub-hundredth-scaled field%s checked)\n", checked, checked == 1 ? "" : "s");
    }
    {
        // A config read decompiles back to its meta PATH, not a raw offset — that is what
        // makes an ECU read cold still readable.
        const std::string path = "sensors.sensor[clt].diag_op_max";
        const auto a = ExprCompiler::compile("clt > [#" + path + "]", g_meta,
                                             (uint32_t)g_cfg.size());
        const std::string text = ExprCompiler::decompile(a.code.data(),
                                                         (uint16_t)a.code.size(), g_meta);
        ck(text.find(path) != std::string::npos, "a setting decompiles back to its path", text);
        std::printf("       (decompiled: %s)\n", text.c_str());
    }

    {
        // A UNIT ON THE LITERAL, folded against what it is compared WITH. Written down, a threshold is
        // checkable; bare, it means "whatever the other side happens to be in" and nothing says so —
        // which is how a fuel-pump template ended up with a stop time that had to be milliseconds
        // because age() is, with no way for anyone to tell.
        struct UCase { const char* src; const char* same_as; const char* why; };
        const UCase cases[] = {
            // age() answers in ms, so both of these are the same program.
            { "age(trigger_teeth) < 1500ms", "age(trigger_teeth) < 1500", "ms against age() is ms" },
            { "age(trigger_teeth) < 1.5s",   "age(trigger_teeth) < 1500", "seconds convert into it" },
            { "age(trigger_teeth) < 1500",   "age(trigger_teeth) < 1500", "a bare number is left alone" },
            // uptime_s is declared in seconds, so the conversion runs the other way.
            { "uptime_s < 3s",               "uptime_s < 3",              "s against a seconds channel" },
            { "uptime_s < 3000ms",           "uptime_s < 3",              "ms converts down to it" },
            // …and either side of the operator.
            { "1.5s > age(trigger_teeth)",   "1500 > age(trigger_teeth)", "the literal may be on the left" },
        };
        for (const UCase& c : cases) {
            const auto a = ExprCompiler::compile(c.src, g_meta, (uint32_t)g_cfg.size());
            const auto b = ExprCompiler::compile(c.same_as, g_meta, (uint32_t)g_cfg.size());
            ck(a.ok && b.ok && a.code == b.code,
               std::string("units: ") + c.src + "  ==  " + c.same_as,
               std::string(c.why) + (a.ok ? "" : " [" + a.error + "]"));
        }
        // A MISMATCH IS AN ERROR, not a guess. Volts against milliseconds has no conversion, and
        // inventing one would be worse than the comparison being plainly wrong.
        const auto bad = ExprCompiler::compile("age(trigger_teeth) < 5V", g_meta, (uint32_t)g_cfg.size());
        ck(!bad.ok, "units: a different quantity is refused, not assumed", bad.error);
        const auto nonsense = ExprCompiler::compile("uptime_s < 5x", g_meta, (uint32_t)g_cfg.size());
        ck(!nonsense.ok, "units: an unknown suffix is refused, not dropped", nonsense.error);
        // A SOURCE ROUND-TRIP keeps the unit — the editor must show back what was typed until the
        // program is compiled, or a threshold silently loses the only thing that documented it.
        std::string perr; int ppos = -1;
        const auto ast = expr_ast::parse("age(trigger_teeth) < 1500ms", perr, ppos);
        ck(ast && expr_ast::unparse(*ast) == "age(trigger_teeth) < 1500ms",
           "units: a literal's unit survives parse -> unparse",
           ast ? expr_ast::unparse(*ast) : perr);
    }

    // ---- an offset in a NUMERIC-element array decompiles to a PATH, not a raw address -----------
    {
        // The compiler emits a byte offset; the editor shows the expression by mapping it back. That
        // mapping walked an array's elementIds, and only arrays whose elements are NAMED have any —
        // sensors have "clt", outputs have nothing. So every reference into an output, a cylinder, a
        // trigger stream came back as "config@119348": an address the reader cannot check and cannot
        // edit, in the one view whose whole job is to show what an expression says.
        const MetaModel::ConfigArray* arr = nullptr;
        std::string aname;
        for (const auto& [n, a] : g_meta.configArrays())
            if (a.elementIds.empty() && a.count > 1 && !a.fields.empty()) { arr = &a; aname = n; break; }
        if (!arr) {
            std::puts("  (no numeric-element config array in this meta — skipped)");
        } else {
            // The SECOND element, so an index of 0 cannot pass by accident.
            const auto& f  = arr->fields.front();
            const int   off = arr->baseOffset + arr->stride + f.relOffset;
            const std::string want = aname + "[1]." + f.name;
            const std::string got  = ExprCompiler::configPathAt(g_meta, off, nullptr);
            ck(got == want, "an unnamed array element decompiles to its path", got + " vs " + want);
            ck(got.rfind("config@", 0) != 0, "…and not to a raw byte address", got);
        }
    }

    // ---- THE MANUAL'S EXAMPLES COMPILE ------------------------------------------------------------
    // Chapters 29 and 34 print these as things to type. An example the ECU would refuse is worse than
    // none, so every one is compiled here against the real meta; a rename that breaks one fails this.
    {
        static const char* kManual[] = {
            "clt > 95 and rpm > 800",
            "oil_pressure < 100 and rpm > 2000",
            "age(oil_pressure) > 1s",
            "age(oil_pressure) > 1000",
            "fuel_pressure < 250",
            "clt > 85C",
            "clt > 200F",
            "clutch_sw and vehicle_spd < 5",
            "clamp(60 * 13.5 / battery, 0, 100)",
            "map > 150 or tps > 80",
            "not (oil_pressure < 100)",
            "gear == 3",
            "launch.enabled",
            "[#rev_limiter.hard_limit_rpm] > 6000",
            "table(generic_tables_table_1)",
            "interp(generic_tables_table_1, clt)",
            "select(gear == 1, 20, 40)",
            "bit(monitor_flags, 0)",
            "min(tps, 50) + max(0, abs(-3))",
        };
        for (const char* src : kManual) {
            const auto r = ExprCompiler::compile(src, g_meta, (uint32_t)g_cfg.size());
            ck(r.ok, std::string("manual example compiles: ") + src, r.error);
        }
        // …and the ones the manual says the ECU REFUSES really are refused.
        for (const char* src : { "rpm ^ 2", "floor(rpm)", "mod(rpm, 2)", "sqrt(map)" }) {
            const auto r = ExprCompiler::compile(src, g_meta, (uint32_t)g_cfg.size());
            ck(!r.ok, std::string("manual: refused for the ECU: ") + src, r.error);
        }
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All expression compiler tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
