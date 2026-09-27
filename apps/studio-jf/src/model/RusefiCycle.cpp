#include "RusefiCycle.h"

#include <algorithm>
#include <cmath>

namespace rusefi_cycle {
namespace {

// The two record layouts. 5 bytes is the legacy form: timestamp + flags and nothing else. 8 appends
// coil, injector and a pad.
constexpr size_t kRecordShort = 5;
constexpr size_t kRecordLong  = 8;

// The composite record, as it sits on the wire.
//
// The modern record is a packed 64-bit value sent with the WHOLE record byte-reversed. So the only way
// to get this right is to undo the reversal first and then index the fields. Doing it by "reading a
// big-endian timestamp" would get the timestamp right and every flag wrong.
//
// After the un-swap the little-endian byte layout is:
//   [0..3] timestamp (uint32, microseconds)
//   [4]    bit0 priLevel, bit1 cam1, bit2 trigger, bit3 sync, bit4 tdc, bit5 cam2, bit6 cam3, bit7 cam4
//   [5]    coil bitmask      | 8-byte records only
//   [6]    injector bitmask  | 8-byte records only
//   [7]    bit0 acr          | 8-byte records only
//
// The first five bytes are common to both layouts, which is what makes one decoder serve both: a
// short record simply has no output bytes to read.
//
// The bit POSITIONS within byte [4] assume little-endian bitfield packing (first field in the low
// bit). The field set, the microsecond timebase and the byte order are confirmed; the individual bit
// offsets rest on that packing rule and have not been checked against a live modern-format ECU.
Sample decodeOne(const uint8_t* r, size_t n) {
    Sample s;
    uint8_t f = 0;

    if (n == kRecordShort) {
        // LEGACY. Only the timestamp is byte-swapped — the wire is [timestamp big-endian][flags].
        // Whole-record reversal belongs to the modern format only.
        //
        // Reversing all five bytes instead yields timestamps that are still MONOTONIC (shifted one
        // byte, so 256x wrong but perfectly ordered), which means a monotonicity check cannot catch
        // the mistake. Verified against captured bytes from a 2026.04.05 ECU.
        s.t_us = (static_cast<uint32_t>(r[0]) << 24) | (static_cast<uint32_t>(r[1]) << 16) |
                 (static_cast<uint32_t>(r[2]) << 8)  |  static_cast<uint32_t>(r[3]);
        f = r[4];
        s.priLevel = (f & 0x01) != 0;
        s.cam[0]   = (f & 0x02) != 0;   // secLevel — the secondary/cam input
        // bit2 carries TDC in this format — NOT bit4, which is where the modern record puts it.
        s.tdc      = (f & 0x04) != 0;
        s.sync     = (f & 0x08) != 0;
        // Single aggregate bits, not per-channel masks: this firmware cannot say WHICH coil fired.
        s.coil     = (f & 0x10) ? 0x01 : 0x00;
        s.injector = (f & 0x20) ? 0x01 : 0x00;
        return s;
    }

    // MODERN. The whole record IS reversed, so undo that and read the little-endian layout straight.
    uint8_t b[kRecordLong] = {};
    for (size_t i = 0; i < n && i < kRecordLong; ++i) b[i] = r[n - 1 - i];
    s.t_us = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
             (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    f = b[4];
    s.priLevel = (f & 0x01) != 0;
    s.cam[0]   = (f & 0x02) != 0;
    s.sync     = (f & 0x08) != 0;
    s.tdc      = (f & 0x10) != 0;
    s.cam[1]   = (f & 0x20) != 0;
    s.cam[2]   = (f & 0x40) != 0;
    s.cam[3]   = (f & 0x80) != 0;
    s.coil     = b[5];
    s.injector = b[6];
    return s;
}

// Score a candidate record size by how monotonic the timestamps it produces are. The wrong size
// slices the timestamp across field boundaries, so the sequence stops rising.
double monotonicScore(const uint8_t* data, size_t len, size_t rec) {
    if (rec == 0 || len < rec * 2 || (len % rec) != 0) return -1.0;
    const size_t n = len / rec;
    size_t rising = 0;
    uint32_t prev = decodeOne(data, rec).t_us;
    for (size_t i = 1; i < n; ++i) {
        const uint32_t t = decodeOne(data + i * rec, rec).t_us;
        if (t >= prev) ++rising;
        prev = t;
    }
    return static_cast<double>(rising) / static_cast<double>(n - 1);
}

} // namespace

size_t detectRecordBytes(const uint8_t* data, size_t len) {
    if (!data || len == 0) return 0;

    // DIVISIBILITY FIRST. It settles most real buffers on its own and needs no data to do it: a
    // 1250-byte buffer (the measured 2026.04.05 case) simply cannot be 8-byte records, and a
    // 16-byte one cannot be 5-byte. Scoring first would throw away buffers too small to score and
    // leave a valid one-record reply undecodable.
    const bool divShort = (len >= kRecordShort) && (len % kRecordShort == 0);
    const bool divLong  = (len >= kRecordLong)  && (len % kRecordLong  == 0);
    if (divShort && !divLong) return kRecordShort;
    if (divLong && !divShort) return kRecordLong;
    if (!divShort && !divLong) return 0;

    // Both divide (the smallest such buffer is 40 bytes, so there is always enough to score). The
    // timestamps decide: the wrong size slices them across field boundaries and they stop rising.
    const double shortScore = monotonicScore(data, len, kRecordShort);
    const double longScore  = monotonicScore(data, len, kRecordLong);
    if (shortScore == longScore) return kRecordLong;   // indistinguishable: prefer current firmware
    return (shortScore > longScore) ? kRecordShort : kRecordLong;
}

std::vector<Sample> decodeSamples(const uint8_t* data, size_t len, size_t recordBytes) {
    std::vector<Sample> out;
    if (!data) return out;
    if (recordBytes == 0) recordBytes = detectRecordBytes(data, len);
    if (recordBytes == 0) return out;
    const size_t n = len / recordBytes;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Sample s = decodeOne(data + i * recordBytes, recordBytes);
        // A timestamp that goes backwards means the buffer spans a counter wrap (or is corrupt).
        // Keeping it would place an event before the cycle it belongs to.
        if (!out.empty() && s.t_us < out.back().t_us) continue;
        out.push_back(s);
    }
    return out;
}

Result decodeCycle(const uint8_t* data, size_t len, double cycleAngle, enginecycle::Cycle& out,
                   double rpmHint) {
    out.clear();

    Result r;
    if (cycleAngle <= 0.0) cycleAngle = 720.0;
    r.recordBytes       = detectRecordBytes(data, len);
    r.layout            = (r.recordBytes == kRecordShort) ? Layout::Legacy5 : Layout::Modern8;
    r.perChannelOutputs = (r.layout == Layout::Modern8);
    const std::vector<Sample> s = decodeSamples(data, len, r.recordBytes);
    r.samples = s.size();

    if (s.size() < 2) {
        r.message = "no composite data from the ECU";
        return r;
    }

    // Find the TDC marks, and this is per-layout because the CONVENTION differs:
    //
    //   MODERN toggles the bit once per cycle, so every CHANGE is a mark. Counting only rising
    //   edges here would find one mark every TWO cycles and double every angle.
    //   LEGACY emits a 10us PULSE — true at t, false at t+10 — so a mark is a RISING edge. Counting
    //   every change here would pair a pulse's own rise and fall and call that a cycle, producing a
    //   10us "cycle" and an rpm in the millions.
    std::vector<size_t> tdcAt;
    for (size_t i = 1; i < s.size(); ++i) {
        const bool mark = (r.layout == Layout::Legacy5) ? (s[i].tdc && !s[i - 1].tdc)
                                                        : (s[i].tdc != s[i - 1].tdc);
        if (mark) tdcAt.push_back(i);
    }
    r.tdcMarks = static_cast<int>(tdcAt.size());

    if (tdcAt.empty()) {
        // No anchor at all. Refusing beats inventing an origin and drawing a plausible lie.
        r.message = "no TDC marks — is the engine turning and synchronised?";
        return r;
    }

    // Pick the anchor(s). TWO marks bracket a cycle and the buffer describes itself. ONE mark is the
    // common case on current firmware (a 250-record buffer is about one cycle, so it holds one
    // toggle), and then the cycle DURATION has to come from the ECU's own rpm — we know where TDC
    // is, but not how fast the crank was turning between events.
    size_t i0 = 0;
    double span_us = 0.0;
    if (tdcAt.size() >= 2) {
        i0 = tdcAt[tdcAt.size() - 2];
        const size_t i1 = tdcAt[tdcAt.size() - 1];
        span_us = static_cast<double>(s[i1].t_us) - static_cast<double>(s[i0].t_us);
        if (span_us <= 0.0) {
            r.message = "TDC marks are not ordered in time";
            return r;
        }
    } else {
        if (rpmHint <= 0.0) {
            r.message = "only one TDC mark and no rpm from the ECU — cannot place the cycle";
            return r;
        }
        i0 = tdcAt.back();
        // One cycle is cycleAngle/360 revolutions; at rpmHint that takes:
        span_us = (cycleAngle / 360.0) * 60e6 / rpmHint;
        r.singleAnchor = true;
    }
    const double t0 = s[i0].t_us;

    out.setCycleAngle(cycleAngle);
    out.setFidelity(enginecycle::Fidelity::Reconstructed);

    // rpm from the cycle's own duration. A four-stroke cycle is two revolutions, so revolutions per
    // cycle is cycleAngle/360 — which keeps this right for a two-stroke and a rotary too, rather
    // than hard-coding the four-stroke's 2.
    const double revsPerCycle = cycleAngle / 360.0;
    r.rpm = (revsPerCycle * 60e6) / span_us;   // identical to rpmHint on the single-anchor path
    out.setRpm(r.rpm);
    // Terse on purpose: the header's amber "reconstructed" marker already makes the general claim,
    // so the note carries only what that marker cannot — WHICH firmware format, and whether the
    // scale came from two measured marks or from the ECU's rpm.
    std::string note = "rusEFI tooth log";
    // Legacy logs ONE coil bit and ONE injector bit for the whole engine — it cannot say which
    // cylinder fired. Saying so matters: a single "Coil" lane would otherwise be read as "this
    // engine has one coil" rather than "this firmware does not report which".
    if (r.layout == Layout::Legacy5)
        note += " \xC2\xB7 aggregate coil/inj bit, not per-cylinder (pre-2026-05-14 fw)";
    // A bracketed cycle is bounded by two measured marks. A single-anchored one is scaled by the
    // ECU's reported rpm, so a changing speed stretches or squashes it — a weaker claim, and the
    // viewer should know which they are looking at.
    if (r.singleAnchor)
        note += " \xC2\xB7 one TDC anchor, scaled by ECU rpm";
    out.setNote(note);

    // Degrees from the anchor. addEdge() wraps into [0, cycleAngle), which is what makes a record
    // from before the mark land at its correct phase rather than at a negative angle.
    auto angleOf = [&](uint32_t t_us) {
        double a = (static_cast<double>(t_us) - t0) / span_us * cycleAngle;
        if (a < 0.0) a += cycleAngle * (1.0 + std::floor(-a / cycleAngle));
        return a;
    };

    // Walk the cycle, emitting an EDGE wherever a channel differs from the previous sample. The
    // records are level snapshots, so this diff is what recovers the edges our model is built from.
    // Carry in whatever was ALREADY high at the opening TDC: the records are level snapshots, so a
    // coil mid-dwell across the boundary never produces a rising edge inside the cycle and diffing
    // alone would draw its spark with no dwell.
    // Legacy has no channel identity, so its single bit becomes one unnumbered lane rather than a
    // "Coil 1" that would imply the other seven were silent.
    const bool perChan = r.perChannelOutputs;
    auto coilLabel = [perChan](int c) {
        return perChan ? "Coil " + std::to_string(c + 1) : std::string("Coil (any)");
    };
    auto injLabel = [perChan](int c) {
        return perChan ? "Inj " + std::to_string(c + 1) : std::string("Inj (any)");
    };

    // Carry-in applies ONLY to the bracketed path. There the walk starts at the opening mark, so a
    // channel already high at the boundary never produces a rising edge inside the window and would
    // draw a spark with no dwell. The single-anchored walk covers the WHOLE buffer, so every
    // transition is already in it — carrying state in as well would inject a phantom edge at angle 0
    // and split one injector pulse into two spans.
    const Sample& base = s[i0];
    for (int c = 0; !r.singleAnchor && c < 8; ++c) {
        if (base.coil & (1u << c))
            out.addEdge(coilLabel(c), 0.0, true, enginecycle::Signal::Coil, c + 1);
        if (base.injector & (1u << c))
            out.addEdge(injLabel(c), 0.0, true, enginecycle::Signal::Injector, c + 1);
    }

    // WHICH RECORDS GO INTO THE PICTURE.
    //
    // Bracketed: exactly the records between the two marks — one true cycle, nothing borrowed.
    //
    // Single-anchored: the WHOLE buffer, with angles wrapped. The anchor sits somewhere in the
    // middle of the buffer, so taking only what follows it would draw a third of a cycle and leave
    // the rest blank — which reads as "nothing happened there". An engine repeats, so a record 30 ms
    // BEFORE the mark belongs at the same phase as one 30 ms before the NEXT mark; wrapping places
    // it there. The cost is that two adjacent cycles are overlaid, so a transient shows as both at
    // once — acceptable on a reconstruction that already says it is one, and the note says so.
    const double tEnd  = t0 + span_us;
    const size_t first = r.singleAnchor ? 1 : (i0 + 1);
    for (size_t i = first; i < s.size(); ++i) {
        if (!r.singleAnchor && static_cast<double>(s[i].t_us) > tEnd) break;
        const Sample& p = s[i - 1];
        const Sample& q = s[i];
        const double a = angleOf(q.t_us);

        // The primary trigger is a LEVEL in rusEFI's log (priLevel), but our model treats a trigger
        // stream as EVENTS — instants, drawn as a comb. Emit the rising edges only, which is the
        // same thing our own decoder records.
        if (q.priLevel && !p.priLevel)
            out.addEdge("Crank 1", a, true, enginecycle::Signal::Crank, 1);

        for (int k = 0; k < 4; ++k)
            if (q.cam[k] && !p.cam[k])
                out.addEdge("Cam " + std::to_string(k + 1), a, true, enginecycle::Signal::Cam, k + 1);

        for (int c = 0; c < 8; ++c) {
            const bool was = (p.coil & (1u << c)) != 0, now = (q.coil & (1u << c)) != 0;
            if (was != now)
                out.addEdge(coilLabel(c), a, now, enginecycle::Signal::Coil, c + 1);
        }
        for (int j = 0; j < 8; ++j) {
            const bool was = (p.injector & (1u << j)) != 0, now = (q.injector & (1u << j)) != 0;
            if (was != now)
                out.addEdge(injLabel(j), a, now, enginecycle::Signal::Injector, j + 1);
        }
    }

    out.sort();
    out.sortTracesForDisplay();

    r.ok = true;
    r.message = "reconstructed one cycle from " + std::to_string(s.size()) + " composite records ("
                + std::to_string(r.recordBytes) + "-byte "
                + (r.layout == Layout::Legacy5 ? "legacy" : "modern") + ")";
    return r;
}

} // namespace rusefi_cycle
