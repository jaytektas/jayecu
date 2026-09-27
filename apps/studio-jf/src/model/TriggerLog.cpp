#include "TriggerLog.h"
#include <algorithm>

namespace triggerlog {
namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

Header parsePage(const uint8_t* data, size_t len, std::vector<Record>& out) {
    Header h;
    if (!data || len < kHeaderBytes) return h;
    h.magic   = rd32(data);
    h.version = data[4];
    h.state   = data[5];
    h.total   = rd32(data + 6);
    h.base    = rd32(data + 10);
    h.first   = rd32(data + 14);
    h.count   = rd16(data + 18);
    if (h.magic != kMagic || h.version != kVersion) return h;   // caller checks; nothing appended
    // Trust the byte count, not the declared record count: a short frame must yield the records it
    // actually contains rather than reading past the buffer to satisfy a header field.
    const size_t avail = (len - kHeaderBytes) / kRecordBytes;
    const size_t n = (h.count < avail) ? h.count : avail;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = data + kHeaderBytes + i * kRecordBytes;
        Record r;
        r.tUs    = rd32(p);
        r.levels = p[4];
        r.flags  = p[5];
        out.push_back(r);
    }
    return h;
}

Result decodeLanes(const std::vector<Record>& recs,
                   const std::vector<std::string>& streamNames, std::vector<Lane>& out) {
    Result r;
    if (recs.size() < 2) {
        r.message = "trigger log holds fewer than two edges — nothing to time between";
        return r;
    }

    // Last edge seen per stream, so an interval is measured WITHIN a stream. Timing a crank edge
    // against the cam edge that happened to precede it would draw the phase relationship as if it
    // were a tooth pitch.
    bool     seen[8]  = {};
    uint32_t prevUs[8] = {};
    int      laneOf[8];
    for (int i = 0; i < 8; ++i) laneOf[i] = -1;

    for (size_t i = 0; i < recs.size(); ++i) {
        const Record& rec = recs[i];
        // The record SAYS which stream fired. Decoding by diffing the level snapshot instead would
        // silently yield nothing whenever a stream is armed for a single edge — the ordinary arming
        // for a crank, whose line is left in the same state by every edge it produces.
        const uint8_t s = static_cast<uint8_t>((rec.flags & kStreamMask) >> kStreamShift);
        ++r.edges;

        if (laneOf[s] < 0) {
            laneOf[s] = static_cast<int>(out.size());
            Lane lane;
            lane.stream = s;
            lane.name = (s < streamNames.size() && !streamNames[s].empty())
                      ? streamNames[s] : ("Stream " + std::to_string(static_cast<int>(s)));
            out.push_back(std::move(lane));
        }
        if (!seen[s]) {                       // first edge on this stream: no predecessor, no bar
            seen[s] = true;
            prevUs[s] = rec.tUs;
            continue;
        }

        Interval iv;
        iv.tUs     = rec.tUs;
        iv.deltaUs = rec.tUs - prevUs[s];     // unsigned, and the firmware's clock is monotonic
        iv.record  = i;
        iv.high      = (rec.levels & (1u << s)) != 0;
        iv.gapBefore = rec.gapBefore;
        out[static_cast<size_t>(laneOf[s])].bars.push_back(iv);
        prevUs[s] = rec.tUs;
    }

    // LANES IN SLOT ORDER, not in the order the streams happened to fire. The slot IS the role -
    // 0 Crank Primary, 1 Crank Secondary, 2+ the cams - so a fixed order means the crank is always
    // the top lane and a given cam is always in the same place. Built in arrival order, whichever
    // stream produced the first edge after the arm landed on top, so the same rig drew its lanes in
    // a different order run to run and a cam could sit above the crank it is measured against.
    std::sort(out.begin(), out.end(),
              [](const Lane& a, const Lane& b) { return a.stream < b.stream; });

    size_t bars = 0;
    for (const Lane& l : out) bars += l.bars.size();
    if (bars == 0) {
        r.message = "every edge in the log is the first on its stream — nothing to time between";
        return r;
    }
    r.ok = true;
    return r;
}

bool windowSpan(const std::vector<Lane>& lanes, size_t first, size_t count,
                uint32_t& t0, uint32_t& t1, size_t& refBars) {
    const Lane* ref = nullptr;
    for (const Lane& l : lanes)
        if (!l.bars.empty() && (!ref || l.bars.size() > ref->bars.size())) ref = &l;
    if (!ref) return false;
    const size_t from = std::min(first, ref->bars.size() - 1);
    const size_t to   = std::min(from + count, ref->bars.size());
    if (to <= from) return false;
    t0 = ref->bars[from].tUs;
    t1 = ref->bars[to - 1].tUs;
    refBars = to - from;
    if (t1 <= t0) t1 = t0 + 1;      // a one-bar window is legal; a zero-width span is not divisible
    return true;
}

void barsInSpan(const Lane& lane, uint32_t t0, uint32_t t1, size_t& from, size_t& to) {
    from = static_cast<size_t>(
        std::lower_bound(lane.bars.begin(), lane.bars.end(), t0,
                         [](const Interval& iv, uint32_t v) { return iv.tUs < v; })
        - lane.bars.begin());
    to = static_cast<size_t>(
        std::upper_bound(lane.bars.begin(), lane.bars.end(), t1,
                         [](uint32_t v, const Interval& iv) { return v < iv.tUs; })
        - lane.bars.begin());
}


uint32_t scaleHeight(const Lane& lane, size_t from, size_t to) {
    if (to <= from || to > lane.bars.size()) return 1;
    std::vector<uint32_t> v;
    v.reserve(to - from);
    for (size_t i = from; i < to; ++i) v.push_back(lane.bars[i].deltaUs);
    std::vector<uint32_t> sorted = v;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const uint32_t med = sorted[sorted.size() / 2];
    if (med == 0) {                       // degenerate: fall back to the plain maximum
        return std::max(1u, *std::max_element(v.begin(), v.end()));
    }
    const uint64_t cap = static_cast<uint64_t>(med) * kOutlierFactor;
    uint32_t best = 0;
    for (uint32_t x : v) if (x <= cap && x > best) best = x;
    // Every bar an outlier (a window entirely inside a silence) — then nothing is out of family and
    // the tallest is the honest scale.
    if (best == 0) best = *std::max_element(v.begin(), v.end());
    return std::max(1u, best);
}

}  // namespace triggerlog
