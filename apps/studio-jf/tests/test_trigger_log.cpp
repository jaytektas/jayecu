// The raw trigger log (0x27 v3) -> lanes of intervals.
//
// What is pinned here is mostly what this REFUSES to do. A trigger log carries edge timestamps and
// nothing else, and every angle, tooth count, rpm or sync point that could be printed beside them
// would be an invention shown at the same confidence as the measurement. So the tests check the wire
// format, that an interval is measured within its own stream, and that a gap the ECU's ring lost is
// carried through rather than smoothed over.
#include "../src/model/TriggerLog.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

static uint8_t stream(int s) { return static_cast<uint8_t>(s << triggerlog::kStreamShift); }

// Build a page exactly as the firmware lays it out, so this tests the WIRE FORMAT and not a struct
// copy — the two are the same only if the parser agrees with the layout.
static std::vector<uint8_t> page(const std::vector<triggerlog::Record>& recs,
                                 uint32_t total = 0, uint32_t base = 0, uint32_t first = 0,
                                 uint32_t magic = triggerlog::kMagic,
                                 uint8_t ver = triggerlog::kVersion) {
    std::vector<uint8_t> b;
    auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); };
    auto p16 = [&](uint16_t v) { for (int i = 0; i < 2; ++i) b.push_back(uint8_t(v >> (8 * i))); };
    // magic4 ver1 state1 total4 base4 first4 count2 — exactly the firmware's packed
    // TriggerLogHeader. This used to push a `streams` byte that is not on the wire, so every
    // field after it landed one byte late and the page measured one byte long.
    p32(magic); b.push_back(ver); b.push_back(2 /*Recording*/);
    p32(total ? total : static_cast<uint32_t>(recs.size())); p32(base); p32(first);
    p16(static_cast<uint16_t>(recs.size()));
    for (const auto& r : recs) { p32(r.tUs); b.push_back(r.levels); b.push_back(r.flags); }
    return b;
}

// A 36-1 armed RISING at 900 rpm: 35 edges per revolution, one per present tooth, the gap showing as
// a double-pitch space. These are the numbers the bench rig measured off the real ECU.
static std::vector<triggerlog::Record> synthetic361(double rpm, int revs) {
    const double revUs = 60.0e6 / rpm;
    const double pitch = revUs / 36.0;
    std::vector<triggerlog::Record> out;
    double t = 0.0;
    for (int rev = 0; rev < revs; ++rev)
        for (int tooth = 0; tooth < 35; ++tooth) {
            out.push_back({ static_cast<uint32_t>(t), 1, stream(0), false });
            t += pitch * (tooth == 34 ? 2.0 : 1.0);      // the gap: the last step is a double
        }
    return out;
}

int main() {
    std::puts("=== raw trigger log -> intervals ===");

    std::puts("\n-- the wire format is parsed, not assumed --");
    {
        const auto recs = synthetic361(900.0, 1);
        const auto bytes = page(recs, /*total*/ 500, /*base*/ 100, /*first*/ 100);
        std::vector<triggerlog::Record> got;
        const triggerlog::Header h = triggerlog::parsePage(bytes.data(), bytes.size(), got);
        ck(bytes.size() == triggerlog::kHeaderBytes + recs.size() * triggerlog::kRecordBytes,
           "the page is exactly header + records — no padding, no slack", std::to_string(bytes.size()));
        ck(h.magic == triggerlog::kMagic, "magic read back");
        ck(h.version == triggerlog::kVersion, "version is STATED, never inferred from record size");
        ck(h.total == 500 && h.base == 100 && h.first == 100,
           "total / base / first survive the wire",
           std::to_string(h.total) + "/" + std::to_string(h.base) + "/" + std::to_string(h.first));
        ck(got.size() == recs.size(), "every record parsed", std::to_string(got.size()));
    }
    {
        auto bytes = page(synthetic361(900.0, 1));
        bytes.resize(triggerlog::kHeaderBytes + 5 * triggerlog::kRecordBytes);
        std::vector<triggerlog::Record> got;
        triggerlog::parsePage(bytes.data(), bytes.size(), got);
        ck(got.size() == 5, "a truncated page yields the records it actually contains",
           std::to_string(got.size()));
    }
    {
        std::vector<triggerlog::Record> got;
        auto bytes = page({}, 0, 0, 0, /*magic*/ 0xDEADBEEF);
        triggerlog::parsePage(bytes.data(), bytes.size(), got);
        ck(got.empty(), "a foreign magic yields nothing");
        bytes = page(synthetic361(900.0, 1), 0, 0, 0, triggerlog::kMagic, /*ver*/ 99);
        got.clear();
        triggerlog::parsePage(bytes.data(), bytes.size(), got);
        ck(got.empty(), "an unknown VERSION yields nothing rather than a guessed layout");
    }

    std::puts("\n-- a SINGLE-EDGE-armed stream decodes, though its snapshot never changes --");
    {
        // The regression the stream field exists for. A crank armed Rising leaves the line high after
        // every edge, so `levels` is one constant byte for the whole capture — bench-measured, 1200
        // records, one value. Recovering edges by diffing that snapshot finds none.
        std::vector<triggerlog::Lane> lanes;
        const auto r = triggerlog::decodeLanes(synthetic361(900.0, 1), {"Crank"}, lanes);
        ck(r.ok, "decoded", r.message);
        ck(lanes.size() == 1 && lanes[0].name == "Crank", "one named lane");
        // 35 records, and the FIRST has no predecessor so it gets no bar.
        ck(lanes[0].bars.size() == 34, "one bar per interval, not per record",
           std::to_string(lanes[0].bars.size()));
    }

    std::puts("\n-- the 36-1's gap is in the DATA, as a double-length interval --");
    {
        std::vector<triggerlog::Lane> lanes;
        triggerlog::decodeLanes(synthetic361(900.0, 2), {"Crank"}, lanes);
        uint32_t lo = 0xFFFFFFFF, hi = 0;
        for (const auto& b : lanes[0].bars) { lo = std::min(lo, b.deltaUs); hi = std::max(hi, b.deltaUs); }
        // Nothing here concludes "36-1". The ratio is what is measured; naming the wheel is the
        // reader's job, and no capture contains enough to do it for them.
        ck(hi > lo * 1.8 && hi < lo * 2.2, "the longest interval is about twice the shortest",
           std::to_string(lo) + " us .. " + std::to_string(hi) + " us");
    }

    std::puts("\n-- an interval is measured WITHIN its stream, never across --");
    {
        // Crank edges at 0 and 1000; a cam edge at 900 between them. Timing the crank edge against
        // the cam edge that happened to precede it would draw a phase relationship as a tooth pitch.
        std::vector<triggerlog::Record> recs = {
            {0,    0b01, stream(0), false},
            {900,  0b11, stream(1), false},
            {1000, 0b01, stream(0), false},
        };
        std::vector<triggerlog::Lane> lanes;
        const auto r = triggerlog::decodeLanes(recs, {"Crank", "Cam"}, lanes);
        ck(r.ok && lanes.size() == 2, "two streams, two lanes", std::to_string(lanes.size()));
        ck(lanes[0].bars.size() == 1 && lanes[0].bars[0].deltaUs == 1000,
           "the crank interval is 1000 us, not the 100 us since the cam edge",
           lanes[0].bars.empty() ? "none" : std::to_string(lanes[0].bars[0].deltaUs));
        ck(lanes[1].bars.empty(), "the cam's only edge is its first, so it has no interval yet");
    }

    std::puts("\n-- a lost span is carried through, not smoothed over --");
    {
        auto recs = synthetic361(900.0, 1);
        recs[10].gapBefore = true;      // the link marks this when the ECU's ring wrapped past it
        std::vector<triggerlog::Lane> lanes;
        triggerlog::decodeLanes(recs, {"Crank"}, lanes);
        int marked = 0;
        for (const auto& b : lanes[0].bars) if (b.gapBefore) ++marked;
        ck(marked == 1, "the break reaches the bar it belongs to", std::to_string(marked));
    }

    std::puts("\n-- unnamed streams still get a lane --");
    {
        std::vector<triggerlog::Record> recs = {
            {0, 0b10, stream(1), false}, {100, 0b00, stream(1), false},
        };
        std::vector<triggerlog::Lane> lanes;
        triggerlog::decodeLanes(recs, {}, lanes);
        ck(lanes.size() == 1 && lanes[0].name == "Stream 1",
           "an unnamed stream is numbered rather than vanishing",
           lanes.empty() ? "none" : lanes[0].name);
    }

    std::puts("\n-- a sparse lane is visible at EVERY window position, not just the first --");
    {
        // THE BUG THIS PINS. A cam fires once per engine cycle; a 60-2 crank fires 58 times per
        // revolution. Window the lanes by each one's own bar INDEX and the cam disappears the moment
        // you scroll past its bar count: a 130-bar cam lane sliced at bar 7704 of a 7900-bar crank
        // yields nothing, so the lane was drawn, named, and permanently empty. Measured on the rig
        // at "bars 9380-9579 of 9579" — the live end of a capture, which is where the view sits by
        // default, so the cam was never visible at all.
        std::vector<triggerlog::Lane> lanes;
        triggerlog::Lane crank; crank.stream = 0; crank.name = "Crank Primary";
        triggerlog::Lane cam;   cam.stream   = 2; cam.name   = "Cam Intake B1";
        for (uint32_t i = 1; i <= 2000; ++i) {           // a crank bar every 1 ms
            triggerlog::Interval iv; iv.tUs = i * 1000u; iv.deltaUs = 1000u; crank.bars.push_back(iv);
        }
        for (uint32_t i = 1; i <= 20; ++i) {             // a cam bar every 100 ms — one engine cycle
            triggerlog::Interval iv; iv.tUs = i * 100000u; iv.deltaUs = 100000u; cam.bars.push_back(iv);
        }
        lanes.push_back(crank);
        lanes.push_back(cam);

        uint32_t t0 = 0, t1 = 0;
        size_t refBars = 0, from = 0, to = 0;

        // The window the view uses while following a live capture: the LAST 200 bars.
        ck(triggerlog::windowSpan(lanes, 1800, 200, t0, t1, refBars), "the live-end window resolves");
        ck(refBars == 200, "sized from the busiest lane");
        ck(t0 == 1801000u && t1 == 2000000u, "the window is a span of TIME, not of bar indices");

        triggerlog::barsInSpan(lanes[0], t0, t1, from, to);
        ck(to - from == 200, "the crank fills it, as it always did");
        triggerlog::barsInSpan(lanes[1], t0, t1, from, to);
        ck(to > from, "AND THE CAM IS IN IT TOO — this was zero, at every window but the first");
        ck(to - from == 2, "both cam bars in that span, placed by timestamp");

        // The first window still works — the only place the old code ever drew a cam.
        ck(triggerlog::windowSpan(lanes, 0, 200, t0, t1, refBars), "the first window resolves");
        triggerlog::barsInSpan(lanes[1], t0, t1, from, to);
        ck(to - from == 2, "the cam is there as well");

        // A lane that fired nothing in the span draws nothing: a truthful empty, not an artefact.
        ck(triggerlog::windowSpan(lanes, 205, 50, t0, t1, refBars), "a mid-capture window resolves");
        triggerlog::barsInSpan(lanes[1], t0, t1, from, to);
        ck(to == from, "a lane silent through the span is genuinely empty");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "trigger log: all pass",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
