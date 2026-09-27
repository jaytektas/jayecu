#pragma once

// RusefiCycle — a rusEFI ECU's composite tooth log, converted into the studio's native
// engine-cycle model.
//
// The studio has ONE cycle view, drawing one model (enginecycle::Cycle); this file is the adapter that
// lets a rusEFI ECU feed it. Everything specific to that log stops here — the byte order, the bit
// assignments, the microsecond timebase, the TDC convention.
//
// WHAT THE LOG CONTAINS, and why it needs converting:
//
//   * THERE ARE TWO WIRE FORMATS, and the wire carries no version marker. rusEFI firmware from
//     mid-May 2026 onward sends the modern record; earlier firmware sends the legacy one. The record
//     size, the byte order and the bit assignments all differ between them:
//
//       LEGACY — 5 bytes, [timestamp big-endian][flags]. ONLY the timestamp is byte-swapped.
//         Flags: bit0 priLevel, bit1 secLevel, bit2 TDC, bit3 sync, bit4 coil, bit5 injector —
//         coil and injector are SINGLE BITS, not per-channel masks.
//         TDC is a PULSE: true at t, false at t+10us — two records per mark.
//
//       MODERN — 8 bytes, and the WHOLE RECORD is byte-reversed, not just its integers. Flags:
//         bit0 priLevel, bit1 cam1, bit2 trigger, bit3 sync, bit4 tdc, bit5 cam2, bit6 cam3,
//         bit7 cam4, then a coil bitmask byte and an injector bitmask byte.
//         TDC is a TOGGLE: one flip per cycle.
//
//     Because every difference comes with the change of size, the RECORD SIZE determines the
//     semantics — which is why detectRecordBytes() decides the layout and nothing has to parse a
//     version string. Measured on a bench ECU running 2026.04.05: 1250-byte buffers, 250 legacy
//     records.
//   * A record is a SNAPSHOT of every channel at one instant, not an edge. A new record is emitted
//     whenever anything changes, so an edge has to be recovered by diffing consecutive records.
//   * Timestamps are MICROSECONDS, not degrees. This is the whole reason the conversion is a
//     reconstruction: to place an event in the cycle we have to know how far the crank had turned
//     at that microsecond, and the log does not say.
//   * On current firmware the timestamp is RELATIVE TO THE START OF ITS BUFFER; older firmware sent
//     absolute microseconds. Measured: every modern buffer starts at 0. Two consequences — buffers
//     cannot be stitched together by timestamp, and each one must stand on its own.
//
// THE ANGLE RECONSTRUCTION, stated plainly because its limits matter:
//
//   The ECU marks TDC once per engine cycle, at the trigger synchronisation point — as a TOGGLE on
//   modern firmware and as a 10us PULSE on legacy, which is why the mark extraction is per-layout. Two
//   consecutive marks bracket exactly one engine cycle. We map that time span linearly onto the
//   configured cycle angle:
//
//       angle = (t - tdc0) / (tdc1 - tdc0) * cycleAngle
//
//   That assumes constant crank speed across the cycle. It is right at steady state and it SMEARS
//   under acceleration — which is when a tuner is most likely to be looking. Worse, the ECU's TDC
//   mark is itself scheduled by angle from the sync point using the rpm of that moment, so the
//   anchors carry that error too.
//
//   None of which makes the picture useless — it makes it a RECONSTRUCTION, and the only honest
//   thing to do is say so. Every cycle produced here is stamped Fidelity::Reconstructed, and the
//   view already captions that differently from a measured one. Our own ECU reports the angle it
//   actually scheduled against and needs none of this.

#include "EngineCycle.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rusefi_cycle {

// Which wire format a buffer is in. The size and the semantics changed together, so one enum
// covers both.
enum class Layout { Legacy5, Modern8 };

// One decoded composite record: the state of every channel at `t_us`.
struct Sample {
    uint32_t t_us     = 0;
    bool     priLevel = false;
    bool     cam[4]   = { false, false, false, false };   // legacy fills cam[0] from secLevel
    bool     sync     = false;
    bool     tdc      = false;   // modern: toggles per cycle. legacy: a 10us pulse. Level is not meaning.
    uint8_t  coil     = 0;       // modern: per-channel mask. legacy: bit0 only (one aggregate bit).
    uint8_t  injector = 0;       // ditto
};

struct Result {
    bool        ok = false;
    std::string message;
    size_t      samples = 0;     // records decoded from the buffer
    int         tdcMarks = 0;    // TDC toggles seen — 2 are needed to bracket a cycle
    double      rpm = 0.0;       // bracketed cycle's duration, or the caller's hint when single-anchored
    bool        singleAnchor = false;  // true when placed from ONE mark + rpmHint, not a bracketed pair
    size_t      recordBytes = 0;              // 5 (legacy) or 8 (modern)
    Layout      layout = Layout::Modern8;
    bool        perChannelOutputs = false;    // modern only: coil/injector are per-channel masks
};

// Which record layout this buffer is in: 5 (legacy) or 8 (modern) bytes, or 0 when it cannot be told.
//
// There is no version field on the wire, so this is decided on EVIDENCE: a candidate size must
// divide the buffer exactly, and is then scored on how monotonic the timestamps it yields are. The
// right size gives a strictly rising sequence; the wrong one shreds the timestamp across field
// boundaries and produces noise. On the measured bench buffer the two scored 249/249 against
// 63/155, so this is not a close call — but the margin is checked rather than assumed.
size_t detectRecordBytes(const uint8_t* data, size_t len);

// Decode a raw composite buffer into samples, newest last. `recordBytes` of 0 means "detect".
// Records with a timestamp that goes backwards are dropped: the ECU hands out buffers that may span
// a wrap, and a negative dt would place an event before the start of the cycle.
std::vector<Sample> decodeSamples(const uint8_t* data, size_t len, size_t recordBytes = 0);

// Convert a composite buffer into one engine cycle.
//
// cycleAngle is the span the ENGINE has (720 for a four-stroke) — rusEFI does not report it, so the
// caller supplies it from the connected tune rather than this file guessing.
//
// rpmHint is the ECU's own reported RPM, or 0 for "unknown". It is needed more often than it looks:
//
//   TWO MARKS (preferred). Two TDC marks bracket exactly one cycle, so the buffer is
//     self-describing and no external rpm is involved.
//
//   ONE MARK (the common case, and this is why rpmHint exists). A buffer holds 250 records, and the
//     number of records an engine generates per cycle is roughly fixed by its wheel and channel
//     count — so a buffer spans about ONE cycle at any rpm, and therefore usually contains exactly
//     one TDC toggle. Measured on a 4-cyl 36-1 at 1200 rpm: every buffer spanned 100.0 ms, one
//     cycle, one mark. With one anchor we know WHERE tdc is but not how fast the crank turned, so
//     the rpm has to come from the ECU. Angle is then
//         (t - t_tdc) * rpm * 360 / 60e6
//     wrapped into the cycle.
//
//   NEITHER — refused, because placing events without an origin is inventing the picture.
//
// Picks the LAST complete cycle in the buffer, because a tuner asking for a capture means "what is
// it doing now".
Result decodeCycle(const uint8_t* data, size_t len, double cycleAngle, enginecycle::Cycle& out,
                   double rpmHint = 0.0);

} // namespace rusefi_cycle
