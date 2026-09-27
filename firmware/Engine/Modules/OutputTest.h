#pragma once
#include <cstdint>
#include "../EngineModule.h"

#include "../../Scheduler/PinArbiter.h"   // PinOwner — a slot remembers who it borrowed from

class PinArbiter;
struct OutputsConfig;

// ---------------------------------------------------------------------------
// OutputTest — the bench output test: fire a coil, click an injector, switch a relay.
//
// WHAT IT IS FOR. "Is this wire the one I think it is" is a question you ask BEFORE anything
// downstream can be trusted — before a producer publishes, before sync, before a tune means
// anything. So a test needs neither a demand signal nor an asserted enable, the same way
// hbridge_bench_nudge() does not.
//
// EVERY SAFETY RULE IS HERE, NOT IN THE STUDIO. A host asks; this decides. That is not defensive
// programming, it is the only arrangement that works: the thing being protected against is the host
// going away.
//
//   * ENGINE STOPPED, and a start cancels mid-test. Firing a coil out of phase or an injector into a
//     running engine is the failure this exists to make impossible.
//   * A DEADLINE CHECKED EVERY FRAME. A closed laptop lid is not an Abort, and neither is a yanked
//     USB cable — the test ends on its own or it is not safe to offer at all.
//   * IT DELIVERS WHAT WAS ASKED. The on-time is used as given. An earlier version clamped a coil to
//     8 ms whatever the request, which is the wrong trade for a bench tool: the operator owns the
//     hardware and has reasons — a weak coil, a CDI, a long pulse for an injector flow test — and a
//     control that silently does something other than what it reads produces measurements that are
//     quietly wrong. Mind the dwell: an ignition output charges the coil and the RELEASE makes the
//     spark, so holding one on saturates it and can take the driver with it. That is the operator's
//     call to make, and the page says so.
//   * THE PIN IS CLAIMED (PinOwner::TEST). A pin the firing layer holds is refused and the conflict
//     recorded, rather than two writers fighting over a coil.
//
// The MODE comes from the row's own `function`, so the operator never types a dwell: an Ignition row
// sparks, an Injector row pulses, anything else is held on as a level. One button per output.
// ---------------------------------------------------------------------------
class OutputTest : public EngineModule {
public:
    // Defaults for a bare CLI call that omits them. NOT ceilings — a value that is given is used.
    static constexpr uint32_t DEF_SPARK_ON_MS = 3;
    static constexpr uint32_t DEF_INJ_ON_MS   = 4;
    static constexpr uint32_t DEF_LEVEL_ON_MS = 2000;
    static constexpr uint32_t DEF_OFF_MS      = 500;
    // The one bound that remains, and it is not a limit on what may be asked for: it is what ends a
    // test when the host that asked for it goes away. Generous enough that a real test finishes
    // (200 injector pulses a second apart is 200 s); short enough that nothing is left driving for ever.
    static constexpr uint32_t MAX_TOTAL_MS    = 600000;  // 10 minutes

    void bind(PinArbiter* arb, const OutputsConfig* cfg) noexcept { arb_ = arb; cfg_ = cfg; }

    // Start (or replace) a test on one output row. count 0 cancels that row.
    // Returns false if the row is out of range or the pin could not be claimed.
    bool start(uint8_t row, uint32_t count, uint32_t on_ms, uint32_t off_ms, uint32_t now_ms) noexcept;

    void cancel(uint8_t row, uint32_t now_ms) noexcept;
    void cancel_all(uint32_t now_ms) noexcept;

    // A frame participant like any other module, in the OUTPUT phase beside OutputManager. It reads
    // the run state off the bus the same way HBridge does — the engine turning is the one input this
    // has, and taking it from the bus means it cannot disagree with what everything else believes.
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    // The frame body, split out so a host test can drive it without a bus.
    void step(bool engine_stopped, uint32_t now_ms) noexcept;

    [[nodiscard]] bool active() const noexcept { return active_n_ != 0; }
    // Is THIS row still firing? Asked per row rather than returned as a bitmask: a 32-bit mask over 48
    // rows aliases — row 34 came back as row 2 on the bench — and a shared bit means finishing one row
    // reports the other as stopped while it is still driving a coil.
    [[nodiscard]] bool active_row(uint8_t row) const noexcept {
        return row < MAX_ROWS && slot_[row].remaining != 0;
    }

    // What a row was resolved to, for the reply the host prints. Mirrors `function`.
    enum class Mode : uint8_t { Level = 0, Spark, Inject };
    [[nodiscard]] Mode mode_of(uint8_t row) const noexcept;

    // WHAT WAS ACTUALLY APPLIED, after clamping — so a host that asked for a 200 ms dwell is told it
    // is getting 8 rather than believing it got what it asked for. A clamp nobody can see is
    // indistinguishable from a clamp that is not there.
    void applied(uint8_t row, uint32_t& on_ms, uint32_t& off_ms) const noexcept {
        if (row >= MAX_ROWS) { on_ms = off_ms = 0; return; }
        on_ms = slot_[row].on_ms; off_ms = slot_[row].off_ms;
    }

private:
    static constexpr uint8_t MAX_ROWS = 48;   // PinArbiter::MAX_PINS

    struct Slot {
        uint32_t remaining  = 0;      // pulses still to fire (0 = idle)
        uint32_t on_ms      = 0;
        uint32_t off_ms     = 0;
        uint32_t next_ms    = 0;      // when the current phase ends
        uint32_t expiry_ms  = 0;      // whole-test deadline, whatever the count says
        bool     driving    = false;  // the output is on right now
        // Who held this pin before the test borrowed it, so it goes back to them and not to FREE.
        PinOwner prev_owner = PinOwner::FREE;
    };

    void set_pin(uint8_t row, bool on) noexcept;
    void finish(uint8_t row) noexcept;
    void cancel_all_rows_() noexcept;

    PinArbiter*          arb_ = nullptr;
    const OutputsConfig* cfg_ = nullptr;
    Slot                 slot_[MAX_ROWS];
    // A COUNT, not a bitmask. update() wants one cheap compare to skip the frame, and the per-row
    // truth lives in the slots where it cannot alias.
    uint8_t              active_n_ = 0;
};

// The one instance, bound at composition. Null on a build without one.
extern OutputTest* g_output_test;
