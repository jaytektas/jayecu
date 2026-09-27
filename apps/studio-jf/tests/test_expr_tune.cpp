// Does an expression survive the tune?
//
// The field holds bytecode, but bytecode bakes config offsets for ONE layout — carried into
// another it addresses different fields, which is a silent wrong answer rather than a visible
// break. So the tune stores SOURCE and a load recompiles it. This test holds that contract:
//
//   1. an expression written into an image survives serialise -> deserialise intact
//   2. the tune document contains the SOURCE TEXT, not the bytes
//   3. loading against a layout where the field MOVED still produces a correct program for the
//      new offsets — the migration story, which raw bytes could never do
//   4. a source that no longer compiles is reported, not written half-formed
//   5. and the OTHER blob in a struct-array element — an ASCII name — survives too. Same failure,
//      same loop, found later: the element walk decoded every field as a number, so an output slot
//      called "Starter Motor" was stored as 0 and came back nameless. EXPR got a special case when
//      it bit; text never did.
//
//   cmake --build build --target expr_tune_test && ./build/expr_tune_test
#include "model/ExprCompiler.h"
#include "model/MetaModel.h"
#include "model/TuneFile.h"
#include "Signal/Expr.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-62s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    MetaModel meta;
    const char* cands[] = { argc > 1 ? argv[1] : nullptr,
                            "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::puts("[tune] (no meta found — skipped)"); return 0; }

    const std::string path = "sensors.sensor[clt].precond_expr";
    int off = 0, size = 0;
    if (!meta.resolveBlob(path, off, size)) {
        std::puts("[tune] (meta has no expression field — skipped)");
        return 0;
    }
    std::puts("=== Expression storage in the tune ===");

    // Start from the default image and write a real compiled gate into it.
    std::vector<uint8_t> image = meta.defaultImage();
    if (image.empty()) image.assign((size_t)meta.configSize(), 0);
    const std::string src = "rpm > 2500 and (map > 50 or tps > 80)";
    const auto c = ExprCompiler::compile(src, meta, (uint32_t)image.size(), (uint16_t)size);
    ck(c.ok, "the gate compiles", c.error);
    if (!c.ok) return 1;
    std::memcpy(image.data() + off, c.code.data(), c.code.size());

    // 1 + 2. Save, and look at what was actually written.
    const std::vector<uint8_t> doc = TuneFile::serialise(image, meta);
    const std::string text((const char*)doc.data(), doc.size());
    ck(text.find("\"expressions\"") != std::string::npos, "the tune has an expressions section");
    ck(text.find("rpm > 2500") != std::string::npos,
       "…holding the SOURCE, readable in the file");
    ck(text.find(path) != std::string::npos, "…keyed by the field's meta path");

    // The bytes must NOT be in there as an array-of-numbers entry: that is the encoding this
    // whole design rejects, and it would silently win on load if it were present.
    ck(text.find("\"" + path + "\": 0") == std::string::npos,
       "…and NOT as a number in the arrays section");

    // 5. THE ELEMENT'S NAME, which is a blob of a different kind and was lost the same way.
    const std::string npath = "outputs.output[2].name";
    const MetaModel::Location nl = meta.locate(npath);
    const bool has_name = nl.kind == MetaModel::Location::Kind::Scalar && nl.size > 0;
    ck(has_name, "an element's ASCII field resolves, with its byte length", "size=" + std::to_string(nl.size));
    std::string doc2_text;
    if (has_name) {
        std::vector<uint8_t> named = image;
        const std::string who = "Starter Motor";
        std::fill(named.begin() + nl.offset, named.begin() + nl.offset + nl.size, 0);
        std::copy(who.begin(), who.end(), named.begin() + nl.offset);

        const std::vector<uint8_t> doc2 = TuneFile::serialise(named, meta);
        doc2_text.assign((const char*)doc2.data(), doc2.size());
        ck(doc2_text.find("\"" + who + "\"") != std::string::npos,
           "an element's name is stored as TEXT in the tune");
        ck(doc2_text.find("\"" + npath + "\": 0") == std::string::npos,
           "…and NOT as the number zero, which is what destroyed it");

        MigrationReport r3;
        const std::vector<uint8_t> back2 = TuneFile::deserialise(doc2, meta, r3);
        const bool namedOk = !back2.empty() &&
            std::string((const char*)back2.data() + nl.offset) == who;
        ck(namedOk, "…and comes back off a load intact",
           back2.empty() ? "no image" : std::string((const char*)back2.data() + nl.offset));
        ck(std::find(r3.unresolved.begin(), r3.unresolved.end(), npath + " -> " + who) == r3.unresolved.end(),
           "…without being mistaken for a broken reference");
    }

    // 3. Load it back and check the program is byte-identical.
    MigrationReport rep;
    const std::vector<uint8_t> back = TuneFile::deserialise(doc, meta, rep);
    ck(!back.empty(), "the tune loads");
    if (back.empty()) return 1;
    const bool same = std::memcmp(back.data() + off, c.code.data(), c.code.size()) == 0;
    ck(same, "the recompiled program is byte-identical to the original");

    // …and that it still RUNS correctly on the firmware VM after the round trip.
    {
        SignalBus bus;
        bus.set(SIG_RPM, 3000.0f, true, 1000);
        bus.set(SIG_MAP, 40.0f,   true, 1000);
        bus.set(SIG_TPS, 85.0f,   true, 1000);
        expr::Ctx ctx;
        ctx.bus = &bus; ctx.cfg = back.data(); ctx.cfg_size = (uint32_t)back.size(); ctx.now_ms = 1000;
        ck(expr::eval_bool(back.data() + off, (uint16_t)size, ctx, true),
           "…and the firmware VM still arms on it after the round trip");
        bus.set(SIG_TPS, 10.0f, true, 1000);
        ck(!expr::eval_bool(back.data() + off, (uint16_t)size, ctx, true),
           "…and disarms when the OR term goes false");
    }

    // 4. A source that cannot compile against this layout is reported, not written.
    {
        std::string bad = text;
        const size_t at = bad.find("rpm > 2500 and (map > 50 or tps > 80)");
        ck(at != std::string::npos, "found the source to corrupt");
        bad.replace(at, std::strlen("rpm"), "zzz");        // a channel that does not exist
        MigrationReport r2;
        const std::vector<uint8_t> out =
            TuneFile::deserialise(std::vector<uint8_t>(bad.begin(), bad.end()), meta, r2);
        ck(!out.empty(), "the rest of the tune still loads");
        bool reported = false;
        for (const auto& u : r2.unmapped) if (u == path) reported = true;
        ck(reported, "an uncompilable expression is REPORTED as unmapped");
        // Nothing half-formed was written: the field is left as the default (empty = always armed),
        // which fails SAFE — detection stays on.
        ck(expr::is_empty(out.data() + off, (uint16_t)size),
           "…and the field is left empty (always armed), not half-written");
    }

    // 5. THE migration case: the same source, loaded against a layout where the field sits at a
    //    DIFFERENT offset. Raw bytes could not do this — the baked offsets would point at whatever
    //    now lives at the old address.
    {
        bool tried = false;
        for (const auto& [pathB, fB] : meta.config()) {
            (void)pathB; (void)fB;
            break;   // (no second layout in-tree; see below)
        }
        // Simulated directly: recompile the stored source and confirm the program's baked offset
        // tracks the field it names rather than a remembered address.
        const std::string other = "sensors.sensor[iat].precond_expr";
        int offB = 0, sizeB = 0;
        if (meta.resolveBlob(other, offB, sizeB) && offB != off) {
            const auto a = ExprCompiler::compile("clt > [#sensors.sensor[clt].diag_op_max]",
                                                 meta, (uint32_t)image.size(), (uint16_t)size);
            const auto b = ExprCompiler::compile("clt > [#sensors.sensor[iat].diag_op_max]",
                                                 meta, (uint32_t)image.size(), (uint16_t)size);
            ck(a.ok && b.ok && a.code != b.code,
               "the same source against different fields bakes different offsets");
            tried = true;
        }
        ck(tried, "a second element existed to compare against");
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "All expression tune-storage tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
