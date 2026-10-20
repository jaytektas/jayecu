// Typing a CAN frame's name is one small edit per keystroke, not a rewrite of the whole frame pool.
//
// Every keystroke in the Receive page's name box went to GenericCanPanel::_commit, which wrote all 96
// frame slots and every field — about 800 config writes, each its own edit transaction (a copy and a
// compare of the whole tune) and each a change notice the app answers by re-evaluating the navigation
// tree. Typing crawled. This holds the fix: one transaction (so one undo step) per keystroke, and a
// change notice only for what actually changed — the name.
//
//   cmake --build build --target gc_typing_test && ./build/gc_typing_test
#include "../src/surface/CanvasWidget.h"
#include "../src/ui/GenericCanPanel.h"
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"

#include <j/core/SceneGraph.h>
#include <j/core/UndoStack.h>
#include <chrono>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-62s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    MetaModel meta;
    const char* cands[] = { argc > 1 ? argv[1] : nullptr,
#ifdef REAL_META
                            REAL_META,
#endif
                            "../../../shared/tuneit-meta.json", "../shared/tuneit-meta.json" };
    bool loaded = false;
    for (const char* c : cands) if (c && meta.loadFile(c)) { loaded = true; break; }
    if (!loaded) { std::puts("[gc-typing] FAIL: no meta"); return 1; }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    std::vector<uint8_t> image = meta.defaultImage();
    if (image.empty()) image.assign((size_t)meta.configSize(), 0);
    C.setConfigImage(image);

    // One receive frame on CAN1 with two fields, the shape the page holds after "Add" and a field or two.
    C.setConfigValue("can.gc_frame[0].flags", 1);
    C.setConfigValue("can.gc_frame[0].bus", 0);
    C.setConfigValue("can.gc_frame[0].id", 0x360);
    C.setConfigValue("can.gc_frame[0].dlc", 8);
    C.setConfigValue("can.gc_frame[0].first_field", 0);
    C.setConfigValue("can.gc_frame[0].field_count", 2);
    C.setConfigValue("can.gc_field[0].sig", 134);
    C.setConfigValue("can.gc_field[1].sig", 61);

    std::puts("=== typing a CAN frame's name ===");
    jf::JSceneGraph graph;
    jf::JPrimitiveBuffer buf;
    GenericCanPanel panel(graph);
    panel.configure(0, false);
    panel.attach();
    panel.setBox({ 0.f, 0.f, 1280.f, 700.f });
    ck(panel.selectFrameForTest(0), "the frame is listed");
    panel.populateRenderPrimitives(buf);
    jf::JLineEdit* name = panel.nameFieldForTest();
    ck(name != nullptr, "the page has a name box");
    if (!name) return 1;

    int notices = 0;
    C.configValueChanged.connect([&notices](const std::string&) { ++notices; });
    const int undo0 = C.undoStack() ? C.undoStack()->count() : 0;

    const std::string word = "Wideband";
    std::string typed;
    const auto t0 = std::chrono::steady_clock::now();
    for (char ch : word) { typed += ch; name->setText(typed); }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const int keys = int(word.size());

    ck(C.configString("can.gc_frame[0].name") == word, "the name lands in the tune", C.configString("can.gc_frame[0].name"));
    // At most one notice a keystroke: only the name moved. (A name is text, written with
    // setConfigString, which sends none; the frame's numbers are unchanged, so nothing else may.)
    ck(notices <= keys, "no flood of change notices — at most one per keystroke",
       std::to_string(notices) + " for " + std::to_string(keys) + " keys");
    const int undo = (C.undoStack() ? C.undoStack()->count() : 0) - undo0;
    ck(undo == keys, "one undo step per keystroke", std::to_string(undo));
    std::printf("  %.2f ms per keystroke\n", ms / keys);
    // Loose on purpose: it guards the order of magnitude (it was hundreds of writes a key), not a machine.
    ck(ms / keys < 20.0, "a keystroke costs well under a frame", std::to_string(ms / keys) + " ms");

    // …AND THE FRAME'S ENABLE IS A TICK BOX'S SIZE, not the 30 px control row it sits in.
    if (jf::JCheckBox* on = panel.enabledBoxForTest())
        ck(jf::JCheckBox::indicatorSize(on->bounds().height) == jf::JStyle::current().checkHeight
               || on->bounds().height < jf::JStyle::current().checkHeight,
           "the Enabled box draws at the style's tick size",
           std::to_string(jf::JCheckBox::indicatorSize(on->bounds().height)));

    std::printf("\n%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
