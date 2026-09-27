// TYPING A CAPTION IN PLACE KEEPS WHAT YOU TYPED.
//
// Double-click a label on the canvas and it opens a caret; the text goes into a shared JTextEditCore and
// Enter writes it to the model. Everything in between resizes the element on every keystroke, which bumps
// the model generation, which re-sources every live instance from the model — so the question this pins is
// whether the in-progress text survives that churn and lands in the element, rather than the instance's
// pre-edit value winning and the caption snapping back to "Label".
//
// Driven through the same public entry points the window uses: a render pass to build the instances, two
// clicks to open the caret, keys, then Enter.
//
//   cmake --build build --target caption_edit_test && ./build/caption_edit_test

#include "model/Cache.h"
#include "model/MetaModel.h"
#include "surface/PanelModel.h"
#include "surface/Surface.h"
#include "surface/widgets/LabelWidget.h"   // labelEdits(): the in-progress caption state

#include <j/core/SceneGraph.h>

#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("[caption-edit] %-56s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

static jf::JKeyEvent chr(char c) {
    jf::JKeyEvent k;
    k.pressed = true;
    k.key = static_cast<jf::JKeyEvent::JKey>(c >= 'a' && c <= 'z' ? c - 32 : c);
    k.utf8[0] = c; k.utf8[1] = '\0';
    return k;
}
static jf::JKeyEvent named(jf::JKeyEvent::JKey k) {
    jf::JKeyEvent e; e.pressed = true; e.key = k; return e;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());
    jf::JSceneGraph graph;

    // A STATIC canvas the exact size of the viewport, so the camera is 1:1 and a canvas coordinate is a
    // screen coordinate — the click below can then aim at the label's own rect.
    PanelModel page;
    page.setCanvasSize(900.f, 600.f);
    page.setCanvasStatic(1);
    const int id = page.add("label", 40.f, 40.f, 160.f, 30.f, { { "labelText", "Label" } });

    Surface s(graph, c, &page);
    s.setBounds({ 0.f, 0.f, 900.f, 600.f });
    s.setMode(Surface::Mode::Edit);
    // PINNED TO 1:1, so this test says what it means. The page canvas and the surface bounds are
    // deliberately the same size here; fitting one to the other is exactly scale 1, whatever the
    // user's "static surface size" preference happens to be. Static is no longer 1:1 — it renders a
    // page AT the preference size — so without this a hit-test lands wherever that preference put it.
    s.setFitToView(true);

    // A render pass is what builds the live instances; the click path needs one to hit-test against.
    jf::JPrimitiveBuffer buf;
    s.populateRenderPrimitives(buf);

    // Two presses in the same spot inside 400 ms = the double-click that opens the caret. Through the
    // camera, because the canvas is scaled and centred in the viewport — canvas coordinates are not
    // screen coordinates, and a click at the raw rect lands on empty page.
    const auto ctr = [&](const PanelElement& e) {
        return std::pair<float,float>{ e.x + e.w * 0.5f, e.y + e.h * 0.5f };
    };
    const auto [cx, cy] = ctr(*page.get(id));
    s.handleMousePress(cx, cy);
    s.handleMouseRelease(cx, cy);
    s.handleMousePress(cx, cy);
    const auto& edits = LabelWidget::labelEdits();
    const auto it = edits.find(page.get(id)->uid);
    ck(it != edits.end() && it->second.active, "a double-click opens the caption for editing");

    // The caret opens with everything selected, so the first keystroke replaces "Label".
    for (char ch : std::string("Exercise pedals")) s.handleKeyEvent(chr(ch));
    ck(LabelWidget::labelEdits()[page.get(id)->uid].core.text() == "Exercise pedals",
       "the typed text is what the editor holds", LabelWidget::labelEdits()[page.get(id)->uid].core.text());
    ck(page.get(id)->prop("labelText") == "Label",
       "…and the model still holds the old caption until it is committed",
       page.get(id)->prop("labelText"));

    // Enter commits. THE REPORT: the caption came back as "Label" instead of what was typed.
    s.handleKeyEvent(named(jf::JKeyEvent::JKey::Return));
    ck(page.get(id)->prop("labelText") == "Exercise pedals",
       "Enter writes the typed caption into the element",
       page.get(id)->prop("labelText"));

    // …and it must survive the next frame, which re-sources every instance from the model, and a save,
    // which flushes every instance's override set back over it.
    s.populateRenderPrimitives(buf);
    s.refreshInstancesIfChanged();
    ck(page.get(id)->prop("labelText") == "Exercise pedals",
       "…survives the frame that re-sources the instances", page.get(id)->prop("labelText"));
    s.commitInstances();
    ck(page.get(id)->prop("labelText") == "Exercise pedals",
       "…and survives the instance flush a save performs", page.get(id)->prop("labelText"));

    // The same again on a WRAPPING label: its width must not move, since that width is what the text is
    // being broken against.
    {
        const int wid = page.add("label", 40.f, 200.f, 160.f, 30.f,
                                 { { "labelText", "Label" }, { "wrap", "1" } });
        s.populateRenderPrimitives(buf);
        const auto [wx, wy] = ctr(*page.get(wid));
        s.handleMousePress(wx, wy); s.handleMouseRelease(wx, wy); s.handleMousePress(wx, wy);
        ck(LabelWidget::labelEdits().count(page.get(wid)->uid) && LabelWidget::labelEdits()[page.get(wid)->uid].active,
           "a wrapping label opens for editing too");
        for (char ch : std::string("a rather long caption that must wrap")) s.handleKeyEvent(chr(ch));
        ck(std::fabs(page.get(wid)->w - 160.f) < 0.51f,
           "typing into it never moves the width it wraps against", std::to_string(page.get(wid)->w));
        s.handleKeyEvent(named(jf::JKeyEvent::JKey::Return));
        ck(page.get(wid)->prop("labelText") == "a rather long caption that must wrap",
           "…and its caption commits like any other", page.get(wid)->prop("labelText"));
        ck(std::fabs(page.get(wid)->w - 160.f) < 0.51f, "…still at the authored width",
           std::to_string(page.get(wid)->w));
    }

    // …and inside a PANEL, which is where captions are usually typed: the scope edits a temporary model
    // built from the panel's children, and leaving serialises it back. A caption committed in there has to
    // survive that round trip, not just the model write.
    {
        const int pid = page.add("panel", 300.f, 40.f, 300.f, 200.f,
            { { "labelText", "Settings" },
              { "children", "[{\"id\":1,\"uid\":\"kid\",\"type\":\"label\",\"x\":10,\"y\":10,"
                            "\"w\":160,\"h\":30,\"groupId\":0,\"props\":{\"labelText\":\"Label\"}}]" } });
        s.populateRenderPrimitives(buf);
        s.enterPanel(pid);
        PanelModel* inner = s.model();
        ck(inner && inner != &page && inner->elements().size() == 1, "inside the panel, editing its child");
        // The scope's page is the panel's content box, which is smaller than the viewport and therefore
        // letterboxed by the canvas anchor. Pin it to the viewport, top-left, so a canvas coordinate is a
        // screen coordinate here too — this test is about the caption, not about the camera.
        inner->setCanvasSize(900.f, 600.f);
        inner->setCanvasStatic(1);
        inner->setCanvasAnchor(0);
        const int kid = inner->elements().front().id;
        // The scope's page is the panel's CONTENT box and starts at the origin, so the child's own rect is
        // where it is drawn (the surface canvas is 1:1 here).
        s.populateRenderPrimitives(buf);
        const auto [kx, ky] = ctr(*inner->get(kid));
        s.handleMousePress(kx, ky); s.handleMouseRelease(kx, ky); s.handleMousePress(kx, ky);
        ck(LabelWidget::labelEdits().count(inner->get(kid)->uid) && LabelWidget::labelEdits()[inner->get(kid)->uid].active,
           "a double-click opens the child caption");
        for (char ch : std::string("Pedal span")) s.handleKeyEvent(chr(ch));
        s.handleKeyEvent(named(jf::JKeyEvent::JKey::Return));
        ck(inner->get(kid)->prop("labelText") == "Pedal span", "…Enter commits it in the scope",
           inner->get(kid)->prop("labelText"));
        s.exitScope();
        const std::string kids = page.get(pid)->prop("children");
        ck(kids.find("Pedal span") != std::string::npos,
           "…and leaving the panel writes it into the panel's children", kids.substr(0, 120));
        ck(kids.find("\"labelText\":\"Label\"") == std::string::npos,
           "…with the old caption gone, not both");
    }

    // A MULTILINE CAPTION, COMMITTED BY LOSING FOCUS. Shift-Enter breaks the line (the core is multiline
    // and the caption only claims a bare Return), and clicking off the label is the other way to commit.
    // Reported: a label typed this way comes back as "Label".
    {
        const int mid = page.add("label", 400.f, 300.f, 200.f, 60.f, { { "labelText", "Label" } });
        s.populateRenderPrimitives(buf);
        const auto [mx, my] = ctr(*page.get(mid));
        s.handleMousePress(mx, my); s.handleMouseRelease(mx, my); s.handleMousePress(mx, my);
        ck(LabelWidget::labelEdits().count(page.get(mid)->uid) && LabelWidget::labelEdits()[page.get(mid)->uid].active,
           "the caption opens for a multiline edit");
        for (char ch : std::string("one")) s.handleKeyEvent(chr(ch));
        jf::JKeyEvent br = named(jf::JKeyEvent::JKey::Return);
        br.shift = true;
        br.utf8[0] = '\n'; br.utf8[1] = '\0';          // what the platform sends with the key
        s.handleKeyEvent(br);
        for (char ch : std::string("two")) s.handleKeyEvent(chr(ch));
        const std::string typed = LabelWidget::labelEdits()[page.get(mid)->uid].core.text();
        ck(typed == "one\ntwo", "shift-Enter breaks the line instead of committing", typed);

        // Click well away from it: that is the "lost focus" commit.
        s.handleMousePress(820.f, 560.f);
        s.handleMouseRelease(820.f, 560.f);
        ck(page.get(mid)->prop("labelText") == "one\ntwo",
           "clicking away commits the caption, both lines of it",
           page.get(mid)->prop("labelText"));
        s.populateRenderPrimitives(buf);
        s.refreshInstancesIfChanged();
        s.commitInstances();
        ck(page.get(mid)->prop("labelText") == "one\ntwo",
           "…and it survives the frame and the save flush", page.get(mid)->prop("labelText"));
    }

    std::printf("[caption-edit] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
