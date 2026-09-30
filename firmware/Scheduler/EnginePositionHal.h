#pragma once
#include "HardwareAssignment.h"
#include "EventScheduler.h"
#include "SchedulerTypes.h"
#include "GenericTrigger.h"
#include "TriggerLogger.h"
#include <cstdint>

// Bumped every time the scheduler (re)claims its coil and injector pins. OutputManager rebuilds on a
// change, so a row handed from a coil back to a generic output is claimed as soon as the coil lets go.
extern volatile uint32_t g_firing_bind_generation;

class SegmentTimer;   // misfire segment timing — fed the real-tooth stream, defined in SegmentTimer.h

// ---------------------------------------------------------------------------
// EnginePositionHal — top-level orchestrator for the engine position scheduler.
//
// Wires together TriggerDecoder and EventScheduler, owns the event pool
// rebuild logic for all firing modes, and exposes shadow registers that
// FreeRTOS tasks can update at any time without locking.
//
// Parameter update discipline (self-correcting — NO per-cycle rebuild):
//   FreeRTOS tasks call set_*() → writes shadow_[] (volatile scalars, atomic).
//   The persistent event skeleton is built ONCE (start/reconfigure). Each cycle
//     its STATIC schedule nodes come due in the grid ISR and call the compute hook
//     (compute_trampoline → recompute_ignition/injection), which reads the freshest
//     shadow_[] + live RPM and re-arms the VOLATILE fire nodes. No active_[] copy,
//     no pool rebuild — the timing self-corrects from live state every cycle.
//   service() (engine task, fixed rate) only acts on sync-LEVEL transitions and
//     rebuilds the skeleton when a config change requested it.
//
// Dependency injection: all hardware resources arrive via EcuHardwareAssignment.
// The EFI subsystem never consults BoardProfile or SchedulerConfig directly.
//
// Thread/ISR safety:
//   set_*():                               any FreeRTOS task context.
//   start() / stop() / service():          engine task context only.
//   Callbacks registered with decoder/scheduler: high-priority ISR context;
//     they touch only volatile shadow state, never a FreeRTOS primitive.
//   get_*() accessors: task context; volatile fields read atomically.
// ---------------------------------------------------------------------------

class EnginePositionHal {
public:
    // Construct and wire all subsystems.
    // assignment: must remain valid for the lifetime of this object.
    EnginePositionHal(const EcuHardwareAssignment& assignment,
                      const TriggerConfig&         trig,    // g_config.trigger (wheel/sync → decoder)
                      const EngineConfig&          eng,     // g_config.engine (count/firing → scheduler)
                      const OutputsConfig&         outs) noexcept;  // g_config.outputs (which pin fires what)

    // ---- Shadow register setters (any task context) -------------------------
    //
    // All writes are to volatile scalars: a single aligned STR on Cortex-M7.
    // Changes take effect on the next engine cycle (gap tooth boundary).

    // Leading ignition: spark angle and coil charge duration.
    void set_spark_btdc(uint8_t cyl, AngleDeg10 spark_btdc) noexcept;
    void set_dwell_us  (uint8_t cyl, uint32_t   dwell_us)   noexcept;

    // Rotary trailing plug (EngineCycleType::ROTARY only — a cycle, not a coil mode).
    void set_trailing_spark_btdc(uint8_t face, AngleDeg10 spark_btdc) noexcept;
    void set_trailing_dwell_us  (uint8_t face, uint32_t   dwell_us)   noexcept;

    // Injection: opening angle and pulse width.
    void set_inj_open_angle(uint8_t cyl, AngleDeg10 open_btdc) noexcept;
    void set_inj_pw_us     (uint8_t cyl, uint32_t   pw_us)     noexcept;
    // One-shot cold-start prime: fire every active injector simultaneously for pw_us (delegated to
    // the scheduler, fired in ISR on the next grid tooth). HW-agnostic — no pin handling in firmware.
    void prime_injectors   (uint32_t   pw_us)                  noexcept;
    // Asynchronous transient injection: up to num_pulses extra all-injector squirts (pw_us each),
    // spread across the cycle between the sequential events (TransientThrottle's async enrichment).
    void request_async_injection(uint32_t pw_us, uint8_t num_pulses) noexcept;

    // Staged secondary injection — enabled per-cylinder by the fuel calc's duty-based staging,
    // orthogonal to the firing mode. The secondary fires at its own open angle (own table).
    // stage: 0 = stage 2, 1 = stage 3, 2 = stage 4 (see MAX_STAGED_STAGES).
    void set_staged_pw_us    (uint8_t cyl, uint8_t stage, uint32_t pw_us)      noexcept;
    void set_staged_enabled  (uint8_t cyl, uint8_t stage, bool     enabled)    noexcept;
    void set_staged_open_angle(uint8_t cyl, uint8_t stage, AngleDeg10 btdc_x10) noexcept;

    // ---- Lifecycle (engine task context) -----------------------------------

    // Arm capture channels and start scheduling. Call after all set_*() setup.
    void start() noexcept;

    // Disarm all channels and clear the event pool.
    void stop() noexcept;

    // Live re-apply of the current (runtime-resolved) assignment + wheel with
    // NO power cycle: quiesce, re-wire the decoder/scheduler from assignment_,
    // re-read the wheel geometry, and re-arm. Engine task context; call only at
    // a safe boundary (engine stopped / unsynced). The caller updates the
    // resolved assignment_ / wheel objects in place before calling this.
    void reconfigure() noexcept;

    // ---- Periodic service (engine task context) ----------------------------
    // Detects sync-LEVEL transitions (NONE/CRANK/PHASE) and acts on them, and
    // rebuilds the persistent skeleton only when a config change requested it.
    // Steady-state cycles do NO work here — the self-correcting chain runs in the
    // grid ISR. Poll at least once per crank revolution (EngineTask calls it 1kHz).
    void service() noexcept;

    // ---- Virtual-trigger grid input (ISR context) --------------------------
    // The scheduler is driven by the VirtualTrigger's uniform angle grid, not the
    // raw decoder teeth. Each virtual tooth arms events for the next grid span,
    // interpolating with the live velocity. vt_step is the grid pitch (decideg).
    void on_grid_tooth(uint16_t index, AngleDeg10 angle, uint32_t tick,
                       uint32_t velocity_ticks_per_vtooth, AngleDeg10 vt_step) noexcept;

    // ---- Lost-trigger watchdog ---------------------------------------------
    // Compute the two timebase-tick bounds the tooth deadline is clamped between: one crank
    // revolution at a fixed low-RPM floor (the longest silence that can ever be tolerated) and a
    // small absolute minimum so trigger noise cannot trip it. Called from start()/reconfigure() and
    // from the composer. Task context.
    void configure_stall_watchdog() noexcept;

    // THE DECODER'S CLOCK (tooth_alarm ISR). A tooth the decoder predicted did not arrive. Past the
    // overdue threshold this reports a missed tooth; past the floor bound the wheel has stopped or
    // the wire is gone, and it drops sync — get_rpm_x10() -> 0, service() re-narrows the grid and
    // disarms firing, EngineProtection's sync-loss fuel/ign cut latches.
    void on_tooth_deadline() noexcept;

    // Drop the decoder to NONE so the normal sync-loss path takes over. ISR or task context.
    void on_trigger_lost() noexcept;

    // ---- State accessors (task context) ------------------------------------
    [[nodiscard]] DecoderTelemetry get_decoder_telemetry() const noexcept;
    [[nodiscard]] VvtResult        get_vvt_result(uint8_t cam_index) const noexcept;
    [[nodiscard]] SyncLevel        get_sync_level()    const noexcept;
    [[nodiscard]] uint32_t         get_rpm_x10()       const noexcept;
    // Output-pin mis-assignment in the current tune (config-time indicator).
    [[nodiscard]] bool             pin_conflict()      const noexcept { return scheduler_.pin_conflict(); }

    // Engine-cycle capture. The HAL owns the cycle BOUNDARY and the trigger lane (it is the
    // position authority); the scheduler records the output lanes into the same recorder. One
    // call wires both, so the two halves of a capture can never be pointed at different buffers.
    void assign_cycle_recorder(CycleRecorder* r) noexcept {
        recorder_ = r;
        scheduler_.assign_cycle_recorder(r);
    }

    // The raw trigger log — time-domain edges for a wheel the decoder does not understand. Only the
    // HAL wires it: the edges are logged from the capture ISR before the decoder is handed them, so
    // the scheduler has nothing to say about it (there is no schedule when there is no sync).
    void assign_trigger_logger(TriggerLogger* l) noexcept { trig_log_ = l; }
    // SEGMENT TIMING — the misfire signal. Fed the SAME real-tooth stream the PLL gets, upstream of
    // the filter whose job is to smooth exactly this away. Configured from the live wheel + cylinder
    // layout on start(); null = the feature is simply absent and costs one branch per tooth.
    void assign_segment_timer(SegmentTimer* t) noexcept { seg_timer_ = t; }

    // TRIGGER-LOG MODE: the decoder is not fed while this is on. See on_gen_edge for why it is an
    // absence rather than an output mask. Leaving it drops the decoder's sync so it re-acquires from
    // the next real teeth instead of resuming on an angle that stopped being true when we stopped
    // feeding it — the same path the lost-trigger watchdog takes.
    // Is the crank TURNING, independent of whether the decoder understands it? Answered from raw
    // capture edges, which arrive whatever the decoder is doing — including while it is switched off
    // for a trigger log. rpm cannot answer this: it comes from the decoder, and an unsynced decoder
    // reports 0 for "I do not know" exactly as it does for "stopped".
    [[nodiscard]] bool edges_within(uint32_t ms) const noexcept {
        const uint32_t hz = timebase_hz();
        if (!hz || !last_edge_tick_valid_) return false;
        const uint32_t age = timebase_now() - last_edge_tick_;   // unsigned: wraps correctly
        return age < static_cast<uint32_t>((static_cast<uint64_t>(hz) * ms) / 1000ull);
    }

    // How long the decoder has been trying to re-acquire since it was last reset, in timebase ticks.
    // A reset makes rpm 0 and the run state STOPPED, and that zero means UNKNOWN until it either
    // syncs or the teeth stop — telling the two apart is the whole point of this.
    [[nodiscard]] uint32_t ticks_since_decoder_reset() const noexcept {
        return timebase_now() - decoder_reset_tick_;
    }

    // The capture timebase, for arming the log: edges are stamped in these ticks, so the log is
    // measured against the same clock that timestamps them and no second clock can drift from it.
    [[nodiscard]] uint32_t timebase_now() const noexcept {
        return assignment_.timebase ? assignment_.timebase->get_current_ticks() : 0u;
    }
    [[nodiscard]] uint32_t timebase_hz() const noexcept {
        return assignment_.timebase ? assignment_.timebase->get_ticks_per_second() : 0u;
    }

    // Master firing gate (system-active / key-on). Closed (false) -> no spark/injection fires even
    // when synced — so a bench trigger on USB power can't drive the coils/injectors. Opening it
    // restores firing if currently synced; the sync-change path also honours it. Called each frame.
    void set_firing_gate(bool open) noexcept;

    // --- Sync-epoch firing arm: the "no fire without a fresh compute" interlock ---
    // Firing requires not just key-on + sync, but a schedule computed for the CURRENT sync epoch.
    // Every sync transition bumps the epoch (disarming). The per-cycle task snapshots firing_epoch()
    // before it computes, and arm_firing(epoch) re-enables firing only if that epoch is still live —
    // so a schedule computed for a stale epoch (e.g. a sync drop mid-compute) can never fire, at any
    // RPM. This is the clock-free guarantee that a fire can't precede its own computation.
    [[nodiscard]] uint32_t firing_epoch() const noexcept { return sync_epoch_; }
    void arm_firing(uint32_t epoch) noexcept;
    // OUTPUT CUTS. A cut stops the OUTPUT — it does not alter what was commanded. Clearing a
    // channel's execution bit suppresses IGN_DWELL_START / INJ_OPEN, so the coil never charges and
    // the injector never opens, while advance and pulse width go on reporting what the tables asked
    // for. A cut asserted between dwell-start and spark still fires that one coil: the charge is
    // already in it.
    //
    // Both cuts USED TO BE FAKED in the modules — Ignition set advance to 0 and FuelCalculator set
    // pulse width to 0. Zero advance is not "no spark", it is a spark at TDC, so hitting the rev
    // limiter dumped every plug at TDC instead of cutting; and zero pulse width told the tuner the
    // model had asked for no fuel when it had asked for plenty. This mechanism existed the whole
    // time (EventScheduler::set_execution_mask) and nothing ever called it.
    void set_output_cuts(bool ign_cut, bool inj_cut) noexcept {
        scheduler_.set_output_cuts(ign_cut, inj_cut);
    }
    // Per-cylinder protective cut (bit c = cut cylinder c) — pre-ignition's response, which is a CUT
    // and never a retard: the spark is not what lit the charge, so retarding does not address the
    // cause. Composes with the global cuts rather than overwriting them; see EventScheduler for the
    // wasted-spark caveat about a shared coil.
    void set_cylinder_cuts(uint16_t cyl_mask) noexcept { scheduler_.set_cylinder_cuts(cyl_mask); }
    void set_cylinder_cuts(uint16_t fuel_mask, uint16_t spark_mask) noexcept {
        scheduler_.set_cylinder_cuts(fuel_mask, spark_mask); }
    [[nodiscard]] uint16_t cylinder_cuts() const noexcept { return scheduler_.cylinder_cuts(); }

    // Debug readout (the 'fire' CLI): is firing actually enabled, and the epoch arm state.
    [[nodiscard]] bool     firing_enabled() const noexcept { return scheduler_.firing_enabled(); }
    [[nodiscard]] uint32_t armed_epoch()    const noexcept { return armed_epoch_; }
    [[nodiscard]] bool     firing_gate()     const noexcept { return firing_gate_; }
    // Is the current tune's firing order a valid permutation? False -> firing is gated off (see
    // update_firing_enable). Recomputed each reconfigure; the studio reads it via the firing_order_fault
    // telemetry bit EngineTask publishes from it.
    [[nodiscard]] bool     firing_order_valid() const noexcept { return firing_order_valid_; }
    // The APPLIED rows' unserved firing cylinders (bit c = cylinder slot c) — see EventScheduler.
    [[nodiscard]] uint16_t cylinders_without_coil() const noexcept     { return scheduler_.cylinders_without_coil(); }
    [[nodiscard]] uint16_t cylinders_without_injector() const noexcept { return scheduler_.cylinders_without_injector(); }
    // Do the LIVE output rows bind differently from what the scheduler applied? True after a row's
    // function, cylinder, stage, plug or polarity was edited — the save task reconfigures on it once the
    // engine is stopped, the same boundary every other firing change waits for.
    [[nodiscard]] bool     output_map_pending() const noexcept {
        return output_map_from(outs_src_.output, OUTPUTS_OUTPUT_COUNT) != scheduler_.output_map();
    }
    // Read-only view of the firing layer, so a test can ask the question that matters after a bad
    // tooth — is anything still allowed to fire — without going through the task loop that would
    // mask the answer by fixing it.
    [[nodiscard]] const EventScheduler& scheduler() const noexcept { return scheduler_; }
    // Mutable access for tests that must assert on the INTERACTION between sync state and the
    // protective cuts — raising a cut is the caller's job, not this class's, so a test has to be
    // able to raise one from outside.
    [[nodiscard]] EventScheduler& scheduler() noexcept { return scheduler_; }
    // The highest sync level THIS CONFIGURATION can ever reach, decided from the tune at every
    // (re)configure. NONE means no stream in it can fix position — the engine could never know where
    // it is, so firing is refused outright rather than waiting for a sync that cannot arrive.
    [[nodiscard]] SyncLevel config_sync_ceiling() const noexcept { return config_ceiling_; }
    [[nodiscard]] uint8_t   config_trigger_fault() const noexcept { return config_fault_; }

    // --- Knock sampling window (A1) ---
    // Enable + crank angle (decidegrees ATDC) of the per-cylinder ADC knock burst window. When enabled,
    // the compute hook arms it each cycle at TDC + start_atdc; the window's dispatch deposits the fired
    // cylinder into the ISR->worker mailbox. Set from the Knock config (enabled && source==onboard).
    void set_knock_window(bool enabled, AngleDeg10 start_atdc) noexcept {
        knock_window_en_ = enabled; knock_window_start_ = start_atdc;
    }
    // Drain one fired-window cylinder for the knock worker. Returns false when empty. Single consumer
    // (worker) against the single-producer ISR deposit — lock-free SPSC.
    [[nodiscard]] bool pop_knock_cyl(uint8_t& cyl) noexcept;
    // Total knock windows dispatched since boot — bench diagnostic ('fire' CLI): confirms the
    // per-cylinder angle scheduling is live before the ADC burst exists (Stage C).
    [[nodiscard]] uint32_t knock_window_fires() const noexcept { return knock_window_fires_; }

    // TDC-relative angle -> absolute crank angle. TWO helpers, not one, because the two senses are
    // opposite and a single misnamed converter is exactly how spark ended up scheduled on the wrong
    // side of TDC: the angle axis INCREASES with rotation, so BTDC subtracts and ATDC adds.
    //
    //   angle_btdc  spark advance, injection firing angle — a POSITIVE value is BEFORE TDC
    //   angle_atdc  the knock sampling window — a POSITIVE value is AFTER TDC
    //
    // The sense is in the NAME at every call site, so picking the wrong one has to be visible.
    // `cycle` is the engine cycle span — 720 deg four-stroke, 360 two-stroke. Not defaulted: an
    // angle wrapped into the wrong cycle is a silently mis-scheduled event.
    [[nodiscard]] static AngleDeg10 angle_btdc(AngleDeg10 tdc, AngleDeg10 btdc,
                                               AngleDeg10 cycle) noexcept;
    [[nodiscard]] static AngleDeg10 angle_atdc(AngleDeg10 tdc, AngleDeg10 atdc,
                                               AngleDeg10 cycle) noexcept;

    // Trigger reference -> ENGINE angle. The decoder's zero is the first tooth after SYNC — a feature
    // of the WHEEL, which has no idea where TDC #1 is. `trigger_offset_btdc` is the one place that
    // relationship is stated, and it is quoted the way every angle in this firmware is quoted:
    //
    //     POSITIVE = the reference tooth occurs that many degrees BTDC #1  (a 2JZ 36-2 is 155)
    //     NEGATIVE = it occurs ATDC
    //
    // So TDC #1 arrives `offset` after the reference — it sits at decoder angle +offset — and the
    // engine angle is the decoder angle MINUS the offset.
    //
    // It is a POSITION DECLARATION, not an advance control. Turning it up tells the ECU that TDC is
    // further away than it thought, so events fire later. That is correct and is not a sign error:
    // "+155 BTDC" describes where the wheel sits. The convention is NOT universal across engine
    // management in general -- the same relationship is quoted the other way round, ATDC of the
    // first tooth after the gap, just as often -- so it is stated here rather than inferred from
    // the field name.
    //
    // `cycle` is the span the caller is working in: 3600 at CRANK sync, 7200 at PHASE. Wrapping into
    // the live span (rather than always 720) keeps the CRANK-time grid, which only knows 360, from
    // being handed an angle it cannot place.
    [[nodiscard]] static AngleDeg10 engine_angle(AngleDeg10 decoder_angle,
                                                 AngleDeg10 offset_btdc,
                                                 AngleDeg10 cycle) noexcept;

private:
    // ---- Generic-trigger path ----------------------------------------------
    // One capture trampoline per stream (ud = &gen_ctx_[i]); routes the edge to
    // gtrig_.on_edge() then feeds the VirtualTrigger when the fine source advanced.
    static void gen_edge_trampoline(uint32_t tick, void* user_data) noexcept;
    void on_gen_edge(uint8_t stream_idx, uint32_t tick) noexcept;

    // Compute hook: the grid ISR calls this when a STATIC schedule node comes due.
    // Recomputes the cylinder's live spark/dwell or injector angles from the
    // shadow registers + RPM and re-arms the VOLATILE fire nodes. ISR context.
    static void compute_trampoline(void* ud, uint8_t index, EventAction kind) noexcept;
    void recompute_ignition(uint8_t cyl) noexcept;    // IGN_SCHEDULE  → arm spark(s) + knock window
    void recompute_dwell(uint8_t cyl) noexcept;       // spark discharge → arm next dwell(s)
    void recompute_injection(uint8_t event) noexcept; // INJ_SCHEDULE  → arm one injection EVENT
    void on_knock_window(uint8_t cyl) noexcept;      // ADC_TRIGGER   → deposit fired cyl (ISR)

    // ---- Channel wiring (engine task context) ------------------------------
    // Force outputs low, then bind decoder/scheduler channels from assignment_
    // and register callbacks. Idempotent — safe to re-run on start/reconfigure.
    // boot selects the module config-shadow pull: FULL on the first start (after the
    // flashed tune is loaded), engine_stop-only on a later reconfigure.
    void wire_from_assignment(bool boot = true) noexcept;

    // ---- Sync-level transitions (engine task context) ----------------------
    // Drive the scheduler's phase gate / firing enable, quiesce on downgrade,
    // and widen/narrow the grid (3600/36 ⇄ 7200/72) across the CRANK⇄PHASE edge.
    void handle_sync_level_change(SyncLevel from, SyncLevel to) noexcept;
    // Recompute scheduler firing-enable from (key gate AND armed-this-epoch AND synced). Called on
    // every input to that condition: key gate, sync transition, arm.
    void update_firing_enable() noexcept;

    // Dwell lead in crank decidegrees from live RPM: rpm·dwell_us/16666, clamped
    // so the dwell-start cannot precede the STATIC schedule node's lead.
    [[nodiscard]] uint32_t dwell_angle_dd(uint32_t rpm_x10, uint32_t dwell_us) const noexcept;
public:
    // The dwell window clamp (shared-coil aware), pure for testing — see the .cpp.
    [[nodiscard]] static uint32_t clamp_dwell_dd(uint32_t dd, IgnitionCoilMode mode, AngleDeg10 cycle, uint8_t ncyl,
                                                 bool phase_known, bool rotary) noexcept;
private:
    // Pulse width (µs) → engine angle (decidegrees) at live RPM, for the end-of-injection open offset.
    // Same rate as dwell_angle_dd but clamped to the cycle span, not the dwell window.
    [[nodiscard]] AngleDeg10 pw_angle_dd(uint32_t rpm_x10, uint32_t pw_us) const noexcept;

    // ---- Data members ------------------------------------------------------
    const EcuHardwareAssignment& assignment_;

    // The config SHADOW lives with each module (the safe pattern): the decoder keeps
    // its own TriggerConfig shadow, the scheduler its own CylindersConfig + cylinder
    // count — each refreshed from the live tune ONLY at start()/reconfigure()
    // (engine-stopped). The HAL holds just the live g_config references it passes to
    // them at construction (decoder_) and at every reconfigure (scheduler_.set_config).
    // It reads firing geometry through the scheduler (single owner) — never live
    // g_config in the hot path. A 'w' to g_config can't perturb a spinning engine.
    const TriggerConfig& trig_src_;   // g_config.trigger (live source → generic stream config)
    const EngineConfig&  eng_src_;    // g_config.engine  (live source → scheduler shadow)
    const OutputsConfig& outs_src_;   // g_config.outputs (live source → scheduler output map)

    EventScheduler               scheduler_; // owns its CylindersConfig shadow + cyl count
    bool                         firing_gate_ = false;  // system-active/key-on master firing gate
    bool                         firing_order_valid_ = false;  // firing_order a valid permutation; else refuse to fire
    SyncLevel                    config_ceiling_ = SyncLevel::NONE;  // best this tune can ever reach
    uint8_t                      config_fault_   = 0;                // TriggerConfigFault, why not
    volatile uint32_t            sync_epoch_  = 0;            // bumped on every sync-level transition
    volatile uint32_t            armed_epoch_ = 0xFFFFFFFFu;  // epoch the last per-cycle commit armed (!=0 -> disarmed at boot)

    // The generic trigger decoder. Built from the stream config at wire_from_assignment();
    // fed by gen_edge_trampoline per capture edge → fusion → VirtualTrigger feed.
    GenericTrigger               gtrig_;
    struct GenCtx { EnginePositionHal* self; uint8_t idx; };
    GenCtx                       gen_ctx_[MAX_STREAMS];
    ICaptureChannel*             gen_ch_[MAX_STREAMS] = {};   // per-stream capture channel (polarity)
    // Per-stream cam slot (0..MAX_CAM_CHANNELS-1) for cam-rate streams, 0xFF otherwise.
    // Set in wire_from_assignment alongside the channel binding; lets on_gen_edge know
    // which VVT result a cam stream's measured angle belongs to.
    uint8_t                      gen_cam_index_[MAX_STREAMS] = {};

    // Shadow: written by FreeRTOS tasks, read live by the compute hook (ISR) when
    // a STATIC schedule node re-arms its cylinder. No active_[] snapshot — the
    // self-correcting chain reads the freshest shadow value at the moment of use.
    CylinderShadowParams shadow_[MAX_CYLINDERS];

    volatile VvtResult vvt_results_[MAX_CAM_CHANNELS];

    // Knock sampling window + the ISR->worker mailbox (SPSC ring of fired cylinders). Producer is the
    // ADC_TRIGGER dispatch (compute hook, ISR); consumer is the knock worker via pop_knock_cyl.
    bool                     knock_window_en_    = false;
    AngleDeg10               knock_window_start_ = 0;      // decidegrees ATDC
    static constexpr uint8_t KNOCK_RING          = 8;      // pow2; drops on overflow (worker behind)
    volatile uint8_t         knock_ring_[KNOCK_RING] = {};
    volatile uint8_t         knock_head_ = 0;              // producer (ISR)
    volatile uint8_t         knock_tail_ = 0;              // consumer (worker)
    volatile uint32_t        knock_window_fires_ = 0;      // total windows dispatched (bench diagnostic)

    // Last sync level service() acted on, to detect NONE/CRANK/PHASE transitions.
    SyncLevel last_level_;

    // Optional one-cycle capture (null = feature absent, one pointer test per trigger edge).
    CycleRecorder* recorder_ = nullptr;
    SegmentTimer*  seg_timer_ = nullptr;
    TriggerLogger* trig_log_ = nullptr;
    // Raw edge liveness, written in the capture ISR BEFORE the decoder is (or is not) fed, so it
    // survives the decoder being switched off. This is the only thing that can distinguish a crank
    // that is genuinely stopped from one the decoder simply has not resolved yet.
    volatile uint32_t last_edge_tick_ = 0;
    volatile bool     last_edge_tick_valid_ = false;
    volatile uint32_t decoder_reset_tick_ = 0;

    // WHEN THE LAST POSITION-CARRYING TOOTH ARRIVED — the fine stream's, not just any stream's.
    // last_edge_tick_ above is stamped for EVERY enabled stream, which is why it cannot answer this:
    // a cam-rate stream, or a noisy pin nobody is decoding, keeps it fresh while the crank is dead.
    // Measured on a rig: 20 edges/s on an unconnected cam input held "the wheel is turning" true
    // with no crank signal at all. The tooth deadline times THIS one.
    //
    // Same clock at both ends: stamped with the capture tick (TIM5->CNT) and compared against the
    // alarm's now() (also TIM5->CNT). The old watchdog got this wrong once — it stamped with the
    // capture tick and compared against a different timer's count, and the unsigned wrap fired it
    // every tooth. If a board ever puts capture and alarm on different timers, this breaks and must
    // be revisited; ITimerChannel/IAlarmTimer permit it, jaytek_v1 does not do it.
    volatile uint32_t last_fine_tick_ = 0;
    volatile bool     last_fine_tick_valid_ = false;
    // Deadline bounds in timebase ticks (see configure_stall_watchdog). floor = one crank rev at the
    // low-RPM floor: the longest silence ever tolerated, whatever the wheel. 0 = watchdog disabled.
    uint32_t stall_floor_ticks_ = 0;
    uint32_t stall_min_ticks_   = 0;

    // Set when the deadline has already reported this silence, so a stopped engine reports ONE
    // missed tooth and then one absent-signal, rather than a code per virtual tooth for ever.
    volatile bool tooth_overdue_reported_ = false;
    // The key as service() last saw it. Starts ON so an ECU that boots with the key off releases the
    // firing pins its boot wiring claimed on the very first frame.
    bool key_was_on_ = true;
    // Latched by the deadline when it declares the wheel stopped; cleared by the next position
    // tooth. This is the one trigger-health signal that is TRUE while nothing is happening, which is
    // exactly what the fault table had no way to express before.
    volatile bool trigger_absent_ = false;
    // Was the engine RUNNING (at or above the Cranking Threshold) when the last tooth went missing?
    // Decides whether that loss is a fault or just the engine stopping (see on_tooth_deadline).
    volatile bool lost_at_speed_  = false;

    // Arm the deadline for the next tooth this stream predicts; disarm when there is no position to
    // lose. Both cheap enough for the capture ISR (one CCR write).
    void arm_tooth_deadline(uint32_t from_tick) noexcept;
    void disarm_tooth_deadline() noexcept;
    static void tooth_deadline_trampoline(void* ud) noexcept;

    // Set on CRANK→PHASE promotion; the next cycle-start ISR re-enables firing
    // after the grid has re-anchored to the full 720° angle (one suppressed cycle).
    volatile bool promotion_pending_;

    // Grid pitch (decideg) of the VirtualTrigger feeding the scheduler; 0 until
    // the first virtual tooth. When set, RPM is derived from the grid basis
    // (3600/vt_step virtual teeth per crank rev) rather than the real wheel.
    volatile AngleDeg10 grid_vt_step_;

    // Trigger offset, refreshed from the live tune by service() and read in the capture ISR.
    //
    // Deliberately NOT part of the start()/reconfigure() config shadow that every other decoder
    // setting uses. Base timing is set with a TIMING LIGHT on a running engine — a setting you can
    // only apply by stopping the engine is a setting you cannot use for the job it exists for. It is
    // a single aligned 16-bit scalar, so an ISR read cannot tear, and a change simply takes effect on
    // the next tooth. That is the whole exception, and it is the reason for it.
    volatile AngleDeg10 trig_offset_btdc_ = 0;
    // Engine-cycle capture: the crank wheel's teeth per revolution, from the tune, and the running
    // count since the last cycle opened. Both touched only by the capture ISR, which cannot preempt
    // itself — see on_gen_edge for why the boundary is counted rather than read.
    uint16_t teeth_per_rev_   = 0;   // present teeth in ONE PERIOD of the crank pattern
    uint8_t  crank_repeats_   = 2;   // how many of those periods make an engine cycle
    uint16_t teeth_since_mark_ = 0;
    // Previous COUNTED angle from the decoder, for the engine-cycle capture's cycle boundary. Written
    // and read only by the capture ISR (prio 1), which cannot preempt itself — so it needs no
    // synchronisation and, unlike a flag shared with the grid ISR, nothing can race it.
    AngleDeg10 last_fine_angle_ = 0;

    // Live PLL ticks-per-virtual-tooth, mirrored from on_grid_tooth. The SINGLE
    // velocity source: RPM (and thus dwell_angle) reads this — the same value the
    // scheduler's fire-tick interpolation uses. 0 until the first virtual tooth.
    volatile uint32_t grid_tpv_;

    bool running_;
    // Cleared until the first start() AFTER the flashed tune is loaded; that start does
    // the FULL shadow pull (incl. reboot-flagged fields). Subsequent starts (reconfigure)
    // do the engine_stop-only runtime pull, so reboot fields hold their boot value.
    bool primed_ = false;
};
