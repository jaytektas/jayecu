#pragma once
#include <cstdint>

// Forward declarations — modules only see what they need.
struct EnginePosition;
struct EngineFrame;
class  SignalBus;
class  EngineTask;   // threads modules into its scheduler lists (friend, below)

// ---------------------------------------------------------------------------
// EngineModule — base class for all engine control modules.
//
// Modules are NOT tasks. They are called synchronously by EngineTask inside
// a single run_frame() call. Execution order is explicit in EngineTask.
//
// Lifecycle:
//   init()             — called once after config is loaded
//   on_config_change() — called when the host burns new constants
//   update()           — called every frame (100Hz or cycle-driven)
//   on_engine_start()  — called once on the transition INTO running (engine caught); anchor
//                        post-start clocks / re-arm per-run state here
//   on_engine_stop()   — called once on the transition INTO stopped; reset integrators etc.
//
// EngineTask drives the engine-state machine (STOPPED/CRANKING/RUNNING) and dispatches these two
// edge hooks to each participant ON ITS OWN CADENCE TASK, so a module never sees a lifecycle call
// race with its own update(). See EngineTask::run_frame / run_cycle.
//
// Data flow:
//   EnginePosition — decoder state (RPM, crank angle, sync level)
//   SignalBus      — all sensor signals, routable from any source
//   EngineFrame    — outputs (fuel PW, spark angle, cuts)
// ---------------------------------------------------------------------------

// TTL for bus.set() publishes. A module publishes with a freshness lifetime so its signals expire when
// the module stops running (disabled, engine stopped, dropped) — consumers then fail-safe (bus.valid()
// returns false, same as never-published). The scheduler derives each module's TTL from its cadence and
// stamps it on the module; modules just pass ttl() (below) to every publish.
static constexpr uint32_t TTL_FRAME_MS = 5;    // full-rate (1 kHz) modules — also = ttl_for(1000)
static constexpr uint32_t TTL_CYCLE_MS = 500;  // per-cycle modules: covers ~250 RPM minimum
static constexpr uint32_t FRAME_SLIP_MS = 4;   // absolute worst-case frame-scheduling slip under load

// Freshness TTL (ms) for a producer publishing at `hz`: its period + grace. Grace is the ABSOLUTE
// frame-slip budget OR half a period at low rates, whichever is larger — so the ttl is "period + slack",
// NOT "period × N". A 1 kHz signal → 5 ms, 100 Hz → 15 ms, 30 Hz → 49 ms, 10 Hz → 150 ms: each ≈ its own
// period plus a little, so a consumer fail-safes within ~one producer-period of the producer stopping,
// instead of a fixed multiplier making a slow signal linger 100× longer than a fast one. hz==0 → never
// expire (0), for the handful of signals that must persist.
inline constexpr uint32_t ttl_for(uint16_t hz) {
    if (hz == 0) return 0;
    const uint32_t period = 1000u / hz;                                    // ms (>=1 for hz<=1000)
    const uint32_t grace  = (period / 2u > FRAME_SLIP_MS) ? period / 2u : FRAME_SLIP_MS;
    return period + grace;
}

class EngineModule {
public:
    virtual ~EngineModule() = default;

    virtual void on_engine_start() {}
    virtual void on_engine_stop()  {}

    virtual void update(const EnginePosition& pos,
                        SignalBus&      bus,
                              EngineFrame&    frame) = 0;

protected:
    // The freshness TTL (ms) the scheduler derived from this module's cadence. Pass it as the ttl_ms arg
    // of EVERY bus.set()/set_bool() publish, so the signal expires ~one cadence-period after this module
    // stops running. The module never picks a number — cadence lives in the schema, the scheduler owns it.
    uint32_t ttl() const { return sched_ttl_; }

    // HOW LONG A FAULT THIS MODULE RAISED STAYS CURRENT WITHOUT BEING SAID AGAIN.
    //
    // The bus ttl is "period + a little": a consumer should notice a stopped producer within about one
    // of its periods, because a stale READING is worse than an absent one. A fault is the other way
    // round — dropping one because a single evaluation was late turns a fault light off on a car that
    // still has the fault — so this is deliberately slacker: eight periods, and never less than a
    // second however fast the module runs.
    //
    // Derived rather than a constant because the producers are not alike: a 20 Hz module and a 1 kHz
    // one sharing one number means it is either too tight for the slow one or pointlessly long for the
    // fast one. The number a module publishes its READINGS with already encodes its rate; this is the
    // same rate, with a fault's tolerance instead of a reading's.
    uint32_t dtc_ttl() const {
        const uint32_t t = sched_ttl_ ? sched_ttl_ * 8u : 1000u;
        return t < 1000u ? 1000u : (t > 30000u ? 30000u : t);
    }

    // This module's OWN period in ms — the 1 kHz frame count between runs, which is the same number.
    // ttl() is this plus grace, which is what freshness wants and the opposite of what a PULSE wants: a
    // cut is released by its signal expiring, so publishing one with ttl() makes it outlast the frame
    // that asked for it. Anything delivering a duty publishes with this instead. See CutDuty.
    uint32_t frame_ms() const { return sched_period_; }

private:
    // Intrusive scheduler state — EngineTask threads each module into ONE (cadence, phase) list at
    // compose time (see EngineTask::add_participant). A module therefore belongs to a single list; these
    // fields are owned solely by EngineTask and never touched by the module itself. Storing the link in
    // the module means the participant lists have NO array and NO fixed cap — adding a module can never
    // overflow or be silently dropped.
    friend class EngineTask;
    EngineModule* sched_next_   = nullptr;   // next participant in this module's list (null = tail)
    bool          sched_linked_ = false;     // already threaded into a list — guards a double-add

    // Cadence (set by add_participant from the module's schema cadence_hz). The scheduler runs this module
    // only on frames where (frame_count % period == phase); phase staggers same-rate modules so their work
    // spreads across frames instead of landing together. ttl matches the cadence (see ttl_for).
    uint16_t      sched_period_ = 1;                // run every Nth 1 kHz frame (1 = every frame)
    uint16_t      sched_phase_  = 0;                // frame offset within the period
    uint32_t      sched_ttl_    = TTL_FRAME_MS;     // publish ttl derived from cadence

    // WHAT THIS MODULE COSTS THE FRAME. The name is already handed to add_participant and was thrown
    // away; kept now so the answer to "the frame is 22 % busy doing nothing, which module is that?"
    // is a question the ECU can answer about itself instead of one that needs a debugger and a guess.
    // Cycles, smoothed 1/8 over the frames it actually runs on (a 50 Hz module is not charged for the
    // nineteen frames it sits out). Two DWT reads per participant per frame — a load each.
    const char*   sched_name_   = "?";
    uint32_t      sched_cyc_    = 0;
};
