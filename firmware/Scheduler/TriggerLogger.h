// TriggerLogger — raw trigger edges in the TIME domain, for a wheel nobody has decoded yet.
//
// WHAT IT IS. The time between edges, shipped out continuously while the engine is cranked. The
// host draws a bar per edge whose height is the gap since the previous edge on that stream, and the
// proportions ARE the picture: a 36-1 draws thirty-five short bars and one double.
//
// WHAT IT IS NOT. It infers nothing, and nothing downstream is asked to. Teeth-per-revolution and
// revolutions-per-second are conflated in every trigger log — a log measures edges per second, and
// that is (features per revolution) x (revolutions per second): one equation, two unknowns. A
// one-cylinder distributor at a million rpm and a million-cylinder distributor at 1 rpm produce
// byte-identical captures. So there is no angle here, no rpm, no tooth count and no sync point, and
// no capture length or timing resolution can produce one.
//
// IT TAKES A COPY AND CHANGES NOTHING. EnginePositionHal records each edge here BEFORE handing it to
// the decoder, and the decoder carries on exactly as it would without a capture — so a capture can be
// armed at any time, on a running engine as well as a cranking one, and arming can never stop an
// engine starting. (It once switched the decoder off for the duration, and was refused while running
// for that reason; neither is true any more.) It also records with the key off, ahead of the key-on
// gate that keeps the decoder itself quiet on USB power.
//
// IT SEES ONLY THE EDGES THE CHANNEL IS ARMED FOR. The capture hardware is configured per stream —
// Rising, Falling or Both — and this sits behind that, so a stream set to Rising logs one edge per
// tooth and the mark/space widths are simply not in the data. Measured on the bench: a 36-1 armed
// Rising gives 35 records per revolution, not 70. For a wheel whose identity is in its SLOT WIDTHS
// (an optical CAS, a variable-width cam window) the stream must be set to Both before the log can
// answer the question at all — the capture cannot recover an edge nobody asked the hardware for.
//
// THE RING IS A HAND-OFF BUFFER, NOT THE CAPTURE. A fixed ring cannot be the unit of capture: at 200
// rpm, 4096 records is about twenty minutes of a one-trigger-per-revolution engine and 3.4 seconds
// of a 180-slot crank wheel armed Both. Nobody cranks to fill a buffer. So it WRAPS, the host drains
// it far faster than cranking fills it, and the user stops when they have seen enough.
//
// AND OVERFLOW IS DETECTABLE RATHER THAN AVOIDED BY ASSERTION. Every record has a monotonic absolute
// index. The host asks for everything after N; if N has already been overwritten the reply says
// where the data now starts, and the host draws that span as missing instead of splicing two
// disconnected stretches into one continuous-looking picture. Silently plausible is the failure mode
// worth engineering against.
#pragma once
#include <cstdint>
#include "SchedulerTypes.h"
#include "ITimerChannel.h"

// One edge: which stream, what the lines looked like, and when.
//
// 6 bytes, packed. `t_us` is microseconds since the capture was armed — 32 bits is 71 minutes, and a
// cranking session is seconds. `levels` is one bit per stream (bit i = stream i's line AFTER this
// edge), which is what says what the OTHER lines were doing at this instant — the whole question
// when working out how a cam relates to a crank.
struct __attribute__((packed)) TriggerLogRecord {
    uint32_t t_us;
    uint8_t  levels;      // bit i = stream i level (post-edge)
    uint8_t  flags;       // see TriggerLogFlags
};
static_assert(sizeof(TriggerLogRecord) == 6, "the wire format is 6 bytes");

enum TriggerLogFlags : uint8_t {
    // WHICH STREAM FIRED, in the top three bits. Not decoration — without it a log from a
    // SINGLE-EDGE-armed stream is undecodable: every edge on a Rising channel leaves the line high,
    // so `levels` never changes and a reader diffing consecutive rows recovers zero edges from a log
    // that is full of them. Measured on the bench: a 36-1 armed Rising gave 1200 records whose
    // levels byte was constant.
    TLOG_STREAM_SHIFT = 5,
    TLOG_STREAM_MASK  = 0xE0u,
    // Bits 0-4 reserved. They used to carry the decoder's sync level; the field was retired while the
    // decoder was still switched off during a capture, and nothing has needed it back since.
};

class TriggerLogger {
public:
    // 4096 x 6 B = 24 KB, and this IS the capture length: the buffer fills once and stops. It was a
    // wrapping ring sized as a hand-off depth, on the assumption the host would stream it forever —
    // it could not, and the overwriting is what shredded captures. Nothing here wraps now, so there
    // is no mask and no modulo on the ISR path: the index is simply total_.
    static constexpr uint32_t CAPACITY = 4096;

    // Full is the END of a capture, not a failure: the buffer holds the snapshot the operator asked
    // for and nothing further is appended until the next arm. The host pages it out at its own pace.
    enum class State : uint8_t { Idle = 0, Armed = 1, Recording = 2, Full = 3 };

    // The capture timebase, wired once at bring-up. The logger owns it so ARMING needs no reference
    // to the position HAL: the comms layer knows about a logger and nothing else, which is the same
    // contract the engine-cycle capture has.
    void set_clock(const ITimerChannel* tb) noexcept { clock_ = tb; }
    // A SNAPSHOT, NOT A RING. This used to wrap for ever, on the reasoning that a fixed buffer is not
    // a unit of capture and the host should stream it. The arithmetic never supported that: a page
    // carries 168 records and the host asks for one every 200 ms, so it drains 840 records/s, while a
    // 60-2 plus a cam at 1200 rpm produces 1180. The ring absorbed the 340/s deficit for twelve
    // seconds and then overwrote records the host had not yet read, for ever after — and those
    // vanished edges drew as enormous intervals, which is a picture of the LINK falling behind
    // presented as though it were the wheel.
    //
    // Bounded, the deficit stops being data loss and becomes latency: nothing is overwritten, so a
    // host that trails simply catches up. And because the host draws the buffer AS IT FILLS rather
    // than waiting for the end, there is no need to let the operator choose a shorter capture — the
    // only reason to want one was to see something sooner, and they already do. base() is then always 0 and the
    // wrap-detection on both sides can never fire, which is the point — it was the only thing that
    // could, and it was firing constantly.
    void arm_now() noexcept {
        arm(clock_ ? clock_->get_current_ticks() : 0u,
            clock_ ? clock_->get_ticks_per_second() : 1u);
    }

    // Arm from task context. `t0` is the timebase tick the capture is measured from and
    // `ticks_per_second` converts to microseconds at record time (the reader never needs the rate).
    void arm(uint32_t t0, uint32_t ticks_per_second) noexcept {
        state_   = State::Idle;                 // stop the ISR writing while we reset
        total_   = 0;
        levels_  = 0;
        t0_      = t0;
        tps_     = ticks_per_second ? ticks_per_second : 1u;
        state_   = State::Armed;
    }
    void stop() noexcept { state_ = State::Idle; }

    // A CAPTURE IS LIVE — armed, filling, or full and awaiting readout. True until stop(); the comms
    // layer stops a live capture when the link drops, so the next session starts from Idle.
    [[nodiscard]] bool     recording() const noexcept {
        const State s = state_;
        return s == State::Armed || s == State::Recording || s == State::Full;
    }
    [[nodiscard]] State    state() const noexcept { return state_; }
    // Records written since the arm. Monotonic and never clamped — it is the host's cursor.
    [[nodiscard]] uint32_t total() const noexcept { return total_; }
    // Absolute index of the oldest record still held — ALWAYS 0, because nothing is ever
    // overwritten. It stays in the wire header on purpose: it is the host's proof of that, and the
    // one number that would go non-zero if this ever wrapped again. A reader that stopped checking
    // it would not notice.
    [[nodiscard]] uint32_t base() const noexcept { return 0u; }

    // ISR CONTEXT (capture, priority 1). Single writer — no other context appends, which is what
    // lets total_ be a plain counter rather than an atomic claim.
    void record(uint8_t stream, bool level, uint32_t tick) noexcept {
        if (state_ != State::Armed && state_ != State::Recording) return;
        // The snapshot is complete. Stop appending rather than overwriting what the host has not yet
        // read: a shorter capture that is whole beats a longer one with holes in it.
        if (total_ >= CAPACITY) { state_ = State::Full; return; }
        if (stream < 8) {
            if (level) levels_ |= static_cast<uint8_t>(1u << stream);
            else       levels_ &= static_cast<uint8_t>(~(1u << stream));
        }
        TriggerLogRecord& r = buf_[total_];        // bounded above, so no wrap and no mask
        // Unsigned subtraction wraps correctly across a timer overflow, so a capture spanning one
        // costs nothing. 32 bits of microseconds is 71 minutes.
        const uint32_t dt = tick - t0_;
        r.t_us   = static_cast<uint32_t>((static_cast<uint64_t>(dt) * 1000000ull) / tps_);
        r.levels = levels_;
        r.flags  = static_cast<uint8_t>((stream & 0x7u) << TLOG_STREAM_SHIFT);
        ++total_;
        state_ = State::Recording;
    }

    // Task context. Copies up to `max` records starting at ABSOLUTE index `first` into `out`.
    // `out_first` receives the index actually served. It can no longer differ from `first`: nothing
    // is overwritten, so a host that falls behind loses nothing and merely arrives late. The
    // out-parameter stays because the host still compares the two and draws a break if they differ,
    // which is the check that would catch this silently regressing.
    [[nodiscard]] uint32_t read(uint32_t first, uint32_t max, TriggerLogRecord* out,
                                uint32_t& out_first) const noexcept {
        const uint32_t t = total_;
        out_first = first;
        if (!out || first >= t) return 0;
        uint32_t n = t - first;
        if (n > max) n = max;
        for (uint32_t i = 0; i < n; ++i) out[i] = buf_[first + i];
        return n;
    }

private:
    TriggerLogRecord buf_[CAPACITY]{};
    volatile State   state_ = State::Idle;
    volatile uint32_t total_ = 0;   // records written; also the next index, since nothing wraps
    uint8_t  levels_ = 0;           // running snapshot of every stream's line
    uint32_t t0_  = 0;
    // 0, not 1: arm() sets it before the ISR can divide by it, and a single non-zero default here
    // makes the whole object (buffer included) an initialised image — 24 KB of zeros in flash.
    uint32_t tps_ = 0;
    const ITimerChannel* clock_ = nullptr;
};
