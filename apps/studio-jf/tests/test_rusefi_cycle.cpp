// RusefiCycle — a rusEFI ECU's tooth log, converted into the studio's engine-cycle model.
//
// What has to be right here is not "does it parse" but the three things
// that would produce a confident, wrong picture:
//
//   * the WHOLE 8-byte record is byte-reversed on the wire, so reading it forwards gets a plausible
//     timestamp and every flag wrong;
//   * the records are level SNAPSHOTS, so edges only exist as differences between them;
//   * the tdc bit is a TOGGLE, not a level, and two toggles bracket one cycle — get that wrong and
//     every angle on screen is scaled by the wrong span.
//
//   cmake --build build --target rusefi_cycle_test && ./build/rusefi_cycle_test
#include "model/RusefiCycle.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-68s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static bool near(double a, double b, double eps = 0.05) { return std::fabs(a - b) < eps; }

// Build a composite record the way the FIRMWARE does: fill the little-endian layout, then reverse
// the whole 8 bytes. Building it any other way would let the test
// agree with a decoder that has the byte order backwards.
static void push(std::vector<uint8_t>& buf, uint32_t t_us, bool pri, bool tdc,
                 uint8_t coil, uint8_t inj, uint8_t camMask = 0, bool sync = true) {
    uint8_t b[8] = {};
    b[0] = uint8_t(t_us); b[1] = uint8_t(t_us >> 8); b[2] = uint8_t(t_us >> 16); b[3] = uint8_t(t_us >> 24);
    uint8_t f = 0;
    if (pri)            f |= 0x01;
    if (camMask & 0x01) f |= 0x02;   // cam1
    if (sync)           f |= 0x08;
    if (tdc)            f |= 0x10;
    if (camMask & 0x02) f |= 0x20;   // cam2
    b[4] = f;
    b[5] = coil;
    b[6] = inj;
    for (int i = 7; i >= 0; --i) buf.push_back(b[i]);      // reversed, as sent
}

static const enginecycle::Trace* find(const enginecycle::Cycle& c, const std::string& label) {
    for (const auto& t : c.traces()) if (t.label == label) return &t;
    return nullptr;
}

int main() {
    using namespace enginecycle;
    std::puts("=== RusefiCycle (composite tooth log -> native cycle) ===");

    std::puts("\n-- the wire record is byte-REVERSED, and decodes field by field --");
    {
        std::vector<uint8_t> buf;
        push(buf, 0x11223344, /*pri*/true, /*tdc*/true, /*coil*/0x05, /*inj*/0x82, /*cam*/0x03);
        const auto s = rusefi_cycle::decodeSamples(buf.data(), buf.size());
        ck(s.size() == 1, "one record in, one sample out", std::to_string(s.size()));
        if (s.size() == 1) {
            // Read forwards, the timestamp would come out 0x44332211 — plausible, and wrong.
            ck(s[0].t_us == 0x11223344, "timestamp survives the swap", std::to_string(s[0].t_us));
            ck(s[0].priLevel, "primary trigger level");
            ck(s[0].tdc, "tdc bit");
            ck(s[0].coil == 0x05, "coil bitmask", std::to_string(s[0].coil));
            ck(s[0].injector == 0x82, "injector bitmask", std::to_string(s[0].injector));
            ck(s[0].cam[0] && s[0].cam[1] && !s[0].cam[2], "cam1+cam2 set, cam3 clear");
        }
    }

    std::puts("\n-- two TDC toggles bracket ONE cycle, and set the angle scale --");
    {
        // A 720 deg cycle spanning 100 ms is TWO revolutions in 0.1 s => 1200 rpm. Coil 1 charges a
        // quarter of the way in and fires halfway: 180 deg and 360 deg.
        std::vector<uint8_t> buf;
        push(buf, 1000,   false, false, 0x00, 0x00);   // pre-roll
        push(buf, 10000,  false, true,  0x00, 0x00);   // TDC toggle #1 -> cycle starts here
        push(buf, 35000,  false, true,  0x01, 0x00);   // coil 1 on   (25 ms  => 180 deg)
        push(buf, 60000,  false, true,  0x00, 0x00);   // coil 1 off  (50 ms  => 360 deg)
        push(buf, 110000, false, false, 0x00, 0x00);   // TDC toggle #2 -> cycle ends (100 ms span)

        Cycle c;
        const auto r = rusefi_cycle::decodeCycle(buf.data(), buf.size(), 720.0, c);
        ck(r.ok, "decodes a cycle", r.message);
        ck(r.tdcMarks == 2, "two TDC marks found", std::to_string(r.tdcMarks));
        ck(near(c.cycleAngle(), 720.0), "span is the caller's, since rusEFI never reports one");
        ck(near(r.rpm, 1200.0, 1.0), "rpm derived from the cycle duration", std::to_string(r.rpm));
        ck(c.isReconstructed(), "stamped RECONSTRUCTED — the angles were interpolated");
        ck(!c.note().empty(), "and carries a note saying so", c.note());

        const Trace* t = find(c, "Coil 1");
        ck(t != nullptr, "coil 1 has a trace");
        if (t) {
            const auto sp = c.spans(*t);
            ck(sp.size() == 1, "one dwell span", std::to_string(sp.size()));
            if (sp.size() == 1) {
                ck(near(sp[0].from, 180.0), "dwell starts at 180 deg", std::to_string(sp[0].from));
                ck(near(sp[0].to,   360.0), "spark at 360 deg",        std::to_string(sp[0].to));
            }
        }
    }

    std::puts("\n-- the cycle SPAN is the caller's, so a two-stroke scales differently --");
    {
        // The same microseconds, declared as a 360 deg engine: the event that was 180 deg is now 90,
        // and the rpm HALVES because one cycle is now one revolution rather than two.
        std::vector<uint8_t> buf;
        push(buf, 1000,   false, false, 0x00, 0x00);   // pre-roll: the toggle needs something to differ from
        push(buf, 10000,  false, true,  0x00, 0x00);
        push(buf, 35000,  false, true,  0x01, 0x00);
        push(buf, 110000, false, false, 0x00, 0x00);
        Cycle c;
        const auto r = rusefi_cycle::decodeCycle(buf.data(), buf.size(), 360.0, c);
        ck(r.ok, "decodes", r.message);
        ck(near(r.rpm, 600.0, 1.0), "rpm follows the declared span", std::to_string(r.rpm));
        const Trace* t = find(c, "Coil 1");
        double rise = -1.0;
        if (t) for (const Edge& e : t->edges) if (e.high) { rise = e.angle; break; }
        ck(near(rise, 90.0), "the 25 ms event is 90 deg on a 360 cycle", std::to_string(rise));

        // The coil's OFF lands exactly on the closing TDC, and 360 on a 360 span is 0 — a cycle is
        // circular, so an event at the end IS at the start. Worth pinning: it means edge order is
        // not event order, and a test that assumed edges.front() was the first thing to happen
        // would read this correct wrap as a fault.
        double fall = -1.0;
        if (t) for (const Edge& e : t->edges) if (!e.high) { fall = e.angle; break; }
        ck(near(fall, 0.0), "an event on the cycle boundary wraps to 0, not 360",
           std::to_string(fall));
    }

    std::puts("\n-- the LAST complete cycle wins --");
    {
        // Three TDC marks = two cycles. A capture means "what is it doing NOW", so the second one is
        // the answer and the first cycle's coil event must not appear.
        std::vector<uint8_t> buf;
        push(buf, 0,      false, false, 0x00, 0x00);
        push(buf, 10000,  false, true,  0x00, 0x00);   // cycle A starts
        push(buf, 35000,  false, true,  0x02, 0x00);   // coil 2 fires in cycle A
        push(buf, 45000,  false, true,  0x00, 0x00);
        push(buf, 110000, false, false, 0x00, 0x00);   // cycle A ends / B starts
        push(buf, 135000, false, false, 0x01, 0x00);   // coil 1 in cycle B (25 ms in => 180 deg)
        push(buf, 160000, false, false, 0x00, 0x00);
        push(buf, 210000, false, true,  0x00, 0x00);   // B ends
        Cycle c;
        const auto r = rusefi_cycle::decodeCycle(buf.data(), buf.size(), 720.0, c);
        ck(r.tdcMarks == 3, "three marks seen", std::to_string(r.tdcMarks));
        ck(find(c, "Coil 1") != nullptr, "the LAST cycle's coil is present");
        ck(find(c, "Coil 2") == nullptr, "the previous cycle's coil is NOT");
        const Trace* t = find(c, "Coil 1");
        if (t) ck(near(t->edges.front().angle, 180.0), "…and is placed against the LAST cycle's TDC",
                  std::to_string(t->edges.front().angle));
    }

    std::puts("\n-- a channel already high at the boundary is carried in, not lost --");
    {
        // Coil 3 is mid-dwell when the cycle opens. Only diffing consecutive records would miss it
        // entirely and draw a spark with no dwell — a wrong picture, not a missing one.
        std::vector<uint8_t> buf;
        push(buf, 1000,   false, false, 0x04, 0x00);   // pre-roll, coil 3 already dwelling
        push(buf, 10000,  false, true,  0x04, 0x00);   // TDC #1, coil 3 ALREADY on
        push(buf, 35000,  false, true,  0x00, 0x00);   // coil 3 off at 180 deg
        push(buf, 110000, false, false, 0x00, 0x00);   // TDC #2
        Cycle c;
        rusefi_cycle::decodeCycle(buf.data(), buf.size(), 720.0, c);
        const Trace* t = find(c, "Coil 3");
        ck(t != nullptr, "coil 3 exists despite never switching on inside the cycle");
        if (t) {
            const auto sp = c.spans(*t);
            ck(sp.size() == 1 && near(sp[0].from, 0.0) && near(sp[0].to, 180.0),
               "its dwell runs from the cycle start to its spark",
               sp.empty() ? "no span" : std::to_string(sp[0].from) + ".." + std::to_string(sp[0].to));
        }
    }

    std::puts("\n-- the trigger becomes EVENTS: rising edges only, as a comb --");
    {
        std::vector<uint8_t> buf;
        push(buf, 1000,  false, false, 0x00, 0x00);    // pre-roll
        push(buf, 10000, false, true, 0x00, 0x00);
        for (int i = 1; i <= 6; ++i) {                       // 6 teeth, each a rise then a fall
            push(buf, uint32_t(10000 + i * 10000),     true,  true, 0x00, 0x00);
            push(buf, uint32_t(10000 + i * 10000 + 2000), false, true, 0x00, 0x00);
        }
        push(buf, 110000, false, false, 0x00, 0x00);
        Cycle c;
        rusefi_cycle::decodeCycle(buf.data(), buf.size(), 720.0, c);
        const Trace* t = find(c, "Crank 1");
        ck(t != nullptr, "a crank trace exists");
        // Six teeth => six marks. Emitting the falling edges too would double the comb, and letting
        // the level model collapse them would leave a single bar across the cycle.
        if (t) ck(t->edges.size() == 6, "six teeth, one mark each", std::to_string(t->edges.size()));
        if (t) ck(isEventSignal(t->signal), "the trigger is an EVENT trace");
    }





    std::puts("\n-- SINGLE ANCHOR: one TDC mark + the ECU's rpm --");
    {
        // The COMMON case on current firmware, not a fallback. A buffer holds 250 records and an
        // engine emits a roughly fixed number per cycle, so a buffer spans about one cycle and
        // therefore holds exactly one TDC toggle. Measured on a 4-cyl 36-1 at 1200 rpm: every
        // buffer spanned 100.0 ms with one mark. Timestamps are also buffer-relative on this
        // firmware ("timestamp is offset to buffer begin"), so buffers cannot be stitched together.
        std::vector<uint8_t> buf;
        push(buf, 0,     false, false, 0x00, 0x00);
        push(buf, 10000, false, true,  0x00, 0x00);   // the ONE TDC mark, 10 ms in
        push(buf, 35000, false, true,  0x01, 0x00);   // coil 1 on, 25 ms after the mark
        push(buf, 60000, false, true,  0x00, 0x00);   // coil 1 off, 50 ms after
        push(buf, 99000, false, true,  0x00, 0x00);

        Cycle c;
        // No rpm: refuse rather than invent a scale.
        const auto none = rusefi_cycle::decodeCycle(buf.data(), buf.size(), 720.0, c, 0.0);
        ck(!none.ok && c.empty(), "one mark and NO rpm is refused", none.message);

        // With rpm: 1200 rpm => a 720 deg cycle takes 100 ms, so 25 ms after the mark is 180 deg.
        const auto r = rusefi_cycle::decodeCycle(buf.data(), buf.size(), 720.0, c, 1200.0);
        ck(r.ok, "one mark + rpm places the cycle", r.message);
        ck(r.singleAnchor, "and reports that it is single-anchored, not bracketed");
        ck(near(r.rpm, 1200.0, 1.0), "rpm is the ECU's", std::to_string(r.rpm));
        ck(c.note().find("scaled by ECU rpm") != std::string::npos,
           "the note says the scale came from rpm, not two marks", c.note());

        const Trace* t = find(c, "Coil 1");
        ck(t != nullptr, "coil 1 present");
        if (t) {
            const auto sp = c.spans(*t);
            ck(sp.size() == 1 && near(sp[0].from, 180.0) && near(sp[0].to, 360.0),
               "dwell 180..360 deg, scaled by rpm",
               sp.empty() ? "none" : std::to_string(sp[0].from) + ".." + std::to_string(sp[0].to));
        }

        // Records BEFORE the anchor are wrapped in, not discarded. Taking only what follows the
        // mark would draw a fraction of the cycle and leave the rest blank, which reads as
        // "nothing happened there".
        std::vector<uint8_t> pre;
        push(pre, 0,     false, false, 0x02, 0x00);   // coil 2 already on, BEFORE the mark
        push(pre, 5000,  false, false, 0x00, 0x00);   // coil 2 off 5 ms before the mark
        push(pre, 10000, false, true,  0x00, 0x00);   // the mark
        push(pre, 90000, false, true,  0x00, 0x00);
        Cycle d;
        rusefi_cycle::decodeCycle(pre.data(), pre.size(), 720.0, d, 1200.0);
        const Trace* t2 = find(d, "Coil 2");
        ck(t2 != nullptr, "a pre-anchor event still reaches the picture");
        // 5 ms before the mark = 36 deg before 0 = 684 deg.
        if (t2) ck(near(t2->edges.back().angle, 684.0, 0.5),
                   "…wrapped to its phase (684 deg), not dropped",
                   std::to_string(t2->edges.back().angle));
    }

    std::puts("\n-- LEGACY: TDC is a 10us PULSE, not a toggle --");
    {
        // The two firmwares mark TDC differently and the mistake is symmetric: counting every
        // change on legacy pairs a pulse's own rise and fall and calls THAT the cycle (10 us, rpm
        // in the millions); counting only rising edges on modern finds one mark every two cycles
        // and doubles every angle. So the rule is per-layout.
        auto legacy = [](std::vector<uint8_t>& b, uint32_t t, uint8_t flags) {
            b.push_back(uint8_t(t >> 24)); b.push_back(uint8_t(t >> 16));
            b.push_back(uint8_t(t >> 8));  b.push_back(uint8_t(t));
            b.push_back(flags);                       // bit0 pri, bit2 TDC, bit3 sync, bit4 coil
        };
        std::vector<uint8_t> buf;
        legacy(buf, 1000,   0x08);                    // synced, no TDC yet
        legacy(buf, 10000,  0x0c);                    // TDC pulse: rises
        legacy(buf, 10010,  0x08);                    //            falls 10 us later
        legacy(buf, 35000,  0x18);                    // coil on, 25 ms in
        legacy(buf, 60000,  0x08);                    // coil off, 50 ms in
        legacy(buf, 110000, 0x0c);                    // next TDC pulse: cycle is 100 ms
        legacy(buf, 110010, 0x08);

        ck(rusefi_cycle::detectRecordBytes(buf.data(), buf.size()) == 5, "detected as legacy");

        Cycle c;
        const auto r = rusefi_cycle::decodeCycle(buf.data(), buf.size(), 720.0, c);
        ck(r.ok, "decodes", r.message);
        // Two PULSES, not four marks — the falling edge of each is the same event.
        ck(r.tdcMarks == 2, "two TDC marks, not four", std::to_string(r.tdcMarks));
        ck(near(r.rpm, 1200.0, 1.0), "rpm from the 100 ms cycle, not the 10 us pulse",
           std::to_string(r.rpm));

        // The aggregate coil bit becomes ONE unnumbered lane. "Coil 1" would imply this firmware
        // said cylinder 1 fired, which it cannot.
        const Trace* t = find(c, "Coil (any)");
        ck(t != nullptr, "the aggregate coil bit is an unnumbered lane");
        if (t) {
            const auto sp = c.spans(*t);
            ck(sp.size() == 1 && near(sp[0].from, 180.0) && near(sp[0].to, 360.0),
               "and its dwell lands at 180-360 deg",
               sp.empty() ? "none" : std::to_string(sp[0].from) + ".." + std::to_string(sp[0].to));
        }
        ck(c.note().find("per-cylinder") != std::string::npos,
           "the note explains the missing per-cylinder detail", c.note());
    }

    std::puts("\n-- the two layouts are told apart, including when BOTH sizes divide --");
    {
        // The dangerous length is any multiple of 40: 5 and 8 both divide it, so divisibility
        // cannot settle it and the CONTENT must. A full modern buffer is 250*8 = 2000 bytes, which
        // is divisible by 5 — so this is not a corner case, it is the normal modern buffer.
        std::vector<uint8_t> longBuf;                       // 250 x 8-byte records
        for (int i = 0; i < 250; ++i)
            push(longBuf, uint32_t(100000 + i * 700), (i & 1) != 0, false, uint8_t(i & 0x0f), 0);
        ck(longBuf.size() == 2000, "a full modern buffer is 2000 bytes", std::to_string(longBuf.size()));
        ck(longBuf.size() % 5 == 0 && longBuf.size() % 8 == 0, "…and BOTH sizes divide it");
        ck(rusefi_cycle::detectRecordBytes(longBuf.data(), longBuf.size()) == 8,
           "still detected as 8-byte",
           std::to_string(rusefi_cycle::detectRecordBytes(longBuf.data(), longBuf.size())));

        // And the mirror: 5-byte records at a length both divide.
        std::vector<uint8_t> shortBuf;                      // 40 x 5-byte records = 200 bytes
        for (int i = 0; i < 40; ++i) {
            const uint32_t t = uint32_t(100000 + i * 700);
            shortBuf.push_back(uint8_t(t >> 24)); shortBuf.push_back(uint8_t(t >> 16));
            shortBuf.push_back(uint8_t(t >> 8));  shortBuf.push_back(uint8_t(t));
            shortBuf.push_back(uint8_t(i & 1));   // flags: primary level toggling
        }
        ck(shortBuf.size() % 5 == 0 && shortBuf.size() % 8 == 0, "200 bytes: both sizes divide");
        ck(rusefi_cycle::detectRecordBytes(shortBuf.data(), shortBuf.size()) == 5,
           "detected as 5-byte",
           std::to_string(rusefi_cycle::detectRecordBytes(shortBuf.data(), shortBuf.size())));

        // An 8-byte buffer still reports its outputs; a 5-byte one must declare it has none.
        Cycle c;
        const auto rl = rusefi_cycle::decodeCycle(longBuf.data(), longBuf.size(), 720.0, c);
        ck(rl.recordBytes == 8 && rl.perChannelOutputs && rl.layout == rusefi_cycle::Layout::Modern8,
           "8-byte logs carry PER-CHANNEL coil/injector masks");
    }

    std::puts("\n-- REAL bytes from a 2026.04.05 rusEFI ECU: 5-byte records --");
    {
        // Captured off the bench while a 36-1 stim turned at 1200 rpm. Firmware of this age sends
        // 5-byte records with NO coil or injector fields, and the wire carries no version marker. Decoded as 8 these bytes give plausible garbage — the
        // full 1250-byte buffer scored 249/249 monotonic at 5 bytes against 63/155 at 8.
        static const uint8_t kReal[] = {
        0x06, 0x04, 0x53, 0xb8, 0x00, 0x06, 0x04, 0x56, 0x6e, 0x01, 0x06, 0x04, 0x59, 0x25,
        0x00, 0x06, 0x04, 0x5b, 0xdb, 0x01, 0x06, 0x04, 0x5e, 0x92, 0x00, 0x06, 0x04, 0x61,
        0x48, 0x01, 0x06, 0x04, 0x63, 0xff, 0x00, 0x06, 0x04, 0x66, 0xba, 0x01, 0x06, 0x04,
        0x69, 0x6c, 0x00, 0x06, 0x04, 0x6c, 0x22, 0x01, 0x06, 0x04, 0x6e, 0xda, 0x00, 0x06,
        0x04, 0x71, 0x8f, 0x01, 0x06, 0x04, 0x74, 0x46, 0x00, 0x06, 0x04, 0x76, 0xfc, 0x01,
        0x06, 0x04, 0x79, 0xb3, 0x00, 0x06, 0x04, 0x7c, 0x69, 0x01, 0x06, 0x04, 0x7f, 0x20,
        0x00, 0x06, 0x04, 0x81, 0xd6, 0x01, 0x06, 0x04, 0x84, 0x8d, 0x00, 0x06, 0x04, 0x87,
        0x43, 0x01, 0x06, 0x04, 0x89, 0xfa, 0x00, 0x06, 0x04, 0x8c, 0xb0, 0x01, 0x06, 0x04,
        0x8f, 0x67, 0x00, 0x06, 0x04, 0x92, 0x1f, 0x01, 0x06, 0x04, 0x94, 0xd4, 0x00, 0x06,
        0x04, 0x97, 0x8a, 0x01, 0x06, 0x04, 0x9a, 0x41, 0x00, 0x06, 0x04, 0x9c, 0xf7, 0x01,
        0x06, 0x04, 0x9f, 0xae, 0x00, 0x06, 0x04, 0xa2, 0x64, 0x01, 0x06, 0x04, 0xa5, 0x1b,
        0x00, 0x06, 0x04, 0xad, 0x3e, 0x01, 0x06, 0x04, 0xaf, 0xf5, 0x00, 0x06, 0x04, 0xb2,
        0xab, 0x01, 0x06, 0x04, 0xb5, 0x62, 0x00, 0x06, 0x04, 0xb8, 0x18, 0x01, 0x06, 0x04,
        0xba, 0xcf, 0x00, 0x06, 0x04, 0xbd, 0x85, 0x01, 0x06, 0x04, 0xc0, 0x3c, 0x00, 0x06,
        0x04, 0xc2, 0xf2, 0x01
        };
        ck(rusefi_cycle::detectRecordBytes(kReal, sizeof(kReal)) == 5,
           "detected as 5-byte records",
           std::to_string(rusefi_cycle::detectRecordBytes(kReal, sizeof(kReal))));

        const auto s = rusefi_cycle::decodeSamples(kReal, sizeof(kReal));
        ck(s.size() == 40, "40 records decoded", std::to_string(s.size()));
        bool mono = true;
        for (size_t i = 1; i < s.size(); ++i) if (s[i].t_us < s[i - 1].t_us) mono = false;
        ck(mono, "timestamps rise monotonically \xE2\x80\x94 the layout is right");

        // The measured wheel: ~694 us between edges, which is both edges of a 36-tooth wheel at
        // 1200 rpm. If the byte order or record size were wrong this would be noise.
        const double dt = double(s[1].t_us) - double(s[0].t_us);
        ck(dt > 600 && dt < 800, "edge spacing is the wheel's, not noise", std::to_string(dt));

        // This ECU was NOT synchronised: only bit0 (primary level) ever toggles, so there are no
        // TDC marks and no cycle can be built. The arm must REFUSE rather than invent an origin.
        enginecycle::Cycle c;
        const auto r = rusefi_cycle::decodeCycle(kReal, sizeof(kReal), 720.0, c);
        ck(!r.ok && c.empty(), "unsynchronised real data is refused", r.message);
        ck(r.recordBytes == 5 && !r.perChannelOutputs && r.layout == rusefi_cycle::Layout::Legacy5,
           "and it reports the LEGACY layout, so aggregate coil/inj bits are explained");
    }

    std::puts("\n-- refuses to invent an origin --");
    {
        Cycle c;
        c.addEdge("stale", 10.0, true, Signal::Coil, 1);

        std::vector<uint8_t> one;                            // a single TDC mark: no span to map onto
        push(one, 10000, false, true,  0x01, 0x00);
        push(one, 20000, false, true,  0x00, 0x00);
        const auto r1 = rusefi_cycle::decodeCycle(one.data(), one.size(), 720.0, c);
        ck(!r1.ok && c.empty(), "one TDC mark is refused, and the cycle is left empty", r1.message);

        std::vector<uint8_t> none;                           // teeth but no TDC at all
        push(none, 10000, true,  false, 0x00, 0x00);
        push(none, 20000, false, false, 0x00, 0x00);
        const auto r2 = rusefi_cycle::decodeCycle(none.data(), none.size(), 720.0, c);
        ck(!r2.ok && c.empty(), "no TDC marks is refused", r2.message);
        ck(r2.message.find("turning") != std::string::npos,
           "…and the message points at the actual cause", r2.message);

        const auto r3 = rusefi_cycle::decodeCycle(nullptr, 0, 720.0, c);
        ck(!r3.ok && c.empty(), "an empty buffer is refused", r3.message);
    }

    std::puts("\n-- a backwards timestamp is dropped, not drawn --");
    {
        std::vector<uint8_t> buf;
        push(buf, 1000,   false, false, 0x00, 0x00);   // pre-roll
        push(buf, 10000,  false, true,  0x00, 0x00);
        push(buf, 35000,  false, true,  0x01, 0x00);
        push(buf, 5000,   false, true,  0x08, 0x00);   // wrapped/corrupt: would land before the start
        push(buf, 110000, false, false, 0x00, 0x00);
        const auto s = rusefi_cycle::decodeSamples(buf.data(), buf.size());
        ck(s.size() == 4, "the out-of-order record is dropped", std::to_string(s.size()));
        Cycle c;
        const auto r = rusefi_cycle::decodeCycle(buf.data(), buf.size(), 720.0, c);
        ck(r.ok && find(c, "Coil 4") == nullptr, "and its coil never reaches the picture", r.message);
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All RusefiCycle tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
