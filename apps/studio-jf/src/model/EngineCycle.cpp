#include "EngineCycle.h"

#include <algorithm>
#include <cmath>

namespace enginecycle {

double Cycle::wrap(double angle) const {
    if (cycle_ <= 0.0) return angle;
    double a = std::fmod(angle, cycle_);
    if (a < 0.0) a += cycle_;
    return a;
}

Trace& Cycle::trace(const std::string& label, Signal signal, int index) {
    for (Trace& t : traces_) {
        // (signal, index) is the identity when the producer gives one — two bridges may label the
        // same coil differently, and re-labelling should not split a trace in two.
        if (index >= 0 && t.index == index && t.signal == signal) return t;
        if (index < 0 && t.label == label) return t;
    }
    traces_.push_back(Trace{});
    Trace& t = traces_.back();
    t.label = label;
    t.signal = signal;
    t.index = index;
    return t;
}

void Cycle::addEdge(const std::string& label, double angle, bool high,
                    Signal signal, int index) {
    trace(label, signal, index).edges.push_back(Edge{ wrap(angle), high });
}

void Cycle::sort() {
    for (Trace& t : traces_) {
        std::stable_sort(t.edges.begin(), t.edges.end(),
                         [](const Edge& a, const Edge& b) { return a.angle < b.angle; });
        // Drop consecutive same-state edges: a second "high" adds nothing to a LEVEL and would
        // otherwise produce a zero-length span that draws as a hairline.
        //
        // Only for levels. An EVENT trace is a series of instants of the same state by nature — a
        // trigger stream is 35 rising edges and no falling ones, because the decoder captures one
        // polarity — so this rule would erase the entire wheel down to its first tooth.
        if (isEventSignal(t.signal))
            continue;
        t.edges.erase(std::unique(t.edges.begin(), t.edges.end(),
                                  [](const Edge& a, const Edge& b) { return a.high == b.high; }),
                      t.edges.end());
    }
}

std::vector<Span> Cycle::spans(const Trace& t) const {
    std::vector<Span> out;
    if (t.edges.empty()) return out;

    // Walk to the first rising edge; a trace that only ever falls within this cycle was already
    // high when the cycle began, which the wrap case below picks up.
    for (size_t i = 0; i < t.edges.size(); ++i) {
        if (!t.edges[i].high) continue;
        // Find the next falling edge after it.
        size_t j = i + 1;
        while (j < t.edges.size() && t.edges[j].high) ++j;
        if (j < t.edges.size()) {
            out.push_back(Span{ t.edges[i].angle, t.edges[j].angle, false });
            i = j;                                   // continue after the fall
        } else {
            // Rise with no fall left in the cycle: it closes at the first falling edge, which on a
            // repeating cycle is the one at the start. A dwell that begins at 700 deg and fires at
            // 10 deg is ONE span of 30 deg, not a negative one.
            double closeAt = t.edges.front().angle;
            for (const Edge& e : t.edges) {
                if (!e.high) { closeAt = e.angle; break; }
            }
            out.push_back(Span{ t.edges[i].angle, closeAt, true });
            break;
        }
    }
    return out;
}

double Cycle::spanLength(const Span& s) const {
    // ONE correction, not two. `to <= from` is exactly the condition for a span that crosses the
    // origin — including the degenerate case where it closes on its own start, which is a full cycle
    // of dwell rather than none. `wrapped` is left purely descriptive, for the view to draw with;
    // duplicating the test here made the two paths cover for each other, so a broken one could not
    // be detected.
    double d = s.to - s.from;
    if (d <= 0.0) d += cycle_;
    return d;
}

void Cycle::sortTracesForDisplay() {
    auto rank = [](Signal s) {
        switch (s) {
            case Signal::Crank:    return 0;
            case Signal::Cam:      return 1;
            // Directly under the real teeth, because it is read AGAINST them: a virtual mark that
            // has drifted off its tooth is the whole point, and that comparison needs the two lanes
            // adjacent rather than separated by the outputs.
            case Signal::Virtual:  return 2;
            case Signal::Marker:   return 3;
            case Signal::Coil:     return 4;
            case Signal::Injector: return 5;
            default:               return 6;
        }
    };
    std::stable_sort(traces_.begin(), traces_.end(), [&](const Trace& a, const Trace& b) {
        const int ra = rank(a.signal), rb = rank(b.signal);
        if (ra != rb) return ra < rb;
        if (a.index != b.index) {
            if (a.index < 0) return false;           // unindexed traces sort after numbered ones
            if (b.index < 0) return true;
            return a.index < b.index;
        }
        // Leading before trailing on a rotary, so a face's pair reads in firing order.
        if (a.trailing != b.trailing) return !a.trailing;
        return a.label < b.label;
    });
}

} // namespace enginecycle
