// The reflow floor: how narrow a page is allowed to get before it scrolls instead.
//
// A reflow page takes the area it is given, which is right until the area is smaller than the page can
// usefully be. Past that the rows do not become narrower, they become unreadable — a caption clipped
// mid-word beside a control squeezed to a sliver, which is a page turned to mush rather than a page
// you scroll. So a page may DECLARE the width it refuses to go below, and the surface stops reflowing
// there and lets the scroll bars do the rest.
//
// The floor is deliberately opt-in. A lamp grid or a viewport host declares none: it has columns to
// give up, not rows to ruin, and flooring it would make the status dock scroll sideways instead of
// re-wrapping to whatever width the dock was dragged to.
//
//   cmake --build build --target reflow_floor_test && ./build/reflow_floor_test
#include "../src/surface/Surface.h"
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"

#include <j/core/SceneGraph.h>
#include <cstdio>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[reflow] %-58s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  - " + detail).c_str());
    if (!ok) ++fails;
}
static std::string px(float v) { char b[32]; std::snprintf(b, sizeof b, "%.0f px", v); return b; }

int main() {
    jf::JSceneGraph graph;

    // A converted dialog: reflow, with a declared floor of 468 (what the importer lays one out to).
    PanelModel page;
    page.setCanvasSize(468.f, 736.f);
    page.setCanvasStatic(3);
    page.setMinW(468.f);
    page.add("field", 0, 0, 200, 30, {});

    Surface surf(graph, Cache::instance(), &page);
    surf.setMode(Surface::Mode::Run);      // reflow is a RUN behaviour; edit pins to the canvas

    // Roomy: the page reflows into the window, which is the whole point of reflow.
    surf.setBounds({0.f, 0.f, 900.f, 600.f});
    const float wide = surf.xform().cw;
    check(wide > 800.f, "with room to spare the page reflows into the window", px(wide));

    // Cramped: it stops at its floor rather than compressing the rows into mush.
    surf.setBounds({0.f, 0.f, 300.f, 600.f});
    const float tight = surf.xform().cw;
    check(tight >= 468.f, "in a narrow window it stops at its declared minimum", px(tight));
    check(tight > 300.f, "...which means the surface has something to scroll", px(tight));

    // The same page with NO declared floor keeps reflowing all the way down: the status lamp grid.
    PanelModel lamps;
    lamps.setCanvasSize(1232.f, 162.f);
    lamps.setCanvasStatic(3);
    lamps.add("indicator", 0, 0, 120, 24, {});
    Surface lampSurf(graph, Cache::instance(), &lamps);
    lampSurf.setMode(Surface::Mode::Run);
    lampSurf.setBounds({0.f, 0.f, 300.f, 200.f});
    const float lampW = lampSurf.xform().cw;
    check(lampW < 320.f, "a page with no floor still reflows all the way down", px(lampW));

    std::printf("\n%s\n", fails ? (std::to_string(fails) + " FAILED").c_str() : "ALL PASSED");
    return fails ? 1 : 0;
}
