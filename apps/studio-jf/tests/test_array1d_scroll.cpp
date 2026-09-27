// Can you reach the cells a 1-D strip cannot fit?
//
// Until now, no. The strip drew `min(count, fit)` rows and clamped the cursor to the same number, so a
// stream configured with 20 cells in a box that fits 15 showed cells 1..15 and had no way — key, click or
// wheel — to reach the last five. Worse than a display bug: cells 16..20 are read by the decoder, so the
// tune had five values in it that the page could not show and the user could not edit.
//
// The rule now: the SELECTION addresses the whole window, the VIEW follows it, and the wheel moves the view
// without moving the selection. The check that matters is the last one here — that after scrolling, a typed
// value lands in the cell the row says it is. An off-by-scroll write is silent and looks like a working edit.
//
//   cmake --build build --target array1d_scroll_test && ./build/array1d_scroll_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/MathEvaluator.h"
#include "model/SigilResolvers.h"
#include "surface/widgets/Array1DWidget.h"
#include "surface/WidgetRegistry.h"
#include <j/core/SceneGraph.h>

#include <cstdio>
#include <memory>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-64s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());
    static ConfigSigilResolver configSigils;
    MathEvaluator::instance().registerResolver(&configSigils);
    jf::JSceneGraph graph;

    // 20 of a trigger stream's 32 cells, in a box that fits 6 rows. Without an atlas lineHeight() is a
    // fixed 16, so rowH is 20 and the fit is arithmetic, not a rendering accident.
    const std::string run = "trigger.streams[0].cell[].v";   // the run form: one field of a sub-array
    const int kCount = 20, kFit = 6;
    PanelElement el;
    el.id = 4242; el.uid = "test-array1d-uid";
    el.type = "array1d";
    el.props["signalName"] = run;
    el.props["rowCount"]   = std::to_string(kCount);

    std::unique_ptr<CanvasWidget> base = makeWidgetInstance(el.type, graph);
    auto* w = dynamic_cast<Array1DWidget*>(base.get());
    if (!w) { std::fputs("array1d does not instantiate\n", stderr); return 1; }
    w->setSource(el); w->setData(el); w->setCache(&c);
    const jf::JRect r{ 0.f, 0.f, 200.f, 6.f * 20.f + 12.f };   // pad 6 top and bottom -> exactly kFit rows

    auto& cur = Array1DWidget::array1dCursors()[el.uid];
    cur = {};
    auto key = [&](jf::JKeyEvent::JKey k, const char* utf8 = "") {
        jf::JKeyEvent ke{}; ke.key = k; ke.pressed = true;
        std::snprintf(ke.utf8, sizeof(ke.utf8), "%s", utf8);
        ControlInput in; in.kind = ControlInput::Kind::Key; in.key = &ke;
        return w->onControlInput(r, in);
    };
    using K = jf::JKeyEvent::JKey;

    // 1 — the far end is REACHABLE. This is the whole bug: 20 cells, 6 visible, and Down used to stop at 5.
    for (int i = 0; i < 40; ++i) key(K::Down);
    ck(cur.sel == kCount - 1, "Down reaches the last cell of the run, not the last VISIBLE one",
       "sel " + std::to_string(cur.sel) + " of " + std::to_string(kCount));

    // 2 — …and the view followed it there, so the selected row is one you can see.
    ck(cur.scroll == kCount - kFit, "…and the view scrolled to keep it on screen",
       "scroll " + std::to_string(cur.scroll) + " want " + std::to_string(kCount - kFit));
    ck(cur.sel - cur.scroll >= 0 && cur.sel - cur.scroll < kFit, "…the selection is inside the visible rows");

    // 3 — a write after scrolling lands in the cell the row NAMES. Silent when wrong: the strip would
    //     report the edit on row 20 and put the number in cell 6.
    {
        const MetaModel::Location last = m.locate("trigger.streams[0].cell[" + std::to_string(kCount - 1) + "].v");
        const MetaModel::Location sixth = m.locate("trigger.streams[0].cell[5].v");
        c.writeRaw(last, Raw{0}, run);  c.writeRaw(sixth, Raw{0}, run);
        key(K::Unknown, "7"); key(K::Unknown, "7"); key(K::Return);   // type 77 into the selected row
        ck(c.readRaw(last).v == 77, "a value typed on the last row is stored in the LAST cell",
           "cell[19] = " + std::to_string(c.readRaw(last).v));
        ck(c.readRaw(sixth).v == 0, "…and not in the cell that row would have been without the scroll",
           "cell[5] = " + std::to_string(c.readRaw(sixth).v));
    }

    // 4 — Up walks back and the view follows the other way.
    for (int i = 0; i < 40; ++i) key(K::Up);
    ck(cur.sel == 0 && cur.scroll == 0, "Up walks back to the first cell and the view returns with it",
       "sel " + std::to_string(cur.sel) + " scroll " + std::to_string(cur.scroll));

    // 5 — the wheel moves the VIEW and leaves the selection alone: the row being edited must not change
    //     because you looked further down the run.
    {
        cur.sel = 2; cur.scroll = 0;
        ControlInput in; in.kind = ControlInput::Kind::Scroll; in.wheel = -1.f;   // wheel down
        const bool took = w->onControlInput(r, in);
        ck(took && cur.scroll > 0, "the wheel scrolls the strip",
           "scroll " + std::to_string(cur.scroll));
        ck(cur.sel == 2, "…without moving the selection", "sel " + std::to_string(cur.sel));
        for (int i = 0; i < 20; ++i) w->onControlInput(r, in);
        ck(cur.scroll == kCount - kFit, "…and stops at the end of the run",
           "scroll " + std::to_string(cur.scroll));
    }

    // 6 — a click selects the row UNDER THE CURSOR at the current scroll, not the same row of the run.
    {
        cur.scroll = 10; cur.sel = 10;
        ControlInput in; in.kind = ControlInput::Kind::Press; in.mx = 40.f; in.my = 6.f + 2.f * 20.f + 4.f;
        w->onControlInput(r, in);
        ck(cur.sel == 12, "a click selects the row shown there, offset by the scroll",
           "sel " + std::to_string(cur.sel) + " want 12");
    }

    // 7 — a run that FITS gets no bar and no scroll: the gutter would cost width for nothing.
    {
        el.props["rowCount"] = "4";
        std::unique_ptr<CanvasWidget> b2 = makeWidgetInstance(el.type, graph);
        auto* w2 = dynamic_cast<Array1DWidget*>(b2.get());
        w2->setSource(el); w2->setData(el); w2->setCache(&c);
        auto& c2 = Array1DWidget::array1dCursors()[el.uid];
        c2 = {};
        ControlInput in; in.kind = ControlInput::Kind::Scroll; in.wheel = -1.f;
        ck(!w2->onControlInput(r, in), "a run that fits does not consume the wheel");
        ck(c2.scroll == 0, "…and never scrolls");
    }

    // 8 — THE BAR IS WHERE IT WAS DRAWN. render() is handed contentRect(bounds()) and input the FULL rect,
    //     so a padded strip hit-tested a box bigger than the one it painted: the 6px gutter was tested at
    //     the element's edge, not the content's, and dragging the thumb did nothing whatsoever. Padding is
    //     the default on a dropped widget, so this is the case a user meets first.
    {
        el.props["rowCount"] = std::to_string(kCount);
        el.props["padding"]  = "10";
        std::unique_ptr<CanvasWidget> b3 = makeWidgetInstance(el.type, graph);
        auto* w3 = dynamic_cast<Array1DWidget*>(b3.get());
        w3->setSource(el); w3->setData(el); w3->setCache(&c);
        auto& c3 = Array1DWidget::array1dCursors()[el.uid];
        c3 = {};
        // A box tall enough that the run still overflows once the padding is taken off both ends.
        const jf::JRect screen{ 0.f, 0.f, 200.f, 6.f * 20.f + 12.f + 20.f };
        ck(w3->wantsWheel(screen, 100.f, 60.f), "a padded strip still knows its run overflows");

        // The painted bar sits at the CONTENT's right edge, 10px in from the element's.
        ControlInput press; press.kind = ControlInput::Kind::Press;
        press.mx = screen.x + screen.width - 10.f - 3.f; press.my = screen.y + 10.f + 6.f + 4.f;
        ck(w3->onControlInput(screen, press) && c3.barDrag, "a press on the painted thumb grabs it",
           c3.barDrag ? "" : "the press missed the bar");
        ControlInput move; move.kind = ControlInput::Kind::Move;
        move.mx = press.mx; move.my = screen.y + screen.height;
        w3->onControlInput(screen, move);
        ck(c3.scroll > 0, "…and dragging it down scrolls the strip",
           "scroll " + std::to_string(c3.scroll));
    }

    // 9 — SELECTING A CELL DOES NOT CHANGE IT. Leaving a cell commits it, so an untouched one must not be
    //     written back: where the control's Min/Max is narrower than what the tune holds — a GAP cell index
    //     capped at the anomaly ceiling over a longer stored run — a commit would clamp it, and the
    //     out-of-range value the strip exists to show you would be destroyed by looking at it.
    {
        el.props["rowCount"] = std::to_string(kCount);
        el.props["padding"]  = "";
        el.props["maxExpr"]  = "10";                      // the control allows far less than the field does
        std::unique_ptr<CanvasWidget> b4 = makeWidgetInstance(el.type, graph);
        auto* w4 = dynamic_cast<Array1DWidget*>(b4.get());
        w4->setSource(el); w4->setData(el); w4->setCache(&c);
        Array1DWidget::array1dCursors()[el.uid] = {};

        const MetaModel::Location wide = m.locate("trigger.streams[0].cell[0].v");   // the FIELD's own range
        c.writeRaw(wide, Raw{30}, run);
        ck(c.readRaw(wide).v == 30, "a cell holds a value the control would not accept");

        ControlInput press; press.kind = ControlInput::Kind::Press;
        press.mx = 40.f; press.my = r.y + 6.f + 4.f;      // the first row, on the label
        w4->onControlInput(r, press);
        ControlInput blur; blur.kind = ControlInput::Kind::Blur;
        w4->onControlInput(r, blur);
        ck(c.readRaw(wide).v == 30, "…and selecting it, then leaving, does not clamp it away",
           "cell[0] = " + std::to_string(c.readRaw(wide).v));
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "every cell of a run is reachable, and edits land where the row says",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
