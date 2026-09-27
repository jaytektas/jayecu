#include "EventScheduler.h"
#include "Log.h"
#include <atomic>
#include <cstring>

// Even-fire: compute each firing cylinder's TDC angle FROM the firing order and write it into config
// (cyl[].tdc_angle); the scheduler then FETCHES that in resolve_binding. Walks the firing_order array by
// POSITION — firing_order[i].cyl is the cylinder that fires i-th — and gives it TDC = interval × i,
// interval = cycle / cylinders. Unused cylinders cleared to 0. Odd-fire left untouched (tuner's values).
void update_cylinder_angles(EngineConfig& e) noexcept {
    if (e.odd_fire) return;
    const uint8_t n = e.cylinder_count;
    if (n == 0 || n > ENGINE_CYL_COUNT) return;
    const AngleDeg10 cyc      = engine_cycle_angle(e.cycle_type);   // 720/360/1080° in decidegrees
    const AngleDeg10 interval = static_cast<AngleDeg10>(cyc / n);
    for (uint8_t c = 0; c < ENGINE_CYL_COUNT; ++c) e.cyl[c].tdc_angle = 0;   // clear, then set by firing order
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t cyl_num = e.firing_order[i].cyl;             // the cylinder firing at position i (1-based)
        if (cyl_num >= 1 && cyl_num <= ENGINE_CYL_COUNT)
            e.cyl[cyl_num - 1].tdc_angle = static_cast<int16_t>(interval * i);
    }
}

bool firing_order_valid(const EngineConfig& e) noexcept {
    const uint8_t n = e.cylinder_count;
    if (n == 0 || n > ENGINE_CYL_COUNT) return false;
    uint16_t seen = 0;                                              // bit (c-1) set once cylinder c appears
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t c = e.firing_order[i].cyl;
        if (c < 1 || c > n) return false;                          // out of 1..n (0 = unused is invalid inside [0,n))
        const uint16_t bit = static_cast<uint16_t>(1u << (c - 1));
        if (seen & bit) return false;                             // duplicate cylinder
        seen |= bit;
    }
    return seen == static_cast<uint16_t>((1u << n) - 1);           // every cylinder 1..n present exactly once
}

// ---------------------------------------------------------------------------
// Alarm callback trampolines (file-static; ud = the scheduler).
// ---------------------------------------------------------------------------
static void angle_alarm_tramp(void* ud) noexcept {
    static_cast<EventScheduler*>(ud)->on_angle_alarm();
}
static void time_alarm_tramp(void* ud) noexcept {
    static_cast<EventScheduler*>(ud)->on_time_alarm();
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
EventScheduler::EventScheduler() noexcept
    : timebase_(nullptr)
    , ign_channels_{}
    , inj_channels_{}
    , angle_alarm_(nullptr)
    , time_alarm_(nullptr)
    , arbiter_(nullptr)
    , ticks_per_second_(0)
    , cyl_cfg_{}
    , schedule_{}
    , nodes_{}
    , inj_nodes_{}
    , inj_events_{}
    , vt_step_(0)
    , buckets_per_rev_(0)
    , compute_cb_(nullptr)
    , compute_ud_(nullptr)
    , angle_a_(nullptr)
    , angle_b_(nullptr)
    , angle_c_(nullptr)
    , pending_angle_(nullptr)
    , tooth_tick_(0)
    , tooth_tpv_(0)
    , tooth_angle_(0)
    , tooth_seq_(0)
    , recorder_(nullptr)
    , time_pool_{}
    , time_free_(nullptr)
    , time_head_(nullptr)
    , phase_known_(false)
    , firing_enabled_(false)
    , exec_ign_(static_cast<OutputMask>((OutputMask(1) << MAX_IGN_CHANNELS) - 1))
    , exec_inj_(static_cast<OutputMask>((OutputMask(1) << MAX_INJ_CHANNELS) - 1))
    , prime_req_pending_(false)
    , prime_req_pw_us_(0)
    , async_req_pending_(false)
    , async_req_pw_us_(0)
    , async_req_pulses_(0)
    , async_pw_us_(0)
    , async_pulses_left_(0)
    , async_countdown_(0)
    , async_spacing_(0)
{
    // Chain the time-list pool into the free list.
    for (uint8_t i = 0; i < MAX_INJ_CHANNELS; ++i)
        time_pool_[i].next = (i + 1 < MAX_INJ_CHANNELS) ? &time_pool_[i + 1] : nullptr;
    time_free_ = &time_pool_[0];
}

// ---------------------------------------------------------------------------
// Hardware assignment
// ---------------------------------------------------------------------------
// IGN/INJ channels are now dumb output sinks: only force_output_now() is used
// (the angle/time alarms own the timing). No per-channel match callback.
void EventScheduler::assign_ign_channel(uint8_t idx, ITimerChannel& ch) noexcept {
    if (idx >= MAX_IGN_CHANNELS) return;
    ign_channels_[idx] = &ch;
    ch.enable_output(ign_level(idx, false));   // Hi-Z → driven PP at this coil's de-energized level
    if (ticks_per_second_ == 0) ticks_per_second_ = ch.get_ticks_per_second();
}

void EventScheduler::assign_inj_channel(uint8_t idx, ITimerChannel& ch) noexcept {
    if (idx >= MAX_INJ_CHANNELS) return;
    inj_channels_[idx] = &ch;
    ch.enable_output(inj_level(idx, false));   // Hi-Z → driven PP at this injector's closed level
    if (ticks_per_second_ == 0) ticks_per_second_ = ch.get_ticks_per_second();
}

void EventScheduler::assign_timebase(const ITimerChannel& tb) noexcept {
    timebase_ = &tb;
    if (ticks_per_second_ == 0) ticks_per_second_ = tb.get_ticks_per_second();
}

void EventScheduler::assign_angle_alarm(IAlarmTimer& a) noexcept {
    angle_alarm_ = &a;
    a.register_callback(angle_alarm_tramp, this);
}

void EventScheduler::assign_time_alarm(IAlarmTimer& a) noexcept {
    time_alarm_ = &a;
    a.register_callback(time_alarm_tramp, this);
}

void EventScheduler::set_config(const EngineConfig& eng, const OutputMap& map, bool boot) noexcept {
    // codegen-owned shadow refresh (value copy). boot = first config after the flashed
    // tune is loaded → FULL pull; a later stopped-engine reconfigure does the runtime
    // pull (engine_stop fields only) so reboot-flagged fields hold their boot value.
    if (boot) engine_shadow_pull(cyl_cfg_, eng);
    else      engine_shadow_pull_runtime(cyl_cfg_, eng);
    map_ = map;          // the output rows' firing half, by value
    resolve_binding();   // firing order → per-cylinder TDC; output rows → per-cylinder coil/injector masks
    recompute_execution_mask();   // a cylinder's coils and injectors may have moved
}

// ---------------------------------------------------------------------------
// resolve_binding — expand the stored config into the per-cylinder scheduler inputs the rest of the
// module reads. Task context, at set_config only.
//
//   Order        firing_order[i].cyl = the cylinder firing i-th; only cylinders in it are scheduled.
//   TDC          FETCHED from cyl[].tdc_angle (update_cylinder_angles wrote the even-fire values).
//   Ignition     every coil row whose cylinder value names c (output_serves): a leading row into
//                resolved_ign_[c], a trailing row (rotary) into resolved_ign_trail_[c]. On WASTED SPARK a
//                row names ONE cylinder, and that coil also fires for the cylinder's companion — the one
//                half a cycle away, found here from the TDCs the firing order produced — so a changed
//                firing order re-pairs the coils without an output moving.
//   Injection    every injector row of an ACTIVE stage (< num_inj_stages) that names c, into
//                resolved_inj_[stage][c]. The stage's mode decides how build_inj_events groups them.
//
// Nothing is allocated: the rows are the studio's, and a cylinder no row names gets nothing.
// ---------------------------------------------------------------------------
void EventScheduler::resolve_binding() noexcept {
    for (uint8_t c = 0; c < MAX_CYLINDERS; ++c) {
        resolved_tdc_[c]       = 0;
        resolved_ign_[c]       = 0;
        resolved_ign_trail_[c] = 0;
        for (uint8_t s = 0; s < MAX_INJ_STAGES; ++s) resolved_inj_[s][c] = 0;
    }
    no_coil_mask_ = no_inj_mask_ = 0;
    const uint8_t ncyl = (cyl_cfg_.cylinder_count <= MAX_CYLINDERS)
                         ? cyl_cfg_.cylinder_count : MAX_CYLINDERS;
    if (ncyl == 0) return;

    const AngleDeg10 cyc   = cycle_angle();
    const bool    rotary   = is_rotary();
    const uint8_t nstages  = (cyl_cfg_.num_inj_stages <= MAX_INJ_STAGES)
                           ? cyl_cfg_.num_inj_stages : MAX_INJ_STAGES;

    uint16_t firing = 0;
    for (uint8_t i = 0; i < ncyl; ++i) {
        const uint8_t cn = cyl_cfg_.firing_order[i].cyl;
        if (cn >= 1 && cn <= ncyl) firing = static_cast<uint16_t>(firing | (1u << (cn - 1)));
    }

    for (uint8_t c = 0; c < ncyl; ++c) {
        if (!(firing & (1u << c))) continue;
        resolved_tdc_[c] = angle_wrap(cyl_cfg_.cyl[c].tdc_angle, cyc);

        for (uint8_t k = 0; k < MAX_IGN_CHANNELS; ++k) {
            const FiringOutput& f = map_.ign[k];
            if (!f.used || !output_serves(cyl_cfg_, f.cylinder, c)) continue;
            // A trailing row on a piston engine has no plug to fire; it is not bound at all.
            if (f.plug == 0)   resolved_ign_[c]       |= (OutputMask(1) << k);
            else if (rotary)   resolved_ign_trail_[c] |= (OutputMask(1) << k);
        }
        for (uint8_t k = 0; k < MAX_INJ_CHANNELS; ++k) {
            const FiringOutput& f = map_.inj[k];
            if (!f.used || f.stage >= nstages || !output_serves(cyl_cfg_, f.cylinder, c)) continue;
            resolved_inj_[f.stage][c] |= (OutputMask(1) << k);
        }
    }

    // WASTED SPARK: each coil fires for its row's cylinder AND that cylinder's companion. From a copy, so
    // a pair is joined once in each direction rather than accumulating across the loop. Pistons only —
    // a rotary has no companion (see build_skeleton's crank-mask note).
    if (!rotary && static_cast<IgnitionCoilMode>(cyl_cfg_.ign_mode) == IgnitionCoilMode::WASTED_SPARK) {
        OutputMask own[MAX_CYLINDERS];
        for (uint8_t c = 0; c < ncyl; ++c) own[c] = resolved_ign_[c];
        for (uint8_t c = 0; c < ncyl; ++c) {
            if (!(firing & (1u << c))) continue;
            const AngleDeg10 comp = wasted_companion_tdc(resolved_tdc_[c], cyc);
            for (uint8_t k = 0; k < ncyl; ++k)
                if (k != c && (firing & (1u << k)) && resolved_tdc_[k] == comp) resolved_ign_[c] |= own[k];
        }
    }

    // WHAT THE ROWS LEAVE UNSERVED — reported, never filled in.
    for (uint8_t c = 0; c < ncyl; ++c) {
        if (!(firing & (1u << c))) continue;
        if (resolved_ign_[c] == 0)    no_coil_mask_ = static_cast<uint16_t>(no_coil_mask_ | (1u << c));
        if (resolved_inj_[0][c] == 0) no_inj_mask_  = static_cast<uint16_t>(no_inj_mask_  | (1u << c));
    }
}

// ---------------------------------------------------------------------------
// claim_outputs — claim from the arbiter ONLY the pins the configured cylinders
// use (the distinct ign/inj channels over the firing order, + rotary trailing /
// staged secondary), and bring each out of Hi-Z to its de-energized idle level.
// Spares are never claimed → they stay Hi-Z, free for boost/aux modules. The
// scheduler's prior pins are released first so a reconfigure that drops a pin
// returns it to the pool.
// ---------------------------------------------------------------------------
void EventScheduler::claim_outputs() noexcept {
    if (!arbiter_) return;
    arbiter_->clear_conflict();   // re-evaluate against the current config
    arbiter_->release_owner(PinOwner::IGNITION);
    arbiter_->release_owner(PinOwner::INJECTION);
    for (uint8_t i = 0; i < MAX_IGN_CHANNELS; ++i) ign_channels_[i] = nullptr;
    for (uint8_t i = 0; i < MAX_INJ_CHANNELS; ++i) inj_channels_[i] = nullptr;

    // Exactly the channels some firing cylinder resolved to — a row naming no firing cylinder, or an
    // injector in a stage that is switched off, claims nothing and its pin stays Hi-Z.
    OutputMask ign = 0, inj = 0;
    for (uint8_t c = 0; c < MAX_CYLINDERS; ++c) {
        ign |= resolved_ign_[c] | resolved_ign_trail_[c];
        for (uint8_t s = 0; s < MAX_INJ_STAGES; ++s) inj |= resolved_inj_[s][c];
    }
    for (uint8_t k = 0; k < MAX_IGN_CHANNELS; ++k) if (ign & (OutputMask(1) << k)) claim_one_ign(k);
    for (uint8_t k = 0; k < MAX_INJ_CHANNELS; ++k) if (inj & (OutputMask(1) << k)) claim_one_inj(k);
}

void EventScheduler::claim_one_ign(uint8_t idx) noexcept {
    if (idx >= MAX_IGN_CHANNELS || ign_channels_[idx]) return;   // unused slot / already claimed
    const uint8_t row = static_cast<uint8_t>(OUT_ROW_IGN_BASE + idx);
    ITimerChannel* ch = arbiter_->claim(row, PinOwner::IGNITION);
    if (ch) assign_ign_channel(idx, *ch);   // store + enable_output(idle) + timebase
    else    EFI_LOG_WARN("pins", "IGN%u claim denied (held by owner %u)",
                         (unsigned)(idx + 1), (unsigned)arbiter_->owner_of(row));
}

void EventScheduler::claim_one_inj(uint8_t idx) noexcept {
    if (idx >= MAX_INJ_CHANNELS || inj_channels_[idx]) return;
    const uint8_t row = static_cast<uint8_t>(OUT_ROW_LS_BASE + idx);
    ITimerChannel* ch = arbiter_->claim(row, PinOwner::INJECTION);
    if (ch) assign_inj_channel(idx, *ch);
    else    EFI_LOG_WARN("pins", "LS%u claim denied (held by owner %u)",
                         (unsigned)(idx + 1), (unsigned)arbiter_->owner_of(row));
}

void EventScheduler::register_compute_hook(ComputeHook cb, void* ud) noexcept {
    compute_cb_ = cb;
    compute_ud_ = ud;
}

// ---------------------------------------------------------------------------
// Bucket helpers — full-cycle (no fold). bucket ∈ [0 .. MAX_VTEETH-1] over 720°.
// ---------------------------------------------------------------------------
uint8_t EventScheduler::bucket_of(AngleDeg10 angle720) const noexcept {
    if (vt_step_ <= 0) return 0;
    uint32_t a = static_cast<uint32_t>(angle_wrap(angle720, cycle_angle()));   // 0..cycle-1
    uint8_t b = static_cast<uint8_t>(a / static_cast<uint32_t>(vt_step_));
    if (b >= MAX_VTEETH) b = MAX_VTEETH - 1;
    return b;
}

FractionQ0_15 EventScheduler::suboffset_of(AngleDeg10 angle720) const noexcept {
    if (vt_step_ <= 0) return 0;
    uint32_t a = static_cast<uint32_t>(angle_wrap(angle720, cycle_angle()));
    uint32_t rem = a % static_cast<uint32_t>(vt_step_);
    return static_cast<FractionQ0_15>((rem << 15) / static_cast<uint32_t>(vt_step_));
}

// ---------------------------------------------------------------------------
// build_inj_events — compile the injection mode into a flat list of events.
//
// THIS FUNCTION IS THE WHOLE OF THE MODE DIFFERENCE. Everything downstream (skeleton layout, the
// compute hook, dispatch) treats an event as an opaque {mask, ref_cyl, base_angle} and never asks
// which mode produced it — so adding a mode is adding a case here, not a branch in the ISR path.
//
// Sources ONLY the resolved masks (resolve_binding), i.e. the output rows. A per-cylinder stage has one
// row per cylinder (the output map check refuses anything else), so its events never share an output; a
// grouped row naming several cylinders is ORed into its group's mask once, which needs no dedupe either.
//
// EACH stage is compiled independently on its OWN mode (inj_stage[st].mode) — one event = one stage's
// channels at one angle; no bundled primary+staged event. Per stage:
//   PER-CYLINDER (sequential, semi-sequential, sequential-any-sync) — one event per cylinder on that
//     cylinder's own output, repeated once per crank revolution (semi-seq) or once per cycle (the
//     sequentials). Only these deliver a per-cylinder pulse.
//   MULTI-POINT — every one of this stage's outputs together, off one engine reference angle,
//     'injections_per_cycle' times. Overall correction only.
//   BANK — one group per cylinder bank (cyl[i].bank), each fired at its bank's earliest TDC; a
//     single-bank engine collapses to one group (= multi-point). Carries per-bank correction.
// ---------------------------------------------------------------------------
void EventScheduler::build_inj_events() noexcept {
    inj_event_count_ = 0;

    const uint8_t ncyl = (cyl_cfg_.cylinder_count <= MAX_CYLINDERS)
                         ? cyl_cfg_.cylinder_count : MAX_CYLINDERS;
    if (ncyl == 0) return;

    const AngleDeg10 cyc = cycle_angle();

    // One event = ONE stage's channels firing at one angle (no staged sub-nodes). Each stage — the
    // primary included — is compiled independently on its OWN mode in the loop below.
    const auto push = [&](uint8_t stage, OutputMask m, uint8_t ref, AngleDeg10 base) noexcept {
        if (!m) return;
        if (inj_event_count_ >= MAX_INJ_EVENTS) {
            EFI_LOG_WARN("sched", "injection event pool full (%u) — truncated", (unsigned)MAX_INJ_EVENTS);
            return;
        }
        InjEvent& e  = inj_events_[inj_event_count_++];
        e.mask       = m;
        e.stage      = stage;
        e.ref_cyl    = ref;
        e.base_angle = angle_wrap(base, cyc);
    };

    // THIS stage's injectors for cylinder c (0 when none): every row of the stage that names c.
    const auto stage_bit = [&](uint8_t stage, uint8_t c) noexcept -> OutputMask {
        return (stage < MAX_INJ_STAGES && c < MAX_CYLINDERS) ? resolved_inj_[stage][c] : OutputMask(0);
    };

    const uint8_t nstages = (cyl_cfg_.num_inj_stages <= MAX_INJ_STAGES)
                          ? cyl_cfg_.num_inj_stages : MAX_INJ_STAGES;

    for (uint8_t st = 0; st < nstages; ++st) {
        const InjectionMode mode  = static_cast<InjectionMode>(cyl_cfg_.inj_stage[st].mode);
        const uint8_t events = injection_events_per_cycle(cyl_cfg_.inj_stage[st].mode, cyl_cfg_.cycle_type,
                                                          cyl_cfg_.inj_stage[st].injections_per_cycle);
        const AngleDeg10 interval = static_cast<AngleDeg10>(cyc / (events ? events : 1));

        OutputMask all = 0;                              // every output this stage drives
        for (uint8_t c = 0; c < ncyl; ++c) all |= stage_bit(st, c);
        if (!all) continue;                              // no injector row in this stage → skip

        // ---- Per-cylinder modes: one event per cylinder on its own output, at its own TDC ---------
        if (mode == InjectionMode::SEQUENTIAL ||
            mode == InjectionMode::SEQUENTIAL_ANY_SYNC ||
            mode == InjectionMode::SEMI_SEQUENTIAL) {
            for (uint8_t c = 0; c < ncyl; ++c) {
                const OutputMask m = stage_bit(st, c);
                if (!m) continue;
                for (uint8_t k = 0; k < events; ++k)
                    push(st, m, c, static_cast<AngleDeg10>(resolved_tdc_[c] + k * interval));
            }
            continue;
        }

        // ---- MULTI_POINT: every output of this stage together, off the earliest TDC --------------
        if (mode == InjectionMode::MULTI_POINT) {
            AngleDeg10 ref_tdc = 0; uint8_t ref = 0; bool have = false;
            for (uint8_t c = 0; c < ncyl; ++c) {
                if (!stage_bit(st, c)) continue;
                if (!have || resolved_tdc_[c] < ref_tdc) { ref_tdc = resolved_tdc_[c]; ref = c; have = true; }
            }
            for (uint8_t k = 0; k < events; ++k)
                push(st, all, ref, static_cast<AngleDeg10>(ref_tdc + k * interval));
            continue;
        }

        // ---- BANK: one group per cylinder bank, each fired at its bank's earliest TDC -------------
        //      Bank correction is a per-bank uniform value, so grouping the outputs BY BANK is what
        //      lets a grouped stage carry it. A single-bank engine collapses to one group = every
        //      output together (the correct degenerate: one bank IS the whole engine).
        for (uint8_t bank = 1; bank <= 2; ++bank) {
            OutputMask bm = 0; AngleDeg10 btdc = 0; uint8_t bref = 0; bool bhave = false;
            for (uint8_t c = 0; c < ncyl; ++c) {
                const OutputMask b = stage_bit(st, c);
                if (!b || cyl_cfg_.cyl[c].bank != bank) continue;
                bm |= b;
                if (!bhave || resolved_tdc_[c] < btdc) { btdc = resolved_tdc_[c]; bref = c; bhave = true; }
            }
            if (!bm) continue;
            for (uint8_t k = 0; k < events; ++k)
                push(st, bm, bref, static_cast<AngleDeg10>(btdc + k * interval));
        }
    }
}

// ---------------------------------------------------------------------------
// build_skeleton — lay out the persistent node skeleton (task context, reconfig
// only). Derives the dual (crank/phase) output masks per cylinder from the tune
// (cyl_cfg_) and links the STATIC schedule nodes into their buckets. VOLATILE
// fire nodes start UNSCHEDULED; the STATIC nodes insert them from live state.
//
// Two passes over two pools, because ignition is per cylinder and injection is per EVENT (see the
// pool comment in SchedulerTypes.h): the cylinder pass lays out coils + knock windows, then the event
// pass lays out whatever build_inj_events() compiled the injection mode into.
// ---------------------------------------------------------------------------
void EventScheduler::build_skeleton(AngleDeg10 vt_step) noexcept {
    // Quiesce any prior skeleton first (coil-safe).
    firing_enabled_ = false;
    for (uint16_t b = 0; b < MAX_VTEETH; ++b) schedule_[b] = nullptr;

    vt_step_ = (vt_step > 0) ? vt_step : 100;
    buckets_per_rev_ = static_cast<uint8_t>(ANGLE_360 / vt_step_);   // e.g. 36 at 10°

    const uint8_t ncyl = (cyl_cfg_.cylinder_count <= MAX_CYLINDERS)
                         ? cyl_cfg_.cylinder_count : MAX_CYLINDERS;

    const bool rotary = is_rotary();   // the CYCLE, not the coil wiring — see EventScheduler::is_rotary

    build_inj_events();   // compile the mode before laying its nodes out

    for (uint8_t c = 0; c < ncyl; ++c) {
        SchedNode* N = nodes_[c];
        for (uint8_t r = 0; r < SCHED_NODES_PER_CYLINDER; ++r) {
            std::memset(&N[r], 0, sizeof(SchedNode));
            N[r].state     = EventState::UNSCHEDULED;
            N[r].cyl_index = c;
        }

        // ---- Ignition output masks (dual: CRANK wasted-pair vs PHASE COP) ----
        // The coils come from the output rows (resolve_binding). Under wasted spark a companion pair
        // already shares its coil there, so the companion OR below adds nothing; it is
        // what lets a coil-on-plug map fire before cam phase is known — both cylinders of the pair spark
        // each revolution, one of them harmlessly on its exhaust stroke.
        OutputMask phase_ign = resolved_ign_[c];
        OutputMask crank_ign = phase_ign;
        // Wasted companion = the cylinder half an engine cycle away, sharing this coil. On a
        // four-stroke that is the cylinder on its exhaust stroke (360 deg); on a two-stroke it is
        // the one at BDC (180 deg). See wasted_companion_tdc().
        //
        // A ROTARY HAS NO COMPANION — and firing a phantom one is not a harmless wasted spark. Both plugs
        // (leading + trailing) fire intentionally every combustion, and the 1080 deg geometry has no
        // half-cycle twin on a non-firing stroke: a "companion" discharge on another rotor lands mid-cycle
        // where a real charge is, 180 deg out — detonation, not a wasted spark. So the companion OR runs
        // for piston cycles ONLY; a rotary's crank mask is exactly its phase mask (its own coils, nothing else).
        if (!rotary) {
            const AngleDeg10 comp_tdc = wasted_companion_tdc(resolved_tdc_[c], cycle_angle());
            for (uint8_t k = 0; k < ncyl; ++k) {
                if (k == c) continue;
                if (resolved_tdc_[k] == comp_tdc) crank_ign |= resolved_ign_[k];
            }
        }

        // ---- STATIC ignition schedule node (re-arms leading + trailing) ------
        SchedNode& isch = N[ROLE_IGN_SCHED];
        isch.type         = EventType::STATIC;
        isch.state        = EventState::READY;
        isch.action       = EventAction::IGN_SCHEDULE;
        isch.priority     = EventPriority::SCHEDULE;
        isch.target_angle = angle_wrap(
            static_cast<AngleDeg10>(resolved_tdc_[c] - SCHED_COMPUTE_LEAD), cycle_angle());
        isch.bucket       = bucket_of(isch.target_angle);
        isch.sub_offset   = suboffset_of(isch.target_angle);
        insert_angle_event(&isch);

        // VOLATILE leading dwell/spark (armed later by the compute hook).
        N[ROLE_IGN_DWELL].type = EventType::VOLATILE;
        N[ROLE_IGN_DWELL].action = EventAction::IGN_DWELL_START;
        N[ROLE_IGN_DWELL].priority = EventPriority::DWELL;
        N[ROLE_IGN_DWELL].crank_mask = crank_ign;
        N[ROLE_IGN_DWELL].phase_mask = phase_ign;

        N[ROLE_IGN_SPARK].type = EventType::VOLATILE;
        N[ROLE_IGN_SPARK].action = EventAction::IGN_SPARK;
        N[ROLE_IGN_SPARK].priority = EventPriority::DISCHARGE;
        N[ROLE_IGN_SPARK].crank_mask = crank_ign;
        N[ROLE_IGN_SPARK].phase_mask = phase_ign;

        // VOLATILE rotary trailing (own coil channel, own masks).
        if (rotary) {
            const OutputMask tmask = resolved_ign_trail_[c];
            N[ROLE_IGN_TR_DWELL].type = EventType::VOLATILE;
            N[ROLE_IGN_TR_DWELL].action = EventAction::IGN_DWELL_START;
            N[ROLE_IGN_TR_DWELL].priority = EventPriority::DWELL;
            N[ROLE_IGN_TR_DWELL].crank_mask = tmask;
            N[ROLE_IGN_TR_DWELL].phase_mask = tmask;
            N[ROLE_IGN_TR_SPARK].type = EventType::VOLATILE;
            N[ROLE_IGN_TR_SPARK].action = EventAction::IGN_SPARK;
            N[ROLE_IGN_TR_SPARK].priority = EventPriority::DISCHARGE;
            N[ROLE_IGN_TR_SPARK].crank_mask = tmask;
            N[ROLE_IGN_TR_SPARK].phase_mask = tmask;
        }

        // VOLATILE per-cylinder knock sampling window. Armed each cycle by the compute hook (only when
        // knock is enabled) at TDC + window_start; drives no pin — SAMPLE priority so it dispatches
        // after the same-tooth spark/injection, and ADC_TRIGGER routes to the compute hook (which fires
        // the burst). Left UNSCHEDULED when knock is off, so it's inert until armed.
        N[ROLE_KNOCK_WINDOW].type     = EventType::VOLATILE;
        N[ROLE_KNOCK_WINDOW].action   = EventAction::ADC_TRIGGER;
        N[ROLE_KNOCK_WINDOW].priority = EventPriority::SAMPLE;
    }

    // ---- Injection pass: one node triple per compiled event ----------------
    // cyl_index carries the event's REFERENCE cylinder, not its slot: that is what the compute hook
    // is handed back and what it looks the pulse width up by. The slot index is just storage.
    for (uint8_t e = 0; e < inj_event_count_; ++e) {
        const InjEvent& ev = inj_events_[e];
        SchedNode* J = inj_nodes_[e];
        for (uint8_t r = 0; r < SCHED_NODES_PER_INJ_EVENT; ++r) {
            std::memset(&J[r], 0, sizeof(SchedNode));
            J[r].state     = EventState::UNSCHEDULED;
            J[r].cyl_index = e;   // the EVENT index — what INJ_SCHEDULE hands the compute hook
        }

        SchedNode& jsch = J[ROLE_INJ_SCHED];
        jsch.type         = EventType::STATIC;
        jsch.state        = EventState::READY;
        jsch.action       = EventAction::INJ_SCHEDULE;
        jsch.priority     = EventPriority::SCHEDULE;
        jsch.target_angle = angle_wrap(
            static_cast<AngleDeg10>(ev.base_angle - SCHED_COMPUTE_LEAD), cycle_angle());
        jsch.bucket       = bucket_of(jsch.target_angle);
        jsch.sub_offset   = suboffset_of(jsch.target_angle);
        insert_angle_event(&jsch);

        // One mask for both sync levels. Unlike ignition — where CRANK merges a wasted pair onto one
        // coil — an injection event's channel set is decided by the MODE, not by what the decoder
        // currently knows: batch drives half the outputs whether or not the cam has been seen. What
        // crank-only sync still changes is the bucket fold, which fires the second half of the
        // cycle's events on every revolution; that is the documented degradation of full-sequential
        // into semi-sequential, and it needs no second mask to express.
        J[ROLE_INJ_OPEN].type       = EventType::VOLATILE;
        J[ROLE_INJ_OPEN].action     = EventAction::INJ_OPEN;
        J[ROLE_INJ_OPEN].priority   = EventPriority::INJECT;
        J[ROLE_INJ_OPEN].crank_mask = ev.mask;
        J[ROLE_INJ_OPEN].phase_mask = ev.mask;
    }
}

// ---------------------------------------------------------------------------
// Intrusive list ops (ISR-safe). Ordered ascending by (sub_offset, priority).
// ---------------------------------------------------------------------------
void EventScheduler::insert_angle_event(SchedNode* n) noexcept {
    if (!n || n->bucket >= MAX_VTEETH) return;
    n->next = nullptr;
    n->prev = nullptr;
    n->state = EventState::READY;

    SchedNode* p = schedule_[n->bucket];
    // New head?
    if (!p ||
        n->sub_offset < p->sub_offset ||
        (n->sub_offset == p->sub_offset && n->priority <= p->priority)) {
        n->next = p;
        if (p) p->prev = n;
        schedule_[n->bucket] = n;
        return;
    }
    // Walk to the insertion point.
    while (p->next &&
           (n->sub_offset > p->next->sub_offset ||
            (n->sub_offset == p->next->sub_offset && n->priority > p->next->priority))) {
        p = p->next;
    }
    n->prev = p;
    n->next = p->next;
    p->next = n;
    if (n->next) n->next->prev = n;
}

void EventScheduler::remove_event(SchedNode* n) noexcept {
    if (!n) return;
    if (n->prev) n->prev->next = n->next;
    if (n->next) n->next->prev = n->prev;
    if (n->bucket < MAX_VTEETH && schedule_[n->bucket] == n) schedule_[n->bucket] = n->next;
    n->next = nullptr;
    n->prev = nullptr;
    n->state = EventState::UNSCHEDULED;
}

// ---------------------------------------------------------------------------
// arm_volatile — point a VOLATILE node at a fresh 720° angle and (re)insert it.
// Guarded: a still-pending node (READY) is left where it is rather than moved,
// so a second grid pass within a cycle cannot double-insert it.
// ---------------------------------------------------------------------------
void EventScheduler::arm_volatile(SchedNode* n, AngleDeg10 angle720) noexcept {
    if (!n) return;
    if (n->state != EventState::UNSCHEDULED) return;   // already pending — leave it
    n->target_angle = angle_wrap(angle720, cycle_angle());
    n->bucket       = bucket_of(n->target_angle);
    n->sub_offset   = suboffset_of(n->target_angle);
    insert_angle_event(n);
}

// ---- Re-arm hooks (called from the compute hook, ISR) ----------------------
// Spark is armed by the STATIC schedule node (fresh advance, ~90° ahead). Dwell
// is armed when the previous spark discharges — a full cycle of lead, so dwell
// length is unconstrained (full-cycle dwelling possible).
void EventScheduler::arm_spark(uint8_t cyl, AngleDeg10 spark_angle) noexcept {
    if (cyl >= MAX_CYLINDERS) return;
    arm_volatile(&nodes_[cyl][ROLE_IGN_SPARK], spark_angle);
}
void EventScheduler::arm_spark_trailing(uint8_t cyl, AngleDeg10 spark_angle) noexcept {
    if (cyl >= MAX_CYLINDERS) return;
    arm_volatile(&nodes_[cyl][ROLE_IGN_TR_SPARK], spark_angle);
}
void EventScheduler::arm_dwell(uint8_t cyl, AngleDeg10 dwell_start_angle) noexcept {
    if (cyl >= MAX_CYLINDERS) return;
    arm_volatile(&nodes_[cyl][ROLE_IGN_DWELL], dwell_start_angle);
}
void EventScheduler::arm_dwell_trailing(uint8_t cyl, AngleDeg10 dwell_start_angle) noexcept {
    if (cyl >= MAX_CYLINDERS) return;
    arm_volatile(&nodes_[cyl][ROLE_IGN_TR_DWELL], dwell_start_angle);
}
// Take a node out of the list without breaking the walk that may be standing on it. remove_event()
// nulls n->next, so a head left pointing at n would lose the rest of its bucket.
void EventScheduler::unlink_walk_node(SchedNode* n) noexcept {
    if (angle_a_ == n) angle_a_ = n->next;
    if (angle_b_ == n) angle_b_ = n->next;
    if (angle_c_ == n) angle_c_ = n->next;
    if (pending_angle_ == n) pending_angle_ = nullptr;
    remove_event(n);
}

// Is `angle` in the grid tooth being walked right now? The walk has already moved past the start of
// that bucket, so a node inserted there may land behind its head and not fire until the next cycle.
bool EventScheduler::in_current_tooth(AngleDeg10 angle) const noexcept {
    uint8_t b = bucket_of(angle), cur = bucket_of(tooth_angle_);
    if (!phase_known_ && buckets_per_rev_) { b %= buckets_per_rev_; cur %= buckets_per_rev_; }
    return b == cur;
}

// ---------------------------------------------------------------------------
// fit_dwell — at IGN_SCHEDULE, just after the spark was re-armed: make the dwell fit THAT spark.
//
// The dwell is armed a full cycle ahead, when the previous spark fires, at the OLD spark angle less
// the dwell angle at the OLD rpm. The spark is re-armed ~90 deg before TDC with fresh advance. When the
// advance jumps by more than the dwell angle — cranking table to main map is exactly that: about 5 deg
// of dwell at 250 rpm — the new spark came BEFORE the pending dwell. The spark then fired on a coil that
// was not charging, the dwell started after it, and nothing ended it for a whole cycle. And a spark
// with no dwell armed at all — the first after sync, or after any sync change — was simply lost.
//
// Now, if the coil is not already charging: a pending dwell is moved to where the fresh values put it,
// and one that is not armed is armed. When there is no room left before the spark, charge now: a short
// dwell this once, never an unfired cylinder. This also puts the dwell on the CURRENT rpm whenever its
// start lies after the schedule node, rather than on rpm a cycle old.
// ---------------------------------------------------------------------------
void EventScheduler::fit_dwell(uint8_t cyl, AngleDeg10 dwell_start, bool trailing) noexcept {
    if (cyl >= MAX_CYLINDERS || !firing_enabled_) return;
    SchedNode* dw = &nodes_[cyl][trailing ? ROLE_IGN_TR_DWELL : ROLE_IGN_DWELL];
    SchedNode* sp = &nodes_[cyl][trailing ? ROLE_IGN_TR_SPARK : ROLE_IGN_SPARK];
    if (dw->type != EventType::VOLATILE || sp->state != EventState::READY) return;   // no spark to fit
    const OutputMask coil = (phase_known_ ? dw->phase_mask : dw->crank_mask) & exec_ign_;
    if (!coil) return;
    if (dw->state != EventState::READY && (charging_ & coil)) return;   // charging already: the spark ends it

    const AngleDeg10 W   = phase_known_ ? cycle_angle() : static_cast<AngleDeg10>(ANGLE_360);
    const AngleDeg10 now = angle_wrap(angle_now(), W);
    const auto ahead = [&](AngleDeg10 a) {
        return angle_wrap(static_cast<AngleDeg10>(angle_wrap(a, W) - now), W);
    };
    const AngleDeg10 d_spark = ahead(sp->target_angle);
    const AngleDeg10 d_dwell = ahead(dwell_start);

    if (dw->state == EventState::READY) unlink_walk_node(dw);
    if (d_dwell < d_spark && !in_current_tooth(dwell_start)) {
        arm_volatile(dw, dwell_start);
        return;
    }
    if (d_spark > 0) {
        drive_ign(coil, true);
        ++dwell_started_late_;
    }
}

void EventScheduler::set_max_dwell_us(uint32_t us) noexcept {
    max_dwell_ticks_ = (ticks_per_second_ > 0)
        ? static_cast<uint32_t>(((uint64_t)us * ticks_per_second_) / 1000000ULL) : 0u;
}

void EventScheduler::arm_injection(uint8_t event, AngleDeg10 open_angle,
                                   uint32_t pw_us) noexcept {
    if (event >= inj_event_count_) return;
    inj_nodes_[event][ROLE_INJ_OPEN].time_domain_us = pw_us;
    arm_volatile(&inj_nodes_[event][ROLE_INJ_OPEN], open_angle);
}
void EventScheduler::arm_knock_window(uint8_t cyl, AngleDeg10 window_angle) noexcept {
    if (cyl >= MAX_CYLINDERS) return;
    arm_volatile(&nodes_[cyl][ROLE_KNOCK_WINDOW], window_angle);
}

// ---------------------------------------------------------------------------
// on_virtual_tooth — one grid tooth (ISR). Set up the tooth's angle walk and arm
// Timer 1. Velocity is NOT re-filtered here (RPM/dwell read the PLL's live
// ticks_per_vtooth directly). PHASE: one full-cycle bucket. CRANK: the lower-half
// bucket + its +rev twin — both already sorted — merged by the two-pointer walk.
// ---------------------------------------------------------------------------
void EventScheduler::on_virtual_tooth(uint16_t index, uint32_t tick,
                                      uint32_t ticks_per_vtooth) noexcept {
    if (buckets_per_rev_ == 0) return;

    // THE ANGLE CLOCK IS NOT PART OF FIRING. This used to return here when firing was disabled, so
    // tooth_tick_/tooth_tpv_/tooth_angle_ kept their constructor zeros and angle_at() answered 0 for
    // every caller — which is how the engine-cycle view came to stamp all 116 crank teeth and both
    // cam edges at 0 deg while the Virtual lane, which does not go through angle_at, spread across
    // the full 720. On a bench ECU with no outputs mapped, firing is never enabled, so the trigger
    // lanes of the one view meant for diagnosing a trigger were flat at zero permanently.
    //
    // firing_enabled_ gates DISPATCH — whether events go out — not whether the ECU knows where it
    // is. Knowing where it is, is precisely what a diagnostic wants when it is not firing.
    //
    // Seqlock write: bump to ODD, store, bump to EVEN. Compiler barriers only — this is a single
    // core, so a reader in a higher-priority ISR needs the STORES ordered, not cache coherence.
    tooth_seq_ = tooth_seq_ + 1;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    tooth_tick_ = tick;
    tooth_tpv_  = ticks_per_vtooth;
    // The interpolation base for angle_now(). Unsynced, the caller feeds a rev-folded index,
    // so this is a folded angle too — which the capture's sync_level tells the host about
    // rather than the angle silently pretending to be a full-cycle position.
    tooth_angle_ = static_cast<AngleDeg10>(static_cast<int32_t>(index) * vt_step_);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    tooth_seq_ = tooth_seq_ + 1;
    // MAXIMUM DWELL. Nothing used to bound how long a coil could stay charged: any path that started a
    // dwell and then lost its spark — a dropped event, a spark re-armed ahead of its dwell, sync lost
    // mid-charge — held the coil on until the NEXT spark, a whole cycle later (half a second while
    // cranking). A coil driven that long cooks itself and its driver. Checked every grid tooth, before
    // the firing gate so it still runs with firing off; a coil past the limit is released (it sparks,
    // at whatever angle this is — the price of not burning it).
    if (charging_ && max_dwell_ticks_) {
        const uint32_t now = angle_alarm_ ? angle_alarm_->now() : tick;
        for (OutputMask k = charging_; k; k &= (k - 1)) {
            const uint8_t ci = static_cast<uint8_t>(__builtin_ctz(k));
            if (now - charge_start_[ci] > max_dwell_ticks_) {
                drive_ign(OutputMask(1) << ci, false);
                ++overdwell_;
            }
        }
    }

    // FROM HERE DOWN IS DISPATCH, and that is what firing gates.
    if (!firing_enabled_) { angle_a_ = angle_b_ = angle_c_ = nullptr; pending_angle_ = nullptr; return; }

    // FINISH THE LAST TOOTH BEFORE STARTING THIS ONE. Every event still pending in the old walk sits at
    // an angle this tooth has already passed, so it is late — and it used to be DROPPED: the heads were
    // simply re-pointed at the new bucket. A dropped dwell is a missed cylinder; a dropped SPARK leaves
    // the coil charging for a whole cycle. It happens whenever an event's time lands after the next
    // tooth — the tooth pulled in by the clock's correction, an event late in its tooth, or the tooth
    // ISR simply serviced ahead of the event's alarm. Late by a fraction of a tooth beats not at all.
    flush_angle_walk();

    // Without the cam every REVOLUTION of the cycle is walked each revolution — two on a four-stroke,
    // THREE on a rotary. This walked only two, so a rotary at crank sync never visited 720-1080 deg:
    // anything scheduled there (a rotor with TDC at 0 has its schedule node at 990 and spark near 1060)
    // did not fire at all until the cam was seen.
    angle_c_ = nullptr;
    if (phase_known_) {
        angle_a_ = (index < MAX_VTEETH) ? schedule_[index] : nullptr;
        angle_b_ = nullptr;
    } else if (index < buckets_per_rev_) {
        const uint32_t cycle_buckets = (vt_step_ > 0) ? static_cast<uint32_t>(cycle_angle() / vt_step_) : 0u;
        const uint32_t b1 = index + buckets_per_rev_, b2 = index + 2u * buckets_per_rev_;
        angle_a_ = schedule_[index];
        angle_b_ = (b1 < cycle_buckets && b1 < MAX_VTEETH) ? schedule_[b1] : nullptr;
        angle_c_ = (b2 < cycle_buckets && b2 < MAX_VTEETH) ? schedule_[b2] : nullptr;
    } else {
        angle_a_ = angle_b_ = nullptr;
    }
    arm_next_angle();

    // Async transient injection train (latched from the task, fired HERE at TIM5 prio 2 so the open +
    // insert_close stay serialized with the other list ops). On latch, spread num_pulses squirts across
    // the cycle: fire one now-ish, the rest every async_spacing_ teeth. Only runs while synced + firing
    // (we returned early above if !firing_enabled_), so a stalled engine never delivers a stale burst.
    if (async_req_pending_) {
        async_req_pending_ = false;
        async_pulses_left_ = async_req_pulses_;
        async_pw_us_       = async_req_pw_us_;
        async_countdown_   = 0;                                       // first pulse on the next tooth
        // Teeth in ONE ENGINE CYCLE — derived from the configured span, not assumed to be two
        // revolutions. buckets_per_rev_ is teeth per 360 deg, so the old `* 2` was the four-stroke
        // answer hardcoded: a two-stroke (360 deg cycle) got spacing twice too wide and trailed its
        // burst into the NEXT cycle, and a rotary (1080) got it a third too narrow and crammed every
        // pulse into the first two thirds. Same family as the rev_per_intake bug in the fuel path.
        const uint16_t teeth_per_cycle = (vt_step_ > 0)
            ? static_cast<uint16_t>(cycle_angle() / vt_step_)
            : static_cast<uint16_t>(buckets_per_rev_);
        async_spacing_ = (async_req_pulses_ > 1)
            ? static_cast<uint16_t>(teeth_per_cycle / async_req_pulses_) : teeth_per_cycle;
        if (async_spacing_ == 0) async_spacing_ = 1;
    }
    if (async_pulses_left_ > 0) {
        if (async_countdown_ == 0) {
            fire_async_pulse();
            --async_pulses_left_;
            // spacing - 1, not spacing. The countdown is tested BEFORE it is decremented, so
            // reloading it with N puts the next pulse N+1 teeth away — the burst crept wider than
            // the spread it was asked for (3 pulses at 24 landed 25 apart). Guarded at 0 so a
            // spacing of 1 still fires every tooth rather than wrapping to 65535.
            async_countdown_ = (async_spacing_ > 0) ? static_cast<uint16_t>(async_spacing_ - 1) : 0;
        } else {
            --async_countdown_;
        }
    }
}

// Fire, in order, every event left in the current walk (see on_virtual_tooth). Bounded: a STATIC
// schedule node's compute hook arms volatiles ~90 deg ahead, never back into the tooth being flushed,
// and dispatch unlinks every volatile it fires — the cap is a guard, not a limit expected to bind.
void EventScheduler::flush_angle_walk() noexcept {
    pending_angle_ = nullptr;
    for (uint16_t guard = 0; guard < 64; ++guard) {
        SchedNode* n = peek_soonest_angle();
        if (!n) break;
        if      (n == angle_a_) angle_a_ = angle_a_->next;
        else if (n == angle_b_) angle_b_ = angle_b_->next;
        else if (n == angle_c_) angle_c_ = angle_c_->next;
        dispatch_fire(n);
        ++late_fired_;
    }
    angle_a_ = angle_b_ = angle_c_ = nullptr;
}

// Advance the two heads past any non-READY (already-fired/unlinked) nodes.
void EventScheduler::skip_dead_heads() noexcept {
    while (angle_a_ && angle_a_->state != EventState::READY) angle_a_ = angle_a_->next;
    while (angle_b_ && angle_b_->state != EventState::READY) angle_b_ = angle_b_->next;
    while (angle_c_ && angle_c_->state != EventState::READY) angle_c_ = angle_c_->next;
}

// Soonest (smallest sub_offset) of the two sorted heads.
SchedNode* EventScheduler::peek_soonest_angle() noexcept {
    skip_dead_heads();
    SchedNode* best = angle_a_;
    for (SchedNode* h : { angle_b_, angle_c_ })
        if (h && (!best || h->sub_offset < best->sub_offset)) best = h;
    return best;
}

// Arm Timer 1 for the next angle event, or go idle when the tooth is drained.
void EventScheduler::arm_next_angle() noexcept {
    SchedNode* n = peek_soonest_angle();
    pending_angle_ = n;
    if (n && angle_alarm_) angle_alarm_->arm(fire_tick(n));
}

// ---------------------------------------------------------------------------
// on_angle_alarm — Timer 1 fired. Drive the current angle event NOW, advance the
// walk, re-arm. Advance the source head BEFORE dispatch (a STATIC node's compute
// hook may insert into the same bucket; we must not revisit the new node).
// ---------------------------------------------------------------------------
void EventScheduler::on_angle_alarm() noexcept {
    // Cold-start prime consumed HERE (Timer ISR, priority 2) — NOT in the capture ISR (priority 1),
    // which preempts this one and would race the close-list mutation insert_close performs. At this
    // priority the prime's open + close-schedule are serialized with every other list op. During
    // cranking the ignition events keep this alarm firing, so the one-shot lands within an event.
    if (prime_req_pending_) { prime_req_pending_ = false; fire_prime(); }
    SchedNode* n = pending_angle_;
    if (!n) return;
    if      (n == angle_a_) angle_a_ = angle_a_->next;
    else if (n == angle_b_) angle_b_ = angle_b_->next;
    else if (n == angle_c_) angle_c_ = angle_c_->next;
    pending_angle_ = nullptr;
    dispatch_fire(n);
    arm_next_angle();
}

// ---------------------------------------------------------------------------
// dispatch_fire — act on one due angle node (ISR). STATIC schedule nodes call the
// compute hook (re-arm); VOLATILE fire nodes drive their mask NOW and unlink. A
// SPARK additionally re-arms the NEXT dwell (full cycle of lead).
// ---------------------------------------------------------------------------
void EventScheduler::dispatch_fire(SchedNode* n) noexcept {
    const OutputMask sel = phase_known_ ? n->phase_mask : n->crank_mask;

    // FIRING OFF STARTS NOTHING, BUT FINISHES WHAT IS STARTED. set_firing_enabled(false) leaves an
    // armed alarm armed, and this path used to act on it regardless: a dwell could begin after sync
    // was lost, to be cut ~1 ms later by emergency_off at an uncontrolled angle. Now no dwell, no
    // injector open and no re-arm happens with firing off — but a SPARK still fires, because the one
    // thing worse than a spark at a stale angle is a coil left charging with nothing to end it.
    if (!firing_enabled_ && n->action != EventAction::IGN_SPARK) {
        if (n->type == EventType::VOLATILE) remove_event(n);
        return;
    }

    switch (n->action) {
    case EventAction::IGN_SCHEDULE:
        if (compute_cb_) compute_cb_(compute_ud_, n->cyl_index, EventAction::IGN_SCHEDULE);
        return;   // STATIC — stays READY
    case EventAction::INJ_SCHEDULE:
        if (compute_cb_) compute_cb_(compute_ud_, n->cyl_index, EventAction::INJ_SCHEDULE);
        return;   // STATIC — stays READY

    case EventAction::IGN_DWELL_START: {
        const OutputMask exec_ign = exec_ign_;
        drive_ign(sel & exec_ign, true);    // dwell = charge (each coil at its own polarity)
        remove_event(n);
        return;
    }
    case EventAction::IGN_SPARK:
        drive_ign(sel, false);              // discharge unconditional (each coil at its own polarity)
        remove_event(n);
        if (compute_cb_) compute_cb_(compute_ud_, n->cyl_index, EventAction::IGN_DWELL_START);
        return;

    case EventAction::INJ_OPEN: {
        // SEQUENTIAL_ANY_SYNC keeps its promise of once per cycle in ANY sync: without the cam the walk
        // visits this event every revolution, so it opens on alternate visits (injection_deliveries_per_cycle).
        // (Not on a rotary: its faces already fold onto one opening a revolution — see the function.)
        if (!phase_known_ && !is_rotary() && n->cyl_index < MAX_INJ_EVENTS && inj_events_[n->cyl_index].stage < MAX_INJ_STAGES &&
            static_cast<InjectionMode>(cyl_cfg_.inj_stage[inj_events_[n->cyl_index].stage].mode)
                == InjectionMode::SEQUENTIAL_ANY_SYNC) {
            uint8_t& byte = any_sync_skip_[n->cyl_index >> 3];
            const uint8_t bit = static_cast<uint8_t>(1u << (n->cyl_index & 7u));
            byte ^= bit;
            if (byte & bit) { remove_event(n); return; }
        }
        const OutputMask exec_inj = exec_inj_;
        const OutputMask m = sel & exec_inj;
        const uint32_t pw_ticks = (ticks_per_second_ > 0 && n->time_domain_us > 0)
            ? static_cast<uint32_t>(((uint64_t)n->time_domain_us * ticks_per_second_) / 1000000ULL)
            : 0u;
        remove_event(n);
        // NO CLOSE, NO OPEN. This opened first and then asked for a close slot; with the pool empty the
        // injector stayed open until its next pulse — a cylinder full of fuel. fire_async_pulse always
        // had this guard. A skipped squirt is lean for one cycle; a stuck one is a hydrolock.
        if (!m || !pw_ticks) return;
        if (!time_free_) { ++inj_no_close_; return; }
        drive_inj(m, true);   // open = energize (each injector at its own polarity)
        const uint32_t now = angle_alarm_ ? angle_alarm_->now() : tooth_tick_;
        insert_close(now + pw_ticks, m);
        return;
    }

    case EventAction::ADC_TRIGGER:
        // Knock sampling window: hand the firing cylinder to the compute hook (which arms the ADC
        // burst — no pin driven). One-shot: re-armed next cycle by the compute hook.
        remove_event(n);
        if (compute_cb_) compute_cb_(compute_ud_, n->cyl_index, EventAction::ADC_TRIGGER);
        return;

    default:
        remove_event(n);
        return;
    }
}

// ---------------------------------------------------------------------------
// drive_ign / drive_inj — immediate per-pin output (force_output_now) for each
// set bit of the mask. Each bit resolves to the user's pin via its ITimerChannel.
// ---------------------------------------------------------------------------
//
// These are also the engine-cycle capture point, and deliberately so: EVERY coil and
// injector edge in the scheduler goes through here — the angle-scheduled dwell/spark and
// injector open, the time-list close, the cold-start prime and the async transient train.
// Recording at the source means the capture cannot drift from what the pins did, and a
// future output path gets recorded by construction rather than by remembering to add it.
//
// What is recorded is the LOGICAL state (energized or not), not the pin level: polarity is a
// property of each output row and is resolved per channel, here, where the pin is driven.
void EventScheduler::drive_ign(OutputMask m, bool energize) noexcept {
    const bool rec    = recorder_ && recorder_->recording();
    const AngleDeg10 a = rec ? angle_now() : 0;
    if (energize) {
        const uint32_t now = angle_alarm_ ? angle_alarm_->now() : tooth_tick_;
        for (OutputMask k = m; k; k &= (k - 1)) charge_start_[__builtin_ctz(k)] = now;
        charging_ = charging_ | m;
    } else {
        charging_ = charging_ & ~m;
    }
    while (m) {
        const uint8_t ci = static_cast<uint8_t>(__builtin_ctz(m));
        m &= (m - 1);
        ITimerChannel* hw = ign_hw(ci);
        if (hw) hw->force_output_now(ign_level(ci, energize));
        if (rec) recorder_->record_output(CycleSignal::Coil, ci, a, energize);
    }
}

void EventScheduler::drive_inj(OutputMask m, bool energize) noexcept {
    const bool rec    = recorder_ && recorder_->recording();
    const AngleDeg10 a = rec ? angle_now() : 0;
    while (m) {
        const uint8_t ci = static_cast<uint8_t>(__builtin_ctz(m));
        m &= (m - 1);
        ITimerChannel* hw = inj_hw(ci);
        if (hw) hw->force_output_now(inj_level(ci, energize));
        if (rec) recorder_->record_output(CycleSignal::Injector, ci, a, energize);
    }
}

// ---------------------------------------------------------------------------
// angle_now — engine angle at this instant, interpolated inside the current tooth.
// ---------------------------------------------------------------------------
AngleDeg10 EventScheduler::angle_now() const noexcept {
    return angle_alarm_ ? angle_at(angle_alarm_->now()) : tooth_angle_;
}

// ---------------------------------------------------------------------------
// angle_at — the believed engine angle at an arbitrary tick, torn-read safe.
// ---------------------------------------------------------------------------
AngleDeg10 EventScheduler::angle_at(uint32_t tick) const noexcept {
    uint32_t base_tick = 0, tpv = 0;
    AngleDeg10 base_angle = 0;
    // Retry a torn read a couple of times, then fall back to the tooth angle. Looping forever in an
    // ISR to win a race against a lower-priority one is how a capture turns into a hang.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t s0 = tooth_seq_;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        base_tick  = tooth_tick_;
        tpv        = tooth_tpv_;
        base_angle = tooth_angle_;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        if ((s0 & 1u) == 0u && tooth_seq_ == s0) break;   // stable and unchanged
        if (attempt == 2) return tooth_angle_;
    }
    if (tpv == 0 || vt_step_ <= 0) return base_angle;
    const uint32_t d = tick - base_tick;
    // More than a few teeth past the last grid tooth means the grid has stopped feeding us
    // (stall, sync loss) and the tick delta no longer maps to an angle. Report the last known
    // tooth rather than extrapolating a position the engine may never have reached.
    // 32-BIT ON PURPOSE. The guard bounds d < 4*tpv, so the product below is < 4*tpv*vt_step_ —
    // 22 M at the 30 rpm stall floor on a 10 deg grid, and 8e8 in the worst coarse-grid case, both
    // inside 2^32. The uint64_t casts here were defensive rather than necessary, and they pulled
    // __aeabi_uldivmod (a 64-bit SOFTWARE divide, ~40-90 cycles) onto the priority-1 capture path
    // that stamps every trigger edge. 32-bit uses the M7's hardware UDIV instead.
    //
    // `d / 4 >= tpv` is the same test as `d >= 4 * tpv` for unsigned values, and cannot overflow
    // even if tpv were absurd — which `4 * tpv` could.
    if (d / 4u >= tpv) return base_angle;
    const uint32_t adv = (d * static_cast<uint32_t>(vt_step_)) / tpv;
    return angle_wrap(static_cast<AngleDeg10>(base_angle + adv), cycle_angle());
}

// ---------------------------------------------------------------------------
// fire_prime — open every ACTIVE injector simultaneously and schedule the close after pw_us, reusing
// the normal injector drive + time-list close. MUST run in the Timer ISR (priority 2, via
// on_angle_alarm), NOT the capture ISR (priority 1) — insert_close mutates the close-list the timer
// ISRs own, so it has to be at that priority to stay serialized. The mask is the same active-injector
// set the angle-scheduled INJ_OPEN path uses (exec_inj_); `now` is read the same way too.
// ---------------------------------------------------------------------------
void EventScheduler::fire_prime() noexcept {
    const OutputMask all_inj = exec_inj_;
    if (!all_inj) return;
    const uint32_t pw_ticks = (ticks_per_second_ > 0 && prime_req_pw_us_ > 0)
        ? static_cast<uint32_t>(((uint64_t)prime_req_pw_us_ * ticks_per_second_) / 1000000ULL)
        : 0u;
    if (!pw_ticks) return;                           // zero/invalid pw → nothing to deliver
    if (!time_free_) { ++inj_no_close_; return; }    // no close slot → don't open (see INJ_OPEN)
    drive_inj(all_inj, true);                        // open = energize (per-injector polarity)
    const uint32_t now = angle_alarm_ ? angle_alarm_->now() : tooth_tick_;
    insert_close(now + pw_ticks, all_inj);
}

// ---------------------------------------------------------------------------
// fire_async_pulse — one asynchronous transient squirt: open every active injector and schedule the
// close, exactly like fire_prime, but GUARDED on a free close-list slot first. If no slot is free we
// skip the OPEN entirely — an async pulse must never leave an injector energized with no scheduled
// close (that would flood). Runs in the Timer ISR (prio 2, from on_virtual_tooth), same as fire_prime.
// ---------------------------------------------------------------------------
void EventScheduler::fire_async_pulse() noexcept {
    if (!time_free_) return;                          // no close slot → don't open (never leave stuck open)
    const OutputMask all_inj = exec_inj_;
    if (!all_inj) return;
    const uint32_t pw_ticks = (ticks_per_second_ > 0 && async_pw_us_ > 0)
        ? static_cast<uint32_t>(((uint64_t)async_pw_us_ * ticks_per_second_) / 1000000ULL)
        : 0u;
    if (!pw_ticks) return;                            // zero pw → nothing to deliver
    drive_inj(all_inj, true);                        // open = energize (per-injector polarity)
    const uint32_t now = angle_alarm_ ? angle_alarm_->now() : tooth_tick_;
    insert_close(now + pw_ticks, all_inj);           // slot guaranteed (time_free_ checked above)
}

// ---------------------------------------------------------------------------
// insert_close — add an injector-close (drive LOW) to the sorted time list and
// (re)arm Timer 2 if it became the head. One free slot per injector channel.
// ---------------------------------------------------------------------------
void EventScheduler::insert_close(uint32_t tick, OutputMask mask) noexcept {
    if (!time_free_) return;                 // pool exhausted (extreme overlap)
    TimeEvt* e = time_free_;
    time_free_ = e->next;
    e->tick = tick;
    e->mask = mask;
    // Sorted insert by tick (signed wrap-safe).
    TimeEvt** pp = &time_head_;
    while (*pp && (int32_t)((*pp)->tick - tick) <= 0) pp = &(*pp)->next;
    e->next = *pp;
    *pp = e;
    if (time_head_ == e && time_alarm_) time_alarm_->arm(e->tick);
}

// ---------------------------------------------------------------------------
// on_time_alarm — Timer 2 fired. Drive the head injector-close, advance, re-arm.
// ---------------------------------------------------------------------------
void EventScheduler::on_time_alarm() noexcept {
    TimeEvt* e = time_head_;
    if (!e) return;
    time_head_ = e->next;
    drive_inj(e->mask, false);   // close = de-energize (per-injector polarity)
    e->next = time_free_;                    // return to the pool
    time_free_ = e;
    if (time_head_ && time_alarm_) time_alarm_->arm(time_head_->tick);
}

// ---------------------------------------------------------------------------
// emergency_off — ISR-safe panic stop. Disable firing + every pending match and
// force all outputs LOW. Touches NO list pointers, so it is safe from the capture
// ISR which can preempt the dispatch ISR.
// ---------------------------------------------------------------------------
void EventScheduler::emergency_off() noexcept {
    firing_enabled_ = false;
    charging_ = 0;
    if (angle_alarm_) angle_alarm_->disarm();
    if (time_alarm_)  time_alarm_->disarm();
    for (uint8_t i = 0; i < MAX_IGN_CHANNELS; ++i) {
        if (ign_channels_[i]) ign_channels_[i]->force_output_now(ign_level(i, false));  // de-energized
    }
    for (uint8_t i = 0; i < MAX_INJ_CHANNELS; ++i) {
        if (inj_channels_[i]) inj_channels_[i]->force_output_now(inj_level(i, false));  // de-energized
    }
}

// ---------------------------------------------------------------------------
// quiesce_volatiles — task-context quiesce. emergency_off() + unlink every
// VOLATILE node. The STATIC skeleton stays intact. Do NOT call from the capture
// ISR (it mutates the intrusive list).
// ---------------------------------------------------------------------------
void EventScheduler::quiesce_volatiles() noexcept {
    emergency_off();
    for (uint8_t c = 0; c < MAX_CYLINDERS; ++c) {
        for (uint8_t r = 0; r < SCHED_NODES_PER_CYLINDER; ++r) {
            SchedNode& n = nodes_[c][r];
            if (n.type == EventType::VOLATILE && n.state != EventState::UNSCHEDULED) {
                remove_event(&n);
            }
        }
    }
    for (uint8_t e = 0; e < MAX_INJ_EVENTS; ++e) {
        for (uint8_t r = 0; r < SCHED_NODES_PER_INJ_EVENT; ++r) {
            SchedNode& n = inj_nodes_[e][r];
            if (n.type == EventType::VOLATILE && n.state != EventState::UNSCHEDULED) {
                remove_event(&n);
            }
        }
    }
    // Reset the angle walk + time list.
    angle_a_ = angle_b_ = angle_c_ = pending_angle_ = nullptr;
    time_head_ = nullptr;
    for (uint8_t i = 0; i < MAX_INJ_CHANNELS; ++i)
        time_pool_[i].next = (i + 1 < MAX_INJ_CHANNELS) ? &time_pool_[i + 1] : nullptr;
    time_free_ = &time_pool_[0];
}

