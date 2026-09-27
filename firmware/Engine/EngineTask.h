#pragma once

#include "FreeRTOS.h"
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "task.h"
#include "EngineFrame.h"
#include "EngineModule.h"
#include "EngineStateMachine.h"
#include "../Signal/EnginePosition.h"
#include "../Signal/SignalBus.h"
#include "../Scheduler/EnginePositionHal.h"
#include "../Can/CanBroker.h"

// EngineTask talks to its environment ONLY through the SignalBus (+ the injected DTC table,
// the one cross-cutting error store). Pin-conflict status and the OBD-clear command are bus
// signals (wk::pin_conflict_fault / wk::obd_clear_cmd), not manager back-references.
class DtcManager;

// ---------------------------------------------------------------------------
// EngineTask -- the engine control FRAME SCHEDULER. It owns the SignalBus + frame
// timing + the position/firing HAL, and runs an ordered set of registered participants
// (EngineModule::update) in three phases each frame:
//
//   read_position
//   INPUT phase   -- producers fill the bus (sensors / inputs)
//   [SD-key arbitration, OBD fault-clear, pin-conflict DTC]   (orchestration glue)
//   MODULE phase  -- bus->bus transforms (protection, rev-limit, fuel, ignition, lua)
//   OUTPUT phase  -- bus consumers (outputs, diagnostics pack)
//   freeze-frames, commit-to-HAL, epos_hal.service, expire-stale, telemetry publish
//
// EngineTask is composition-agnostic: it does NOT own, construct, init, or include the
// concrete modules. SystemComposer creates them, cross-wires them, and registers them
// here in phase order. See docs/polymorphic-pipeline-architecture.md (decouple pass).
// ---------------------------------------------------------------------------

class EngineTask {
public:
    enum class Phase : uint8_t { INPUT = 0, MODULE = 1, OUTPUT = 2, COUNT = 3 };

    // Execution cadence a participant registers for (multi-cadence model, docs §B7):
    //   KHZ_1      — the fixed 1 kHz frame (sensors, lambda/trims, protection, telemetry,
    //                fast-transient detection). Self-decimating modules also live here.
    //   PER_CYCLE  — woken by the grid ISR each firing interval, at most every 8 ms (the heavy fuel base).
    // Per-cylinder is a future cadence; the framework is sized to grow into it.
    enum class Cadence : uint8_t { KHZ_1 = 0, PER_CYCLE = 1, COUNT = 2 };

    EngineTask(EnginePositionHal& epos_hal, CanBroker& can_broker);

    // The shared signal layer — comms packs the telemetry frame from this on demand.
    SignalBus& bus() { return signal_bus_; }

    // --- Composition API (called by SystemComposer before start()) ---
    // `name` labels the participant in a compose-time warning (a module registered twice).
    // cadence_hz: how often the module's OUTPUT must refresh (its schema cadence_hz). The scheduler runs it
    // every ⌈1000/hz⌉ frames, staggered against same-rate modules, and stamps the matching publish ttl on
    // it. 1000 = every frame (default). Ignored for PER_CYCLE participants (they run per engine cycle).
    // PASS THE MODULE'S GENERATED cadence:: CONSTANT. The `cadence_hz` default is 1000, and omitting
    // the argument silently pins the module to 1 kHz no matter what its schema `cadence_hz` says —
    // codegen emits the constant into module_cadence.h and nothing forces anyone to consume it. The
    // ElectronicThrottle was moved to 500 Hz in the schema, the constant duly said 500, and the module
    // went on running at 1 kHz on the bench because this call site did not name it.
    void add_participant(EngineModule* m, const char* name, Phase phase,
                         Cadence cadence = Cadence::KHZ_1, uint16_t cadence_hz = 1000);

    void set_dtc(DtcManager* d) { dtc_ = d; }   // the one cross-cutting error store
    // Output-side pin-conflict probe (OutputManager::conflict_ptr()) OR'd into the P1650 merge so the
    // code heals only when BOTH the input and output pin conflicts clear. A plain value probe, not a
    // manager back-ref — EngineTask never sees OutputManager's type.
    void set_output_conflict(const bool* f) { output_conflict_ = f; }
    // Engine run-state thresholds (from the schema Engine group) — drive the STOPPED/CRANKING/RUNNING
    // machine. Pure primitives, so EngineTask stays decoupled from the generated config type.
    void set_engine_state_params(const EngineStateMachine::Params& p) { state_machine_.configure(p); }
    // Arm the lost-trigger watchdog. The position HAL derives the timeout from its live timebase and
    // pushes it to the VirtualTrigger, which scales it by the wheel's tooth count. Re-applied on
    // every reconfigure (the timebase is fixed, so this is idempotent).
    void configure_stall_watchdog() { epos_hal_.configure_stall_watchdog(); }

    // Start the frame task. Call after the composer has registered participants + injected refs.
    void start(uint8_t num_cylinders);

    // The per-cycle compute task — woken by the crank-pended soft IRQ (see main.cpp's wake relay),
    // not by the 1 kHz frame. Exposed so the relay can notify it from ISR context.
    TaskHandle_t cycle_task_handle() const { return cycle_task_handle_; }
    // 1 kHz control-frame counter — the watchdog's liveness heartbeat. Monotonic while the control
    // frame (sensors/protection/ElectronicThrottle/telemetry) is running; stops if the frame task hangs.
    uint32_t frame_wakes() const noexcept { return frame_wakes_; }
    // HOW MUCH OF THE 1 kHz FRAME THE FRAME ITSELF USES, in percent — the number that says whether the
    // engine loop is about to miss one. Whole-MCU load does not: it can read 40 % while this frame is
    // the thing running out of room, because everything else on the chip runs at lower priority and
    // simply waits. Smoothed for the gauge, plus the worst single frame since boot, which is what
    // actually decides whether a frame is ever late.
    uint8_t  frame_load_pct() const noexcept;
    uint8_t  frame_load_max_pct() const noexcept;
    // …and the worst frame in MICROSECONDS. The percentage saturates at 255 (it is a u8, and over 100
    // is already the report), which is fine for a gauge and useless for a diagnosis: a config write was
    // measured "255 %" and that could be 2.6 ms or 26. This is the number you take to the cause.
    uint32_t frame_max_us() const noexcept;
    // WHICH MODULES THE FRAME IS SPENT ON. One row per participant, heaviest first; `period` is how
    // many 1 kHz frames apart it runs, so a 50 Hz module showing 200 us costs 10 us a frame on average
    // and 200 on the one it lands in — both of which matter, for different reasons.
    // `per_cycle` is the distinction that makes the rest of the row mean anything: a PER_CYCLE module
    // runs on the crank, on its own task, and its microseconds are NOT part of the 1 kHz frame at all.
    // Reporting them in the same list without saying so put the fuel base — the heaviest thing in the
    // system — inside a frame it has never run in.
    struct FrameCost { const char* name; uint32_t us; uint16_t period; bool per_cycle; };
    uint8_t  frame_costs(FrameCost* out, uint8_t max) const;
    void     frame_load_reset() noexcept { frame_cyc_max_ = 0; }

    // The unified DTC table (injected) — the single error store; consumers read it here.
    DtcManager& dtc() { return *dtc_; }

    static void task_fn(void* pv);         // 1 kHz frame trampoline — not application code
    static void cycle_task_fn(void* pv);   // per-cycle task trampoline — not application code

private:
    void           run();
    void           run_frame();
    void           run_cycle();            // per-cycle task loop: wait on the ISR notify, run PER_CYCLE
    void           run_phase(Cadence cadence, Phase phase, const EnginePosition& pos, EngineFrame& frame);
    // Dispatch a lifecycle edge to every participant of one cadence (all phases). Called from that
    // cadence's OWN task so the call never races a participant's update().
    void           dispatch_lifecycle(Cadence cadence, bool started);
    EnginePosition read_position();
    EnginePosition read_position_raw() const;   // non-mutating decoder read (per-cycle task safe)
    // Commit is split by cadence: the 1 kHz frame owns ignition (spark/dwell), the per-cycle task
    // owns injection (open-angle/PW). shadow_ fields are per-field atomic, so the two tasks writing
    // disjoint fields need no lock.
    void           commit_ignition(const EngineFrame& frame);   // 1 kHz   -> spark + dwell
    void           commit_injection(const EngineFrame& frame);  // per-cyc -> inj angle + PW
    void           update_telemetry(const EnginePosition& pos, SignalBus& bus, const EngineFrame& frame);
    void           capture_freeze_frames(const EnginePosition& pos, SignalBus& bus);

    // Participant lists — intrusive singly-linked, NO heap and NO fixed capacity, so a module can never
    // overflow a bound and be silently dropped (an over-16 array cap once killed ETB/idle/lambda/etc.).
    // Indexed [cadence][phase]: each cadence task walks its own list in phase order. The link, label and
    // per-participant profiling live IN each EngineModule (sched_*); we thread them here at compose time.
    // Append at the TAIL so the explicit registration order is preserved — execution order is significant
    // (e.g. ElectronicThrottle publishes its duty BEFORE the H-bridge that consumes it).
    static constexpr uint8_t CADENCE_COUNT = static_cast<uint8_t>(Cadence::COUNT);
    static constexpr uint8_t PHASE_COUNT   = static_cast<uint8_t>(Phase::COUNT);
    EngineModule* phase_head_[CADENCE_COUNT][PHASE_COUNT] = {};
    EngineModule* phase_tail_[CADENCE_COUNT][PHASE_COUNT] = {};

    // Cadence gating: the 1 kHz frame bumps frame_count_; run_phase runs a KHZ_1 module only when
    // (frame_count_ % sched_period_ == sched_phase_). add_seq_ hands out a monotonic index so consecutive
    // registrations get consecutive phases — same-rate modules end up staggered across frames, not synced.
    uint32_t frame_count_ = 0;
    uint32_t add_seq_     = 0;

    // Per-cycle detection (angle wrap) for self-decimating compute. Crank angle runs 0..720;
    // a large drop is a cycle boundary -> bump the count.
    float    last_cycle_angle_ = 0.0f;
    uint32_t cycle_count_      = 0;
    // Last decoder edge count PUBLISHED. The tooth channel is written only when it changes, so that
    // age(trigger_teeth) reads as "since the last tooth" rather than "since the last frame".
    uint32_t last_edges_       = 0;
    bool     trigger_config_dtc_raised_ = false;   // edge-tracking for P1652 (see run_frame)

    // Engine run-state machine (owned by the 1 kHz frame task — the authoritative rpm source).
    // It publishes wk::engine_state and produces the start/stop edges. KHZ_1 participants get the
    // edge dispatched inline (same task); PER_CYCLE participants get it via the generation counters
    // below — single-writer (this task) monotonic uint32s the per-cycle task observes on its own task.
    EngineStateMachine state_machine_;
    volatile uint32_t  stop_gen_  = 0;       // bumped on each STOPPED edge
    volatile uint32_t  start_gen_ = 0;       // bumped on each RUNNING edge
    uint32_t           seen_stop_gen_  = 0;  // per-cycle task: last stop_gen_ it has dispatched
    uint32_t           seen_start_gen_ = 0;  // per-cycle task: last start_gen_ it has dispatched

    SignalBus          signal_bus_;
    EnginePositionHal& epos_hal_;
    CanBroker&         can_broker_;
    DtcManager*        dtc_ = nullptr;          // injected (the one error store)
    const bool*        output_conflict_ = nullptr;   // OutputManager pin-conflict probe (P1650 merge)

    uint8_t num_cylinders_ = 4;
    bool    key_on_        = false;
    bool    firing_order_dtc_raised_ = false;   // edge-track the CONFIG firing-order DTC (raise/heal once)
    bool    no_coil_dtc_raised_      = false;   // P1653: a firing cylinder has no coil row
    bool    no_inj_dtc_raised_       = false;   // P1654: a firing cylinder has no stage-1 injector row

    StaticTask_t task_tcb_      = {};
    StackType_t  task_stack_[1024] = {};
    TaskHandle_t task_handle_   = nullptr;

    // Per-cycle compute task: woken by the 1 kHz frame task when it detects the engine-cycle
    // boundary (xTaskNotifyGive — plain task context, syscall-safe, no cross-ISR-ceiling call),
    // with a 50 ms timeout fallback so it ticks while stopped. Prio 2 — BELOW the 1 kHz frame
    // task (3), so the heavy base compute runs in the slack and never delays sensor/lambda/
    // protection freshness; firing is protected by the fire ISR (NVIC 2), not by task priority.
    // Runs the PER_CYCLE participant list. per_cycle_wakes_ is published as telemetry so the
    // cadence can be verified live against RPM.
    StaticTask_t     cycle_tcb_         = {};
    StackType_t      cycle_stack_[1024] = {};
    TaskHandle_t     cycle_task_handle_ = nullptr;
    volatile uint32_t per_cycle_wakes_  = 0;
    uint32_t         last_notified_cycle_ = 0;   // 1 kHz task: last cycle_count it notified on
    uint32_t         frame_wakes_         = 0;   // 1 kHz frame counter (diagnostic: loop-rate health)
    uint32_t         frame_cyc_ema_       = 0;   // CPU cycles per frame body, smoothed (1/8 EMA)
    uint32_t         frame_cyc_max_       = 0;   // …and the worst single frame since boot/reset
};
