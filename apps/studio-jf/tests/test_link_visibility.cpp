// A LINK CANNOT REACH A HIDDEN PAGE.
//
// A nav node carries a visibility condition and the run-mode filter hides it — and its whole subtree —
// when that condition is false. That is how a tune with a module switched off stops showing that
// module's pages. A Label's Link ignored all of it: it underlined on hover, swallowed the press, and
// navigated to a page the operator's own settings had removed from the menu.
//
// Pinned here:
//   - a link to a visible node is active
//   - a link to a condition-hidden node is NOT, and does not consume the click
//   - a node hidden by an ANCESTOR is unreachable too (hiding propagates down)
//   - a link to a path that no longer exists is unreachable (renamed/deleted target)
//   - with no resolver installed at all, links still work (hosts without a nav tree)
//
//   cmake --build build --target link_visibility_test && ./build/link_visibility_test

#include "../src/surface/PanelLibrary.h"
#include <cstdio>
#include <set>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("[link-vis] %-64s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    // No resolver installed: a host with no navigation tree must not lose its links.
    hyperlink::reachable() = nullptr;
    check(hyperlink::canFollow("Tuning/Fuel"), "no resolver -> every path reachable");
    check(!hyperlink::canFollow(""),           "an empty link is never followable");

    // Stand in for the tree: these paths are hidden, and hiding propagates to descendants.
    const std::set<std::string> hidden = { "Tuning/Boost", "Diagnostics" };
    const std::set<std::string> exists = { "Tuning", "Tuning/Fuel", "Tuning/Boost",
                                           "Tuning/Boost/Targets", "Diagnostics", "Diagnostics/DTCs" };
    hyperlink::reachable() = [&](const std::string& p) {
        if (!exists.count(p)) return false;                       // renamed / deleted target
        for (const std::string& h : hidden)                       // self or any hidden ancestor
            if (p == h || p.rfind(h + "/", 0) == 0) return false;
        return true;
    };

    check(hyperlink::canFollow("Tuning/Fuel"),           "a visible node is followable");
    check(!hyperlink::canFollow("Tuning/Boost"),         "a condition-hidden node is not");
    check(!hyperlink::canFollow("Tuning/Boost/Targets"), "a node under a hidden ancestor is not");
    check(!hyperlink::canFollow("Diagnostics/DTCs"),     "hiding propagates down the whole subtree");
    check(!hyperlink::canFollow("Tuning/Ignition"),      "a path that no longer exists is not");
    check(hyperlink::canFollow("Tuning"),                "an ancestor of a hidden node stays followable");

    hyperlink::reachable() = nullptr;
    std::printf("[link-vis] %s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
