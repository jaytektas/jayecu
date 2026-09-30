#include "OutputTest.h"
#include "../../Scheduler/PinArbiter.h"
#include "../../Scheduler/ITimerChannel.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../EngineStateMachine.h"      // EngineRunState — a test is engine-stopped only
#include "../../Platform/platform_hal.h"
#include "../../../generated/modules/outputs_config.h"
#include "../../../generated/well_known_signals.h"   // wk::engine_state

OutputTest* g_output_test = nullptr;

namespace {
// outputs.output[].function — see the schema's enum: None / Ignition / Injector / Generic.
constexpr uint8_t FN_IGNITION = 1;
constexpr uint8_t FN_INJECTOR = 2;
}  // namespace

OutputTest::Mode OutputTest::mode_of(uint8_t row) const noexcept {
    if (!cfg_ || row >= OUTPUTS_OUTPUT_COUNT) return Mode::Level;
    switch (cfg_->output[row].function) {
        case FN_IGNITION: return Mode::Spark;
        case FN_INJECTOR: return Mode::Inject;
        default:          return Mode::Level;
    }
}

// An output's idle state is the one that leaves the load OFF, which is not always LOW: `active_high`
// is a per-row board/driver fact. Driving the wrong sense would turn "test this injector" into
// "hold this injector open", which is exactly the accident this whole module exists to prevent.
void OutputTest::set_pin(uint8_t row, bool on) noexcept {
    if (!arb_ || row >= MAX_ROWS) return;
    ITimerChannel* ch = arb_->owner_of(row) == PinOwner::TEST ? arb_->claim(row, PinOwner::TEST) : nullptr;
    if (!ch) return;
    const bool active_high = !cfg_ || row >= OUTPUTS_OUTPUT_COUNT || cfg_->output[row].active_high != 0;
    const bool level = on ? active_high : !active_high;
    ch->enable_output(level ? OutputAction::DRIVE_HIGH : OutputAction::DRIVE_LOW);
    ch->force_output_now(level ? OutputAction::DRIVE_HIGH : OutputAction::DRIVE_LOW);
}

void OutputTest::finish(uint8_t row) noexcept {
    if (row >= MAX_ROWS) return;
    set_pin(row, false);                     // off in the row's own sense FIRST, then hand the pin back
    // HAND IT BACK TO WHOEVER HAD IT, not to FREE. A coil released to FREE stays unowned until the next
    // rebuild, so the cylinder you just tested would not fire on the next start — the test would have
    // broken the thing it was checking.
    if (arb_) arb_->restore(row, PinOwner::TEST, slot_[row].prev_owner);
    const bool was = slot_[row].remaining != 0;
    slot_[row] = Slot{};
    if (was && active_n_) --active_n_;
}

bool OutputTest::start(uint8_t row, uint32_t count, uint32_t on_ms, uint32_t off_ms,
                       uint32_t now_ms) noexcept {
    if (row >= MAX_ROWS || !cfg_ || row >= OUTPUTS_OUTPUT_COUNT) return false;
    if (count == 0) { cancel(row, now_ms); return true; }        // 0 is the Off button
    // NOTHING IS DRIVEN WITH THE KEY OFF — the test included. Every other output lets go of its pin then
    // (OutputManager, and the firing pins in EnginePositionHal), and a test that drove one anyway would
    // be the single exception to a rule a person wiring the car relies on.
    extern bool g_system_active;
    if (!g_system_active) return false;

    // WHAT WAS ASKED FOR IS WHAT HAPPENS. The only substitution is for a value the caller omitted —
    // a bare CLI call — and the function picks a sensible one. Nothing is clamped down: the operator
    // owns the hardware, and a control that reads 20 ms while doing 8 makes every measurement taken
    // through it quietly wrong.
    const Mode m = mode_of(row);
    const uint32_t def_on = (m == Mode::Spark)  ? DEF_SPARK_ON_MS
                          : (m == Mode::Inject) ? DEF_INJ_ON_MS
                                                : DEF_LEVEL_ON_MS;
    if (on_ms == 0u)  on_ms  = def_on;
    if (off_ms == 0u) off_ms = (count > 1u) ? DEF_OFF_MS : 0u;   // no gap needed for a single shot

    // BORROW, not claim. The firing layer owns every coil and injector pin for as long as the tune
    // says so, so claim() can never grant one — and a test that cannot fire a coil is no test. The
    // engine is stopped (update() enforces it every frame), so the firing layer is not driving them.
    PinOwner prev = PinOwner::FREE;
    if (!arb_ || arb_->borrow(row, PinOwner::TEST, prev) == nullptr) return false;

    Slot& s   = slot_[row];
    const bool was_active = s.remaining != 0;                     // a re-press replaces, never double-counts
    s.prev_owner = (prev == PinOwner::TEST) ? slot_[row].prev_owner : prev;   // a re-press keeps the original
    s.remaining = count;
    s.on_ms     = on_ms;
    s.off_ms    = off_ms;
    s.driving   = true;
    s.next_ms   = now_ms + on_ms;
    // The whole-test deadline, independent of the count: it is what survives a host that stops asking.
    // 64-bit: 100000 pulses of a long on/off overflows 32 bits and would wrap to a deadline that ends
    // the test early.
    const uint64_t span = (static_cast<uint64_t>(on_ms) + off_ms) * count + 1000u;
    s.expiry_ms = now_ms + static_cast<uint32_t>(span > MAX_TOTAL_MS ? MAX_TOTAL_MS : span);
    if (s.expiry_ms == 0) s.expiry_ms = 1u;                      // 0 is never a deadline
    if (!was_active) ++active_n_;
    set_pin(row, true);
    return true;
}

void OutputTest::cancel(uint8_t row, uint32_t /*now_ms*/) noexcept { finish(row); }

void OutputTest::cancel_all(uint32_t /*now_ms*/) noexcept { cancel_all_rows_(); }

void OutputTest::cancel_all_rows_() noexcept { for (uint8_t r = 0; r < MAX_ROWS; ++r) if (slot_[r].remaining) finish(r); }

void OutputTest::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    const auto engine_state = static_cast<EngineRunState>(
        static_cast<int>(bus.get(wk::engine_state, 0.0f)));
    step(engine_state == EngineRunState::STOPPED, platform_get_tick_ms());
}

void OutputTest::step(bool engine_stopped, uint32_t now_ms) noexcept {
    if (active_n_ == 0) return;
    // …and the key going off ends a test in progress, the same way.
    extern bool g_system_active;
    if (!g_system_active) { cancel_all(now_ms); return; }
    // A START DROPS EVERYTHING, before any phase is advanced. Same rule as the H-bridge nudge, for the
    // same reason: the engine turning means the firing layer wants these pins back this instant.
    if (!engine_stopped) { cancel_all(now_ms); return; }

    for (uint8_t r = 0; r < MAX_ROWS; ++r) {
        Slot& s = slot_[r];
        if (!s.remaining) continue;
        if (static_cast<int32_t>(now_ms - s.expiry_ms) >= 0) { finish(r); continue; }   // whole-test deadline
        if (static_cast<int32_t>(now_ms - s.next_ms) < 0) continue;                     // phase still running

        if (s.driving) {
            // End of the on-phase. For a coil this edge IS the spark.
            set_pin(r, false);
            s.driving = false;
            // DECREMENT AFTER the last-pulse test, not before it. Decrementing first left `remaining`
            // at 0 when finish() ran, so finish() saw an already-idle slot and never decremented the
            // active count — the status said something was still firing, for ever, with no row to name.
            if (s.remaining <= 1u) { finish(r); continue; }
            --s.remaining;
            s.next_ms = now_ms + s.off_ms;
        } else {
            set_pin(r, true);
            s.driving = true;
            s.next_ms = now_ms + s.on_ms;
        }
    }
}
