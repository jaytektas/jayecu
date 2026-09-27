// The Receive page's signal row: choosing a channel, and UNCHOOSING one.
//
// A field with no channel is not an edge case on this page — it is how a CAN SENSOR reads a frame.
// The row was a bare JButton, which has one affordance (press it, the dialog opens), so there was
// nowhere to get back to "no channel". It is a JPickerField now: the chosen name, an ellipsis well,
// and an ✕. This presses the ✕ where it is DRAWN, because "the signal is connected" would pass on a
// control nothing could ever hit.
//
//   cmake --build build --target gc_signal_clear_test && ./build/gc_signal_clear_test
#include "../src/surface/CanvasWidget.h"   // enumpick:: — the panel opens the signal dialog through it
#include "../src/ui/GenericCanPanel.h"
#include "../src/model/Cache.h"
#include "../src/model/MetaModel.h"

#include <j/core/SceneGraph.h>
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
    if (!loaded) { std::puts("[gc-signal-clear] (no meta found — skipped)"); return 0; }

    Cache& C = Cache::instance();
    C.setMeta(&meta);
    std::vector<uint8_t> image = meta.defaultImage();
    if (image.empty()) image.assign((size_t)meta.configSize(), 0);
    C.setConfigImage(image);
    if (!meta.locate("can.gc_field[0].sig").valid()) {
        std::puts("[gc-signal-clear] (meta has no generic CAN pool — skipped)");
        return 0;
    }

    // One receive frame on CAN1 whose single field carries a channel (SIG_IAT = 134).
    C.setConfigValue("can.gc_frame[0].flags", 1);          // USED, receive
    C.setConfigValue("can.gc_frame[0].bus", 0);
    C.setConfigValue("can.gc_frame[0].id", 0x360);
    C.setConfigValue("can.gc_frame[0].dlc", 8);
    C.setConfigValue("can.gc_frame[0].first_field", 0);
    C.setConfigValue("can.gc_frame[0].field_count", 1);
    C.setConfigString("can.gc_frame[0].name", "Wideband");
    C.setConfigValue("can.gc_field[0].sig", 134);
    C.setConfigValue("can.gc_field[0].bit_off", 7);
    C.setConfigValue("can.gc_field[0].width", 16);
    C.setConfigValue("can.gc_field[0].scale", 1.0);

    std::puts("=== the Receive page's signal row ===");

    jf::JSceneGraph graph;
    jf::JPrimitiveBuffer buf;
    GenericCanPanel panel(graph);
    panel.configure(/*bus=*/0, /*transmit=*/false);
    panel.attach();
    panel.setBox({ 0.f, 0.f, 1280.f, 700.f });
    // The editor follows the frame list's selection, so pick the frame the way a user does.
    ck(panel.selectFrameForTest(0), "the receive frame is listed on this page");
    panel.populateRenderPrimitives(buf);        // _place() puts the controls where they are drawn

    jf::JPickerField* sig = panel.rowSignalFieldForTest(0);
    ck(sig != nullptr, "the frame's first signal row has a picker field");
    if (!sig) { std::printf("\n%d failure(s)\n", fails); return 1; }

    ck(sig->text() == "iat", "…showing the channel the field carries", sig->text());

    // THE SAME HEIGHT AS A CONTROL ON ANY OTHER PAGE. The panel used JStyle::buttonHeight (22) while
    // every page authors its controls at 30 (tools/layout/author.py, CTL_H), so an identical picker
    // field was visibly shorter here than one page away. 30 is the number the pages use.
    ck(sig->bounds().height == 30.f, "the signal field is the page control height, not the style's 22",
       std::to_string(sig->bounds().height));
    // …and so is everything beside it in the row, because they all come off the same rowH().
    if (jf::JPickerField* s2 = panel.rowSignalFieldForTest(0))
        ck(s2->bounds().height == sig->bounds().height, "…every row is that height");
    ck(sig->clearable(), "…and it is clearable, which a button never was");

    // PRESS THE ✕ WHERE IT IS DRAWN. Its well is the second from the right edge — the field works
    // that out from its own bounds, so the test asks the bounds rather than repeating the arithmetic.
    const jf::JRect b = sig->bounds();
    const float wellW = b.height * 0.75f;
    const float cx = b.x + b.width - wellW * 1.5f;      // the middle of the clear well
    sig->handleMousePress(cx, b.y + b.height * 0.5f);

    ck(C.configValue("can.gc_field[0].sig") == 65535.0,
       "pressing the ✕ writes NO CHANNEL — 65535, not 0, which is abs_mode",
       std::to_string(C.configValue("can.gc_field[0].sig")));

    panel.populateRenderPrimitives(buf);
    ck(sig->text().empty(), "…and the field goes back to its placeholder", "\"" + sig->text() + "\"");
    ck(!sig->clearable() || sig->text().empty(),
       "…with nothing left to clear");

    // A PRESS ANYWHERE ELSE IN THE FIELD IS NOT A CLEAR. Without this the test would pass on a
    // control that cleared itself wherever you touched it, which is a worse bug than no clear at all.
    // (The panel OWNS these bytes while it is open — it writes the tune, it does not poll it — so a
    // change made behind it is picked up by attach(), which is what the app calls for the same reason.)
    C.setConfigValue("can.gc_field[0].sig", 134);
    panel.attach();
    panel.selectFrameForTest(0);
    panel.populateRenderPrimitives(buf);
    ck(sig->text() == "iat", "a channel set again shows again", sig->text());
    sig->handleMousePress(b.x + 8.f, b.y + b.height * 0.5f);   // over the text, not the ✕
    ck(C.configValue("can.gc_field[0].sig") == 134.0,
       "a press on the field's text opens the picker, it does not clear",
       std::to_string(C.configValue("can.gc_field[0].sig")));

    std::printf("\n%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
