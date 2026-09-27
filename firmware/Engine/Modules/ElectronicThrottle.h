#pragma once

#include "../EngineModule.h"
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "../../Diagnostics/DtcManager.h"   // L2 supervisor raises throttle P-codes into the one table
#include "../../../generated/modules/electronic_throttle_config.h"
#include "../../../generated/signal_ids.h"
#include "../../Comms/CommandState.h"   // command_state telemetry: report each bench routine's result to the studio
#include <type_traits>   // std::extent_v — derive FF_BINS from the ff_table axis member

// ---------------------------------------------------------------------------
// ElectronicThrottle — electronic throttle (ETB / drive-by-wire) control. ETB-ONLY: a cable throttle is
// just a TPS sensor, nothing here. Per enabled ETB, two ANALOG TPS feedback signals (both calibrated
// to plate-% in the SAME sense) are A/B cross-checked; the module publishes a duty demand (etb_duty_N)
// that HBridge realises on the bridge (so the Lua plane can sit between demand and output), and
// owns the LATCHING fail-safe state, published as the enable signal etb_en_N (1 = drive, 0 = cut)
// that HBridge's enable_sig gate consumes — the bridge drives only while EN is asserted. The H-bridge
// HAL owns the physical pin (LOW=enabled; default/reset disabled -> spring-return). The ETB never
// energizes without an explicit command. See docs/throttle-control-design.md.
//
// STAGED. STAGE 1 (this) — CAPPED MANUAL NUDGE, open-loop: a CLI `throttle <etb> <pct>` arms a
// slew-limited demand for a short auto-expiring window, gated to engine-stopped. While armed it
// asserts etb_en_N (enable) and publishes a ramped etb_duty_N; on expiry / disarm / disable it slews
// demand to 0 then drops etb_en_N (cut). Open-loop (no PID, no trusted cal yet), so the A/B-disagreement
// latch is INHIBITED during a manual nudge (the feedback isn't calibrated) — bounded instead by the
// duty cap (HBridge dc_max_pct), the slew rate, the engine-stopped gate, and the auto-expiry. Stage 0's
// read-only A/B resolve (etb_position_N + state) still runs. Stage 2 = autocal; Stage 3 = closed-loop.
// ---------------------------------------------------------------------------

class ElectronicThrottle : public EngineModule {
public:
    void init(const ElectronicThrottleConfig& cfg);
    void on_config_change(const ElectronicThrottleConfig& cfg);
    void on_engine_stop() override;

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    // Bench manual nudge (Stage 1): command an open-loop demand 0..100% on `etb` for `hold_ms`, after
    // which it auto-disarms (slews to 0 + DIS re-asserts). Called from the `throttle` CLI command.
    // Engine-stopped is still enforced in update(); this only latches the request + its expiry.
    void set_manual(uint8_t etb, float pct, uint32_t hold_ms) noexcept;

    // Autocal (Stage 2): start the engine-stopped calibration sweep on `etb` — settle to the closed
    // (spring-rest) reference, creep open until the plate reaches its stop, then write both feedback
    // sensors' cal (closed->0% / open->100%, auto-handling opposite slopes) + raw-fault DTC thresholds.
    // Called from the `autocal` CLI command.
    void start_autocal(uint8_t etb, bool verify = false) noexcept;

    // Stage-2 FF fill: engine-stopped sweep (requires stage-1 cal) that measures the static hold duty at
    // each configured ff_table X breakpoint (down+up -> midpoint) and writes them into etb[i].ff_table at
    // the CLIENT-CHOSEN Y row. A Y-keyed (CLT) row can't be acquired by "being at that temperature" — the
    // engine can't run the throttle there until that row is filled (a bootstrap deadlock) — so the target
    // row is supplied explicitly. The client sets the bins + picks the row; firmware just fills.
    void start_fillff(uint8_t etb, uint8_t row) noexcept;

    // PID autotune: engine-stopped relay-feedback (Aström) at EACH filled ff_table X breakpoint — the FF
    // map supplies the hold-duty bias so the relay oscillates about equilibrium at every point. Measures
    // Ku/Tu per point, takes the WORST case (smallest Ku -> gentlest gains, stable across the whole travel),
    // applies the chosen tuning rule (`rule`), and writes kp/ki/kd. Requires a prior findlimits + fillff.
    // Console reports each point's Ku/Tu and every rule's candidate gains. From the `autotune` CLI.
    enum TuneRule : uint8_t { TR_TYREUS_LUYBEN = 0, TR_ZN_CLASSIC, TR_ZN_NO_OVERSHOOT,
                              TR_ZN_SOME_OVERSHOOT, TR_PESSEN, TR_COUNT };
    void start_autotune(uint8_t etb, uint8_t rule) noexcept;

    // Raw-ADC read seam (ADC counts by analog pool index). Defaults to platform_read_ain_raw; tests override it.
    static void set_raw_reader(uint16_t (*fn)(uint8_t)) noexcept;

    // The one error table — the L2 supervisor raises throttle P-codes (feedback / stuck) into it.
    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }

    // ETB state-machine values published on etb_state_N.
    enum State : uint8_t { ST_UNCAL = 0, ST_AUTOCAL = 1, ST_READY = 2, ST_FAULT = 3 };

private:
    static constexpr unsigned N = ELECTRONIC_THROTTLE_ETB_COUNT;

    const ElectronicThrottleConfig* cfg_ = nullptr;

    // Autocal sub-state machine (per ETB). Bidirectional: settle -> drive CLOSED to the closed stop ->
    // drive OPEN to the open stop -> write cal. Captures the true mechanical span (not the spring rest).
    // settle -> drive CLOSED -> drive OPEN (writes the position cal) -> FF sweep up + down (learns the
    // spring feed-forward map) -> done. The FF phases run AFTER the position cal so they bin by %.
    // ... and PID autotune (AT_*): relay-feedback at each FF X breakpoint -> worst-case (min-Ku) gains.
    // AT phases sit before AC_DONE/AC_FAIL so the busy-range state check (AC_SETTLE..AT_RELAY) covers them.
    enum AcPhase : uint8_t { AC_OFF = 0, AC_SETTLE, AC_CLOSE, AC_OPEN,
                             AC_FF_PREP, AC_FF_OPEN, AC_FF_CLOSE,
                             AT_CHECK, AT_SETTLE, AT_RELAY, AC_DONE, AC_FAIL };

    // Per-ETB state.
    bool     pos_primed_[N]    = {};   // filter has a prior sample
    float    disagree_ms_[N]   = {};   // accumulated A/B disagreement time
    float    manual_pct_[N]    = {};   // commanded open-loop demand 0..100% (manual nudge)
    uint32_t manual_until_ms_[N] = {}; // tick the manual nudge auto-expires (0 = disarmed)
    float    duty_cmd_[N]      = {};   // slew-limited demand currently published on etb_duty_N
    uint32_t last_ms_          = 0;
    uint32_t last_cyc_         = 0;   // previous frame's CPU cycle count — the loop's real timestep
    // Autocal working state.
    AcPhase  ac_phase_[N]      = {};   // AC_OFF = no autocal running
    uint16_t ac_op_[N]         = {};   // cmdstate::Op latched at start — emitted (OK/FAIL) at the shared terminal
    uint32_t ac_t_[N]          = {};   // phase-start tick
    float    ac_still_ms_[N]   = {};   // how long raw-A has been ~stationary (stop detect)
    uint16_t ac_rest_a_[N]     = {};   // raw mV at the CLOSED stop reference (-> 0%) (TPS A / B)
    uint16_t ac_rest_b_[N]     = {};
    uint16_t ac_relax_a_[N]    = {};   // raw mV at the de-energized SPRING-REST point (TPS A / B), captured
    uint16_t ac_relax_b_[N]    = {};   //   in AC_SETTLE; -> relax_pct once the closed/open span is known
    uint16_t ac_last_a_[N]     = {};   // last raw-A sample (motion delta)
    // FF-cal (stage 2): per-X-bin crossing duty in BOTH sweep directions (midpoint -> friction cancels).
    // Sized to the ff_table X allocation; one ETB calibrates at a time, so a single set suffices.
    static constexpr unsigned FF_BINS = std::extent_v<decltype(EtbConfig::ff_table_x_axis)>;
    float    ff_down_[FF_BINS] = {};   // duty as the plate crossed each X break going CLOSED (down sweep)
    float    ff_up_[FF_BINS]   = {};   // ... and going OPEN (up sweep)
    bool     ff_dn_ok_[FF_BINS]= {};   // crossed-this-bin flags
    bool     ff_up_ok_[FF_BINS]= {};
    float    ff_prev_pos_      = 0.0f; // previous-frame plate position (crossing detect)
    uint8_t  ff_row_           = 0;    // resolved Y row this sweep writes (clamped target)
    uint8_t  ff_row_target_    = 0;    // client-chosen Y row to fill (from the `fillff <etb> <row>` CLI)
    uint8_t  ff_etb_           = 0;    // which ETB the active FF sweep is filling
    float    last_pos_[N]      = {};   // previous-frame position — moving-detect for anti-windup
    // Stage 3 — closed-loop + L2 supervisor.
    bool     calibrated_[N]    = {};   // a successful autocal ran -> trust the feedback -> closed-loop
    float    target_cmd_[N]    = {};   // slew-limited target position (%) the PID chases
    float    integ_[N]         = {};   // PID integrator
    float    last_err_[N]      = {};   // PID derivative memory
    bool     loop_live_[N]     = {};   // closed_loop ran last frame: last_err_ is a real previous error
    float    stall_ms_[N]      = {};   // how long the plate has been commanded-but-not-moving (stall)
    float    stall_pos_[N]     = {};   // position when motion was last seen (stall detect)
    bool     fault_latched_[N] = {};   // L2 fail-safe latch (A/B / feedback-lost / stuck) -> DIS until cleared
    uint16_t active_dtc_[N]    = {};   // P-code currently raised for this ETB (0 = none) — edge raise/heal
    DtcManager* dtc_           = nullptr;
    // PID autotune working state. One ETB tunes at a time, so a single set suffices (like the FF sweep).
    uint8_t  at_etb_           = 0;    // which ETB the active autotune runs on
    uint8_t  at_rule_          = 0;    // selected TuneRule (which rule's gains are written)
    uint8_t  at_point_         = 0;    // current ff_table X-breakpoint index under test
    float    at_target_        = 0.0f; // test position (= ff_table_x_axis[at_point_])
    float    at_bias_          = 0.0f; // hold duty at the test point — the relay oscillates about this
    float    at_settle_i_      = 0.0f; // settle integrator: absorbs the steady hold duty (FF + correction)
    float at_band_ms_       = 0;    // how long the plate has sat in-band at the point (settle dwell)
    bool     at_above_         = false;// last relay side (pos above target) — edge = a half-cycle crossing
    float    at_pmax_          = 0.0f; // position extremes within the current cycle (amplitude)
    float    at_pmin_          = 0.0f;
    uint32_t at_cross_t_       = 0;    // tick of the last full-period crossing (period measure)
    float    at_tu_sum_        = 0.0f; // accumulated period (s) + amplitude (%) over the measured cycles
    float    at_a_sum_         = 0.0f;
    uint8_t  at_cyc_           = 0;    // measured cycles this point (first few discarded as transient)
    float    at_ku_worst_      = 0.0f; // worst-case across points: SMALLEST Ku (gentlest gains) + its Tu
    float    at_tu_worst_      = 0.0f;
    bool     at_any_           = false;// at least one point produced a valid Ku/Tu

    void reset_state(bool clear_faults = false);   // faults survive unless an ECU reset (init) asks
    // True while ANY body is running findlimits/fillff/autotune — those share one set of
    // accumulators, so a second body must not start one on top of the first.
    bool routine_busy() const noexcept;
    void adopt_stored_cal();   // cal_pending_ <- whether the tune holds a feedback cal
    bool     cal_pending_[N]   = {};   // stored cal found, awaiting key-on verify
    // KEY-ON, not boot, is what arms the verify. Tracked so the rising edge can re-arm it: the ECU can
    // sit powered with the key off for as long as you like, and the check has to happen when the
    // throttle actually becomes drivable, not once at reset and never again.
    bool     key_prev_         = false;
    // ENGINE-STOPPED, CACHED FOR THE COMMAND HANDLERS. start_autocal/fillff/autotune are entry points
    // called from the CLI and the studio, not from update(), so they have no bus to ask. Refusing at
    // the door needs the answer here. Defaults to true so a command before the first update() is not
    // refused on a state nobody has measured yet.
    bool     engine_stopped_   = true;
    bool     ac_verify_[N]     = {};   // this autocal run COMPARES against the stored cal instead of writing it
    float    ac_closed_pct_[N] = {};   // plate %, in stored-cal terms, measured at the closed stop
    uint32_t sweep_cut_until_  = 0;    // hold fuel+spark cut until this tick (routine + spring-return)
    void run_autocal(unsigned i, const EtbConfig& e, SignalBus& bus, float pos, uint32_t now, float dt_ms, bool& dis);
    void run_autotune(unsigned i, const EtbConfig& e, SignalBus& bus, float pos, uint32_t now, float dt_ms, bool& dis);
    void finalize_autotune(unsigned i);   // worst-case Ku/Tu -> report every rule + write the selected gains
    void write_ff_row(int xn);   // stage-2: midpoint + gap-fill the swept crossings into etb[ff_etb_].ff_table
    // Closed-loop position step: slews target, runs the PID, returns duty + dis. Trips the L2 latch
    // (raising a P-code) on a post-cal A/B / lost-feedback fault or a commanded-but-stuck plate.
    float closed_loop(unsigned i, const EtbConfig& e, SignalBus& bus, float pos, bool armed, float demand,
                      bool match_fault, float dt_s, uint32_t now, bool& dis);
    // Latch the L2 fail-safe for ETB i and edge-raise `code` into the DTC table (once per fault).
    void latch_fault(unsigned i, uint16_t code, uint32_t now) noexcept;
    void clear_fault(unsigned i) noexcept;   // clear the latch + heal its DTC (via autocal)
};

// CLI entry point (defined in ElectronicThrottle.cpp): forwards to the composed instance.
void throttle_bench_nudge(uint8_t etb, float pct, uint32_t hold_ms) noexcept;
void throttle_bench_autotune(uint8_t etb, uint8_t rule) noexcept;
