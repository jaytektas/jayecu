// THE LEGEND'S EYE HIDES ONE TRACE AND LEAVES THE REST ALONE.
//
// A trace view carries several channels, and reading one out of six means hiding the others for a moment.
// That has to be a glance, not an edit: the hidden line keeps its place, its colour and its range, because
// coming back to a trace whose colour has changed while it was away is worse than never hiding it.
//
// So this drives the real widget — parse the compact list, hit the legend where it is drawn, toggle — and
// checks what the click changed and, more importantly, what it did not.
//
//   cmake --build build --target trace_legend_test && ./build/trace_legend_test

#include "model/Cache.h"
#include "model/LineGraphModel.h"
#include "model/MetaModel.h"
#include "surface/PanelModel.h"
#include "surface/Surface.h"
#include "surface/widgets/LiveGraphWidget.h"

#include <j/core/SceneGraph.h>
#include <j/graphics/FontEngine.h>

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const char* what, const std::string& note = {}) {
    std::printf("[legend] %-58s %s%s\n", what, ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    // ---- THE COMPACT FORM ------------------------------------------------------------------------
    // hidden is a SIXTH field, appended. Rows written before the eye existed have five, and must come back
    // shown — a graph saved last week is not a graph with everything switched off.
    {
        LineGraphModel old = LineGraphModel::fromCompact("rpm,0,8000,0,0\nmap,0,300,0,0");
        ck(old.lines.size() == 2, "a five-field row still parses");
        ck(!old.lines[0].hidden && !old.lines[1].hidden, "...and its lines are shown");

        LineGraphModel m;
        m.lines.push_back({ "rpm", 0, 8000, false, false, false });
        m.lines.push_back({ "map", 0, 300, false, false, true });
        const LineGraphModel back = LineGraphModel::fromCompact(m.toCompact());
        ck(back.lines.size() == 2 && !back.lines[0].hidden && back.lines[1].hidden,
           "hidden survives the round trip", m.toCompact());
        ck(back.lines[1].min == 300 * 0 && back.lines[1].max == 300,
           "...and so does the hidden line's range");
    }

    // ---- THE CLICK -------------------------------------------------------------------------------
    MetaModel meta;
    if (!meta.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&meta);
    c.setConfigImage(meta.defaultImage());
    jf::JFontEngine fe;
    if (!fe.loadFromFile(TEST_FONT)) { std::fprintf(stderr, "cannot load %s\n", TEST_FONT); return 77; }
    jf::JTextHelper::setAtlas(fe.buildAtlas(14.f));

    jf::JSceneGraph graph;
    PanelModel page;
    page.setCanvasSize(900.f, 600.f);
    page.setCanvasStatic(1);
    const std::string lines = "boost_target,0,300,0,0\nmap,0,300,0,0\nwastegate_duty,0,100,0,0";
    const int id = page.add("livegraph", 40.f, 40.f, 400.f, 200.f,
                            { { "lines", lines }, { "showLegend", "1" } });

    Surface s(graph, c, &page);
    s.setBounds({ 0.f, 0.f, 900.f, 600.f });
    s.setMode(Surface::Mode::Run);
    // PINNED TO 1:1, so this test says what it means. The page canvas and the surface bounds are
    // deliberately the same size here; fitting one to the other is exactly scale 1, whatever the
    // user's "static surface size" preference happens to be. Static is no longer 1:1 — it renders a
    // page AT the preference size — so without this a hit-test lands wherever that preference put it.
    s.setFitToView(true);
    jf::JPrimitiveBuffer buf;
    s.populateRenderPrimitives(buf);

    const PanelElement& e = *page.get(id);
    const jf::JRect box{ e.x, e.y, e.w, e.h };
    std::vector<std::string> labels = { "boost_target", "map", "wastegate_duty" };
    // Aim at the SECOND entry, measured the way the widget measures it.
    const float legendH = LiveGraphWidget::legendHeight(box);
    const float sw = legendH * 0.5f;
    const float e0w = sw + 6.f + jf::JTextHelper::measureWidth(labels[0]);
    const float midY = box.y + legendH * 0.5f;
    const float atE1 = box.x + 4.f + e0w + 10.f + 2.f;
    ck(LiveGraphWidget::legendEntryAt(box, labels, atE1, midY) == 1,
       "the hit-test finds the entry the legend drew there",
       std::to_string(LiveGraphWidget::legendEntryAt(box, labels, atE1, midY)));
    ck(LiveGraphWidget::legendEntryAt(box, labels, atE1, box.y + box.height - 4.f) == -1,
       "a click in the PLOT is not a legend click");

    s.handleMousePress(atE1, midY);
    s.handleMouseRelease(atE1, midY);
    // Widgets are the running state: an override lives on the instance until a serialization boundary
    // flushes it, so ask for the flush rather than reading a model that has not been told yet.
    s.commitInstances();
    const LineGraphModel after = LineGraphModel::fromCompact(page.get(id)->prop("lines"));
    ck(after.lines.size() == 3, "the line list still has every line in it",
       std::to_string(after.lines.size()));
    if (after.lines.size() == 3) {
        ck(!after.lines[0].hidden && after.lines[1].hidden && !after.lines[2].hidden,
           "clicking one eye hides exactly that trace", page.get(id)->prop("lines"));
        ck(after.lines[1].channel == "map" && after.lines[1].max == 300,
           "...and the hidden line keeps its channel, its place and its range");
    }

    // Clicking it again brings it back — the eye is a toggle, not a delete.
    s.handleMousePress(atE1, midY);
    s.commitInstances();
    const LineGraphModel back2 = LineGraphModel::fromCompact(page.get(id)->prop("lines"));
    ck(back2.lines.size() == 3 && !back2.lines[1].hidden, "clicking it again shows it",
       page.get(id)->prop("lines"));

    std::printf("[legend] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
