// Does a control inside a panel SHOW that it has the keyboard?
//
// The Surface draws one focus ring per top-level element, for every widget that does not paint its own.
// A panel reports drawsOwnFocus() — correctly, or the ring would land around the whole container when one
// control inside it is focused — and a panel's children are not elements of the page at all (they live as
// JSON in the panel's "children" prop), so the Surface never sees them. The hosted framework controls were
// fine, because they paint their own ring. Everything that relies on the Surface for one — a table, a
// curve, a 1D array — got nothing the moment it was a panel's child.
//
// Same widget, same document: ringed on its own page, silent inside a panel. On the injector stage pages
// every table is a panel's child, so clicking one gave no sign it had taken the keyboard — and it HAD,
// which is worse than not taking it, because the arrow keys were live on a table that looked inert.
//
// ViewportWidget has drawn this for its mirrored children all along. PanelWidget now does the same.
//
//   cmake --build build --target panel_focus_ring_test && ./build/panel_focus_ring_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/MathEvaluator.h"
#include "model/SigilResolvers.h"
#include "surface/widgets/PanelWidget.h"
#include "surface/WidgetRegistry.h"
#include <j/core/SceneGraph.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// The panel's own rect. Free layout at this size draws its children unscaled, so a ring's size can be
// compared against the authored child size directly.
static constexpr float kPanelW = 420.f, kPanelH = 300.f;
static constexpr float kChildW = 300.f, kChildH = 200.f;

// Every focus ring in a paint: a transparent rect with a 2px Accent border, which is what both the
// Surface and the viewport push and nothing else in a panel draws.
struct Ring { float x, y, w, h; };
static std::vector<Ring> ringsIn(CanvasWidget& pw, const Cache& c) {
    jf::JPrimitiveBuffer buf;
    pw.render(buf, jf::JRect{ 0.f, 0.f, kPanelW, kPanelH }, c);
    std::vector<Ring> out;
    for (const auto& cmd : buf.getCommands()) {
        if (cmd.kind != jf::JPrimitiveBuffer::JDrawCommand::JKind::JRect) continue;
        const auto& r = cmd.rect;
        if (r.color[3] != 0 || std::fabs(r.borderWidth - 2.f) > 0.01f) continue;   // filled, or not a ring
        const bool accent = r.borderColor[0] == jf::Colors::Accent[0] &&
                            r.borderColor[1] == jf::Colors::Accent[1] &&
                            r.borderColor[2] == jf::Colors::Accent[2] &&
                            r.borderColor[3] == jf::Colors::Accent[3];
        if (!accent) continue;
        // A TABLE PAINTS ITS OWN CELL CURSOR in the same accent at the same 2px, so "an accent ring
        // was drawn" is not the question — "was one drawn around the whole child" is. The focus ring is
        // the child's rect inflated by 2 on every side and nothing else in here is that size.
        if (std::fabs(r.rectBounds[2] - (kChildW + 4.f)) > 0.01f ||
            std::fabs(r.rectBounds[3] - (kChildH + 4.f)) > 0.01f) continue;
        out.push_back({ r.rectBounds[0], r.rectBounds[1], r.rectBounds[2], r.rectBounds[3] });
    }
    return out;
}

// A panel holding one table. `focusRing` is the child's own "Focus Ring" property ("" = follow the global).
static PanelElement panelWithTable(const std::string& focusRing) {
    PanelElement panel;
    panel.id = 1;
    panel.type = "panel";
    panel.x = 0;   panel.y = 0;             // AUTHORED at the size it is drawn at, so Free layout is 1:1
    panel.w = (int)kPanelW; panel.h = (int)kPanelH;
    panel.props["labelText"]  = "Dead Time";
    panel.props["layoutMode"] = "0";                       // Free — the stage pages' panels
    panel.props["padding"]    = "0";
    panel.props["children"] =
        "[{\"id\":2,\"type\":\"table\",\"x\":10,\"y\":12,\"w\":" + std::to_string((int)kChildW) +
        ",\"h\":" + std::to_string((int)kChildH) + ",\"props\":{"
        "\"signalName\":\"fuel_calculator.stage1_dead_time_table\",\"axisMode\":\"1\"" +
        (focusRing.empty() ? "" : ",\"focusRing\":\"" + focusRing + "\"") + "}}]";
    return panel;
}

static std::unique_ptr<CanvasWidget> build(const PanelElement& el, jf::JSceneGraph& g, Cache& c) {
    std::unique_ptr<CanvasWidget> pw = makeWidgetInstance(el.type, g);
    pw->setSource(el); pw->setData(el); pw->setCache(&c);
    return pw;
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

    std::puts("=== a table inside a panel shows its keyboard focus ===");

    // The table is the widget the stage pages use, and it does NOT paint its own ring — that is the whole
    // reason the Surface draws one for it, and the reason it had none in here.
    {
        const PanelElement el = panelWithTable("");
        std::unique_ptr<CanvasWidget> pw = build(el, graph, c);
        const auto kids = pw->childWidgets();
        ck(kids.size() == 1, "the panel built its child");
        if (!kids.empty())
            ck(!kids[0]->drawsOwnFocus(),
               "a table does not paint its own ring, so somebody else must");
    }

    // NOTHING FOCUSED, NOTHING DRAWN. The check that makes the next one mean something: if a ring were
    // painted unconditionally, "it has a ring after the click" would pass without the keyboard moving.
    {
        const PanelElement el = panelWithTable("");
        std::unique_ptr<CanvasWidget> pw = build(el, graph, c);
        ck(ringsIn(*pw, c).empty(), "before anything is clicked there is no ring");
    }

    // THE CLICK. A press inside the child moves the panel's keyboard to it (_blurChildExcept), which is
    // the same state a Tab into the panel leaves behind.
    {
        const PanelElement el = panelWithTable("");
        std::unique_ptr<CanvasWidget> pw = build(el, graph, c);
        ControlInput press;
        press.kind = ControlInput::Kind::Press;
        press.mx = 10.f + kChildW * 0.5f;
        press.my = 12.f + kChildH * 0.5f;
        pw->onControlInput(jf::JRect{ 0.f, 0.f, kPanelW, kPanelH }, press);

        const std::vector<Ring> rings = ringsIn(*pw, c);
        ck(rings.size() == 1, "the focused table is ringed, exactly once",
           std::to_string(rings.size()) + " ring(s)");
        if (rings.size() == 1) {
            // …and in the right PLACE: the child sits at (10,12) in the panel's content box, and the ring
            // starts 2 above and 2 left of it. A ring at the panel's own origin would pass a size check.
            ck(rings[0].x > 0.f && rings[0].y > 0.f &&
               rings[0].x < kPanelW && rings[0].y < kPanelH,
               "…positioned on the child, inside the panel",
               std::to_string(rings[0].x) + "," + std::to_string(rings[0].y));
        }
    }

    // FOCUS RING = OFF is still off in here. The Surface asks focusRingEnabled() before drawing its own;
    // a panel that ignored it would put a ring on a page where the author had turned it off.
    {
        const PanelElement el = panelWithTable("2");        // 2 = Off
        std::unique_ptr<CanvasWidget> pw = build(el, graph, c);
        ControlInput press;
        press.kind = ControlInput::Kind::Press;
        press.mx = 10.f + kChildW * 0.5f;
        press.my = 12.f + kChildH * 0.5f;
        pw->onControlInput(jf::JRect{ 0.f, 0.f, kPanelW, kPanelH }, press);
        ck(ringsIn(*pw, c).empty(), "a child whose Focus Ring is Off is still not ringed");
    }

    std::printf("%s\n", fails ? "FAILED" : "all good");
    return fails ? 1 : 0;
}
