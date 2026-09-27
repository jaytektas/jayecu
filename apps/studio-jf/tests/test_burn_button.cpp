// THE TOOLBAR'S BURN BUTTON, AND WHAT IT SAYS ABOUT THE TUNE.
//
// The studio's editing model is that an edit lands in ECU RAM immediately and in flash only when asked,
// so the one thing this control must never do is look the same whether or not there is something to
// lose. Three states, and they have to be distinguishable AND correctly reachable:
//
//   clean  — disabled, hollow dot. A click cannot pretend to commit something that is not there.
//   dirty  — enabled, amber. Set from the ECU's own config_dirty channel (wired in main.cpp).
//   burned — a held green confirmation, because a dot going out is not evidence that anything happened.
//
// The interesting transition is the last one: an edit arriving DURING the confirmation must end it, or
// the button sits there saying "saved" over a tune that has changed since.
//
//   cmake --build build --target burn_button_test && ./build/burn_button_test

#include "ui/BurnButton.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const char* what) {
    std::printf("[burn] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    jf::JSceneGraph g;
    BurnButton b(g);

    ck(!b.dirty() && !b.isEnabled(), "starts clean and disabled");

    b.setDirty(true);
    ck(b.dirty() && b.isEnabled(), "dirty enables the button");

    b.setDirty(false);
    ck(!b.dirty() && !b.isEnabled(), "clean again disables it");

    b.setDirty(true);
    b.showBurned();
    ck(b.justBurned(), "a burn shows its confirmation");

    // The tune moved again while the confirmation was still up.
    b.setDirty(false);
    b.setDirty(true);
    ck(b.dirty() && !b.justBurned(), "a new edit ends the confirmation");

    std::printf(fails ? "[burn] %d FAILED\n" : "[burn] all passed\n", fails);
    return fails ? 1 : 0;
}
