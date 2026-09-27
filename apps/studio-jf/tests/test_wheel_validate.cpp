// Does validateWheel actually catch an undecodable wheel — and stay quiet on a good one?
//
// The point of the check is that a wheel the firmware cannot sync on looks exactly like one that
// works: the dial draws it, the trace draws it, describe() names it. So the test that matters is not
// "it returns a list" but "it returns THIS fault for THIS wheel, and nothing for a 36-1".
//
//   cmake --build build --target wheel_validate_test && ./build/wheel_validate_test
#include "model/TriggerWheel.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-62s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static bool has(const std::vector<WheelIssue>& v, WheelIssue::Level l, const char* frag) {
    for (const WheelIssue& i : v)
        if (i.level == l && i.text.find(frag) != std::string::npos) return true;
    return false;
}
static int worst(const std::vector<WheelIssue>& v) { return v.empty() ? -1 : int(v.front().level); }

// A plain 36-1: the wheel every other case is a deviation from.
static WheelParams base36_1() {
    WheelParams p;
    p.name = "36-1"; p.crankType = WheelParams::CRANK_MISSING;
    p.teeth = 36; p.missing = 1; p.gapPos = { 0 };
    p.windowPct = 25;
    return p;
}

int main() {
    std::puts("=== validateWheel ===");

    // A GOOD WHEEL MUST BE SILENT, or every real fault is one more line in a list nobody reads.
    {
        const std::vector<WheelIssue> v = validateWheel(buildWheel(base36_1()));
        ck(v.empty(), "a 36-1 raises nothing", v.empty() ? "" : v.front().text);
    }

    // More missing than there are teeth: nothing left to count.
    {
        WheelParams p = base36_1(); p.teeth = 4; p.missing = 6;
        const std::vector<WheelIssue> v = validateWheel(buildWheel(p));
        ck(has(v, WheelIssue::Error, "leaves nothing to count"), "missing >= teeth is an error");
        ck(worst(v) == WheelIssue::Error, "…and it sorts to the front");
    }

    // A gap at a tooth the wheel does not have — the decoder waits for an arrival that never comes.
    {
        WheelParams p = base36_1(); p.gapPos = { 99 };
        ck(has(validateWheel(buildWheel(p)), WheelIssue::Error, "outside a 36-tooth wheel"),
           "a gap position past the last tooth is an error");
    }

    // The same tooth named twice: one gap described as two.
    {
        WheelParams p = base36_1(); p.missing = 2; p.gapPos = { 3, 3 };
        ck(has(validateWheel(buildWheel(p)), WheelIssue::Error, "listed as a gap twice"),
           "a duplicated gap position is an error");
    }

    // A match window of 0 or 100% matches nothing / everything.
    {
        WheelParams p = base36_1(); p.windowPct = 0;
        ck(has(validateWheel(buildWheel(p)), WheelIssue::Error, "cannot match anything"),
           "a zero match window is an error");
        WheelParams q = base36_1(); q.windowPct = 60;
        ck(has(validateWheel(buildWheel(q)), WheelIssue::Warn, "match the wrong tooth"),
           "a very wide match window is a warning");
    }

    // PHASE with no cam: the sync mode promises something no stream can deliver.
    {
        Wheel w = buildWheel(base36_1());
        w.sync = "PHASE";
        ck(has(validateWheel(w), WheelIssue::Error, "no cam stream"),
           "PHASE sync with no cam is an error");
    }

    // A cam present but sync left on CRANK — works, wastes what the engine is giving you.
    {
        WheelParams p = base36_1();
        p.cams.push_back(CamParams{});
        Wheel w = buildWheel(p);
        w.sync = "CRANK";
        ck(has(validateWheel(w), WheelIssue::Warn, "the phase it gives is unused"),
           "a cam with CRANK sync is a warning, not an error");
    }

    // An empty wheel says so once and stops, rather than reporting every field of nothing.
    {
        Wheel w; w.sync = "CRANK";
        const std::vector<WheelIssue> v = validateWheel(w);
        ck(v.size() == 1 && v.front().level == WheelIssue::Error, "a wheel with no streams is one error",
           std::to_string(v.size()) + " issue(s)");
    }

    std::printf("%s\n", fails ? "FAILED" : "all good");
    return fails ? 1 : 0;
}
