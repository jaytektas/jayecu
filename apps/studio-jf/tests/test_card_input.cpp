// Does a click go to the control you can SEE?
//
// Under the Card layout every child gets the whole content rect and each one's own condition decides which
// is shown — that is what the layout is for: one card per case, the page picks. The PAINT honoured that and
// the HIT-TEST did not, so every click went to the LAST child in the list whatever was on screen. On the
// trigger stream page that meant a "Cells" card showing a GAP stream's 32 cells handed its presses to the
// WIDTH label stacked behind it: no row selected, no scroll bar dragged, nothing happened at all, and the
// strip looked broken rather than unreachable.
//
// The second half is the same rule on the other axis. Input is delivered in the element scope the paint
// runs in, so a template page's "[*]" resolves for a click exactly as it does for a draw. Without that, a
// strip bound to trigger.streams[*].cell[].v asked for [#trigger.streams[*].cell_len] rows, was told 0 rows
// and returned early — drawn correctly, dead to the mouse.
//
//   cmake --build build --target card_input_test && ./build/card_input_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/MathEvaluator.h"
#include "model/UnitManager.h"
#include "model/SigilResolvers.h"
#include "surface/widgets/Array1DWidget.h"
#include "surface/widgets/PanelWidget.h"
#include "surface/WidgetRegistry.h"
#include <j/core/SceneGraph.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-64s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

// One child's element, as the panel's "children" JSON carries it.
static std::string childJson(int id, const std::string& type, const std::string& cond,
                             const std::string& extra = {}) {
    return "{\"id\":" + std::to_string(id) + ",\"type\":\"" + type + "\",\"x\":0,\"y\":0,\"w\":200,\"h\":450,"
           "\"props\":{\"condition\":\"" + cond + "\"" + (extra.empty() ? "" : "," + extra) + "}}";
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

    // A stream with a run long enough to be worth clicking, so the strip has rows to select.
    c.setConfigValue("trigger.streams[0].cell_len", 32);

    // The page's own card stack: the strip that is showing, and a label that is not. The label is LAST,
    // which is exactly the case that used to win every click.
    PanelElement panel;
    panel.id = 63;
    panel.type = "panel";
    panel.props["layoutMode"] = "4";                       // Card
    panel.props["children"] =
        "[" + childJson(1, "array1d", "1",
                        "\"signalName\":\"trigger.streams[*].cell[].v\","
                        "\"rowCount\":\"[#trigger.streams[*].cell_len]\"") +
        "," + childJson(2, "label", "0", "\"labelText\":\"this stream uses no cells\"") + "]";

    std::unique_ptr<CanvasWidget> pw = makeWidgetInstance(panel.type, graph);
    pw->setSource(panel); pw->setData(panel); pw->setCache(&c);
    // The element the page is about — what a viewport pushes down, and what "[*]" means here.
    pw->setElementContext("trigger.streams[0]");

    const jf::JRect r{ 0.f, 0.f, 200.f, 6.f * 20.f + 12.f };
    Array1DWidget::array1dCursors().clear();

    ControlInput press;
    press.kind = ControlInput::Kind::Press;
    press.mx = 40.f;                        // a row, well clear of the scroll bar's gutter
    press.my = r.y + 6.f + 2.f * 20.f + 4.f;   // the third visible row
    const bool took = pw->onControlInput(r, press);

    ck(took, "the press is consumed by the card that is SHOWING, not the hidden one behind it");

    // The proof is the strip's own state: it only exists once the strip has handled input.
    const auto& cursors = Array1DWidget::array1dCursors();
    const bool reached = !cursors.empty();
    ck(reached, "…the strip received it");
    if (reached) {
        const int sel = cursors.begin()->second.sel;
        ck(sel == 2, "…and selected the row under the cursor", "sel " + std::to_string(sel));
    }

    // The element scope reached it too: rowCount is "[#trigger.streams[*].cell_len]", which resolves to 32
    // only if "[*]" was in force during the INPUT — the row it selected proves the window was non-empty.
    {
        Array1DWidget::array1dCursors().clear();
        pw->setElementContext("");                     // no element: the template cannot resolve
        const bool blind = pw->onControlInput(r, press);
        ck(!blind || Array1DWidget::array1dCursors().empty(),
           "with no element in scope a template strip has no rows, and takes nothing");
    }

    // A control on a TEMPLATE page explains itself. The tooltip is the bound field's help, and the lookup
    // is by field name — so it has to be asked for with "[*]" already resolved. It was not: every control
    // on a templated page came up blank while the same control on a fixed page had its help, which reads
    // as the definition being silent rather than as the question being wrong.
    {
        PanelElement te;
        te.id = 77;
        te.type = "configedit";
        te.props["signalName"] = "trigger.streams[*].slots";
        std::unique_ptr<CanvasWidget> tw = makeWidgetInstance(te.type, graph);
        tw->setSource(te); tw->setData(te); tw->setCache(&c);

        const std::string direct = c.help("trigger.streams[0].slots");
        ck(!direct.empty(), "the field HAS help in the definition");

        tw->setElementContext("trigger.streams[0]");
        tw->syncTooltipForTest();
        ck(tw->tooltip() == direct, "a templated control's tooltip is its element's help",
           "tooltip \"" + tw->tooltip() + "\"");

        // …and with no element there is nothing to resolve against, so it stays quiet rather than
        // inventing the help of some other stream.
        tw->setElementContext("");
        tw->setTooltip("");
        tw->syncTooltipForTest();
        ck(tw->tooltip().empty(), "with no element in scope it says nothing",
           "tooltip \"" + tw->tooltip() + "\"");

        // A BIG CANVAS SAYS NOTHING ON HOVER. A table covers most of a page and the page already
        // names it; the field's help as a hover tip meant a paragraph over every cell, and once a
        // tooltip correctly re-appeared after each pause that became constant. An authored tooltip
        // still shows — this suppresses the DERIVED help, not a deliberate one.
        PanelElement ve;
        ve.id = 78;
        ve.type = "table";
        ve.props["signalName"] = "fuel_calculator.ve_table";
        std::unique_ptr<CanvasWidget> vw = makeWidgetInstance(ve.type, graph);
        vw->setSource(ve); vw->setData(ve); vw->setCache(&c);
        ck(!c.help("fuel_calculator.ve_table").empty(), "the VE table HAS help in the definition");
        vw->syncTooltipForTest();
        ck(vw->tooltip().empty(), "a table does not answer with it on hover",
           "tooltip \"" + vw->tooltip() + "\"");

        ve.props["tooltip"] = "Right-click to reseed";
        vw->setSource(ve); vw->setData(ve);
        vw->syncTooltipForTest();
        ck(vw->tooltip() == "Right-click to reseed", "…but an authored tooltip still shows",
           "tooltip \"" + vw->tooltip() + "\"");
    }

    // A CHILD OF A CARD IS IN THE WIDGET TREE. The framework's tooltip search descends kids from the
    // window's roots, so a control with no parent edge is never reached however good its tooltip text is
    // — which is why a page that puts its fields in cards was silent on hover from end to end.
    {
        const std::vector<CanvasWidget*> kids = pw->childWidgets();
        ck(!kids.empty(), "the panel has children at all", std::to_string(kids.size()));
        bool edged = !kids.empty();
        for (CanvasWidget* k : kids) if (!k || k->parentWidget() != pw.get()) edged = false;
        ck(edged, "…and each one's parent is the panel, so a tooltip search can reach it");
    }

    // UNITS ON A TEMPLATE PAGE. dispV/srcV/displayUnitOf are static — they take an element, not a widget —
    // and read the AUTHORED binding, so on a templated page Cache::unit() was asked for a path no field has.
    // The source unit came back empty, the conversion was skipped and there was nothing to label with: a
    // page showing ADC counts beside a fixed page showing volts.
    {
        PanelElement ue;
        ue.type = "field";
        ue.props["signalName"] = "sensors.sensor[*].diag_raw_min";      // declares ADC; preference shows volts
        const std::string fixedUnit = UnitManager::instance().displayUnitFor(c.unit("sensors.sensor[0].diag_raw_min"));
        ck(!fixedUnit.empty(), "the field has a display unit when named outright", fixedUnit);

        MathEvaluator::ElementScope scope("sensors.sensor[0]");          // what a viewport pushes down
        ck(CanvasWidget::displayUnitOf(ue) == fixedUnit,
           "…and the same unit through a template", "got \"" + CanvasWidget::displayUnitOf(ue) + "\"");
        // The conversion follows the unit: a raw count is not the number volts would be.
        const double shown = CanvasWidget::dispV(ue, Raw{512.0});
        ck(shown != 512.0, "…and the value is converted, not passed through raw",
           std::to_string(shown));
        ck(std::fabs(CanvasWidget::srcV(ue, shown).v - 512.0) < 0.01,
           "…and converts back on the way in", std::to_string(CanvasWidget::srcV(ue, shown).v));
    }

    // A WIDGET SHOWS ITS OWN UNIT, in the unit's PRETTY form. The studio places no units label — a second
    // label naming what the widget already says is a copy to keep in step — so the value carries it.
    {
        PanelElement ve;
        ve.type = "field";
        ve.props["signalName"] = "sensors.sensor[0].diag_raw_min";     // ADC, shown as volts
        const std::string shown = CanvasWidget::fmtVal(ve, c);
        ck(shown.find(' ') != std::string::npos, "a readout carries its unit", shown);
        const std::string id = CanvasWidget::displayUnitOf(ve);
        const std::string lab = CanvasWidget::displayUnitLabelOf(ve);
        ck(lab != id && !lab.empty(), "…the unit's LABEL, not the id it converts by",
           "id " + id + " label " + lab);
        ck(shown.find(lab) != std::string::npos, "…and that is what the value shows", shown);

        // RAW is the number as stored, shown as itself — no conversion and no unit.
        ve.props["displayUnit"] = "Raw";
        ck(CanvasWidget::displayUnitLabelOf(ve).empty(), "Raw shows no unit");
        const std::string raw = CanvasWidget::fmtVal(ve, c);
        ck(raw.find(lab) == std::string::npos, "…and the value beside it is bare", raw);
    }

    // A UNIT IS SHOWN BY ITS SYMBOL. unitLabel() can only pretty-print a unit that belongs to a QUANTITY,
    // so "deg" printed as "deg" while a temperature printed as "°C" — angles had no quantity to look in.
    {
        UnitManager& u = UnitManager::instance();
        ck(u.findQuantityForUnit("deg") == "Angle", "degrees belong to a quantity",
           u.findQuantityForUnit("deg"));
        ck(u.unitLabel("deg") == "\xC2\xB0", "…so the symbol is what a widget prints", u.unitLabel("deg"));
        ck(u.unitLabel("C") == "\xC2\xB0" "C", "…as it already was for temperature", u.unitLabel("C"));

        PanelElement ae;
        ae.type = "field";
        ae.props["signalName"] = "trigger.trigger_offset_btdc";      // deg, scale 0.1
        ck(CanvasWidget::displayUnitLabelOf(ae) == "\xC2\xB0", "an angle field shows the symbol",
           CanvasWidget::displayUnitLabelOf(ae));
        const std::string shown = CanvasWidget::fmtVal(ae, c);
        ck(shown.find("\xC2\xB0") != std::string::npos, "…and so does its value", shown);
    }

    std::printf("\n%s (%d failure%s)\n",
                fails ? "FAILED" : "a click lands on the control that is on screen",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
