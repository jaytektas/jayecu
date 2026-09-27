// The UI path, end to end: what the EXPRESSION WIDGET writes is what the FIRMWARE runs.
//
// The compiler is tested against the VM elsewhere; this tests the layer the tuner actually touches.
// It drives the real ExpressionWidget — the same class the dictionary drop instantiates — through
// a real Cache holding a real config image, then hands the resulting bytes to the firmware's own
// expr::exec(). If the widget wrote to the wrong offset, wrote a number instead of bytecode, or
// wrote nothing at all, this fails; a test that stopped at "the compiler works" would not.
//
//   cmake --build build --target expr_widget_test && ./build/expr_widget_test
#include "../src/surface/widgets/ExpressionWidget.h"
#include "../src/surface/PanelModel.h"
#include "../src/surface/Surface.h"
#include "../src/model/Cache.h"
#include "../src/model/ExprCompiler.h"
#include "../src/model/MetaModel.h"

#include "Signal/Expr.h"                 // the firmware's executor
#include "signal_ids.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-60s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    MetaModel meta;
    // REAL_META is the definition this build was configured against; the relative paths are the
    // fallback for running the binary from somewhere else. Without the first, this test SKIPPED
    // silently from any cwd but one — passing without checking anything.
    const char* cands[] = { argc > 1 ? argv[1] : nullptr,
#ifdef REAL_META
                            REAL_META,
#endif
                            "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::puts("[expr-widget] (no meta found — skipped)"); return 0; }

    const std::string path = "sensors.sensor[clt].precond_expr";
    int off = 0, size = 0;
    if (!meta.resolveBlob(path, off, size)) {
        std::puts("[expr-widget] (meta has no expression field — skipped)");
        return 0;
    }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    std::vector<uint8_t> image = meta.defaultImage();
    if (image.empty()) image.assign((size_t)meta.configSize(), 0);
    C.setConfigImage(image);

    jf::JSceneGraph graph;
    ExpressionWidget w(graph);

    std::puts("=== Expression widget -> config image -> firmware VM ===");

    // The widget binds the same way the dictionary drop binds it.
    PanelElement el;
    el.type = "expression";
    el.props["signalName"] = "[#" + path + "]";
    w.loadSkin(el);          // the same element load the surface performs on a dropped widget
    ck(w.elementType() == "expression", "the widget is the expression editor");

    // Type a gate. commit() is what the JLineEdit's onTextChanged calls.
    const std::string src = "rpm > 2500 and (map > 50 or tps > 80)";
    w.setSource(src);
    ck(w.compileError().empty(), "the typed expression compiles", w.compileError());

    // What landed in the image?
    const std::vector<uint8_t> stored = C.configBlob(path);
    ck(!stored.empty() && !expr::is_empty(stored.data(), (uint16_t)stored.size()),
       "bytecode landed in the config image");
    ck((int)stored.size() == size, "…filling exactly the field's block", std::to_string(stored.size()));

    // It must be the SAME bytes the compiler produces — i.e. the widget wrote at the right offset
    // and did not truncate.
    const auto ref = ExprCompiler::compile(src, meta, (uint32_t)meta.configSize(), (uint16_t)size);
    ck(ref.ok && std::equal(ref.code.begin(), ref.code.end(), stored.begin()),
       "…byte-identical to a direct compile");

    // And the firmware VM agrees about what it means.
    {
        SignalBus bus;
        expr::Ctx ctx;
        const std::vector<uint8_t>& img = C.configImage();
        ctx.bus = &bus; ctx.cfg = img.data(); ctx.cfg_size = (uint32_t)img.size(); ctx.now_ms = 1000;

        bus.set(SIG_RPM, 3000.0f, true, 1000);
        bus.set(SIG_MAP, 40.0f,   true, 1000);
        bus.set(SIG_TPS, 85.0f,   true, 1000);
        ck(expr::eval_bool(img.data() + off, (uint16_t)size, ctx, true),
           "the FIRMWARE arms on it (rpm high, tps carries the OR)");

        bus.set(SIG_TPS, 10.0f, true, 1000);
        ck(!expr::eval_bool(img.data() + off, (uint16_t)size, ctx, true),
           "…and disarms when both OR terms go false");

        bus.set(SIG_MAP, 80.0f, true, 1000);
        ck(expr::eval_bool(img.data() + off, (uint16_t)size, ctx, true),
           "…and re-arms on the other OR term (map now over its threshold)");

        bus.set(SIG_RPM, 1000.0f, true, 1000);
        ck(!expr::eval_bool(img.data() + off, (uint16_t)size, ctx, true),
           "…and the AND still gates it");
    }

    // Reading it back: the widget shows the stored program as source again.
    {
        w.refresh();
        ck(!w.source().empty(), "the widget reads the stored program back as text", w.source());
        const auto again = ExprCompiler::compile(w.source(), meta,
                                                 (uint32_t)meta.configSize(), (uint16_t)size);
        ck(again.ok && again.code == ref.code, "…and that text recompiles to the same program",
           w.source());
    }

    // A typo must NOT be written. Silently storing a half-typed gate would leave the ECU running
    // something the tuner never finished.
    {
        const std::vector<uint8_t> before = C.configBlob(path);
        w.setSource("rpm > 2500 and (map > ");
        ck(!w.compileError().empty(), "an incomplete expression reports an error",
           w.compileError());
        ck(C.configBlob(path) == before, "…and the stored program is UNCHANGED");

        w.setSource("nosuchchannel > 5");
        ck(!w.compileError().empty(), "an unknown channel reports an error", w.compileError());
        ck(C.configBlob(path) == before, "…and the stored program is still unchanged");
    }

    // Clearing it is a real edit: an empty program means "always armed", not "leave the old gate".
    {
        w.setSource("");
        ck(w.compileError().empty(), "an empty expression is valid");
        const std::vector<uint8_t> now = C.configBlob(path);
        ck(expr::is_empty(now.data(), (uint16_t)now.size()),
           "…and clears the field to the always-armed program");
    }

    // The `fx` affordance: a press in the right-hand strip must open the EDITOR, not put a caret in
    // the text box, and applying from the dialog must go through the same store path typing does.
    {
        w.setSource("rpm > 2500");                       // something to open the editor on
        std::string opened;
        uint16_t openedBlock = 0;
        Surface::onEditExpression = [&](std::string cur, uint16_t block, std::string /*element*/,
                                        std::function<void(std::string)> apply) {
            opened = cur;
            openedBlock = block;
            apply("map > 50 and tps > 10");              // what the dialog's OK would hand back
        };

        const jf::JRect r{0.f, 0.f, 320.f, 30.f};
        ControlInput press;
        press.kind = ControlInput::Kind::Press;
        press.my = 15.f;

        press.mx = 10.f;                                 // left of the strip: the TEXT BOX takes it
        const std::string beforeText = w.source();
        w.onControlInput(r, press);
        ck(opened.empty(), "a press in the text area does NOT open the editor");
        ck(w.source() == beforeText, "…and does not change the expression");

        press.mx = r.width - 5.f;                        // inside the fx strip
        w.onControlInput(r, press);
        ck(!opened.empty(), "a press on fx opens the editor");
        ck(opened == "rpm > 2500", "…seeded with the current expression", opened);
        ck(openedBlock == (uint16_t)size, "…and told the program block size",
           std::to_string(openedBlock));
        ck(w.compileError().empty(), "the applied expression compiled", w.compileError());

        const auto expect = ExprCompiler::compile("map > 50 and tps > 10", meta,
                                                  (uint32_t)meta.configSize(), (uint16_t)size);
        const std::vector<uint8_t> got = C.configBlob(path);
        ck(expect.ok && std::equal(expect.code.begin(), expect.code.end(), got.begin()),
           "…and the dialog's text was stored as the program the ECU will run");
        Surface::onEditExpression = nullptr;
    }

    // THE TEXT MUST NOT RUN UNDER THE GLYPH. The `fx` strip is painted at the right-hand end, and the
    // hosted line edit used to be given the WHOLE content rect underneath it — so a long expression
    // was drawn beneath "fx" and the box hit-tested there too. The control's bounds are what decide
    // both, so that is what this asserts.
    {
        jf::JPrimitiveBuffer buf;
        const jf::JRect r{0.f, 0.f, 320.f, 30.f};
        w.render(buf, r, C);
        const jf::JRect b = w.controlForTest()->bounds();
        ck(b.width <= r.width - ExpressionWidget::kFxW + 0.01f,
           "the text box stops short of the fx strip", std::to_string(b.width));

        // …and a widget too narrow to carry a target keeps its full width, rather than being squeezed
        // for a glyph that is not drawn.
        const jf::JRect tiny{0.f, 0.f, 40.f, 30.f};
        w.render(buf, tiny, C);
        ck(w.controlForTest()->bounds().width == tiny.width,
           "…and a widget too narrow for the strip keeps all of it",
           std::to_string(w.controlForTest()->bounds().width));
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "All expression widget tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
