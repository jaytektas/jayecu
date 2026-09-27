// VALUE RANGES REACH A HOSTED CONTROL — on the role it actually paints from.
//
// A hosted control (button, spin box, combo) is a real framework control painting itself, so a fill
// drawn behind it cannot show — it paints opaque over it. A range rule therefore reaches it through a
// scoped PALETTE instead, and that only works if the rule sets the role the control reads.
//
// It set Base and Text. Base is the FIELD surface — line edit, spin box, combo — and jstyle::buttonFill
// resolves Button when resting, ToolTipBase when hovered and Highlight when pressed, never Base. So a
// red/green rule on a Command Button was accepted, evaluated, matched, and changed nothing on screen:
// the one control where a status colour is most often wanted was the one control it could not reach.
//
// Pinned here, against the same call the paint makes:
//   - a rule's bg reaches Button (resting) as well as Base
//   - …and the hovered/pressed fills too, so the colour does not vanish under the pointer
//   - a rule's fg reaches ButtonText as well as Text
//   - a rule that overrides nothing asks for no palette at all (the control paints untouched)
//
//   cmake --build build --target hosted_ranges_test && ./build/hosted_ranges_test

#include "../src/surface/widgets/CommandButtonWidget.h"

#include <j/core/JStyle.h>
#include <j/core/SceneGraph.h>

#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[hosted-range] %-58s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  - " + detail).c_str());
    if (!ok) ++fails;
}
static std::string hex(const jf::JColor& c) {
    char b[16]; std::snprintf(b, sizeof b, "%02x%02x%02x", c.r, c.g, c.b); return b;
}
static std::string roleHex(const jf::JPalette& p, jf::JColorRole r) { return hex(p.color(r)); }

int main() {
    jf::JSceneGraph graph;
    CommandButtonWidget btn(graph);

    // The rule a "calibration succeeded" button carries: green fill, black caption, always on so the
    // test asks about the colours and not about the expression (that is range_when's job).
    CanvasWidget::Range ok;
    ok.bg = "#2ea043"; ok.fg = "#000000"; ok.when = "1";
    btn.m_ranges.push_back(ok);

    const CanvasWidget::Range* active = btn.activeRange();
    check(active != nullptr, "a `when` band that holds is the active rule");

    jf::JPalette pal;
    const bool wants = CommandButtonWidget::rulePalette(active, pal);
    check(wants, "…and it asks for a palette");

    // THE REGRESSION: this was the theme's own Button colour, whatever the rule said.
    check(roleHex(pal, jf::JColorRole::Button) == "2ea043", "the rule's bg reaches Button — the resting fill",
          roleHex(pal, jf::JColorRole::Button));
    check(roleHex(pal, jf::JColorRole::ToolTipBase) == "2ea043", "…the hovered fill",
          roleHex(pal, jf::JColorRole::ToolTipBase));
    check(roleHex(pal, jf::JColorRole::Highlight) == "2ea043", "…and the pressed fill",
          roleHex(pal, jf::JColorRole::Highlight));
    check(roleHex(pal, jf::JColorRole::Base) == "2ea043", "a field control still gets it on Base",
          roleHex(pal, jf::JColorRole::Base));
    check(roleHex(pal, jf::JColorRole::ButtonText) == "000000", "the rule's fg reaches ButtonText",
          roleHex(pal, jf::JColorRole::ButtonText));
    check(roleHex(pal, jf::JColorRole::Text) == "000000", "…and Text", roleHex(pal, jf::JColorRole::Text));

    // A DISABLED button keeps the status colour rather than fading toward some other surface — the
    // button is commonly disabled while the routine it reports on is running.
    check(hex(pal.color(jf::JColorRole::Button, jf::JColorGroup::Disabled)) == "2ea043",
          "the colour survives the disabled group",
          hex(pal.color(jf::JColorRole::Button, jf::JColorGroup::Disabled)));

    // A band that overrides no colour channel must not install a palette at all.
    {
        CanvasWidget::Range blank;
        blank.when = "1";
        jf::JPalette p2;
        check(!CommandButtonWidget::rulePalette(&blank, p2), "a rule with no colours asks for nothing");
        check(!CommandButtonWidget::rulePalette(nullptr, p2), "…and neither does no rule at all");
    }

    std::printf("[hosted-range] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
