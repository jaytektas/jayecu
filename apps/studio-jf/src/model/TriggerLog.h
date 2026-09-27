#pragma once

// TriggerLog — the ECU's RAW TRIGGER LOG (0x27): the time between edges, and nothing else.
//
// WHAT THIS IS. A bar per edge, its height the microseconds since the previous edge on that same
// stream. Laid out in order, the proportions ARE the picture: a 36-1 draws thirty-five short bars
// and one double-height one. That is a truthful account of what the pin did.
//
// WHAT THIS IS NOT, AND CANNOT BE. It infers nothing. No angles, no teeth per revolution, no
// synchronisation point, no rpm. Those are not missing features — they are not in the signal:
//
//   * TEETH AND SPEED ARE CONFLATED. A log measures edges per second, and edges/sec is
//     (features per revolution) x (revolutions per second). One equation, two unknowns. A
//     single-cylinder distributor at a million rpm and a million-cylinder distributor at 1 rpm
//     produce byte-identical captures. No capture length and no timing resolution separates them,
//     because the whole signal is one number.
//   * A REPEAT IS NOT A REVOLUTION. A wheel with three identical sectors 120 degrees apart repeats
//     three times per turn, and nothing in the timestamps says whether one repeat is a rotation or a
//     third of one. So the period of the pattern — where a pattern exists at all — is not a rotation.
//   * A UNIFORM WHEEL CARRIES NO SIGNATURE. A distributor's evenly spaced pulses give an interval
//     sequence of all-equal, identical to every other uniform wheel of every tooth count at every
//     speed. No gap to anchor to, no structure to match.
//   * WHICH SHAFT IS UNKNOWN. The same train from a crank sensor, and from a cam sensor at half the
//     speed, are the same data.
//
// So the axis is the EDGE ORDINAL and the value is MICROSECONDS. Both are measured. Anything that
// would place this on a crank angle has to come from the person holding the engine, who can count
// teeth and knows what the sensor is bolted to — and identification stays where it already was:
// they state the wheel, and the decoder either syncs or it does not.
//
// A record NAMES the stream that fired and carries a snapshot of every stream's line at that
// instant. The named stream is what the interval is measured within; the snapshot says what the
// other lines were doing, which is the whole question when relating a cam to a crank.
//
// The named stream is used and NOT a diff of the snapshot, because a stream armed for one edge only
// — the ordinary arming for a crank — leaves the line in the same state after every edge. Its
// snapshot is constant, and a diff recovers zero edges from a log that is full of them.
// Bench-measured on a 36-1 armed Rising: 1200 records, one levels byte.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace triggerlog {

// The 0x27 page header, little-endian. Mirrors firmware/Comms/OmniProtocol.h — deliberately NOT
// shared with it: the firmware struct is a wire contract this side must parse defensively, and a
// header that changes shape should break the parser here rather than silently re-interpret bytes.
constexpr uint32_t kMagic       = 0x474F4C54u;   // 'TLOG'
constexpr uint8_t  kVersion     = 5u;   // 5: bounded snapshot; ARM carries a record limit
// magic4 + ver1 + state1 + total4 + base4 + first4 + count2. PACKED, so 20 — and counted out field
// by field rather than eyeballed, which is how it was briefly wrong by one and read every record a
// byte early while the header itself still parsed correctly.
constexpr size_t   kHeaderBytes = 4 + 1 + 1 + 4 + 4 + 4 + 2;
constexpr size_t   kRecordBytes = 6;             // t_us4 levels1 flags1

// Bits 5-7 of `flags` name the stream that fired; 0-4 are reserved. See the header comment: a diff
// of `levels` recovers nothing from a single-edge-armed stream, which is the ordinary arming for a
// crank. There are no sync bits: what the decoder makes of these edges is a separate question, and
// this exists for wheels it cannot read at all.
constexpr uint8_t kStreamShift = 5;
constexpr uint8_t kStreamMask  = 0xE0u;

struct Record {
    uint32_t tUs    = 0;
    uint8_t  levels = 0;
    uint8_t  flags  = 0;
    // NOT on the wire. Set by the link when the ECU served from further on than it asked for, which
    // means records before this one were dropped. Since v5 the ECU's buffer does not wrap, so this
    // should never be set by a jayecu ECU — it is kept as the check that would catch it if it were.
    bool     gapBefore = false;
};

struct Header {
    uint32_t magic = 0;
    uint8_t  version = 0;
    uint8_t  state = 0;      // 0 Idle, 1 Armed, 2 Recording
    uint32_t total = 0;      // records written since the arm. Monotonic — this is the cursor.
    uint32_t base = 0;       // oldest absolute index the ECU still holds; always 0 since v5
    uint32_t first = 0;      // absolute index of the first record in THIS page
    uint16_t count = 0;
};

// One bar: the gap between this edge and the previous one on the same stream.
struct Interval {
    uint32_t tUs     = 0;      // when this edge landed, microseconds since the capture was armed
    uint32_t deltaUs = 0;      // since the previous edge ON THIS STREAM — the bar's height
    size_t   record  = 0;      // index into the capture, so a readout can name the edge
    bool     high    = false;  // the line's level after this edge, as the firmware measured it
    // The ECU's ring wrapped past the record before this one, so an unknown number of edges are
    // missing here. The view draws it as a break rather than letting two disconnected stretches
    // butt together and read as continuous.
    bool     gapBefore = false;
};

// Every interval on one stream, in capture order.
struct Lane {
    uint8_t               stream = 0;
    std::string           name;
    std::vector<Interval> bars;
};

struct Result {
    bool        ok = false;
    std::string message;      // why it failed, or what qualifies the success
    size_t      edges = 0;    // records consumed
};

// THE VIEW'S WINDOW IS A SPAN OF TIME, and this works out which.
//
// Streams fire at wildly different rates — a cam once per engine cycle against 58 crank teeth — so
// a window expressed as "bars [first, first+count) of each lane" put every lane but the busiest
// entirely out of range: a 130-bar cam lane sliced at bar 7704 of a 7900-bar crank yields nothing,
// and the lane was drawn, named, and always empty. The window is therefore stated in bars of the
// BUSIEST lane (which is what the scrubber scrolls and what the readout counts) and converted to
// the time span those bars cover; every lane then draws the bars that fall inside it, positioned by
// timestamp. That is also the only way a cam bar can sit between the crank teeth it fell between,
// which is the entire reason a record names its stream.
//
// Returns false when there is nothing to show. t1 is nudged past t0 for a one-bar window so callers
// can divide by the span.
bool windowSpan(const std::vector<Lane>& lanes, size_t first, size_t count,
                uint32_t& t0, uint32_t& t1, size_t& refBars);

// The half-open range of `lane.bars` whose timestamps fall in [t0, t1].
void barsInSpan(const Lane& lane, uint32_t t0, uint32_t t1, size_t& from, size_t& to);

// The height to scale a lane's bars against, over [from, to).
//
// NOT the tallest bar. When the trigger signal is torn away — a pulled connector, a stim halted,
// a stalled engine — the interval spanning the silence is not a tooth spacing at all: it is a
// hiatus, and on a 60-2 at 1200 rpm it is 60x a pitch and 20x the wheel's own gap. Scaling to it
// squashes every real tooth into the bottom 2% of the lane, so the one thing worth looking at
// after the signal goes — the teeth either side of the break — becomes unreadable.
//
// This infers NOTHING about the wheel, which the view must not do: it does not know teeth per
// revolution, and 'the gap is 3 pitches' is exactly the knowledge a trigger log cannot carry. It
// makes only a statement about the data in front of it — this bar is wildly out of family with its
// neighbours — using the median as the reference and a generous multiple so that every plausible
// wheel FEATURE survives. A 12-3's gap is 4x a pitch and a cranking wobble maybe 2x; a signal
// absence is tens of times. kOutlierFactor sits in the wide gap between the two.
//
// Bars above the returned height are drawn clipped and marked, never hidden, and the hover still
// reports their true duration.
constexpr uint32_t kOutlierFactor = 10;
uint32_t scaleHeight(const Lane& lane, size_t from, size_t to);

// Parse one page. `out` receives the records appended; the header is returned.
Header parsePage(const uint8_t* data, size_t len, std::vector<Record>& out);

// Records (already concatenated across pages) -> one lane per stream that fired.
//
// The FIRST edge on each stream has no predecessor, so it gets no bar — inventing one from the
// capture's arm instant would put a bar of arbitrary height at the left of every log, which reads
// as a feature.
//
// `streams` names the lanes; entries beyond its size fall back to "Stream N". There is no rpm
// parameter and no cycle span: see the header comment for why neither can exist here.
Result decodeLanes(const std::vector<Record>& recs,
                   const std::vector<std::string>& streamNames, std::vector<Lane>& out);

}  // namespace triggerlog
