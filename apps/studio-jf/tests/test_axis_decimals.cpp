// How many decimals does a table axis show?
//
// It used to be: the same as the cells, unless somebody set decimalsX/Y/Z by hand in the properties dock.
// So a table dragged out of the dictionary showed its rpm axis to one decimal place ("6000.0") and its
// pedal axis to none ("30"), because both simply copied the cell format — and changing an axis's channel
// in Axis Setup changed nothing about how its numbers read.
//
// An axis is READ by a channel, and that channel states its own precision, so that is the default now:
// explicit choice first, else the channel, else the cells. Nothing is stored, so it is right when the
// table is created, right when the channel changes under it, and right for tables that already exist.
//
//   cmake --build build --target axis_decimals_test && ./build/axis_decimals_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "surface/PanelModel.h"
#include "surface/widgets/TableWidget.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-62s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    std::puts("=== axis decimals: the channel says, unless you say otherwise ===");

    const TableImage t = c.resolveTable("fuel_calculator.ve_table");
    PanelElement el;
    el.type = "table";
    el.props["signalName"] = "fuel_calculator.ve_table";
    const int kCellDec = 1;                       // what the cells are showing, the old fallback

    // 1 — dragged out of the dictionary: no props at all, and each axis already reads correctly.
    {
        Cache::WriteGuard wg(c);
        c.tiSetSrc(t, 0, m.signalMap().at("rpm"));      // X: whole numbers
        c.tiSetSrc(t, 1, m.signalMap().at("app_1"));    // Y: tenths
        ck(TableWidget::axisDecimals(el, c, t, 0, kCellDec) == 0, "an rpm axis shows whole numbers");
        ck(TableWidget::axisDecimals(el, c, t, 1, kCellDec) == 1, "a pedal axis shows one decimal");
    }

    // 2 — the channel changes in Axis Setup: the axis follows, with nothing stored to update.
    {
        Cache::WriteGuard wg(c);
        c.tiSetSrc(t, 1, m.signalMap().at("aux_1"));    // generic input, no type set -> unknown
        ck(TableWidget::axisDecimals(el, c, t, 1, kCellDec) == kCellDec,
           "an unconfigured generic input states nothing: fall back to the cells");

        for (size_t i = 0; i < m.sensorTypes().size(); ++i)
            if (m.sensorTypes()[i].id == "lambda") c.setConfigValue("sensors.sensor[aux_1].type", double(i));
        ck(TableWidget::axisDecimals(el, c, t, 1, kCellDec) == 2,
           "…set that input to lambda and the axis follows it to two decimals");

        c.tiSetSrc(t, 1, m.signalMap().at("map"));
        ck(TableWidget::axisDecimals(el, c, t, 1, kCellDec) == 1, "…and to one when pointed at MAP");
    }

    // 3 — an explicit choice still wins: asking for a precision means that precision.
    {
        el.props["decimalsY"] = "4";                    // the prop encodes 1..7 == 0..6 places
        ck(TableWidget::axisDecimals(el, c, t, 1, kCellDec) == 3, "an explicit decimalsY choice overrides");
        el.props.erase("decimalsY");
        ck(TableWidget::axisDecimals(el, c, t, 1, kCellDec) == 1, "…and removing it hands the axis back");
    }

    // 4 — an axis with no channel selector at all has nothing to ask, and reads like the cells did.
    {
        Cache::WriteGuard wg(c);
        c.tiSetSrc(t, 1, -1);
        ck(TableWidget::axisDecimals(el, c, t, 1, kCellDec) == kCellDec, "no channel: the cells' own decimals");
        ck(TableWidget::axisDecimals(el, c, t, 7, kCellDec) == kCellDec, "and an axis that does not exist is safe");
    }

    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "OK", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
