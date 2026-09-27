#pragma once

// CycleDiagnostics — is the frame on screen TRUSTWORTHY, and if not, what is wrong with it.
//
// The engine-cycle view draws whatever it is given. That is the right behaviour for a view, and the
// wrong behaviour for a diagnostic tool: a frame holding 37 crank teeth instead of 116 draws
// perfectly happily, and every bar in it sits at a plausible angle. It looks like data. The only way
// to see that something is wrong is to COUNT, and to count against something.
//
// This is not hypothetical. A real capture bug shipped exactly that picture — a 116-tooth cycle
// served as 37 + 79 after the engine restarted, reported Complete, angles all sane. It was found by
// counting teeth in a bench script, not by looking at the view, because the view had no idea what it
// was supposed to be showing. That is the gap this closes.
//
// WHAT IT COMPARES AGAINST. There is no table of "correct" lane counts here, and there deliberately
// is not one: the studio does not know the wheel, the firing order, or how many teeth a cam should
// produce, and inventing an expectation it cannot justify would produce confident nonsense in the
// opposite direction. What it does know is that an engine turning steadily produces the SAME counts
// cycle after cycle. So a frame is measured against a reference frame — normally the previous one —
// and any lane whose count CHANGED is called out. That catches a dropped tooth, a missing injector
// pulse, a split cycle and a lane that vanished, without needing to be told what any of them should
// have been.
//
// A count that changes is not automatically a fault: the engine may genuinely have done something
// different, which is precisely what a tuner is looking for. So these are reported as observations
// with both numbers, never as a verdict.

#include "EngineCycle.h"

#include <cstdint>
#include <string>
#include <vector>

namespace enginecycle {

// One lane's contribution to the frame.
struct LaneCount {
    std::string label;                    // "Crank", "Coil 3"
    Signal      signal = Signal::Unknown;
    int         edges  = 0;               // edges recorded in this frame
    int         spans  = 0;               // resolved high spans (level lanes only; 0 for event lanes)
    bool        unpaired = false;         // a level lane with an odd edge count — a dwell with no end
};

// Severity, so the view can colour without re-deciding what matters.
enum class Severity : uint8_t {
    Info,      // worth stating, not wrong ("reconstructed from a time-domain log")
    Warning,   // the frame differs from its neighbour — may be real engine behaviour
    Error,     // internally inconsistent, and no engine can produce it
};

struct Finding {
    Severity    severity = Severity::Info;
    std::string text;
};

struct Diagnosis {
    std::vector<LaneCount> lanes;         // display order, as the cycle holds them
    std::vector<Finding>   findings;      // empty = nothing to report
    int  totalEdges = 0;
    bool trustworthy = true;              // false when any Error finding is present

    // One-line summary for the panel header: "116 crank · 72 grid · 2 cam · 8 coil · 8 inj".
    [[nodiscard]] std::string summary() const;
    // The worst severity present, for colouring the header.
    [[nodiscard]] Severity worst() const;
};

// Diagnose `c`. `reference` is the frame to compare lane counts against — normally the previously
// displayed one. Pass nullptr for the first frame, where there is nothing to compare and only
// internal checks apply.
//
// `sequenceGap` is how many engine cycles the producer counted between the two frames (0 = unknown
// or not consecutive). Reported rather than judged: reading every cycle is not the normal case, and
// a gap is only interesting when the user expected not to have one.
[[nodiscard]] Diagnosis diagnose(const Cycle& c, const Cycle* reference);

// Human name for a signal group, used in the summary and the findings.
[[nodiscard]] const char* signalName(Signal s);

} // namespace enginecycle
