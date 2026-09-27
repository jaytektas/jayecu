// Does editing a panel IN PLACE keep what you edited?
//
// "Edit Panel in Place" drills into a panel and edits its children on the canvas. It does that by building
// a TEMPORARY model out of the panel's `children` and making it the surface's model, so everything the
// editor already knows how to do — select, drag, the inspector, undo — works on the children with no
// second implementation. Leaving serialises that model back into `children`.
//
// Back into `children`, and nothing else. The panel's own TITLE is a canvas row while you are inside it —
// the inspector shows the scope's canvas properties, and a panel has a title — so the edit landed on the
// throwaway model and went out with it: type a title, press Esc, re-enter, gone. It looked like the
// property did not stick, when in fact it had nowhere to be kept.
//
//   cmake --build build --target panel_scope_test && ./build/panel_scope_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "surface/Surface.h"
#include "surface/PanelModel.h"
#include <j/core/SceneGraph.h>

#include <cmath>
#include <cstdio>
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
    jf::JSceneGraph graph;

    // A page with one panel on it, titled, holding one child.
    PanelModel page;
    const int id = page.add("panel", 10.f, 10.f, 300.f, 200.f, {
        { "labelText", "Cells" },
        { "children",  "[{\"type\":\"label\",\"uid\":\"kid\",\"x\":0,\"y\":0,\"w\":80,\"h\":20,"
                       "\"props\":{\"labelText\":\"a\"}}]" } });

    Surface s(graph, c, &page);
    s.setMode(Surface::Mode::Edit);

    std::puts("=== editing a panel in place: what survives leaving it? ===");

    // 1 — the panel's title comes IN with it, so the row you are shown is the panel's own and not a blank.
    s.enterPanel(id);
    ck(s.model() != &page, "entering a panel swaps the surface onto its children");
    ck(s.model()->title() == "Cells", "…carrying the panel's own title in with it",
       "title \"" + s.model()->title() + "\"");

    // 2 — …and back out again. This is the report: type a title, Esc, re-enter, gone.
    s.model()->setTitle("Cell pattern");          // what the inspector's Title row does inside the scope
    s.exitScope();
    const PanelElement* back = page.get(id);
    ck(back && back->prop("labelText") == "Cell pattern",
       "leaving writes the title back onto the PANEL",
       "labelText \"" + (back ? back->prop("labelText") : std::string("<gone>")) + "\"");

    // 3 — the children still round-trip, which is what the scope existed for in the first place.
    // Asked by CONTENT, not by the fixture's uid: add() re-mints the uids inside a "children" blob (a
    // panel built from copied props must not share Widget IDs with the panel it was copied from), so the
    // literal "kid" this fixture hands in is gone by the time the panel exists. What the scope promises
    // is that the child survives leaving and re-entering — that is the thing to assert.
    ck(back && back->prop("children").find("\"labelText\":\"a\"") != std::string::npos,
       "…and its children are still there",
       back ? back->prop("children") : std::string("<gone>"));

    // 4 — re-entering shows the new title, not the old one. (The whole complaint in one line.)
    s.enterPanel(id);
    ck(s.model()->title() == "Cell pattern", "re-entering shows the title that was set",
       "title \"" + s.model()->title() + "\"");
    s.exitScope();

    // WHERE A CHILD SITS IS THE SAME QUESTION IN BOTH PLACES. A child's stored x/y live in the panel's
    // CONTENT box — its rect less the 2px frame and less the title bar — because that is the space
    // PanelWidget lays them out in. Editing them on a page the size of the whole panel moved the editor's
    // origin a title-height above the live one, so anything placed clear of the title in here came out a
    // title lower again out there.
    {
        s.enterPanel(id);
        const float lh = jf::JTextHelper::lineHeight();
        const float chrome = lh + 6.f + 4.f;                  // titled: bar + the 2px frame, both edges
        ck(std::fabs(s.model()->canvasH() - (200.f - chrome)) < 0.51f,
           "the scope's page is the panel's content box, not its whole rect",
           "page h " + std::to_string(s.model()->canvasH()) + " panel h 200 chrome "
           + std::to_string(chrome));
        ck(std::fabs(s.model()->canvasW() - (300.f - 4.f)) < 0.51f,
           "…in width as well", "page w " + std::to_string(s.model()->canvasW()));
        s.exitScope();
    }

    // A TRIP THROUGH LIVE MODE AND BACK LEAVES YOU WHERE YOU WERE. In-place editing is edit-mode only, so
    // switching to Live pops the scope — right, since a scope is a page you are INSIDE and Live shows the
    // dashboard. Coming back put you at the TOP level though, so every glance at live values cost a
    // double-click to get back in.
    {
        s.enterPanel(id);
        ck(s.model() != &page, "editing inside the panel");
        s.setMode(Surface::Mode::Run);
        ck(s.model() == &page, "…Live shows the page, not the scope");
        s.setMode(Surface::Mode::Edit);
        ck(s.model() != &page, "…and coming back returns to the panel, not to the top level");
        ck(s.model()->title() == "Cell pattern", "…the same scope, with its own title",
           s.model()->title());
        s.exitScope();
        // Leaving deliberately is still leaving: the next trip through Live must not put it back.
        s.setMode(Surface::Mode::Run);
        s.setMode(Surface::Mode::Edit);
        ck(s.model() == &page, "a scope left on purpose stays left");
    }

    // A GROUP MADE INSIDE A PANEL IS STILL A GROUP WHEN YOU COME BACK. Leaving serialises the scope's
    // model into `children`, and that writer was a hand-rolled copy of PanelModel's own — missing groupId,
    // so every group made in here was dissolved by the act of leaving, and missing id, which is the key a
    // sigil ref names. Both paths now go through PanelModel::elementToJson.
    {
        s.enterPanel(id);
        const int a = s.model()->add("label", 0.f, 40.f, 80.f, 20.f, {});
        const int b = s.model()->add("label", 0.f, 70.f, 80.f, 20.f, {});
        const int gid = s.model()->nextGroupId();
        s.model()->setGroupId(a, gid);
        s.model()->setGroupId(b, gid);
        s.exitScope();

        s.enterPanel(id);
        const PanelElement* ra = s.model()->get(a);
        const PanelElement* rb = s.model()->get(b);
        ck(ra && rb, "the children made inside the panel come back with the ids they had");
        ck(ra && rb && ra->groupId != 0 && ra->groupId == rb->groupId,
           "…and a group made in there is still a group after leaving and returning",
           "groupIds " + std::to_string(ra ? ra->groupId : -1) + " / " + std::to_string(rb ? rb->groupId : -1));
        s.exitScope();
    }

    // SAVING WHILE STILL INSIDE THE PANEL. A panel scope edits a TEMPORARY model, and the only thing that
    // ever wrote it back into the panel was leaving. But a save serialises the page — the outer model —
    // whose `children` prop still holds whatever it held when the scope was entered. So everything done
    // inside a panel and then saved without leaving it was written nowhere and gone on the next launch,
    // which is not a thing a user has any way to see coming.
    {
        s.enterPanel(id);
        const int fresh = s.model()->add("label", 10.f, 120.f, 120.f, 24.f, { { "labelText", "Typed here" } });
        ck(fresh > 0, "a widget added inside the panel");
        s.commitInstances();                       // THE SAVE-TIME FLUSH — no exitScope in sight
        const PanelElement* panel = page.get(id);
        ck(panel && panel->prop("children").find("Typed here") != std::string::npos,
           "saving while still inside the panel writes its contents back",
           panel ? panel->prop("children").substr(0, 100) : std::string("<gone>"));
        s.exitScope();
        ck(page.get(id)->prop("children").find("Typed here") != std::string::npos,
           "…and leaving afterwards keeps it, rather than writing an older copy over it");
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "what you edit inside a panel survives leaving it",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
