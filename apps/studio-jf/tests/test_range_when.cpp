// A BAND CAN BE A CONDITION, NOT JUST AN INTERVAL.
//
// [start,end] can only ask one question about one number. The question worth asking is usually
// narrower — "hot, but only once it is running", "lean under boost", "over target while closed loop is
// engaged" — and an interval cannot express any of them. A band now carries an optional `when`
// expression which, when set, decides the band instead of its interval.
//
// `value` is the control's own reading. It has to be a keyword rather than the existing "$*" because
// "$*" only resolves for a widget bound to a config ELEMENT, and because naming the channel a second
// time inside the band means a rename leaves the band pointing at the old one.
//
// Pinned here:
//   - the compact form round-trips a `when`, INCLUDING the commas and semicolons an expression contains
//   - a string authored before `when` existed still parses, and keeps its interval
//   - `value` substitutes only as a whole word: some.value.thing / myvalue / valueX survive
//   - substitution is exact enough to compare against (no precision loss on a plain reading)
//
//   cmake --build build --target range_when_test && ./build/range_when_test

#include "../src/model/RangeBands.h"   // ColorRules
#include "../src/surface/CanvasWidget.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[range-when] %-58s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  - " + detail).c_str());
    if (!ok) ++fails;
}

int main() {
    // ---- round trip, with delimiters inside the expression -------------------------------------
    {
        ColorRules rb;
        ColorRule b;
        b.bg = "#332200"; b.blink = true;
        // Commas (a call's arguments) and a semicolon: both are the compact form's own delimiters.
        b.when = "value > 100 && select(clt > 90, 1, 0) && a;b";
        rb.rules.push_back(b);
        const std::string compact = rb.toCompact();
        const ColorRules back = ColorRules::fromCompact(compact);
        check(back.rules.size() == 1, "one band survives a round trip",
              "got " + std::to_string(back.rules.size()) + " from " + compact);
        check(!back.rules.empty() && back.rules[0].when == b.when,
              "an expression containing , and ; round-trips intact",
              back.rules.empty() ? "" : back.rules[0].when);
        check(!back.rules.empty() && back.rules[0].bg == "#332200" && back.rules[0].blink,
              "the other fields are undisturbed by the escaping");
    }

    // ---- a pre-`when` string still parses -------------------------------------------------------
    {
        const ColorRules old = ColorRules::fromCompact("6500,8000,#aa0000,,,,1");
        // The interval is GONE from the model, so a rule saved in the old form arrives as the expression
        // it always meant. Normalisation at the parse boundary, not a second model kept alive.
        check(old.rules.size() == 1 && old.rules[0].when == "value >= 6500 && value <= 8000",
              "an old numeric band parses as the expression it always meant",
              old.rules.empty() ? "" : old.rules[0].when);
        check(old.rules.size() == 1 && old.rules[0].bg == "#aa0000" && old.rules[0].blink,
              "... keeping its colours and its flash");
    }

    // ---- two bands, one with an expression ------------------------------------------------------
    {
        ColorRules rb;
        ColorRule a; a.when = "value > 1, 2"; a.bg = "#111111";
        ColorRule c; c.when = "value < 10";   c.bg = "#222222";
        rb.rules = { a, c };
        const ColorRules back = ColorRules::fromCompact(rb.toCompact());
        check(back.rules.size() == 2, "two rules survive, delimiters and all");
        check(back.rules.size() == 2 && back.rules[0].when == "value > 1, 2"
              && back.rules[1].when == "value < 10" && back.rules[1].bg == "#222222",
              "order and colours are preserved across the round trip");
    }

    // ---- a rule can carry the CAPTION to show while it holds -------------------------------------
    // The seventh field. A label with two rules is a lamp, so the words belong on the rule beside the
    // colours — and an old six-field rule has to keep meaning exactly what it meant.
    {
        ColorRules rr;
        ColorRule a; a.bg = "#ff453a"; a.when = "value > 100"; a.text = "FAULT";
        ColorRule b; b.bg = "#30d158"; b.when = "value <= 100";          // no text: keeps the label's own
        rr.rules = { a, b };
        const std::string compact = rr.toCompact();
        const ColorRules back = ColorRules::fromCompact(compact);
        check(back.rules.size() == 2, "both rules survive the round trip", compact);
        check(back.rules[0].text == "FAULT", "a rule's caption round-trips", back.rules[0].text);
        check(back.rules[0].when == "value > 100", "…without disturbing its condition", back.rules[0].when);
        check(back.rules[0].bg == "#ff453a", "…or its colours", back.rules[0].bg);
        check(back.rules[1].text.empty(), "a rule with no caption stays that way");
        check(compact.find(",FAULT") != std::string::npos && compact.find("#30d158,,,,0,value <= 100") != std::string::npos,
              "the empty one writes no seventh field at all", compact);

        // A caption containing the delimiters survives, like the expression beside it.
        ColorRules cr; ColorRule c; c.when = "value > 1"; c.text = "over, by a lot; really";
        cr.rules = { c };
        check(ColorRules::fromCompact(cr.toCompact()).rules[0].text == "over, by a lot; really",
              "a caption may hold commas and semicolons");
    }

    // ---- a rule written before captions existed reads back unchanged ------------------------------
    {
        const auto six = ColorRules::fromCompact("#ff453a,,,,0,value > 6500");
        check(six.rules.size() == 1 && six.rules[0].when == "value > 6500" && six.rules[0].text.empty(),
              "a six-field rule is still a six-field rule");
        // The OLD interval form is seven fields too — field 0 tells them apart, not the count.
        const auto old7 = ColorRules::fromCompact("10,20,#ff453a,,,,1");
        check(old7.rules.size() == 1 && old7.rules[0].when == "value >= 10 && value <= 20",
              "…and a seven-field INTERVAL is still read as an interval, not as a caption",
              old7.rules.empty() ? "" : old7.rules[0].when);
        check(!old7.rules.empty() && old7.rules[0].text.empty(), "…with no caption invented for it");
    }

    // ---- the `value` keyword --------------------------------------------------------------------
    {
        check(CanvasWidget::substValue("value > 100", 42) == "42 > 100", "`value` becomes the reading",
              CanvasWidget::substValue("value > 100", 42));
        check(CanvasWidget::substValue("value > 1 && value < 9", 5) == "5 > 1 && 5 < 9",
              "every occurrence is substituted");
        check(CanvasWidget::substValue("some.value.thing > 1", 42) == "some.value.thing > 1",
              "a path containing `value` is left alone",
              CanvasWidget::substValue("some.value.thing > 1", 42));
        check(CanvasWidget::substValue("myvalue > 1", 42) == "myvalue > 1", "a longer identifier is not it");
        check(CanvasWidget::substValue("valueX > 1", 42) == "valueX > 1", "nor is a longer one the other way");
        check(CanvasWidget::substValue("$value > 1", 42) == "$value > 1", "nor a sigil that happens to spell it");
        check(CanvasWidget::substValue("no keyword here", 42) == "no keyword here", "an expression without it is untouched");
        check(CanvasWidget::substValue("value", 0) == "0", "the bare keyword alone substitutes");
    }

    std::printf("[range-when] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
