#pragma once
#include "ITimerChannel.h"
#include "SchedulerTypes.h"
#include "PinArbiter.h"
#include "OutputMap.h"
#include "CycleRecorder.h"
#include "../../generated/shadow_meta.h"   // EngineConfig + EngineShadow + pull (codegen-owned)
#include <cstdint>

// Refresh each firing cylinder's TDC angle INTO config (engine.cyl[].tdc_angle). Even-fire only: the ECU
// owns the angle and computes it as interval × (firing position − 1), interval = cycle / cylinders, clearing
// the unused tail; odd-fire is left as the tuner's editable value. Callers MUST run this before applying the
// config to the scheduler, because resolve_binding FETCHES tdc_angle rather than recomputing it. Shared so
// the firmware (EnginePositionHal::start) and the host tests populate the angle the same one way.
void update_cylinder_angles(EngineConfig& e) noexcept;

// Is the firing order a valid permutation for this cylinder count? True iff firing_order[0..n-1] holds
// each cylinder 1..n exactly once (n = cylinder_count). A duplicate double-schedules one cylinder and
// silently drops the missing one, so the scheduler must REFUSE to fire on a false result. The unused
// tail (>= n) is not examined — the firmware only reads [0..n). Same test in even- and odd-fire, since
// resolve_binding inverts firing_order in both.
bool firing_order_valid(const EngineConfig& e) noexcept;

// ---------------------------------------------------------------------------
// EventScheduler — self-correcting, persistent intrusive-list scheduler.
//
// A skeleton of SchedNodes is built ONCE per reconfiguration (build_skeleton,
// task context) and re-arms itself every cycle in ISR context — no per-cycle
// rebuild. Nodes live in a statically-partitioned pool; none are heap-allocated.
//
//   STATIC nodes  (IGN_SCHEDULE / INJ_SCHEDULE) sit permanently in their bucket
//     and call the compute hook so the owner recomputes live spark/dwell/PW.
//   VOLATILE nodes (IGN_DWELL_START / IGN_SPARK / INJ_OPEN) are one-shots: they
//     drive their output mask and unlink; the STATIC node re-inserts them.
//
// FIRING (firing-layer rework, docs/firing-layer-two-timer-plan.md): the
// scheduler OWNS the order and drives two dumb "call me at tick X" alarms —
// there is no per-channel deadline and no soonest-scan:
//   * Timer 1 (ANGLE) — on each virtual tooth, walk the tooth's sorted bucket(s)
//     in (sub_offset) order, arming the angle alarm for each event's interpolated
//     tick; the callback drives the event's mask NOW via force_output_now. At
//     CRANK the two half-buckets (index, index+buckets_per_rev_) are merged
//     (both already sorted) with a two-pointer walk.
//   * Timer 2 (TIME) — injector close (open+pw) goes on a small sorted time list;
//     the time alarm fires the head and advances.
// Outputs are immediate sinks (force_output_now); the alarm IS the timing.
//
// All list mutation + dispatch run in the grid/match ISR (TIM5, prio 2), which
// cannot preempt itself; the capture ISR (prio 1) only feeds the PLL. No lock.
// ---------------------------------------------------------------------------

class EventScheduler {
public:
    // Events the tooth flush fired late rather than lose (see flush_angle_walk). Should sit near zero.
    [[nodiscard]] uint32_t late_fired() const noexcept { return late_fired_; }
    // Injections skipped because no close slot was free (an open with no close is a stuck injector).
    [[nodiscard]] uint32_t inj_no_close() const noexcept { return inj_no_close_; }
    // Coils released by the maximum-dwell cutoff; dwells fit_dwell had to start at once.
    [[nodiscard]] uint32_t overdwell_count() const noexcept { return overdwell_; }
    [[nodiscard]] uint32_t dwell_started_late() const noexcept { return dwell_started_late_; }
    // Longest a coil may stay charged; past it the tooth check releases it. 0 = off.
    void set_max_dwell_us(uint32_t us) noexcept;
    // IGN_SCHEDULE: move / arm / start the dwell so it precedes the spark just armed (see .cpp).
    void fit_dwell(uint8_t cyl, AngleDeg10 dwell_start, bool trailing) noexcept;
    EventScheduler() noexcept;

    // ---- Hardware assignment (before build_skeleton) ------------------------
    void assign_ign_channel(uint8_t idx, ITimerChannel& ch) noexcept;
    void assign_inj_channel(uint8_t idx, ITimerChannel& ch) noexcept;
    void assign_timebase(const ITimerChannel& tb) noexcept;
    void assign_angle_alarm(IAlarmTimer& a) noexcept;   // Timer 1
    void assign_time_alarm(IAlarmTimer& a) noexcept;     // Timer 2
    void assign_pin_arbiter(PinArbiter& a) noexcept { arbiter_ = &a; }
    // Optional engine-cycle capture. When set AND armed, every output edge this scheduler
    // drives is recorded with the angle it was driven at. Null (the default) costs one
    // pointer test per edge, which is why it is a pointer and not a compile-time option:
    // the capture has to be available on a shipped ECU, not only a debug build.
    void assign_cycle_recorder(CycleRecorder* r) noexcept { recorder_ = r; }

    // The angle the firing clock BELIEVED it was at, at an arbitrary tick — interpolated inside the
    // current virtual tooth. Public because the capture ISR needs it to stamp a real trigger tooth:
    // stamping teeth with the decoder's own counted position instead makes the tooth lane and the
    // grid lane the same number by construction, so their difference is always zero and the PLL
    // error stays invisible. Read against the grid, a tooth that arrives early or late shows it.
    //
    // SAFE FROM A HIGHER-PRIORITY ISR. The three grid fields are written together by
    // on_virtual_tooth (TIM5, prio 2) and read here from the capture ISR (prio 1), which preempts
    // it — so a naive read can pair a fresh tick with a stale angle and invent a one-tooth error.
    // A sequence counter makes a torn read detectable and retried.
    [[nodiscard]] AngleDeg10 angle_at(uint32_t tick) const noexcept;
    // Refresh this module's config shadow from the live Engine tune via the
    // codegen-owned pull (value copy of the small structural config — no tables), and take a value
    // copy of the output rows' firing half (which pin is which cylinder's coil or injector).
    // Called only at start()/reconfigure() (engine-stopped). boot=true (first start)
    // does a FULL pull; boot=false (reconfigure) refreshes engine_stop fields only,
    // so reboot-flagged fields keep their boot value until the next power cycle.
    void set_config(const EngineConfig& eng, const OutputMap& map, bool boot = true) noexcept;
    // The output map this scheduler is bound to — what the last set_config applied.
    [[nodiscard]] const OutputMap& output_map() const noexcept { return map_; }
    // What the rows resolved to, per cylinder slot (0-based): the channels a cylinder's leading coil,
    // trailing coil (rotary) and stage-s injector fire. Read-only, for tests and diagnostics.
    [[nodiscard]] OutputMask coil_mask(uint8_t c) const noexcept {
        return c < MAX_CYLINDERS ? resolved_ign_[c] : OutputMask(0);
    }
    [[nodiscard]] OutputMask trailing_coil_mask(uint8_t c) const noexcept {
        return c < MAX_CYLINDERS ? resolved_ign_trail_[c] : OutputMask(0);
    }
    [[nodiscard]] OutputMask injector_mask(uint8_t stage, uint8_t c) const noexcept {
        return (stage < MAX_INJ_STAGES && c < MAX_CYLINDERS) ? resolved_inj_[stage][c] : OutputMask(0);
    }
    // THE CYLINDERS THE ROWS LEAVE UNSERVED, as resolved (bit c = cylinder slot c, firing cylinders only):
    // no leading coil — companions included, so a wasted-spark cylinder served by its pair's coil counts
    // as served — and no injector in stage 1. The firmware assigns nothing to fill them; EngineTask
    // raises P1653 / P1654 on them.
    [[nodiscard]] uint16_t cylinders_without_coil() const noexcept     { return no_coil_mask_; }
    [[nodiscard]] uint16_t cylinders_without_injector() const noexcept { return no_inj_mask_; }

    // Firing-config accessors over the shadow (the HAL reads these — single owner).
    [[nodiscard]] uint8_t          cylinder_count() const noexcept { return cyl_cfg_.cylinder_count; }
    [[nodiscard]] IgnitionCoilMode ign_mode() const noexcept { return static_cast<IgnitionCoilMode>(cyl_cfg_.ign_mode); }
    // RESOLVED TDC — computed by resolve_binding() from the firing order (even spacing) or the odd-fire
    // override, NOT stored in the tune. This is the readback studio shows greyed for even-fire.
    [[nodiscard]] AngleDeg10       tdc_angle(uint8_t c) const noexcept {
        return (c < MAX_CYLINDERS) ? resolved_tdc_[c] : 0;
    }
    // A TRAILING PLUG IS A ROTARY. A Wankel has two plugs per rotor because of how it burns, whatever
    // the coils are wired as — so the trailing nodes exist for the cycle type, and which coils they
    // drive is the output rows' business (a row with function Ignition and plug Trailing).
    [[nodiscard]] bool             is_rotary() const noexcept {
        return static_cast<EngineCycleType>(cyl_cfg_.cycle_type) == EngineCycleType::ROTARY;
    }
    // The engine cycle span: 720 deg for a four-stroke, 360 for a two-stroke (every cylinder fires
    // every revolution). The scheduler owns the firing geometry, so it owns this too, and the HAL
    // reads it here rather than keeping a second copy that could disagree.
    [[nodiscard]] AngleDeg10       cycle_angle() const noexcept { return engine_cycle_angle(cyl_cfg_.cycle_type); }
    // Injection is now PER STAGE (engine.inj_stage[]); the scheduler's angle events come from the
    // primary stage (index 0). These read that stage's mode + squirt count.
    [[nodiscard]] InjectionMode    inj_mode() const noexcept {
        return static_cast<InjectionMode>(cyl_cfg_.inj_stage[0].mode);
    }
    // Injector Timing Method: true = the firing angle is the END of the pulse (the default), so
    // the HAL opens a pulse-width of angle earlier; false = the angle is the pulse's START (open).
    [[nodiscard]] bool             inj_end_of_injection() const noexcept {
        return cyl_cfg_.injector_timing_method == 0;
    }
    // The compiled injection schedule (see InjEvent). The HAL walks these to turn a due INJ_SCHEDULE
    // into an opening angle and a pulse width.
    [[nodiscard]] uint8_t          inj_event_count() const noexcept { return inj_event_count_; }
    [[nodiscard]] uint8_t          inj_event_ref_cyl(uint8_t e) const noexcept {
        return (e < inj_event_count_) ? inj_events_[e].ref_cyl : 0;
    }
    [[nodiscard]] AngleDeg10       inj_event_base_angle(uint8_t e) const noexcept {
        return (e < inj_event_count_) ? inj_events_[e].base_angle : 0;
    }
    // Which injection stage this event belongs to (0 = primary inj_stage[0]; 1..MAX_STAGED_STAGES =
    // staged stage 2..4). The compute hook looks up the pulse width from this + the reference cylinder.
    [[nodiscard]] uint8_t          inj_event_stage(uint8_t e) const noexcept {
        return (e < inj_event_count_) ? inj_events_[e].stage : 0;
    }

    // Claim (from the arbiter) ONLY the IGN/LS pins the output rows bind to firing cylinders
    // (and, for injectors, active stages), and bring each out of Hi-Z to its de-energized idle
    // level. Every other pin is left unclaimed → Hi-Z, free for generic outputs. Releases the
    // scheduler's previously-held pins first, so a reconfigure that drops a pin
    // returns it to the pool. Call after set_config(); needs the arbiter.
    void claim_outputs() noexcept;

    // ---- Compute hook (ISR, from a STATIC schedule node) --------------------
    // `index` is indexed BY THE ACTION, because the two schedules are indexed differently:
    //   IGN_SCHEDULE / IGN_DWELL_START / ADC_TRIGGER — a cylinder index.
    //   INJ_SCHEDULE                                 — an injection EVENT index (see InjEvent).
    // They coincided while every injection event belonged to exactly one cylinder; batch and
    // multi-point break that, so the hook says which space it is handing over.
    using ComputeHook = void (*)(void* ud, uint8_t index, EventAction kind) noexcept;
    void register_compute_hook(ComputeHook cb, void* ud) noexcept;

    // ---- Skeleton build (task context, reconfiguration only) ----------------
    void build_skeleton(AngleDeg10 vt_step) noexcept;

    // ---- Re-arm hooks (ISR, from the compute hook) --------------------------
    void arm_spark          (uint8_t cyl, AngleDeg10 spark_angle) noexcept;
    void arm_spark_trailing (uint8_t cyl, AngleDeg10 spark_angle) noexcept;
    void arm_dwell          (uint8_t cyl, AngleDeg10 dwell_start_angle) noexcept;
    void arm_dwell_trailing (uint8_t cyl, AngleDeg10 dwell_start_angle) noexcept;
    // Injection is armed per EVENT, not per cylinder — each event belongs to ONE stage and drives that
    // stage's channels: every injector (multi-point), one bank's worth (bank), or one cylinder's own
    // (sequential family). Use inj_event_stage()/inj_event_ref_cyl()/inj_event_base_angle() to find the
    // stage, the fuel, and the TDC the event's firing angle is measured from.
    void arm_injection        (uint8_t event, AngleDeg10 open_angle, uint32_t pw_us) noexcept;
    // Arm the per-cylinder knock sampling window (fires the ADC burst at window_angle; drives no pin).
    void arm_knock_window     (uint8_t cyl, AngleDeg10 window_angle) noexcept;

    // ---- Prime pulse (TASK context) ----------------------------------------
    // Request a one-shot simultaneous squirt of every active injector for pw_us. Sets a flag the
    // next on_virtual_tooth (ISR) consumes — so the actual fire + close-list mutation stay in ISR
    // context (no task/ISR race on the time list). Fires once per request, on the next grid tooth
    // (i.e. as soon as the engine is turning/synced). Used for the cold-start prime.
    void request_prime(uint32_t pw_us) noexcept { prime_req_pw_us_ = pw_us; prime_req_pending_ = true; }

    // ---- Async transient injection train (TASK context) --------------------
    // Request up to num_pulses extra simultaneous squirts of every active injector (pw_us each),
    // SPREAD across the coming engine cycle — the asynchronous transient enrichment: extra injections
    // inserted BETWEEN the sequential events. Distinct from the one-shot cold-start prime. Latched and
    // fired in on_virtual_tooth (ISR, TIM5 prio 2) so the open + close-list mutation stay serialized.
    void request_async_injection(uint32_t pw_us, uint8_t num_pulses) noexcept {
        async_req_pw_us_ = pw_us; async_req_pulses_ = num_pulses; async_req_pending_ = true;
    }

    // ---- ISR callbacks ------------------------------------------------------
    // One virtual tooth from the grid: set up the tooth's angle walk + arm Timer 1.
    void on_virtual_tooth(uint16_t index, uint32_t tick,
                          uint32_t ticks_per_vtooth) noexcept;
    // Timer 1 fired: drive the current angle event, advance, re-arm.
    void on_angle_alarm() noexcept;
    // Timer 2 fired: drive the head time event (injector close), advance, re-arm.
    void on_time_alarm() noexcept;

    // ---- Sync gate / safety -------------------------------------------------
    void set_phase_known(bool known)    noexcept { phase_known_    = known; }
    void set_firing_enabled(bool en)    noexcept { firing_enabled_ = en; if (!en) async_pulses_left_ = 0; }
    [[nodiscard]] bool firing_enabled() const noexcept { return firing_enabled_; }
    [[nodiscard]] bool phase_known()    const noexcept { return phase_known_; }

    // ISR-SAFE panic stop: firing off, disarm both alarms, force every output LOW.
    // No list mutation → safe from the capture ISR. List cleanup → quiesce_volatiles.
    void emergency_off() noexcept;
    // TASK-CONTEXT quiesce: emergency_off() + unlink all VOLATILE nodes + clear lists.
    void quiesce_volatiles() noexcept;

    // ---- Telemetry & control ------------------------------------------------
    void                   set_execution_mask(uint64_t mask) noexcept { store_exec(mask); }

    // ---- Protective cuts, composed ------------------------------------------
    // Two independent reasons an output can be held off, and they must not overwrite each other: the
    // GLOBAL cut (rev limiter, protection — every channel) and the PER-CYLINDER cut (pre-ignition on
    // one cylinder). Both write the same execution mask, so both live here and the mask is recomputed
    // from the pair rather than either stamping over the other.
    //
    // Per-cylinder is expressed in CYLINDERS, not channels, because that is what a caller knows —
    // "cylinder 3 is pre-igniting". The translation to output channels is this class's business, via
    // the resolved masks: every coil and every injector (all stages) whose row names that cylinder.
    //
    // SHARED-OUTPUT CAVEAT: a per-cylinder cut is only as sharp as the wiring underneath it, and the
    // output rows say exactly what that is.
    //   Ignition — a wasted-spark or distributor coil serves several cylinders, so cutting one
    //     cylinder's coil cuts every cylinder that row names.
    //   Injection — a bank or multi-point injector serves a group, so cutting one cylinder's injector
    //     cuts fuel to that whole group. Only one injector per cylinder can cut a single cylinder's fuel.
    // A caller that must protect exactly one cylinder should check what that cylinder's channels are
    // shared with; on a wasted-spark engine cutting FUEL is the sharper of the two, and on a
    // grouped-injection engine neither cut is per-cylinder at all.
    void set_output_cuts(bool ign_cut, bool inj_cut) noexcept {
        global_ign_cut_ = ign_cut;
        global_inj_cut_ = inj_cut;
        recompute_execution_mask();
    }
    // bit c set = cut cylinder c's FUEL / its SPARK. Zero restores every cylinder.
    //
    // FUEL AND SPARK ARE SEPARATE, because the cuts that use this ask for different things. A
    // pre-ignition cut defaults to fuel only — on wasted spark, cutting a cylinder's coil also kills
    // its partner's spark — and a misfire cut is fuel only by definition. One mask for both made every
    // per-cylinder cut a fuel-AND-spark cut, whatever the setting said.
    void set_cylinder_cuts(uint16_t fuel_mask, uint16_t spark_mask) noexcept {
        cyl_fuel_cut_  = fuel_mask;
        cyl_spark_cut_ = spark_mask;
        recompute_execution_mask();
    }
    // The single mask is BOTH: fuel and spark for every cylinder named.
    void set_cylinder_cuts(uint16_t cyl_mask) noexcept { set_cylinder_cuts(cyl_mask, cyl_mask); }
    [[nodiscard]] uint16_t cylinder_cuts()       const noexcept { return cyl_fuel_cut_ | cyl_spark_cut_; }
    [[nodiscard]] uint16_t cylinder_fuel_cuts()  const noexcept { return cyl_fuel_cut_; }
    [[nodiscard]] uint16_t cylinder_spark_cuts() const noexcept { return cyl_spark_cut_; }
    // The composed cut mask. Read-only: a cut is asserted through set_output_cuts /
    // set_cylinder_cuts and never by writing this. Exists so a test can prove that a cut SURVIVES
    // a sync loss and re-acquisition — the two are independent state, and must stay that way.
    [[nodiscard]] uint64_t execution_mask() const noexcept {
        return static_cast<uint64_t>(exec_ign_) | (static_cast<uint64_t>(exec_inj_) << MAX_IGN_CHANNELS);
    }
    // True if any output-pin claim was rejected for the current config (a tune
    // mis-assignment). Config-time only — surfaced as a TS indicator, not a runtime fault.
    [[nodiscard]] bool     pin_conflict() const noexcept { return arbiter_ && arbiter_->has_conflict(); }

private:
    // Roles in the PER-CYLINDER pool (nodes_): ignition, plus the knock window, which is a
    // per-cylinder observation of a per-cylinder combustion event.
    enum CylRole : uint8_t {
        ROLE_IGN_SCHED = 0,  ROLE_IGN_DWELL, ROLE_IGN_SPARK,
        ROLE_IGN_TR_DWELL,   ROLE_IGN_TR_SPARK,
        ROLE_KNOCK_WINDOW,   // per-cyl knock sample window (fires the ADC burst; drives no pin)
        CYL_ROLE_COUNT
    };
    static_assert(CYL_ROLE_COUNT == SCHED_NODES_PER_CYLINDER,
                  "SCHED_NODES_PER_CYLINDER must match the per-cylinder role count");

    // Roles in the PER-EVENT pool (inj_nodes_). An injection event is not a cylinder — see the pool
    // comment in SchedulerTypes.h.
    enum InjRole : uint8_t {
        ROLE_INJ_SCHED    = 0,
        ROLE_INJ_OPEN     = 1,   // one stage's channels per event — no staged sub-nodes
        INJ_ROLE_COUNT    = 2
    };
    static_assert(INJ_ROLE_COUNT == SCHED_NODES_PER_INJ_EVENT,
                  "SCHED_NODES_PER_INJ_EVENT must match the per-event role count");

    // ---- Binding resolver (task context) ------------------------------------
    // Compute the per-cylinder scheduler inputs: TDC from the stored firing order and cyl[].tdc_angle,
    // and the coil + injector output masks from the output rows (map_). Runs at set_config; the results
    // feed claim_outputs / build_skeleton / build_inj_events.
    void resolve_binding() noexcept;

    // ---- Injection schedule compilation (task context) ---------------------
    // Compile the primary stage's mode into inj_events_ — the whole of the batch/multi-point/sequential
    // difference. Called from build_skeleton before the nodes are laid out.
    void build_inj_events() noexcept;

    // ---- Intrusive list ops (ISR-safe) -------------------------------------
    void insert_angle_event(SchedNode* n) noexcept;
    void remove_event(SchedNode* n) noexcept;
    void arm_volatile(SchedNode* n, AngleDeg10 angle720) noexcept;

    // ---- Angle-walk helpers (ISR) ------------------------------------------
    void                  skip_dead_heads() noexcept;          // advance past non-READY
    void                  flush_angle_walk() noexcept;         // fire what the last tooth left pending
    void                  unlink_walk_node(SchedNode* n) noexcept;
    [[nodiscard]] bool    in_current_tooth(AngleDeg10 angle) const noexcept;
    [[nodiscard]] SchedNode* peek_soonest_angle() noexcept;    // soonest of the two heads
    [[nodiscard]] uint32_t fire_tick(const SchedNode* n) const noexcept {
        return tooth_tick_ + static_cast<uint32_t>(
            (static_cast<uint64_t>(n->sub_offset) * tooth_tpv_) >> 15);
    }
    void arm_next_angle() noexcept;
    // Engine angle at this instant, interpolated within the current virtual tooth. This is
    // the angle an edge is RECORDED at, so it must be measured rather than assumed to be the
    // node's target: a late alarm shows up as a late angle, which is the whole point of
    // recording delivery instead of intent. Falls back to the tooth angle when there is no
    // clock or the tick delta is beyond a plausible interpolation — better a coarse true
    // angle than a precise invented one.
    [[nodiscard]] AngleDeg10 angle_now() const noexcept;
    void dispatch_fire(SchedNode* n) noexcept;
    // energize = dwell (coil) / open (injector); each channel resolves its OWN level from its row's
    // polarity, so one mask may mix active-high and active-low drivers.
    void drive_ign(OutputMask m, bool energize) noexcept;
    void drive_inj(OutputMask m, bool energize) noexcept;
    void fire_prime() noexcept;   // Timer ISR (pri 2): open all active injectors, schedule the close
    void fire_async_pulse() noexcept;  // Timer ISR (pri 2): one async squirt of all active injectors (slot-guarded)

    // Drive level for one channel, from its row's polarity (OutputMap). energize = dwell / open;
    // !energize = the de-energized, safe level.
    [[nodiscard]] static OutputAction level(bool energize, bool active_high) noexcept {
        return (energize == active_high) ? OutputAction::DRIVE_HIGH : OutputAction::DRIVE_LOW;
    }
    [[nodiscard]] OutputAction ign_level(uint8_t ci, bool energize) const noexcept {
        return level(energize, ci < MAX_IGN_CHANNELS ? map_.ign[ci].active_high : true);
    }
    [[nodiscard]] OutputAction inj_level(uint8_t ci, bool energize) const noexcept {
        return level(energize, ci < MAX_INJ_CHANNELS ? map_.inj[ci].active_high : true);
    }
    // Claim one IGN/LS pin from the arbiter (if not already taken this pass) and
    // bring it out of Hi-Z to its idle level.
    void claim_one_ign(uint8_t idx) noexcept;
    void claim_one_inj(uint8_t idx) noexcept;

    // ---- Time list (injector close), small sorted intrusive list ------------
    struct TimeEvt { TimeEvt* next; uint32_t tick; OutputMask mask; };
    void insert_close(uint32_t tick, OutputMask mask) noexcept;

    // 720°→bucket helpers (full cycle, no fold).
    [[nodiscard]] uint8_t  bucket_of(AngleDeg10 angle720) const noexcept;
    [[nodiscard]] FractionQ0_15 suboffset_of(AngleDeg10 angle720) const noexcept;

    [[nodiscard]] ITimerChannel* ign_hw(uint8_t ci) const noexcept {
        return (ci < MAX_IGN_CHANNELS) ? ign_channels_[ci] : nullptr;
    }
    [[nodiscard]] ITimerChannel* inj_hw(uint8_t ci) const noexcept {
        return (ci < MAX_INJ_CHANNELS) ? inj_channels_[ci] : nullptr;
    }

    // ---- Data members -------------------------------------------------------
    const ITimerChannel* timebase_;
    ITimerChannel*       ign_channels_[MAX_IGN_CHANNELS];   // dumb force_output_now sinks
    ITimerChannel*       inj_channels_[MAX_INJ_CHANNELS];
    IAlarmTimer*         angle_alarm_;   // Timer 1
    IAlarmTimer*         time_alarm_;     // Timer 2
    PinArbiter*          arbiter_;        // output-pin ownership (claim/release)
    uint32_t             ticks_per_second_;

    // Config shadow kept WITH this module: a codegen-owned value copy of the Engine
    // config (count/cycle/modes/polarity/per-cyl map — no tables), refreshed by
    // set_config() only at start()/reconfigure() (engine-stopped). The hot ISR reads
    // this, so a 'w' to g_config can't change firing topology mid-rev.
    EngineShadow cyl_cfg_{};   // = EngineConfig (the scheduler's whole module)

    // The output rows' firing half, as applied at the last set_config (value copy — a row edited while
    // the engine runs changes nothing here until the next stopped reconfigure).
    OutputMap            map_{};

    // Per-cylinder scheduler inputs RESOLVED by resolve_binding() — never stored in the tune. tdc from
    // the firing order / cyl[].tdc_angle; each mask is every coil (bit = IGN channel) or injector (bit =
    // LS channel) whose output row names that cylinder. Zero means nothing serves it.
    AngleDeg10           resolved_tdc_[MAX_CYLINDERS]        = {};
    OutputMask           resolved_ign_[MAX_CYLINDERS]        = {};   // leading coils
    OutputMask           resolved_ign_trail_[MAX_CYLINDERS]  = {};   // rotary trailing coils
    OutputMask           resolved_inj_[MAX_INJ_STAGES][MAX_CYLINDERS] = {};   // per active stage
    uint16_t             no_coil_mask_ = 0;   // firing cylinders with no leading coil (resolve_binding)
    uint16_t             no_inj_mask_  = 0;   // firing cylinders with no stage-1 injector

    SchedNode*           schedule_[MAX_VTEETH];
    SchedNode            nodes_[MAX_CYLINDERS][SCHED_NODES_PER_CYLINDER];
    SchedNode            inj_nodes_[MAX_INJ_EVENTS][SCHED_NODES_PER_INJ_EVENT];

    // The compiled injection schedule: what build_inj_events() turns the mode into. One entry per
    // event; inj_nodes_[e] is that event's node triple. Rebuilt only on reconfiguration, read in ISR.
    InjEvent             inj_events_[MAX_INJ_EVENTS];
    uint8_t              any_sync_skip_[(MAX_INJ_EVENTS + 7) / 8] = {};   // SEQUENTIAL_ANY_SYNC alternate-revolution toggle, per event
    uint8_t              inj_event_count_ = 0;

    AngleDeg10           vt_step_;
    uint8_t              buckets_per_rev_;

    ComputeHook          compute_cb_;
    void*                compute_ud_;

    // Angle-walk state for the current tooth (two heads → CRANK twin-bucket merge).
    SchedNode*           angle_a_;
    SchedNode*           angle_b_;
    SchedNode*           angle_c_;         // rotary at CRANK sync: the third revolution
    SchedNode*           pending_angle_;   // node the angle alarm is currently armed for
    volatile uint32_t    late_fired_ = 0;  // events fired late by flush_angle_walk (would have been lost)
    volatile uint32_t    inj_no_close_ = 0; // injections skipped: no close slot free
    // Coils charging right now, and since when — for the maximum-dwell cutoff (on_virtual_tooth).
    volatile OutputMask  charging_ = 0;
    uint32_t             charge_start_[MAX_IGN_CHANNELS > 0 ? MAX_IGN_CHANNELS : 1] = {};
    uint32_t             max_dwell_ticks_ = 0;       // 0 = no cutoff
    volatile uint32_t    overdwell_ = 0;             // coils released by the cutoff
    volatile uint32_t    dwell_started_late_ = 0;    // fit_dwell had no room and charged at once
    uint32_t             tooth_tick_;
    uint32_t             tooth_tpv_;
    AngleDeg10           tooth_angle_;     // engine angle of the current tooth (interpolation base)
    // Seqlock over (tooth_tick_, tooth_tpv_, tooth_angle_): odd while writing, even when stable.
    volatile uint32_t    tooth_seq_;
    CycleRecorder*       recorder_;        // optional one-cycle output capture (null = off)

    // Time list (injector close). One slot per injector channel (≤1 pending each).
    TimeEvt              time_pool_[MAX_INJ_CHANNELS];
    TimeEvt*             time_free_;
    TimeEvt*             time_head_;

    volatile bool        phase_known_;
    volatile bool        firing_enabled_;
    // THE CUTS, AS TWO 32-BIT WORDS. This was one volatile uint64_t, written by the task and read by the
    // firing ISR — two stores on this core, and the injector half straddles them (bits 12.. of a board
    // with 12 coils). An ISR landing between the stores applied half of a new cut for one event. Each
    // event reads exactly one of these, and a 32-bit read/write is a single instruction.
    volatile OutputMask  exec_ign_;
    volatile OutputMask  exec_inj_;
    void store_exec(uint64_t m) noexcept {
        exec_ign_ = static_cast<OutputMask>(m) & ((OutputMask(1) << MAX_IGN_CHANNELS) - 1);
        exec_inj_ = static_cast<OutputMask>(m >> MAX_IGN_CHANNELS) & ((OutputMask(1) << MAX_INJ_CHANNELS) - 1);
    }
    bool                 global_ign_cut_ = false;   // rev limiter / protection — every channel
    bool                 global_inj_cut_ = false;
    uint16_t             cyl_fuel_cut_   = 0;       // bit c = cylinder c's injectors cut
    uint16_t             cyl_spark_cut_  = 0;       // bit c = cylinder c's coils cut

    // Rebuild the execution masks from the global cuts AND the per-cylinder cuts. Called whenever either
    // changes; also after a reconfigure, because a cylinder's output channels can move.
    void recompute_execution_mask() noexcept {
        uint64_t m = ~0ULL;
        if (global_ign_cut_) m &= ~((1ULL << MAX_IGN_CHANNELS) - 1ULL);
        if (global_inj_cut_) m &= ~(((1ULL << MAX_INJ_CHANNELS) - 1ULL) << MAX_IGN_CHANNELS);
        if (cyl_fuel_cut_ | cyl_spark_cut_) {
            const uint8_t n = cyl_cfg_.cylinder_count;
            for (uint8_t c = 0; c < n && c < MAX_CYLINDERS; ++c) {
                if (cyl_spark_cut_ & (1u << c))
                    m &= ~static_cast<uint64_t>(resolved_ign_[c] | resolved_ign_trail_[c]);
                if (cyl_fuel_cut_ & (1u << c))
                    for (uint8_t s = 0; s < MAX_INJ_STAGES; ++s)
                        m &= ~(static_cast<uint64_t>(resolved_inj_[s][c]) << MAX_IGN_CHANNELS);
            }
        }
        store_exec(m);
    }

    // One-shot prime request: written by request_prime() (task), consumed in on_virtual_tooth (ISR).
    volatile bool        prime_req_pending_;
    volatile uint32_t    prime_req_pw_us_;

    // Async transient injection train: request written by request_async_injection() (task), latched +
    // fired in on_virtual_tooth (ISR). The train fires async_pulses_left_ squirts, one every
    // async_spacing_ teeth, so they spread across the cycle between the sequential events.
    volatile bool        async_req_pending_;
    volatile uint32_t    async_req_pw_us_;
    volatile uint8_t     async_req_pulses_;
    uint32_t             async_pw_us_;          // latched per-pulse pw
    volatile uint8_t     async_pulses_left_;    // pulses still to fire this train (task may clear on firing-off)
    uint16_t             async_countdown_;      // teeth until the next pulse
    uint16_t             async_spacing_;        // teeth between pulses
};
