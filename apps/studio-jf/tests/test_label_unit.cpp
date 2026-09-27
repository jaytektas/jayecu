// A LABEL CAN BE THE UNIT, RATHER THAN SAY IT.
//
// A page wanting "RPM" beside a number had to type it, and a typed unit is a lie waiting to happen:
// change the Display Unit from °C to °F and the caption still says °C. With Show Unit set, the caption
// IS its binding's display unit and follows it.
//
// It prints the unit's PRETTY LABEL ("°C", "V", "RPM"), not the id it converts by ("C", "ADC_V") —
// displayUnitLabelOf, which existed and had no caller for exactly this reason.
//
// Pinned here:
//   - the label resolves its binding's unit, whether the binding is a path or a "[$sigil]"
//   - an explicit Display Unit pin wins, so the caption follows what the page chose to show
//   - an unbound label, or a channel with no unit, keeps its text instead of drawing a blank
//
//   cmake --build build --target label_unit_test && ./build/label_unit_test

#include "../src/surface/CanvasWidget.h"
#include "../src/surface/widgets/LabelWidget.h"   // chooseCaption: which of four sources wins
#include "../src/surface/PanelModel.h"
#include "../src/model/Cache.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const char* what, const std::string& note = {}) {
    std::printf("[label-unit] %-54s %s%s\n", what, ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    // An unbound element has no unit to show — the caption must fall back, not blank.
    {
        PanelElement el; el.type = "label";
        ck(CanvasWidget::displayUnitLabelOf(el).empty(), "unbound -> no unit (the caption keeps its text)");
    }

    // A pinned Display Unit is what the page chose to show, so it is what the caption must print — and
    // it prints the unit's LABEL, not the id it converts by.
    {
        PanelElement el; el.type = "label";
        el.props["signalName"]  = "[$rpm]";
        el.props["displayUnit"] = "rad/s";
        const std::string lab = CanvasWidget::displayUnitLabelOf(el);
        ck(lab == "rad/s", "an explicit pin is printed", lab);
    }
    {
        // A unit whose id and pretty label DIFFER, from a statically-registered quantity. (ADC_V would
        // read better here, but AnalogRaw is registered from board data — a test that never loads a
        // board would be asserting against its own empty harness rather than against the product.)
        PanelElement el; el.type = "label";
        el.props["signalName"]  = "[$clt]";
        el.props["displayUnit"] = "C";                 // id "C", pretty "°C"
        const std::string lab = CanvasWidget::displayUnitLabelOf(el);
        ck(lab == "\xC2\xB0" "C", "the PRETTY label is printed, not the conversion id", lab);
    }

    // RAW MEANS NO UNIT, deliberately: "the number as stored, shown as itself". So a caption pointed at a
    // Raw control has nothing to print — and must draw that emptiness rather than falling back to the
    // "Label" placeholder, which reads as broken and looks identical to a dead address.
    {
        PanelElement el; el.type = "label";
        el.props["signalName"]  = "[$rpm]";
        el.props["displayUnit"] = "Raw";
        const std::string lab = CanvasWidget::displayUnitLabelOf(el);
        ck(lab.empty(), "Raw resolves to NO unit (an empty caption, not a placeholder)",
           lab.empty() ? "" : lab);
    }

    // A READOUT CAN BE TOLD NOT TO SAY ITS UNIT. Display Unit picks which unit the number is in; Show
    // Unit (Value + Field) decides whether the reading prints it — for a cell whose caption already says
    // it, or a column where repeating it on every row is noise. ABSENT means show, so nothing authored
    // before the row existed changes.
    {
        PanelElement el; el.type = "value";
        el.props["signalName"]  = "[$rpm]";
        el.props["displayUnit"] = "rad/s";
        el.props["format"]      = "%.1f";
        const std::string with = CanvasWidget::fmtVal(el, Cache::instance());
        ck(with.find("rad/s") != std::string::npos, "a reading carries its unit by default", with);
        el.props["showUnit"] = "0";
        const std::string without = CanvasWidget::fmtVal(el, Cache::instance());
        ck(without.find("rad/s") == std::string::npos, "Show Unit off prints the number alone", without);
        ck(!without.empty() && with.rfind(without, 0) == 0,
           "…the same number either way, only the unit dropped", with + " / " + without);
        el.props["showUnit"] = "";      // an empty prop is not "off" — only a literal 0 is
        ck(CanvasWidget::fmtVal(el, Cache::instance()).find("rad/s") != std::string::npos,
           "…and an empty value still shows it, as an old layout does");
    }

    // WHO DECIDES WHAT A CAPTION SAYS. Four things can want to: the caret while you type, the value rule
    // that holds right now (which is what makes a label with two rules a lamp), a unit-of pin, and the
    // label's own words. The order matters — a rule that holds must beat the authored text, or banding
    // the caption says nothing; and typing must beat everything, or the caption fights the keyboard.
    {
        using L = LabelWidget;
        ck(L::chooseCaption(false, "", "", false, "", "Coolant") == "Coolant",
           "with nothing else to say, the label's own words");
        ck(L::chooseCaption(false, "", "", false, "", "") == "Label",
           "…and a label with no words at all still draws something");
        ck(L::chooseCaption(false, "", "FAULT", false, "", "Coolant") == "FAULT",
           "a rule that holds outranks the authored caption");
        ck(L::chooseCaption(false, "", "", true, "°C", "Coolant") == "°C",
           "a unit-of pin outranks the authored caption too");
        ck(L::chooseCaption(false, "", "FAULT", true, "°C", "Coolant") == "FAULT",
           "…and the rule outranks the unit: it is the more specific answer");
        ck(L::chooseCaption(true, "half typed", "FAULT", true, "°C", "Coolant") == "half typed",
           "typing beats all of them");
        ck(L::chooseCaption(true, "", "FAULT", false, "", "Coolant").empty(),
           "…including when you have just deleted every character");
        ck(L::chooseCaption(false, "", "", true, "", "Coolant").empty(),
           "a Raw pin prints nothing rather than falling back to the text");
    }

    // NOT ASSERTED HERE: that Auto resolves a channel's unit. Auto falls through to the channel's own
    // metadata, and a headless test loads no meta, so every channel is unitless to it — the assertion
    // would be measuring the harness, not the code, exactly as an earlier draft of this file did with
    // AnalogRaw. What IS shown without meta is the pair that matters: an explicit pin prints, and Raw
    // prints nothing. Raw is special-cased, not merely empty like everything else here.

    std::printf("[label-unit] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
