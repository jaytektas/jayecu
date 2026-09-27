// Does the ECU know the engine has STOPPED?
//
// It did not. A bench ECU held sync_level=PHASE, rpm=1348 and engine_state=RUNNING for forty
// minutes with no trigger wire connected to anything, and the fuel pump stayed energised the whole
// time. Nothing was broken in the sense of a wrong calculation: every layer of the decoder is
// driven from on_edge(), so when the edges stop, nothing runs, nothing changes, and the last belief
// stands for ever. The only stall detector in the firmware lived inside the firing clock's match
// ISR — and every sync transition resets that clock, so between a CRANK<->PHASE change and the next
// real tooth there was no detection at all.
//
// These tests drive EnginePositionHal with a fake capture pin, a fake timebase and a fake alarm,
// with NO angle clock at all (vtrig_feed/vtrig_reconfigure are weak and null in a host build). That
// is not a limitation of the harness, it is the point: liveness must not depend on the firing clock
// existing, because the firing clock is exactly what was missing when this went wrong.
bool g_system_active = true;                 // key on: the capture path's own gate

#include "test_helpers.h"
#include "output_rows_helper.h"
#include "Scheduler/EnginePositionHal.h"
#include "ecu_config.h"

// ---------------------------------------------------------------------------------------------
// ONE CLOCK. On jaytek_v1 the capture timestamps and the alarm's now() are both TIM5->CNT, and the
// whole comparison depends on that: an earlier watchdog stamped with the capture tick and compared
// against a different timer's count, and the unsigned wrap fired it every single tooth.
// ---------------------------------------------------------------------------------------------
static constexpr uint32_t TPS = 1000000;     // 1 MHz / 1 us, as the board
static uint32_t g_clock = 0;

struct FakeTimebase final : ITimerChannel {
    uint32_t get_current_ticks()    const noexcept override { return g_clock; }
    uint32_t get_ticks_per_second() const noexcept override { return TPS; }
    void     force_output_now(OutputAction) noexcept override {}
};

struct FakeCapture final : ICaptureChannel {
    CaptureCallback cb = nullptr; void* ud = nullptr;
    bool enabled = false; uint32_t last = 0;
    bool level = false;             // toggles per edge when `alternating`
    bool alternating = false;       // a BOTH-edge capture reports the polarity of each edge
    uint32_t    get_last_capture_ticks() const noexcept override { return last; }
    CaptureEdge get_edge_polarity()      const noexcept override {
        return (!alternating || level) ? CaptureEdge::RISING : CaptureEdge::FALLING; }
    bool        get_current_level()      const noexcept override { return level; }
    void register_callback(CaptureCallback c, void* u) noexcept override { cb = c; ud = u; }
    void set_capture_enabled(bool e) noexcept override { enabled = e; }
    // An edge at the current clock: this is the capture ISR.
    void edge() { if (alternating) level = !level; last = g_clock; if (enabled && cb) cb(g_clock, ud); }
};

struct FakeAlarm final : IAlarmTimer {
    uint32_t deadline = 0; bool armed = false;
    MatchCallback cb = nullptr; void* ud = nullptr;
    uint32_t arms = 0;
    void arm(uint32_t t) noexcept override { deadline = t; armed = true; ++arms; }
    void disarm() noexcept override { armed = false; }
    uint32_t now() const noexcept override { return g_clock; }
    void register_callback(MatchCallback c, void* u) noexcept override { cb = c; ud = u; }
};

static FakeTimebase g_tb;
static FakeCapture  g_crank;
static FakeCapture  g_cam;
static FakeAlarm    g_tooth;
static FakeAlarm    g_dco;

// Run the clock forward to `target`, firing the alarm at every deadline it crosses. Stands in for
// the TIM5 compare ISR, including its "already past" behaviour.
// BOTH alarms, in tick order. On the board the DCO (CCR1) and the tooth deadline (CCR4) share
// TIM5 and interleave; running only the deadline here meant the angle clock never emitted, which
// is why get_rpm_x10() was zero in every section below whatever the decoder believed.
static void advance_to(uint32_t target) {
    int guard = 0;
    for (;;) {
        FakeAlarm* next = nullptr;
        if (g_tooth.armed && static_cast<int32_t>(g_tooth.deadline - target) <= 0) next = &g_tooth;
        if (g_dco.armed && static_cast<int32_t>(g_dco.deadline - target) <= 0 &&
            (!next || static_cast<int32_t>(g_dco.deadline - next->deadline) < 0)) next = &g_dco;
        if (!next) break;
        if (static_cast<int32_t>(next->deadline - g_clock) > 0) g_clock = next->deadline;
        next->armed = false;                      // one-shot; the callback may re-arm
        if (next->cb) next->cb(next->ud);
        if (++guard > 200000) { CHECK(!"runaway alarm loop"); return; }
    }
    g_clock = target;
}
static void advance_by(uint32_t dt) { advance_to(g_clock + dt); }

// ---------------------------------------------------------------------------------------------
// THE ANGLE CLOCK, WIRED. vtrig_feed/vtrig_reconfigure are weak and resolved to null here, so
// grid_tpv_ was never written and get_rpm_x10() returned 0 in every section — including the ones
// that assert it IS 0 after the teeth stop. Those passed because rpm was zero the whole time, not
// because anything zeroed it. A real VirtualTrigger on a fake DCO alarm, as main.cpp builds it,
// makes rpm a live number, so "the velocity dies with the sync" is a claim that can now fail.
// ---------------------------------------------------------------------------------------------
#include "Scheduler/VirtualTrigger.h"
static VirtualTrigger     g_vtrig(g_dco, ANGLE_360, 36);
static EnginePositionHal* g_hal = nullptr;

static void vtrig_dco_match(void*) noexcept { g_vtrig.on_dco_match(); }
static void vtrig_vtooth(const VirtualToothData& vt, void*) noexcept {
    if (g_hal) g_hal->on_grid_tooth(vt.index, vt.angle, vt.tick, vt.ticks_per_vtooth,
                                    g_vtrig.vtooth_angle());
}
void vtrig_feed(AngleDeg10 angle, uint32_t ticks, uint32_t T0,
                AngleDeg10 tooth_angle, bool last_was_gap) noexcept {
    g_vtrig.on_real_tooth(angle, ticks, T0, tooth_angle, last_was_gap);
}
void vtrig_reconfigure(AngleDeg10 cycle_angle, uint16_t teeth) noexcept {
    g_vtrig.reconfigure(cycle_angle, teeth);
}

// ---------------------------------------------------------------------------------------------
// A 36-1 crank wheel on stream 0. One tooth is 100 deci-deg; at 600 rpm one crank revolution is
// 100 ms, so a tooth is 100000/36 ~= 2777 us. Round numbers matter less than the ratio: what the
// deadline judges is the interval against the interval the decoder itself predicted.
// ---------------------------------------------------------------------------------------------
static constexpr uint32_t TOOTH_TICKS = 2777;      // ~600 rpm on a 36-tooth base

// THE FIRING GATE HAS FIVE PRECONDITIONS, and the firing order is one of them
// (update_firing_enable: gate && config_ceiling != NONE && firing_order_valid && armed_epoch ==
// sync_epoch && sync != NONE). The default engine config does NOT carry a valid firing order, so
// without this every `CHECK(!firing_enabled())` in this file passed for the wrong reason — firing
// was never enabled in the first place, and a regression that left it ON would not have been seen.
static void configure_engine() {
    EngineConfig& e = g_config.engine;
    e.cylinder_count = 4;
    const uint8_t order[4] = {1, 3, 4, 2};
    for (unsigned i = 0; i < ENGINE_FIRING_ORDER_COUNT; ++i)
        e.firing_order[i].cyl = (i < 4) ? order[i] : 0;
    for (unsigned c = 0; c < 4; ++c) { e.cyl[c].bank = 0; e.cyl[c].tdc_angle = (int16_t)(c * 1800); }
    // A coil and an injector per cylinder: without output rows nothing is bound to fire, and the
    // cut/halt cases below are about what the decoder does to an engine that WAS firing.
    e.num_inj_stages = 1;
    e.inj_stage[0].mode = static_cast<uint8_t>(InjectionMode::SEQUENTIAL);
    layout_output_rows(e, TestCoils::COP, g_config.outputs.output, OUTPUTS_OUTPUT_COUNT);
}

static void configure_wheel() {
    TriggerConfig& t = g_config.trigger;
    for (auto& st : t.streams) { st.enabled = 0; st.capture_index = 255; }
    StreamsConfig& s = t.streams[0];               // slot 0 = Crank Primary
    s.enabled       = 1;
    s.capture_index = 0;
    s.edge          = 0;                           // rising
    s.primitive     = 0;                           // GAP
    s.slots         = 36;
    s.gap_ratio     = 2;                           // 36-1
    s.cell_len      = 1;
    s.cell[0].v     = 0;                           // the gap follows present-tooth 0
    s.window_pct    = 25;
    s.repeats       = 2;                           // once per crank revolution, four-stroke
}

// The same wheel with a half-moon WIDTH cam on slot 2 — a cam-rate stream (repeats == 1), which
// is what the two regression sections at the bottom of this file need: a second stream that can
// still produce edges after the crank has stopped.
// The GM Gen IV 4x cam, because it is a wheel the library actually carries: four pulses per cycle
// whose rising edges fall 180, 60, 180 and 300 crank degrees apart. A SEQUENCE cam is the case that
// matters here — it identifies itself from its OWN intervals, so unlike a WIDTH cam it can lock
// with the crank dead, which is precisely when it must not be allowed to speak for the engine.
static constexpr AngleDeg10 CAM_CELL[4] = { 1800, 600, 1800, 3000 };
static void configure_cam() {
    StreamsConfig& c = g_config.trigger.streams[2];    // slot 2 = Cam 1
    c.enabled       = 1;
    c.capture_index = 1;
    c.edge          = 0;                               // rising
    c.primitive     = 1;                               // SEQUENCE
    c.cell_len      = 4;
    for (int i = 0; i < 4; ++i) c.cell[i].v = CAM_CELL[i];
    c.nominal_angle = 0;
    c.window_pct    = 25;
    c.repeats       = 1;                               // once per ENGINE CYCLE
}

// Feed n present teeth, walking the wheel so the gap lands where the wheel says it does.
struct Wheel {
    int slot = 0; bool started = false;
    enum { LAST_PRESENT = 34 };
    void tooth() {
        if (!started) started = true;
        else {
            advance_by((slot == LAST_PRESENT) ? 2 * TOOTH_TICKS : TOOTH_TICKS);
            slot = (slot == LAST_PRESENT) ? 0 : slot + 1;
        }
        g_crank.edge();
    }
    void spin(int n) { for (int i = 0; i < n; i++) tooth(); }
    void spin_to(int target) { while (slot != target) tooth(); }
    // Spin with the 1 kHz frame running, as the engine task does. This is the shape that matters:
    // the deadline is LIVE the whole time, so a healthy engine is continuously proving that it
    // re-arms per tooth. Spinning without service() only ever exercises the arm at a sync
    // transition, and a stale deadline would sail through it.
    void spin_serviced(EnginePositionHal& hal, int n) {
        for (int i = 0; i < n; i++) { tooth(); hal.service(); }
    }
};

int main() {
    fprintf(stdout, "=== Trigger liveness: does the ECU know the engine stopped? ===\n");

    EcuHardwareAssignment as{};
    as.timebase      = &g_tb;
    as.crank_primary = &g_crank;
    as.cam[0]        = &g_cam;
    as.cam_nominal_angle[0] = 900;
    as.tooth_alarm   = &g_tooth;

    configure_wheel();
    configure_cam();
    configure_engine();
    EnginePositionHal hal(as, g_config.trigger, g_config.engine, g_config.outputs);
    g_hal = &hal;
    g_dco.register_callback(vtrig_dco_match, nullptr);
    g_vtrig.register_callback(vtrig_vtooth, nullptr);
    hal.set_firing_gate(true);
    hal.start();

    Wheel w;

    SECTION("a turning engine syncs, and the deadline never fires while teeth keep coming");
    {
        w.spin_serviced(hal, 200);                       // acquire (a couple of revolutions)
        CHECK(hal.get_sync_level() != SyncLevel::NONE);

        // THE PER-TOOTH ARM, measured once the wheel is locked so acquisition does not muddy the
        // count. Not a nicety: a deadline armed once at the sync transition and never refreshed
        // expires under a perfectly healthy engine and cries missed tooth at it. One arm per
        // position tooth, and not one false report in two hundred, is what says the deadline
        // follows the wheel rather than a moment in its past.
        const uint32_t arms_before   = g_tooth.arms;
        const uint32_t missed_before = hal.get_decoder_telemetry().missed_total;
        w.spin_serviced(hal, 200);
        CHECK(g_tooth.arms >= arms_before + 200);
        CHECK(hal.get_decoder_telemetry().missed_total == missed_before);
        CHECK(hal.get_decoder_telemetry().trigger_absent == false);
        // No cam edge has been fed: there is no cam angle, and VVT must be told so rather than read 0.
        CHECK(!hal.get_vvt_result(0).valid);
    }

    SECTION("THE REGRESSION: teeth stop and the ECU notices — with no firing clock running at all");
    {
        // This is the shape of the bench failure. In a host build there is no VirtualTrigger DCO
        // (the weak hooks are null), which is exactly the state a sync transition used to leave the
        // real one in — unlocked and disarmed. Under the old design nothing could ever notice from
        // here, and sync/rpm would stand for ever. Nothing is fed from this point on.
        CHECK(hal.get_sync_level() != SyncLevel::NONE);

        // One crank revolution at the 30 rpm floor is 2 s; the deadline is clamped to that, so
        // three seconds of silence is unambiguous.
        advance_by(3 * TPS);
        hal.service();

        CHECK(hal.get_sync_level() == SyncLevel::NONE);
        CHECK(hal.get_rpm_x10() == 0);                     // and not a stale grid velocity
        CHECK(hal.get_decoder_telemetry().trigger_absent == true);
        CHECK(hal.get_decoder_telemetry().current_rpm_x10 == 0);
    }

    SECTION("it re-acquires from the next teeth — no reconfigure, no reset");
    {
        w = Wheel{};
        w.spin_serviced(hal, 200);
        CHECK(hal.get_sync_level() != SyncLevel::NONE);
        CHECK(hal.get_decoder_telemetry().trigger_absent == false);   // healed by a real tooth
    }

    SECTION("...and the deadline works a SECOND time (it re-arms on re-acquisition)");
    {
        advance_by(3 * TPS);
        hal.service();
        CHECK(hal.get_sync_level() == SyncLevel::NONE);
        CHECK(hal.get_decoder_telemetry().trigger_absent == true);
    }

    SECTION("ONE overdue tooth cuts — there is no count at which it stops mattering");
    {
        w = Wheel{};
        w.spin_serviced(hal, 200); w.spin_to(10);
        CHECK(hal.get_sync_level() != SyncLevel::NONE);
        const uint32_t missed_before = hal.get_decoder_telemetry().missed_total;

        // Barely past the window the schedule predicted, and nowhere near "the engine has stopped".
        // This used to be a tolerated miss: reported, ridden through, still firing. But a tooth
        // outside its window means the decoder cannot prove where the engine is, and an engine of
        // unknown position must not go on making sparks — not for eight teeth, not for one.
        advance_by(TOOTH_TICKS * 2);
        CHECK(hal.get_sync_level() == SyncLevel::NONE);
        CHECK(hal.get_rpm_x10() == 0);
        CHECK(hal.get_decoder_telemetry().missed_total == missed_before + 1);
        // ...and it is reported as a missed TOOTH, not as a dead wire. At this instant the two are
        // indistinguishable and only time can separate them.
        CHECK(hal.get_decoder_telemetry().trigger_absent == false);
    }

    SECTION("the diagnosis upgrades to ABSENT once the silence outlasts the floor");
    {
        // The cut has already happened. What is still open is what to tell the operator: a tooth
        // that did not arrive and a trigger wire that is gone need the same cut but send you to two
        // completely different places.
        advance_by(3 * TPS);
        CHECK(hal.get_decoder_telemetry().trigger_absent == true);
        CHECK(hal.get_sync_level() == SyncLevel::NONE);
    }

    SECTION("the cut is a COUNT OF TEETH, so it means the same thing at every speed");
    {
        // Measured on the rig before this: a fixed two-second floor was 11 revolutions of firing
        // into a dead trigger at 400 rpm and 142 at 5000, because the threshold never moved with
        // the engine. Scaling to the wheel's own predicted interval is what makes "stopped" mean
        // one thing everywhere.
        w = Wheel{};
        w.spin_serviced(hal, 200);
        CHECK(hal.get_sync_level() != SyncLevel::NONE);

        const uint32_t t0 = g_clock;
        int guard = 0;
        while (hal.get_sync_level() != SyncLevel::NONE && guard++ < 400) advance_by(TOOTH_TICKS / 8);
        const uint32_t elapsed = g_clock - t0;
        CHECK(hal.get_sync_level() == SyncLevel::NONE);
        // 1.5 tooth-times (the same window the matcher applies to an edge that DOES arrive), give
        // or take the eighth-of-a-tooth polling granularity.
        CHECK(elapsed >= TOOTH_TICKS && elapsed <= TOOTH_TICKS * 2);
        // ...and three orders of magnitude short of the old fixed floor.
        CHECK(elapsed < TPS / 100);
    }

    SECTION("a tooth outside the window HALTS THE SCHEDULER, in the capture ISR");
    {
        // Not on the next 1 kHz service pass. The matcher judges the edge in the capture ISR, and if
        // firing is only recomputed a frame later then at 6000 rpm there are up to 36 degrees of
        // crank spent firing on a position the decoder has already stopped trusting.
        w = Wheel{};
        w.spin_serviced(hal, 200);
        CHECK(hal.get_sync_level() != SyncLevel::NONE);
        hal.set_firing_gate(true);
        hal.arm_firing(hal.firing_epoch());          // pretend a schedule is committed
        hal.service();
        CHECK(hal.scheduler().firing_enabled());     // it was ON, so the check below means something

        // A tooth 1.9 pitches out — outside the window at any rpm.
        g_clock = g_crank.last + TOOTH_TICKS * 19 / 10;
        g_crank.edge();

        CHECK(hal.get_sync_level() == SyncLevel::NONE);
        CHECK(hal.get_rpm_x10() == 0);
        // The scheduler is stopped BEFORE any task has run. No service() call between the edge and
        // this check — that is the whole point.
        CHECK(!hal.scheduler().firing_enabled());
    }

    SECTION("a tooth that NEVER comes halts the scheduler too, at the deadline (audit T8)");
    {
        // The same rule as the edge path above, for the tooth that does not arrive. The deadline
        // dropped sync but left firing on until the next service() — a frame of sparks on a position
        // the decoder had just given up.
        w = Wheel{};
        w.spin_serviced(hal, 200);
        CHECK(hal.get_sync_level() != SyncLevel::NONE);
        hal.set_firing_gate(true);
        hal.arm_firing(hal.firing_epoch());
        hal.service();
        CHECK(hal.scheduler().firing_enabled());
        advance_by(TOOTH_TICKS * 2);                 // no tooth: the deadline fires
        CHECK(hal.get_sync_level() == SyncLevel::NONE);
        CHECK(!hal.scheduler().firing_enabled());    // no service() in between
    }

    SECTION("a CUT survives a sync loss and re-acquisition — the decoder must not un-cut");
    {
        // Sync recovery re-enables FIRING. It must not, on the way past, re-enable an output that
        // something else is deliberately holding off — a rev limiter, over-boost protection, a
        // pre-ignition cylinder cut. Those are two independent pieces of state and the only thing
        // keeping them independent is that set_firing_enabled() writes firing_enabled_ while the
        // cuts write execution_mask_, and recompute_execution_mask() never reads the former.
        //
        // Worth pinning because the failure is silent and specific: an engine bouncing off the
        // limiter is also an engine whose trigger is being shaken hardest, so "drop sync, recover,
        // and hand the spark back mid-limiter" is a plausible sequence rather than a contrived one.
        w = Wheel{};
        w.spin_serviced(hal, 200);
        CHECK(hal.get_sync_level() != SyncLevel::NONE);
        hal.set_firing_gate(true);
        hal.arm_firing(hal.firing_epoch());
        hal.service();
        CHECK(hal.scheduler().firing_enabled());

        const uint64_t uncut = hal.scheduler().execution_mask();
        hal.scheduler().set_output_cuts(true, false);        // the rev limiter comes on
        hal.scheduler().set_cylinder_cuts(0b0010);           // and cylinder 1 is pre-igniting
        const uint64_t cut = hal.scheduler().execution_mask();
        CHECK(cut != uncut);                                 // the cut actually did something

        // Now break the trigger: a tooth 1.9 pitches out drops sync and halts firing in the ISR.
        g_clock = g_crank.last + TOOTH_TICKS * 19 / 10;
        g_crank.edge();
        CHECK(hal.get_sync_level() == SyncLevel::NONE);
        CHECK(!hal.scheduler().firing_enabled());
        CHECK(hal.scheduler().execution_mask() == cut);       // losing sync does not lift a cut

        // ...and re-acquire cleanly.
        w = Wheel{};
        w.spin_serviced(hal, 200);
        CHECK(hal.get_sync_level() != SyncLevel::NONE);
        hal.set_firing_gate(true);
        hal.arm_firing(hal.firing_epoch());
        hal.service();
        CHECK(hal.scheduler().firing_enabled());              // firing IS handed back
        CHECK(hal.scheduler().execution_mask() == cut);        // but the cut is STILL asserted
        CHECK(hal.scheduler().cylinder_cuts() == 0b0010);

        // Only the thing that raised it may lower it.
        hal.scheduler().set_output_cuts(false, false);
        hal.scheduler().set_cylinder_cuts(0);
        CHECK(hal.scheduler().execution_mask() == uncut);
    }

    // -----------------------------------------------------------------------------------------
    // A SECOND STREAM IS STILL A STREAM, AND ITS EDGES ARE NOT TEETH.
    //
    // Both of these were measured against this harness and both failed before the two lines they
    // test. The shape is the one this whole file exists for: the crank is dead and the ECU does
    // not know it. What is new is that it no longer takes silence to hide it — a cam that is
    // still turning, or a floating input on a bench with nothing plugged into it, is enough.
    // -----------------------------------------------------------------------------------------
    SECTION("a cam edge is not a crank tooth: it must not re-arm the deadline");
    {
        w.spin_serviced(hal, 200);
        CHECK(hal.get_sync_level() != SyncLevel::NONE);
        CHECK(hal.get_rpm_x10() > 0);                  // a LIVE rpm, so zero below means something

        // The crank stops. One cam edge a second is all that keeps arriving. fine_advanced_ is
        // cleared per EDGE now; it used to be cleared inside fuse(), which four of on_edge's
        // returns never reach — so a cam edge still carried the last crank tooth's `true` and the
        // HAL read it as "a tooth arrived": last_fine_tick_ re-stamped, trigger_absent_ cleared,
        // the stall deadline re-armed, by an edge that carried no tooth.
        for (int sec = 0; sec < 10; ++sec) {
            for (int ms = 0; ms < 1000; ++ms) { advance_by(TPS / 1000); hal.service(); }
            g_cam.edge();
            hal.service();
        }
        CHECK(hal.get_sync_level() == SyncLevel::NONE);
        CHECK(hal.get_rpm_x10() == 0);
        CHECK(hal.get_decoder_telemetry().trigger_absent == true);
    }

    SECTION("...and the cam cannot then speak for the engine: no PHASE, no stale rpm");
    {
        w.spin_serviced(hal, 400);
        const uint32_t rpm_running = hal.get_rpm_x10();
        CHECK(hal.get_sync_level() != SyncLevel::NONE);
        CHECK(rpm_running > 0);

        // The crank dies and the cam keeps turning — a broken crank sensor, a cut wire, one pickup
        // of two. The cam goes on producing its own four-pulse pattern, and a SEQUENCE matcher
        // locks on that alone. Losing sync used to clear has_crank_, which is configuration and not
        // decode state, so the next cam edge took the "cam-only sensor: cam is everything" branch,
        // granted full PHASE sync, and named itself the velocity source. The DCO was still
        // free-running, so rpm stayed at whatever the engine was doing when the crank stopped.
        for (int cycle = 0; cycle < 40; ++cycle) {     // ~40 engine cycles of it
            for (int i = 0; i < 4; ++i) {
                advance_by((static_cast<uint32_t>(CAM_CELL[i]) / 100u) * TOOTH_TICKS);
                g_cam.edge();
                hal.service();
            }
        }
        CHECK(hal.get_sync_level() == SyncLevel::NONE);
        CHECK(hal.get_rpm_x10() == 0);                 // not the speed it had when the crank died
        CHECK(hal.get_decoder_telemetry().trigger_absent == true);
        // ...nor for its own phaser: with no crank there is nothing to measure the cam against, so
        // whatever the cam stream locked onto is not a cam angle the VVT loop may close on.
        CHECK(!hal.get_vvt_result(0).valid);
    }

    SECTION("an engine WINDING DOWN below the Cranking Threshold is a stop, not a fault");
    {
        // The wheel turns at ~600 rpm; put the threshold above that so this silence reads as the
        // engine stopping. Sync must still drop (firing stops), but nothing is recorded: no missed
        // tooth that would raise P0336 at the next start, and no "no signal" (P0338) while it sits.
        const uint16_t keep = g_config.engine.cranking_rpm;
        g_config.engine.cranking_rpm = 1000;
        w = Wheel{};
        w.spin_serviced(hal, 200);
        CHECK(hal.get_sync_level() != SyncLevel::NONE);
        const uint32_t missed_before = hal.get_decoder_telemetry().missed_total;
        advance_by(3 * TPS);
        hal.service();
        CHECK(hal.get_sync_level() == SyncLevel::NONE);
        CHECK(hal.get_decoder_telemetry().missed_total == missed_before);
        CHECK(hal.get_decoder_telemetry().trigger_absent == false);
        g_config.engine.cranking_rpm = keep;
    }

    return test_summary();
}
