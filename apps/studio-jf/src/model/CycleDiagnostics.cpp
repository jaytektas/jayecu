#include "CycleDiagnostics.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace enginecycle {

const char* signalName(Signal s) {
    switch (s) {
        case Signal::Crank:    return "crank";
        case Signal::Cam:      return "cam";
        case Signal::Coil:     return "coil";
        case Signal::Injector: return "inj";
        case Signal::Virtual:  return "grid";
        case Signal::Marker:   return "marker";
        case Signal::Unknown:  break;
    }
    return "other";
}

namespace {

// Totals per signal group, which is the level the counts are meaningful at: a 60-2 produces 116
// CRANK edges however they are split across streams, and it is that total which must hold steady.
std::map<Signal, int> groupTotals(const Cycle& c) {
    std::map<Signal, int> t;
    for (const Trace& tr : c.traces()) t[tr.signal] += static_cast<int>(tr.edges.size());
    return t;
}

} // namespace

std::string Diagnosis::summary() const {
    // Grouped, in the order the lanes appear, so the line reads the same way the view is stacked.
    // WHAT EACH GROUP IS COUNTED IN, because the two are not the same question and the line has to
    // read the way it is spoken. A coil or an injector is a THING: "6 coil" means six of them, and
    // summing edges there said 12 on a six-cylinder — one per dwell start and one per spark — which
    // reads as twelve coils and was reported as a bug on exactly that basis. A crank, a cam or the
    // PLL grid is not a thing you count, it is a stream of events: "68 crank" is the teeth seen in
    // the cycle, which is the number that has to hold steady and is worth watching.
    //
    // So: output lanes are counted as LANES, trigger streams as EDGES. Each reads as the unit its
    // own name implies, which is why no unit is printed.
    auto counts_lanes = [](Signal s) { return s == Signal::Coil || s == Signal::Injector; };
    std::map<Signal, int> t;
    std::vector<Signal>   order;
    for (const LaneCount& l : lanes) {
        if (!t.count(l.signal)) order.push_back(l.signal);
        t[l.signal] += counts_lanes(l.signal) ? 1 : l.edges;
    }
    std::ostringstream os;
    bool first = true;
    for (Signal s : order) {
        if (!first) os << "  ";
        os << t[s] << ' ' << signalName(s);
        first = false;
    }
    if (first) os << "empty";
    return os.str();
}

Severity Diagnosis::worst() const {
    Severity w = Severity::Info;
    for (const Finding& f : findings)
        if (f.severity > w) w = f.severity;
    return w;
}

Diagnosis diagnose(const Cycle& c, const Cycle* reference) {
    Diagnosis d;

    for (const Trace& tr : c.traces()) {
        LaneCount lc;
        lc.label  = tr.label;
        lc.signal = tr.signal;
        lc.edges  = static_cast<int>(tr.edges.size());
        if (!isEventSignal(tr.signal)) {
            lc.spans    = static_cast<int>(c.spans(tr).size());
            // An odd edge count on a level lane means one end of a span is not in THIS cycle. That
            // is routine at speed: at 15000 rpm a cycle is 8 ms and an injection pulse simply runs
            // past the boundary, opening in one cycle and closing in the next. The model already
            // resolves a wrapped span against the first edge, which is what the engine actually did.
            //
            // It is reported, because it is also what a LOST edge looks like and the two cannot be
            // told apart from one frame — but as an observation, not a fault. Calling it an error
            // marked every high-rpm frame untrustworthy, which is worse than saying nothing: a
            // warning that fires on healthy data teaches the user to ignore warnings.
            lc.unpaired = (lc.edges % 2) != 0;
        }
        d.totalEdges += lc.edges;
        d.lanes.push_back(std::move(lc));
    }

    if (c.isReconstructed())
        d.findings.push_back({ Severity::Info,
                               "angles reconstructed from a time-domain log, not measured" });
    if (!c.note().empty())
        d.findings.push_back({ Severity::Info, c.note() });

    for (const LaneCount& l : d.lanes)
        if (l.unpaired)
            d.findings.push_back({ Severity::Warning,
                                   l.label + ": " + std::to_string(l.edges) +
                                   " edges — a span crosses the cycle boundary, or lost an end" });

    // Against the neighbouring frame. Nothing here knows what the counts SHOULD be; it knows only
    // that a steadily turning engine repeats them.
    if (reference && !reference->empty()) {
        const auto now  = groupTotals(c);
        const auto were = groupTotals(*reference);

        for (const auto& [sig, n] : now) {
            auto it = were.find(sig);
            if (it == were.end()) {
                d.findings.push_back({ Severity::Warning,
                                       std::string(signalName(sig)) + " appeared: " +
                                       std::to_string(n) + " edges, none in the previous frame" });
            } else if (it->second != n) {
                d.findings.push_back({ Severity::Warning,
                                       std::string(signalName(sig)) + " " + std::to_string(n) +
                                       " edges, was " + std::to_string(it->second) });
            }
        }
        for (const auto& [sig, n] : were) {
            if (!now.count(sig))
                d.findings.push_back({ Severity::Warning,
                                       std::string(signalName(sig)) + " missing — " +
                                       std::to_string(n) + " edges in the previous frame" });
        }

        if (reference->cycleAngle() != c.cycleAngle())
            d.findings.push_back({ Severity::Warning,
                                   "span changed: " + std::to_string(static_cast<int>(c.cycleAngle())) +
                                   " deg, was " + std::to_string(static_cast<int>(reference->cycleAngle())) });
    }

    for (const Finding& f : d.findings)
        if (f.severity == Severity::Error) d.trustworthy = false;

    return d;
}

} // namespace enginecycle
