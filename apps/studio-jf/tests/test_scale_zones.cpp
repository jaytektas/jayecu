// SCALE ZONES — the fixed spans of a gauge's face (a tacho's red 6500-8000 and the orange below it),
// as distinct from a value RULE, which asks about the reading.
//
// Pinned here, at the model boundary both the dial (arc bands) and the scale (coloured ticks + numbers)
// paint from:
//   - a zone round-trips through the compact prop form
//   - bounds authored backwards are still a span
//   - a row missing a field drops WITHOUT taking the list with it, while a bound that is not a number
//     is KEPT as an expression for the gauge to evaluate
//   - a value's zone is the FIRST that covers it, and both bounds are inclusive
//   - a value in no zone has none — the gauge keeps its own colours
//
//   cmake --build build --target scale_zones_test && ./build/scale_zones_test

#include "../src/model/RangeBands.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[scale-zone] %-58s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  - " + detail).c_str());
    if (!ok) ++fails;
}

int main() {
    using Zone = ColorRules::Zone;

    // A tacho's face: orange from 5500, red from 6500 to the top of the scale.
    const std::string compact = "5500,6500,#ff8800;6500,8000,#ff0000";
    std::vector<Zone> zs = ColorRules::zonesFromCompact(compact);
    check(zs.size() == 2, "two zones parse from the compact form", std::to_string(zs.size()));
    check(zs[0].start == 5500 && zs[0].end == 6500 && zs[0].color == "#ff8800", "the first keeps its bounds + colour");
    check(zs[1].start == 6500 && zs[1].end == 8000 && zs[1].color == "#ff0000", "and so does the second");
    check(ColorRules::zonesToCompact(zs) == compact, "and the pair round-trips back to what was written",
          ColorRules::zonesToCompact(zs));

    // Authored backwards — dragged from the top of the scale down — is the same span.
    std::vector<Zone> back = ColorRules::zonesFromCompact("8000,6500,#ff0000");
    check(back.size() == 1 && back[0].start == 6500 && back[0].end == 8000, "bounds authored backwards are normalised");

    // A row MISSING A FIELD loses itself, not the zones either side of it.
    //
    // A bound that is not a number does NOT drop any more: it is kept as an expression and evaluated
    // where the gauge knows its channels and its units, which is how a band follows a limit that moves
    // (a rev limiter, a per-gear ceiling). Dropping them here is what silently cost the moving gauges
    // their bands, so "abc,def" is a two-expression zone and only the three-field rule still bites.
    std::vector<Zone> mixed = ColorRules::zonesFromCompact("0,1000,#00ff00;7000,#ff0000;abc,def,#ff0000;6500,8000,#ff0000");
    check(mixed.size() == 3, "a short row drops; the good ones and the expression one stay", std::to_string(mixed.size()));
    check(mixed[0].color == "#00ff00" && mixed[2].start == 6500, "... and the survivors are in the order written");
    check(mixed[1].startExpr == "abc" && mixed[1].endExpr == "def" && mixed[1].start == 0.0,
          "a bound that is not a number is kept AS WRITTEN, for the gauge to evaluate",
          mixed[1].startExpr + ".." + mixed[1].endExpr);

    // Which zone a tick belongs to. Both bounds inclusive, first match wins — 6500 is where the redline
    // starts, so the number printed at 6500 is red, not orange.
    const Zone* z = ColorRules::zoneAt(zs, 6500);
    check(z && z->color == "#ff8800", "a value on a shared edge takes the FIRST zone that covers it",
          z ? z->color : "none");
    z = ColorRules::zoneAt(zs, 8000);
    check(z && z->color == "#ff0000", "the top of the scale is inside the zone that ends there");
    z = ColorRules::zoneAt(zs, 6000);
    check(z && z->color == "#ff8800", "a value inside the warning band gets the warning colour");
    check(ColorRules::zoneAt(zs, 3000) == nullptr, "a value below every zone has none");
    check(ColorRules::zoneAt(zs, 9000) == nullptr, "and so does one past the last");

    // An empty prop is no zones, not one broken one — the state every gauge starts in.
    check(ColorRules::zonesFromCompact("").empty(), "an unset prop is simply no zones");

    // A colour with a delimiter in it is not a thing, but the escaping is shared with the rules, so pin
    // that a zone survives the same round trip a rule's expression needs.
    // Named, not positional: Zone grew its two expression members BETWEEN the bounds and the colour, so
    // `Zone{1, 2, "#aa,bb"}` quietly put the colour in startExpr and left the colour empty.
    Zone one; one.start = 1; one.end = 2; one.color = "#aa,bb";
    std::vector<Zone> esc = { one };
    const std::string ec = ColorRules::zonesToCompact(esc);
    std::vector<Zone> back2 = ColorRules::zonesFromCompact(ec);
    check(back2.size() == 1 && back2[0].color == "#aa,bb", "a comma inside a field survives the round trip", ec);

    std::printf("[scale-zone] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
