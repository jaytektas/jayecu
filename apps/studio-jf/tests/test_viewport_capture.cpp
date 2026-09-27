// A GESTURE BELONGS TO WHOEVER TOOK THE PRESS.
//
// Every tuning page is a viewport onto a node's page model, and the viewport used to route pointer
// events by POSITION on every frame: a child heard from it only while the cursor was inside that
// child's rect. So pressing a button and sliding off it abandoned the gesture — the button got no
// move to spring it back out of its pressed look and no release at all, and sat highlighted for ever,
// still armed. Releasing outside is how a user cancels a click they did not mean; JControl implements
// exactly that, and never received the events to implement it with.
//
// The three things worth pinning, driven through the REAL path (Surface -> viewport -> child) rather
// than by calling the viewport directly:
//   press inside  -> the button looks pressed
//   drag outside  -> it springs back, because the move still reaches it
//   release out   -> it is not pressed, not armed, and the command did NOT run
// plus the ordinary click, so the capture cannot have broken the thing it is there to serve.
//
//   cmake --build build --target viewport_capture_test && ./build/viewport_capture_test

#include "../src/surface/Surface.h"
#include "../src/surface/PanelLibrary.h"
#include "../src/surface/widgets/ViewportWidget.h"
#include "../src/surface/widgets/HostedControlWidget.h"
#include "../src/model/Cache.h"

#include <j/core/SceneGraph.h>
#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("[vp-capture] %-56s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    jf::JSceneGraph graph;
    PanelLibrary lib;
    nodeviewport::resolver() = [&lib](const std::string& n) -> const PanelModel* { return lib.find(n); };

    PanelModel& page = lib.forNode("A");
    const int btn = page.add("command", 40.f, 40.f, 160.f, 44.f,
                             {{"labelText", "Go"}, {"command", "ping"}});

    PanelModel main;
    const int vp = main.add("viewport", 0.f, 0.f, 700.f, 500.f, {{"node", "A"}, {"title", "A"}});

    Surface surf(graph, Cache::instance(), &main);
    surf.setPageAccess([&lib](const std::string& n) -> PanelModel* { return &lib.forNode(n); }, []{});
    surf.setBounds({0.f, 0.f, 900.f, 700.f});
    surf.setActiveNode("A");     // a viewport onto another node is HIDDEN, and a hidden one takes no input
    surf.setMode(Surface::Mode::Run);

    int cliRuns = 0;
    Cache::instance().cliRequested.connect([&cliRuns](const std::string&) { ++cliRuns; });

    auto* vpw = dynamic_cast<ViewportWidget*>(surf.widgetById(vp));
    check(vpw != nullptr, "the surface built the viewport");
    if (!vpw) { std::printf("\n[vp-capture] FAILURES\n"); return 1; }

    // Where the mirrored button actually landed on screen: ask the viewport rather than recomputing its
    // scale and offset here, so the test cannot disagree with the thing it is testing.
    const jf::JRect vr = surf.screenRectOfId(vp);   // the rect input routing scales the mirror by
    PanelModel* out = nullptr;
    jf::JRect br{};
    for (float y = vr.y; y < vr.y + vr.height && !out; y += 4.f)
        for (float x = vr.x; x < vr.x + vr.width; x += 4.f)
            if (vpw->mirroredElementAt(vr, x, y, out, &br) == btn) break;
    check(out != nullptr && br.width > 0.f, "found the mirrored button on screen");
    if (!out) { std::printf("\n[vp-capture] FAILURES\n"); return 1; }

    // A hosted control learns its rectangle when it is PAINTED (the host sets the control's bounds each
    // frame), and JControl hit-tests the press against exactly that. In the app a frame always precedes
    // an event; here the frame has to be asked for.
    auto paint = [&] { jf::JPrimitiveBuffer buf; vpw->setBounds(vr); vpw->setCache(&Cache::instance());
                       vpw->populateRenderPrimitives(buf); };
    paint();

    const float cx = br.x + br.width * 0.5f, cy = br.y + br.height * 0.5f;
    const float outX = vr.x + vr.width - 2.f, outY = vr.y + vr.height - 2.f;   // far from the button

    auto state = [&]() -> jf::JWidgetState {
        auto* host = dynamic_cast<HostedControlWidget*>(vpw->mirroredWidget(btn));
        return host && host->controlForTest() ? host->controlForTest()->getState()
                                             : jf::JWidgetState::Disabled;   // "no control" fails every check
    };

    // ---- press, drag off, release off: a cancelled click -------------------------------------------
    surf.handleMousePress(cx, cy);
    check(state() == jf::JWidgetState::Pressed, "press inside shows the pressed look");

    surf.handleMouseMove(outX, outY);
    check(state() != jf::JWidgetState::Pressed, "sliding off springs it back (the move still arrives)");

    surf.handleMouseRelease(outX, outY);
    check(state() != jf::JWidgetState::Pressed, "release outside leaves it unpressed");
    check(cliRuns == 0,                          "and the command did NOT run — the click was cancelled");

    // ---- press and release on it: the ordinary click ----------------------------------------------
    surf.handleMousePress(cx, cy);
    surf.handleMouseRelease(cx, cy);
    check(cliRuns == 1, "press and release on the button runs the command once");
    check(state() != jf::JWidgetState::Pressed, "and it does not stay pressed afterwards");

    std::printf(fails ? "\n[vp-capture] %d FAILED\n" : "\n[vp-capture] all passed\n", fails);
    return fails ? 1 : 0;
}
