#pragma once
//
// OutputManager — the OUTPUT-side mirror of the Sensors Input manager, fully RUNTIME-CONFIGURABLE.
// `g_config.outputs.output[i]` IS physical output i (OutputMap.h). This manager drives the rows whose
// function is GENERIC — one pure pipeline (arbitrate_roled -> encode_linear -> emit_sink) each. IGNITION
// and INJECTOR rows belong to the scheduler and are never touched here.
//
// It contains NO hardware logic: the emit sink + its hardware ctx (a SoftPwm channel or a GPIO
// ITimerChannel, the arbiter's channel for the row) are wired here at the composition edge and
// passed anonymously through the stage cfg; the manager just steps the function pointers.
//
// REGION-SCOPED LIVE REBUILD (safety): rebuilding an output pipeline forces its pin Hi-Z (the
// PinArbiter release boundary). So the rebuild is gated STRICTLY on a 'w' write that lands in the
// Outputs config offset region — its JAYECU_SHADOW_OUTPUTS shadow bit — or on the scheduler having
// re-claimed its firing pins (g_firing_bind_generation), which is when a row it let go of becomes
// claimable. It NEVER consults the global g_config_generation, so a write to any other tune cell
// disturbs no output.
//
// PIN ARBITRATION (Hi-Z-safe): each row claims ITS OWN pin through the shared PinArbiter
// (PinOwner::AUX). A row cannot name another pin, so the only way a claim fails is a row whose
// function was just changed away from a coil or injector that the scheduler has not released yet
// (it releases at the next stopped reconfigure) -> the row trips P1650, flags conflict_ and builds NO
// pipeline. recompute_live() releases ALL AUX pins to Hi-Z before re-claiming, so a row changed to
// None always leaves its pin safe.
//
// Build is LAZY (first update(), not init()): the scheduler claims its coils/injectors during
// EnginePositionHal::wire() inside EngineTask::start(), which runs AFTER compose()/init(). Building
// on the first OUTPUT-phase update() guarantees the firing pins are already owned, so an output can
// never win a contested coil/injector pin at boot.
//
#include "../Engine/EngineModule.h"
#include "OutputGate.h"                            // the slot latch + its timings
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "../Pipeline/Pipeline.h"
#include "../Pipeline/OutputStages.h"
#include "EmitSinks.h"                              // pwm_emit_sink / digital_emit_sink + their ctxs
#include "../Scheduler/SoftPwm.h"
#include "../Scheduler/PinArbiter.h"               // claim / release_owner (PinOwner::AUX)
#include "../Scheduler/OutputMap.h"                // OutputFunction — what each row is
#include "../Comms/CommsManager.h"                  // shadow_pending_mask() / clear_shadow()
#include "../Diagnostics/DtcManager.h"             // P1650 raise on pin conflict
#include "../../generated/modules/outputs_config.h" // OutputsConfig / OutputConfig / OUTPUTS_OUTPUT_COUNT
#include "../../generated/shadow_meta.h"            // JAYECU_SHADOW_OUTPUTS

class OutputManager : public EngineModule {
public:
    // Wire the composition edge. Does NOT build — the first update() does the initial build (after
    // the scheduler has claimed its firing pins). PWM rows claim SoftPwm channels from the shared
    // pool (no reserved index); digital rows drive their pin.
    void init(SoftPwm& pwm, uint32_t tps, PinArbiter& arbiter, Comms::CommsManager& comms,
              DtcManager& dtc);

    // OUTPUT phase: region-gated rebuild (Outputs shadow bit) + the first-call build, then run each
    // built output pipeline (bus -> arbitrate -> encode -> emit -> pin).
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    // True iff the last rebuild rejected a pin claim (pin still held by the scheduler).
    [[nodiscard]] bool has_conflict() const noexcept { return conflict_; }

    // Stable address of the conflict flag, for EngineTask's P1650 merge to OR in (read in the SAME
    // engine-task context that writes it — no race). The shared wk::pin_conflict_fault bus bool is
    // re-published by Sensors every INPUT frame (before the merge), so an OUTPUT-phase write to it
    // would be stomped; this probe is the non-stomping channel for the output-side conflict.
    [[nodiscard]] const bool* conflict_ptr() const noexcept { return &conflict_; }

private:
    void recompute_live();
    // The slot's on/off decision for this frame: the two conditions, the latch and the timers.
    // Kept here rather than in a stage because it is the only stateful thing in the output path —
    // the expression VM is pure by design (no timers, no latches), and that is what makes a
    // condition safe to evaluate. So the STATE lives with the manager and the QUESTION with the VM.
    void update_gates(SignalBus& bus, uint32_t now_ms);
    // …and the two NUMERIC answers: the duty expression, and the carrier (table or expression).
    void update_values(SignalBus& bus, uint32_t now_ms);

    static constexpr uint8_t N = OUTPUTS_OUTPUT_COUNT;

    // Composition refs (set in init()).
    SoftPwm*             pwm_     = nullptr;
    PinArbiter*          arbiter_ = nullptr;
    Comms::CommsManager* comms_   = nullptr;
    DtcManager*          dtc_     = nullptr;
    uint32_t             tps_         = 0;

    bool           built_    = false;   // first update() performs the initial build
    uint32_t       bind_gen_ = 0;       // g_firing_bind_generation this build saw
    bool           conflict_ = false;   // last rebuild hit a pin conflict
    uint8_t        n_        = 0;       // number of built (active) pipelines
    pipe::Pipeline pipelines_[N];

    // WHICH ROW each built pipeline came from. The build skips rows that are not Generic or are in
    // conflict, so pipeline i is not output i — and the gate has to read the right row's conditions.
    uint8_t slot_of_[N] = {};

    // One gate per built pipeline — the latch and the timers (OutputGate.h). No condition configured
    // leaves it on, which is what every output did before conditions existed.
    OutputGate gates_[N];

    // Per-output stage-config storage (the pipeline stages point into these).
    struct Pool {
        pipe::RoledCandidate     cand[4];    // the 4 flattened candidate slots, packed live
        pipe::ArbitrateRoledCfg  arb;
        // …or the slot's own duty map / constant, when its value does not come from the bus. The
        // descriptor is held here rather than rebuilt each frame: it points into live config, so it
        // stays correct as cells are tuned and is only re-made when the pipeline is.
        tbl::TableDesc           duty;
        pipe::SourceCfg          src;
        pipe::EncodeCfg          enc;
        pipe::PwmEmitCtx         pwm;
        pipe::DigitalEmitCtx     dig;
        pipe::EmitCfg            emit;
        // What the emit stage last handed the sink, so out_<slot> can say it. Lives in the pool
        // because the pipeline stage holds a pointer to it and the pool is what the pipeline is
        // rebuilt with — a value on the stack would be a dangling write a frame later.
        float                    last_cmd = 0.0f;
        // The slot's two NUMERIC programs, answered each frame beside the conditions: the duty and
        // the carrier. They live here for the same reason last_cmd does — a pipeline stage holds a
        // pointer to them, and the pool is what the pipeline is built against.
        float                    duty_val = 0.0f;
        bool                     duty_ok  = false;
        tbl::TableDesc           freq;               // the carrier surface, when it has one
        // …and whether it HAS one. A slot naming a table id this firmware has no table for is not an
        // error worth stopping for; it is a carrier that cannot be answered, and the fixed frequency
        // is what an unanswerable carrier falls back to.
        bool                     freq_ok  = false;
    } pools_[N];
};
