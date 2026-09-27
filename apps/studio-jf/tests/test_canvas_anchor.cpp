// Anchoring: a global default with a PER-CANVAS override (-1 = inherit), the same shape canvasStatic
// uses. One global setting could not say "centre my pages but pin this viewport's little canvas
// top-left"; a purely local one would throw away the convenience of setting it once.
//
// The risky part is the DEFAULT: every layout saved before this existed has no canvasAnchor key, and it
// must read back as INHERIT so nothing in the wild moves on first open.
//
//   cmake --build build --target canvas_anchor_test && ./build/canvas_anchor_test

#include "../src/surface/PanelModel.h"

#include <cstdio>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[anchor] %-58s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  — " + detail).c_str());
    if (!ok) ++fails;
}

int main() {
    PanelModel m;
    check(m.canvasAnchor() == -1, "a fresh canvas inherits the global (-1)", std::to_string(m.canvasAnchor()));

    // Round-trips through the document.
    m.setCanvasAnchor(0);                       // top-left
    const jf::JJson saved = m.toJson();
    PanelModel reloaded;
    reloaded.load(saved);
    check(reloaded.canvasAnchor() == 0, "the anchor survives save/load",
          std::to_string(reloaded.canvasAnchor()));

    // A document written BEFORE this existed has no key at all — it must keep centring, not jump to
    // top-left, or every layout in the wild silently re-lays-out on first open.
    jf::JJson legacy = m.toJson();
    legacy.erase("canvasAnchor");
    PanelModel old;
    old.load(legacy);
    check(old.canvasAnchor() == -1, "a layout with no anchor key inherits, so nothing moves",
          std::to_string(old.canvasAnchor()));

    // It travels with the canvas state, so copying a canvas (viewport paste) brings the anchor along.
    PanelModel src, dst;
    src.setCanvasAnchor(8);                     // bottom-right
    dst.setCanvasState(src.canvasState());
    check(dst.canvasAnchor() == 8, "canvasState carries it (so a pasted canvas keeps it)",
          std::to_string(dst.canvasAnchor()));

    // …and two canvases that differ only by anchor are not considered equal, or a change would not
    // register as an edit worth saving.
    PanelModel a, b;
    a.setCanvasAnchor(1);
    check(!(a.canvasState() == b.canvasState()), "a differing anchor makes the canvas state unequal");

    std::printf("\n[anchor] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
