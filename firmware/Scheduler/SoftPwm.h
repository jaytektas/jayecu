#pragma once
#include "ITimerChannel.h"   // IAlarmTimer, ITimerChannel, OutputAction (all MCU-agnostic)
#include <cstdint>

// ---------------------------------------------------------------------------
// SoftPwm — a software pulse-train / PWM generator for N output pins, driven by
// ONE free-running tick source through a single ordered event list.
//
// This is the "software eTPU"-style emulation for outputs that have no hardware
// timer channel (every IGN/LS/HS pin on this board is plain GPIO). It mirrors the
// EventScheduler TIME domain: each active channel holds exactly ONE pending edge;
// the soonest sits at the list head; the alarm is armed for it; when it fires we
// toggle that channel's pin, recompute its next edge, re-insert it in order, and
// re-arm the new head. O(N) insert with N small — lean, and it scales to many
// pulse outputs (tach now; boost / PWM solenoids / fan later) off one timer.
//
// Portability: depends ONLY on IAlarmTimer (a 32-bit "call me at tick X" source)
// and ITimerChannel (a GPIO sink: force_output_now). No MCU headers — it compiles
// on the host against fakes for unit tests, and on any target that supplies those
// two backends. Moving to other silicon is "wire up a timer + a GPIO sink."
//
// Concurrency model (identical discipline to EventScheduler):
//   * ALL list mutation + pin edges happen in on_alarm() — the timer ISR, which
//     runs BELOW the engine scheduler/decoder and cannot preempt itself.
//   * Task context (the 1 kHz module) ONLY writes per-channel single-word shadows
//     (period/high, atomic on a 32-bit MCU) via set_waveform(), then asks the ISR
//     to reconcile by pending it (request_reconcile -> alarm arm "now"). The task
//     never touches the list. So there is no lock.
//
// Ticks are abstract: whatever unit the bound IAlarmTimer counts in (the STM32
// backend uses 100 kHz / 10 µs). The owner converts frequency<->ticks; this core
// is integer-only and ISR-safe (no heap, no float, no blocking).
// ---------------------------------------------------------------------------
class SoftPwm {
public:
    // The software pulse engine carries every non-bridge PWM (tach, fans, pumps, boost, solenoids),
    // so the pool is widened from the original 8. Cost scales with total edge rate, not channel count
    // (one timer + one sorted list), so SoftPwm-backed outputs cap their freq (~2 kHz) — see
    // docs/pwm-hal-design.md §3.2. 16 channels @ <=2 kHz stays well under ~2% CPU.
    static constexpr int MAX_CHANNELS = 16;

    // ---- Channel allocator (no static partition) -------------------------------------------------
    // The channels are ONE shared pool, claimed per-owner. OutputManager takes one for every PWM output
    // row it builds; nothing reserves a fixed index. Owner 0 = free; pick small distinct
    // non-zero ids per consumer.
    static constexpr uint8_t OWNER_FREE = 0, OWNER_OUTPUTS = 2;

    // Claim the lowest free channel for `owner` (still parked/idle — the owner set_pin's it). -1 if
    // the pool is full.
    [[nodiscard]] int claim(uint8_t owner) noexcept {
        for (int i = 0; i < MAX_CHANNELS; ++i)
            if (ch_owner_[i] == OWNER_FREE) { ch_owner_[i] = owner; return i; }
        return -1;
    }
    // Detach + free every channel held by `owner` (set_pin(nullptr) parks the pin Hi-Z/idle).
    void release_owner(uint8_t owner) noexcept {
        for (int i = 0; i < MAX_CHANNELS; ++i)
            if (ch_owner_[i] == owner) { set_pin(i, nullptr, false); ch_owner_[i] = OWNER_FREE; }
    }

    // Bind the tick source and register the ISR trampoline. Call once at init.
    void bind(IAlarmTimer* alarm) noexcept {
        alarm_ = alarm;
        if (alarm_) alarm_->register_callback(&SoftPwm::trampoline, this);
    }

    // Point channel `idx` at an output pin with a polarity (task context).
    // active_high=true  -> the ON phase drives the pin HIGH (idle = LOW).
    // active_high=false -> the ON phase drives the pin LOW  (idle = HIGH, e.g.
    //                      an open-collector / active-low tach input).
    // pin=nullptr detaches the channel. Re-pointing parks the channel first.
    void set_pin(int idx, ITimerChannel* pin, bool active_high) noexcept {
        if (idx < 0 || idx >= MAX_CHANNELS) return;
        Ch& c = ch_[idx];
        c.on_lvl  = active_high ? OutputAction::DRIVE_HIGH : OutputAction::DRIVE_LOW;
        c.off_lvl = active_high ? OutputAction::DRIVE_LOW  : OutputAction::DRIVE_HIGH;
        if (c.pin && c.pin != pin) c.pin->disable_output();   // release the old pin back to Hi-Z
        c.pin     = pin;
        c.period  = 0;            // detached/re-pointed channels start parked
        c.dirty   = 1;
        // Switch the pin from its reset Hi-Z (input) state to a driven push-pull output,
        // preset to idle — WITHOUT this, force_output_now()'s BSRR writes hit an input pin
        // and nothing comes out. Task-context (a GPIO init), as every pin claimant does it.
        if (pin) pin->enable_output(c.off_lvl);
        request_reconcile();
    }

    // Program channel `idx`'s waveform (task context, lock-free):
    //   period_ticks == 0          -> channel OFF: stop pulsing, park pin at idle.
    //   0 < high_ticks < period    -> square wave; high_ticks is the ON-phase length.
    //   high_ticks == 0            -> hold idle  (0% duty)  — channel stays "live".
    //   high_ticks >= period       -> hold active (100% duty).
    // An ALREADY-running channel adopts a new period/high at its next edge (no
    // glitch); an OFF->ON or ON->OFF transition is reconciled promptly by the ISR.
    // What a channel is CURRENTLY pulsing at, for a caller that has to check it — a host test proving
    // an output's carrier followed its table. Reading the shadow is what the ISR
    // does; a single 32-bit load needs no lock.
    [[nodiscard]] uint32_t period_ticks(int idx) const noexcept {
        return (idx >= 0 && idx < MAX_CHANNELS) ? ch_[idx].period : 0u;
    }
    [[nodiscard]] uint32_t high_ticks(int idx) const noexcept {
        return (idx >= 0 && idx < MAX_CHANNELS) ? ch_[idx].high : 0u;
    }

    void set_waveform(int idx, uint32_t period_ticks, uint32_t high_ticks) noexcept {
        if (idx < 0 || idx >= MAX_CHANNELS) return;
        Ch& c = ch_[idx];
        const bool was_off = (c.period == 0);     // task owns the shadow; reading its own write is safe
        c.high   = high_ticks;
        c.period = period_ticks;                  // single-word store, published last
        const bool now_off = (period_ticks == 0);
        if (was_off != now_off) {                 // membership change -> ISR must (un)list it
            c.dirty = 1;
            request_reconcile();
        }
    }

    // Timer-ISR entry. The bound alarm calls this when the head edge is due.
    void on_alarm() noexcept {
        reconcile();                              // fold in task-side membership changes
        uint32_t now = alarm_ ? alarm_->now() : 0;
        // Fire every edge whose deadline has passed (catch up if we fell behind).
        while (head_ && (int32_t)(now - head_->edge) >= 0) {
            Ch* c = head_;
            remove_head();
            fire(c, now);
            now = alarm_ ? alarm_->now() : now;   // refresh: the edge work took time
        }
        arm_head();
    }

private:
    struct Ch {
        ITimerChannel* pin    = nullptr;
        OutputAction   on_lvl = OutputAction::DRIVE_HIGH;   // ON-phase pin level
        OutputAction   off_lvl= OutputAction::DRIVE_LOW;    // idle pin level
        // shadows (task -> ISR), each a single 32-bit word:
        volatile uint32_t period = 0;     // 0 => OFF
        volatile uint32_t high   = 0;
        volatile uint8_t  dirty  = 0;     // task changed membership; ISR re-reads
        // ISR-owned state:
        bool     listed   = false;        // currently in the ordered list
        bool     phase_on = false;        // currently in the ON phase
        uint32_t edge     = 0;            // absolute tick of this channel's next toggle
        Ch*      next     = nullptr;      // intrusive list link (ascending edge)
    };

    Ch           ch_[MAX_CHANNELS];
    uint8_t      ch_owner_[MAX_CHANNELS] = {};   // OWNER_FREE/TACH/OUTPUTS — the shared-pool allocator
    Ch*          head_  = nullptr;        // soonest pending edge
    IAlarmTimer* alarm_ = nullptr;

    static void trampoline(void* self) noexcept {
        static_cast<SoftPwm*>(self)->on_alarm();
    }

    // Ask the ISR to run a reconcile pass promptly (pend it via an immediate arm).
    // Safe from task context: arm() is ISR-safe and only programs the comparator /
    // pends the IRQ; the actual list work still happens in on_alarm().
    void request_reconcile() noexcept {
        if (alarm_) alarm_->arm(alarm_->now());
    }

    // Bring OFF<->ON membership changes into the (ISR-owned) list.
    void reconcile() noexcept {
        const uint32_t now = alarm_ ? alarm_->now() : 0;
        for (Ch& c : ch_) {
            if (!c.dirty) continue;
            c.dirty = 0;
            if (c.period == 0) {                  // turned OFF (or detached/re-pointed)
                if (c.listed) remove(&c);
                drive(&c, c.off_lvl);             // park at idle
            } else if (!c.listed) {               // turned ON -> enter the list now
                c.listed   = true;
                c.phase_on = false;               // first fire() flips into the ON phase
                c.edge     = now;                 // due immediately so the wave starts this pass
                insert(&c);
            }
            // already-listed channels adopt new period/high at their next fire()
        }
    }

    // Execute one channel edge: pick the next level + phase duration from the LIVE
    // shadow (so period/high changes and 0%/100% holds are honoured every edge),
    // drive the pin, advance the deadline, and re-list. Advancing from the prior
    // `edge` (not `now`) keeps the average frequency exact — no latency drift.
    void fire(Ch* c, uint32_t /*now*/) noexcept {
        const uint32_t p = c->period;
        if (p == 0) { c->listed = false; drive(c, c->off_lvl); return; }   // raced OFF
        uint32_t h = c->high;
        if (h > p) h = p;
        uint32_t dur;
        if (h == 0) {                 // 0% — hold idle, re-check once per period
            c->phase_on = false; drive(c, c->off_lvl); dur = p;
        } else if (h >= p) {          // 100% — hold active, re-check once per period
            c->phase_on = true;  drive(c, c->on_lvl);  dur = p;
        } else {                      // normal square wave
            c->phase_on = !c->phase_on;
            drive(c, c->phase_on ? c->on_lvl : c->off_lvl);
            dur = c->phase_on ? h : (p - h);
        }
        c->edge += dur;
        insert(c);
    }

    static void drive(Ch* c, OutputAction a) noexcept {
        if (c->pin) c->pin->force_output_now(a);
    }

    // Sorted insert by absolute edge, signed-wrap-safe (a precedes b iff the signed
    // 32-bit delta (a-b) < 0). The list horizon (< one timer wrap) keeps this total.
    void insert(Ch* c) noexcept {
        c->listed = true;
        Ch** pp = &head_;
        while (*pp && (int32_t)((*pp)->edge - c->edge) <= 0) pp = &(*pp)->next;
        c->next = *pp;
        *pp = c;
    }

    void remove_head() noexcept {
        if (head_) { Ch* c = head_; head_ = c->next; c->next = nullptr; c->listed = false; }
    }

    void remove(Ch* c) noexcept {
        Ch** pp = &head_;
        while (*pp && *pp != c) pp = &(*pp)->next;
        if (*pp) { *pp = c->next; c->next = nullptr; }
        c->listed = false;
    }

    void arm_head() noexcept {
        if (alarm_ && head_) alarm_->arm(head_->edge);
        else if (alarm_)     alarm_->disarm();
    }
};
