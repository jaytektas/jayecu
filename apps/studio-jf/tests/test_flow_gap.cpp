// A FLOW'S ARITHMETIC: how many children fit across, and where the gutter goes.
//
// Mode 7 (Wrap) is what makes a settings page reflow — the panels keep the size they were authored at
// and the runtime decides how many fit, so hiding the navigation dock buys a column instead of empty
// tab. That decision is pure arithmetic on a width, which is worth testing directly: the page it drives
// lives inside a viewport whose size is set by the workspace, so a screenshot of it cannot be widened
// past what the workspace allows and cannot show the wrap happening at all.
//
// WHAT WOULD MAKE EACH OF THESE RED is stated at the check. The gap ones go red if the gutter is added
// to the child's box instead of to the advance — which is the tempting way to write it, costs a column
// at exactly the widths where one is worth most, and leaves a row that no longer ends flush.
//
//   cmake --build build --target flow_gap_test && ./build/flow_gap_test

#include "../src/surface/ContainerLayout.h"

#include <cstdio>
#include <cmath>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[flow] %-62s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  — " + detail).c_str());
    if (!ok) ++fails;
}
static std::string say(const jf::JRect& r) {
    char b[96];
    std::snprintf(b, sizeof b, "x %.0f y %.0f w %.0f h %.0f", r.x, r.y, r.width, r.height);
    return b;
}

// `n` children of one size, as a settings page lays its groups out.
static std::vector<LayoutChild> groups(int n, float w = 400.f, float h = 120.f) {
    std::vector<LayoutChild> v;
    for (int i = 0; i < n; ++i) v.push_back({ jf::JRect{ 0.f, 0.f, w, h }, "" });
    return v;
}
static jf::JRect at(const std::vector<LayoutChild>& kids, int i, float boxW, float gap) {
    const jf::JRect box{ 0.f, 0.f, boxW, 2000.f };
    return layoutChildRect(7, box, kids, i, 2, 0, 1.f, 1.f, jf::JRect{ -1.f, -1.f, -1.f, -1.f }, gap);
}

int main() {
    const auto five = groups(5);

    // ---- THE AUTHORED WIDTH: three across, and the row ends inside the box -----------------------
    // 1260 is what a page authored 1280 wide leaves after its 10px margins. 400 + 12 gutter puts three
    // across (400, 412, 824 → ends at 1224) and the fourth over the edge.
    check(at(five, 0, 1260.f, 12.f).x ==   0.f, "first group sits at the left edge, no leading gap",
          say(at(five, 0, 1260.f, 12.f)));
    check(at(five, 1, 1260.f, 12.f).x == 412.f, "second is one width + one gutter across",
          say(at(five, 1, 1260.f, 12.f)));
    check(at(five, 2, 1260.f, 12.f).x == 824.f, "third still fits — 1224 of 1260 used",
          say(at(five, 2, 1260.f, 12.f)));
    check(at(five, 3, 1260.f, 12.f).x == 0.f && at(five, 3, 1260.f, 12.f).y == 132.f,
          "fourth wraps to a second row, one height + one gutter down",
          say(at(five, 3, 1260.f, 12.f)));
    check(at(five, 4, 1260.f, 12.f).y == 132.f, "fifth joins it on that row", say(at(five, 4, 1260.f, 12.f)));

    // ---- WIDER: the fourth comes up onto the first row -------------------------------------------
    // 1636 = 4x400 + 3x12 — four boxes and the three gutters BETWEEN them, which is the exact width at
    // which a fourth column appears. RED if the gutter is added to each child's box (that would need
    // 1648) or counted after the last one (1648 again): both make the page ask for 12px it will not use
    // and cost the column at exactly the width where it has just been earned.
    check(at(five, 3, 1636.f, 12.f).y == 0.f, "at exactly 4x400+3x12 the fourth is on the FIRST row",
          say(at(five, 3, 1636.f, 12.f)));
    check(at(five, 3, 1635.f, 12.f).y == 132.f, "one pixel narrower and it wraps again",
          say(at(five, 3, 1635.f, 12.f)));
    check(at(five, 4, 1636.f, 12.f).y == 132.f, "the fifth then starts the second row alone",
          say(at(five, 4, 1636.f, 12.f)));

    // ---- NO GAP: every flow that existed before must be untouched --------------------------------
    // The status-lamp dock passes no gap. RED if the default stopped being zero or the advance picked
    // one up anyway.
    const auto lamps = groups(11, 116.f, 34.f);
    check(at(lamps, 1, 1280.f, 0.f).x == 116.f, "with no gap children still pack edge to edge",
          say(at(lamps, 1, 1280.f, 0.f)));
    check(at(lamps, 10, 1280.f, 0.f).y == 0.f, "all eleven lamps fit one row of the authored width",
          say(at(lamps, 10, 1280.f, 0.f)));
    // The case the mode-7 comment is about: the same strip shown in a 240px dock. Two a row, so the
    // eleventh is on the sixth. RED the day a flow starts SCALING its children to fit instead.
    check(at(lamps, 10, 240.f, 0.f).y == 5 * 34.f && at(lamps, 10, 240.f, 0.f).width == 116.f,
          "in a 240px dock they wrap to two a row at full size, not eleven slivers",
          say(at(lamps, 10, 240.f, 0.f)));

    // ---- A GAP NEVER WIDENS A CHILD -------------------------------------------------------------
    // The box is the size it was authored at whatever the gutter is; a gutter that inflated the child
    // would make every geometry check downstream measure a width that is not the widget's.
    check(at(five, 1, 1260.f, 12.f).width == 400.f && at(five, 1, 1260.f, 12.f).height == 120.f,
          "the gutter is spacing, not size", say(at(five, 1, 1260.f, 12.f)));

    // ---- ONE CHILD WIDER THAN THE BOX still gets placed, at the left ------------------------------
    // Never a wrap before the first of a row: it would push a too-wide child down for ever.
    const auto wide = groups(1, 2000.f);
    check(at(wide, 0, 1260.f, 12.f).x == 0.f && at(wide, 0, 1260.f, 12.f).y == 0.f,
          "a child wider than the flow is placed, not pushed down", say(at(wide, 0, 1260.f, 12.f)));

    std::printf("[flow] %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
