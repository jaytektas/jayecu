#include "TriggerGeometry.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace {

double wrap(double x, double period) { return std::fmod(std::fmod(x, period) + period, period); }

// Present-tooth angles for a GAP wheel. Mirrors GapMatcher: pitch into present-tooth index k is
// ratio×tooth_angle when k is a gap index, else tooth_angle; the missing arc is the >1.5× spacing.
StreamGeometry gapGeometry(int slots, int ratio, const std::vector<int>& cells, int rate) {
    StreamGeometry g;
    g.rate = rate; g.primitive = 0;
    g.period = (rate == 0) ? kCrankPeriod : kCamPeriod;
    slots = std::max(1, slots);
    g.slots = slots;
    const double toothAngle = g.period / slots;
    g.toothAngle = toothAngle;

    const int ngap = static_cast<int>(cells.size());
    std::set<int> gapset(cells.begin(), cells.end());
    const int r = (ngap && ratio) ? ratio : 1;
    const int missing = ngap * (r - 1);
    const int present = std::max(1, slots - missing);
    g.present = present;

    g.teeth.push_back(0.0);
    double pos = 0.0;
    for (int idx = 1; idx < present; ++idx) {
        const double pitch = (gapset.count(idx) ? r : 1) * toothAngle;
        pos += pitch;
        g.teeth.push_back(pos);
    }

    // gap arcs = any consecutive-tooth spacing (incl. the wrap) wider than 1.5× a tooth
    for (int i = 0; i < present; ++i) {
        const double a = g.teeth[i];
        const double b = g.teeth[(i + 1) % present] + ((i + 1 == present) ? g.period : 0.0);
        if (b - a > 1.5 * toothAngle)
            g.gaps.emplace_back(a + toothAngle / 2, b - toothAngle / 2);
    }
    return g;
}

// Edge angles for a SEQUENCE wheel: cumulative inter-edge spans (cell[] in 0.1°).
StreamGeometry seqGeometry(const std::vector<int>& cells, int rate) {
    StreamGeometry g;
    g.rate = rate; g.primitive = 1;
    g.period = (rate == 0) ? kCrankPeriod : kCamPeriod;
    double acc = 0.0;
    for (int c : cells) { g.teeth.push_back(wrap(acc, g.period)); acc += c / 10.0; }
    g.toothAngle = cells.empty() ? 0.0 : (acc / static_cast<double>(cells.size()));
    g.slots = static_cast<int>(g.teeth.size());
    g.present = g.slots;
    return g;
}

// A single representative cam pulse (WIDTH). Seed wheels use a wide-open window; draw a clear
// mid-width block at the target so the trace reads as "one cam pulse per cycle".
StreamGeometry widthGeometry(int /*wmin*/, int wmax, int wtgt, int rate) {
    StreamGeometry g;
    g.rate = rate; g.primitive = 2;
    g.period = (rate == 1) ? kCamPeriod : kCrankPeriod;
    const double start = wrap(wtgt / 10.0, g.period);
    double width = wmax / 10.0;
    if (!(g.period / 12 <= width && width <= g.period / 2)) width = g.period / 4;   // unset → default
    g.slots = 0; g.present = 1; g.toothAngle = 0.0;
    g.teeth.push_back(start);
    g.hasPulse = true;
    g.pulse = {start, start + width};
    return g;
}

}  // namespace

StreamGeometry streamGeometry(const WheelStream& st) {
    StreamGeometry g = (st.prim == 1) ? seqGeometry(st.cell, st.rate)
                     : (st.prim == 2) ? widthGeometry(st.wMin, st.wMax, st.wTgt, st.rate)
                                      : gapGeometry(st.slots, st.ratio, st.cell, st.rate);
    g.edge = st.edge;   // carried so a painter needs nothing but the geometry — see StreamGeometry
    return g;
}

WheelGeometry wheelGeometry(const Wheel& w) {
    WheelGeometry wg;
    wg.name = w.name;
    wg.sync = w.sync;
    wg.tdcOffset = w.tdcOffset;
    for (const WheelStream& s : w.streams) wg.streams.push_back(streamGeometry(s));

    // Display roles, by stream order — crank gets Primary then Secondary, each cam the next slot.
    // This was rolesForWheel() in the designer's UI header, computed alongside the geometry and passed
    // beside it as a parallel vector. It belongs with the geometry: it is derived from the same streams
    // in the same order, and a painter that has one and not the other can only draw half a lane.
    {
        int crankN = 0, camN = 0;
        for (StreamGeometry& g : wg.streams)
            g.displayRole = (g.rate == 0) ? (crankN++ ? 2 : 1) : (kDisplayRoleCamBase + camN++);
    }

    for (std::size_t i = 0; i < wg.streams.size(); ++i) {
        if (wg.streams[i].rate == 0 && wg.crankIdx < 0) wg.crankIdx = static_cast<int>(i);
        if (wg.streams[i].rate == 1 && wg.camIdx   < 0) wg.camIdx   = static_cast<int>(i);
    }
    wg.cycle = (wg.camIdx >= 0) ? kCamPeriod : kCrankPeriod;

    // Absolute reference: a GAP wheel has one (it recognises the gap), a SEQUENCE has one (it matches
    // the cell pattern), an EVEN wheel does not. The zero itself is always 0.0 — see hasReference.
    if (wg.crankIdx >= 0) {
        const StreamGeometry& c = wg.streams[wg.crankIdx];
        wg.hasReference = !c.gaps.empty() || (c.primitive == 1 && !c.teeth.empty());
    }
    return wg;
}
