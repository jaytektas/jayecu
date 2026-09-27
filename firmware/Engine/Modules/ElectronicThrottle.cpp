#include "ElectronicThrottle.h"
#include "../EngineStateMachine.h"          // EngineRunState
#include "../../Platform/platform_hal.h"    // platform_get_tick_ms / platform_read_ain_raw
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../../Signal/SignalRates.h"        // claim the feedback rate this loop needs
#include "../../../generated/module_cadence.h"  // cadence::ElectronicThrottle
#include "../../../generated/ecu_config.h"          // g_config (sensor cal writeback)
#include "../../Integration/PipelineBuilder.h"   // DIAG_RAW_MIN/MAX — arm the wire-fault window
#include "../../../generated/sensors_catalog.h"     // SENSOR_CATALOG / SENSOR_COUNT (signal -> sensor)
#include "../TableEval.h"                            // tbl::tableresolve (typed per-axis coords)
#include "../../../generated/table_descs.h"          // ff_table_desc(cfg)
#include "../../Diagnostics/TextLog.h"               // g_text_log — autocal results to the host console
#include <algorithm>
#include <cmath>

extern uint32_t g_config_generation;   // bump so Sensors re-reads the cal we write during autocal

// Per-ETB published signal ids (array index 0..N-1 -> the generated SIG_ETB_*_<n>).
static const SignalId POS_SIG[]   = { SIG_ETB_POSITION_1, SIG_ETB_POSITION_2 };
static const SignalId DUTY_SIG[]  = { SIG_ETB_DUTY_1,     SIG_ETB_DUTY_2 };
static const SignalId EN_SIG[]    = { SIG_ETB_EN_1,       SIG_ETB_EN_2 };
// The loop's own two numbers: where it is ASKING the plate to be (post slew-limit, post authority clamp
// — not the raw demand), and how much of the duty is coming from the integrator. Position against target
// is what says whether the servo is keeping up; the I-term is what says whether it is having to lean on
// the plate to do it, which is a sticking body or a spring gone weak long before it is a fault.
static const SignalId TGT_SIG[]   = { SIG_ETB_TARGET_1,   SIG_ETB_TARGET_2 };
static const SignalId ITERM_SIG[] = { SIG_ETB_ITERM_1,    SIG_ETB_ITERM_2 };
// How far a stop may sit from where the stored cal puts it before the key-on verify calls it moved.
// Generous on purpose: the cal was written at these same stops, so the honest error is small. This
// only has to catch a stop that has genuinely MOVED, not to re-measure one.
static constexpr float    SWEEP_STOP_TOL_PCT  = 10.0f;
// How long fuel/spark stay inhibited after a plate-driving routine ends — long enough for the
// de-energised plate to spring back to its limp-home rest before the engine may fire.
static constexpr uint32_t SWEEP_CUT_HOLD_MS   = 500;
static const SignalId STATE_SIG[] = { SIG_ETB_STATE_1,    SIG_ETB_STATE_2 };
static_assert(sizeof(POS_SIG) / sizeof(POS_SIG[0]) >= ELECTRONIC_THROTTLE_ETB_COUNT,
              "per-ETB signal table smaller than ELECTRONIC_THROTTLE_ETB_COUNT");

// Raw-ADC read seam (ADC COUNTS by analog pool index). Real build = platform; host tests override it.
static uint16_t (*s_read_ain)(uint8_t) = platform_read_ain_raw;
void ElectronicThrottle::set_raw_reader(uint16_t (*fn)(uint8_t)) noexcept { s_read_ain = fn; }

static inline int iabs(int v) { return v < 0 ? -v : v; }

// L2-supervisor DTCs (SAE J2012DA throttle-actuator-control range). All level 3: a throttle
// the controller can't trust or can't move must fail safe (DIS -> spring-return), not keep driving.
constexpr uint16_t P_TPS_CORRELATION = 0x2135;  // TPS A/B correlation / feedback untrustworthy or lost
constexpr uint16_t P_TAC_STUCK_OPEN  = 0x2111;  // commanded to CLOSE but the plate won't move
constexpr uint16_t P_TAC_STUCK_CLOSED= 0x2112;  // commanded to OPEN but the plate won't move
constexpr uint8_t  S_ETB_FAULT       = 3;       // DTC_SEV_LEVEL3

// Stall detector: a real position error + meaningful drive but the plate isn't moving = stuck (or motor
// disconnected). ETB-AGNOSTIC — pure position/duty logic, no winding-resistance/current assumption.
constexpr float    STALL_ERR_PCT  = 8.0f;       // |target-pos| that counts as "trying to move"
constexpr float    STALL_DUTY_PCT = 25.0f;      // |duty| that should move a healthy plate
constexpr float    STALL_MOVE_PCT = 1.5f;       // position change that counts as "moving" (resets)
constexpr uint32_t STALL_MS       = 600;        // dwell before latching stuck

// signal id -> sensor catalog index (the sensor that publishes it), or -1.
static int find_sensor(SignalId sig) {
    for (int i = 0; i < static_cast<int>(SENSOR_COUNT); i++)
        if (SENSOR_CATALOG[i].primary_channel == sig) return i;
    return -1;
}
// raw ADC counts behind a feedback signal (its sensor's analog source pin). The ECU is counts-native.
static uint16_t raw_adc(SignalId sig) {
    const int idx = find_sensor(sig);
    if (idx < 0) return 0;
    return s_read_ain(g_config.sensors.sensor[idx].source);
}
// Write a 2-point cal so the sensor reads plate-% (closed/rest -> 0%, open -> 100%) regardless of
// slope (cal_raw must be ascending; the descending sensor naturally gets val 1000->0). Also set the
// raw short-GND/VCC DTC window from the observed range ± margin. All raws are ADC COUNTS (cal_val is
// %x10, sensor scale 0.1).
static constexpr uint16_t ADC_FULL_SCALE = 4095u;   // 12-bit; matches KnockDsp/CliCommands
static void write_cal(SignalId sig, uint16_t rest_raw, uint16_t open_raw) {
    const int idx = find_sensor(sig);
    if (idx < 0) return;
    SensorConfig& s = g_config.sensors.sensor[idx];
    if (rest_raw <= open_raw) { s.cal_raw[0]=rest_raw; s.cal_val[0]=0;    s.cal_raw[1]=open_raw; s.cal_val[1]=1000; }
    else                      { s.cal_raw[0]=open_raw; s.cal_val[0]=1000; s.cal_raw[1]=rest_raw; s.cal_val[1]=0;    }
    s.cal_n = 2;
    const uint16_t lo = std::min(rest_raw, open_raw), hi = std::max(rest_raw, open_raw);
    const uint16_t margin = 164u;   // ~200 mV expressed in counts (200 * 4095 / 5000)
    s.diag_raw_min = (lo > margin) ? static_cast<uint16_t>(lo - margin) : 0u;
    s.diag_raw_max = (hi + margin < ADC_FULL_SCALE) ? static_cast<uint16_t>(hi + margin) : ADC_FULL_SCALE;
    // ARM the window we just measured. It was computed and stored but the checks were never enabled, so
    // the wire-fault detection findlimits exists to set up simply did not run: a track moved to an
    // unconnected pin read a clamped 0 % and stayed VALID, which put it only ~6 % from a plate resting at
    // 5.9 % -- inside the 10 % A/B window -- so neither the key-on verify nor the runtime correlation
    // check saw anything wrong, and the loop closed on one live track and one dead one. The thresholds
    // are meaningless unless the checks that read them are on.
    s.diag_enable |= (pipe::DIAG_RAW_MIN | pipe::DIAG_RAW_MAX);
}

// Map the de-energized settle-point raw (A & B) to plate-% against the just-learned closed/open
// references, average the two, return %x10 (config scale 0.1). Slope-agnostic — it's a ratio.
static uint16_t relax_pct_x10(uint16_t relax_a, uint16_t closed_a, uint16_t open_a,
                              uint16_t relax_b, uint16_t closed_b, uint16_t open_b) {
    auto frac = [](int r, int c, int o) -> float {
        const int span = o - c;
        if (span == 0) return 0.0f;
        return std::clamp(100.0f * static_cast<float>(r - c) / static_cast<float>(span), 0.0f, 100.0f);
    };
    const float p = 0.5f * (frac(relax_a, closed_a, open_a) + frac(relax_b, closed_b, open_b));
    return static_cast<uint16_t>(p * 10.0f + 0.5f);
}

// The composed instance, for the `throttle` / `autocal` CLI commands (set in SystemComposer::compose).
ElectronicThrottle* g_electronic_throttle = nullptr;
void throttle_bench_nudge(uint8_t etb, float pct, uint32_t hold_ms) noexcept {
    if (g_electronic_throttle) g_electronic_throttle->set_manual(etb, pct, hold_ms);
}
void throttle_bench_autocal(uint8_t etb) noexcept {
    if (g_electronic_throttle) g_electronic_throttle->start_autocal(etb);
}
void throttle_bench_fillff(uint8_t etb, uint8_t row) noexcept {
    if (g_electronic_throttle) g_electronic_throttle->start_fillff(etb, row);
}
void throttle_bench_autotune(uint8_t etb, uint8_t rule) noexcept {
    if (g_electronic_throttle) g_electronic_throttle->start_autotune(etb, rule);
}

void ElectronicThrottle::set_manual(uint8_t etb, float pct, uint32_t hold_ms) noexcept {
    if (etb >= N) return;
    manual_pct_[etb] = std::clamp(pct, 0.0f, 100.0f);
    uint32_t until = platform_get_tick_ms() + hold_ms;
    manual_until_ms_[etb] = (until == 0) ? 1u : until;
}

// ONE ROUTINE AT A TIME, ACROSS BOTH BODIES. The sweep and autotune accumulators (ff_down_/ff_up_,
// the at_* relay measurements, ff_prev_pos_) are single-instance by design, tagged with ff_etb_/at_etb_
// to say whose they are -- and nothing stopped a second body claiming them mid-sweep. Starting a
// routine on ETB2 while ETB1 swept simply overwrote the tag and cleared the bins, while ETB1's own
// state machine kept stepping: whichever finished first wrote a table built partly from the OTHER
// throttle's crossings. g_command_state is single too, so the first routine's reply was lost and the
// studio waited on it for ever. Two throttles are the normal case for this module, so refuse instead.
bool ElectronicThrottle::routine_busy() const noexcept {
    for (unsigned i = 0; i < N; i++) if (ac_phase_[i] != AC_OFF) return true;
    return false;
}

void ElectronicThrottle::start_autocal(uint8_t etb, bool verify) noexcept {
    if (etb >= N || routine_busy()) return;
    // NOT WITH THE ENGINE TURNING. Every one of these sweeps the plate to a mechanical stop, and doing
    // that to a running engine is the disaster this interlock exists to prevent. The comments here have
    // always said "engine-stopped" and nothing enforced it: the only thing that stopped it was the drive
    // arbitration aborting the routine on the NEXT tick, which is a rollback, not a refusal. Answer the
    // caller so the studio is not left waiting on a reply that will never come.
    if (!engine_stopped_) { set_command_state(cmdstate::etb_op(cmdstate::ETB0_FINDLIMITS, etb), cmdstate::FAIL); return; }
    const cmdstate::Op op = cmdstate::etb_op(cmdstate::ETB0_FINDLIMITS, etb);
    ac_op_[etb] = op; set_command_state(op, cmdstate::RUNNING);   // studio watches this until OK/FAIL
    ac_verify_[etb] = verify;
    ac_phase_[etb] = AC_SETTLE;
    ac_t_[etb] = platform_get_tick_ms();
    manual_until_ms_[etb] = 0;   // cancel any manual nudge
    duty_cmd_[etb] = 0.0f;
    integ_[etb] = 0.0f; stall_ms_[etb] = 0; target_cmd_[etb] = 0.0f;
    // Autocal is also the authorized fail-safe CLEAR: drop the latch so a recal recovers a faulted ETB.
    // A VERIFY must not do that. It is an automatic key-on check, and if clearing the latch were part of
    // it then every key cycle would quietly forgive a fault that is supposed to require deliberate
    // intervention -- the latch would mean nothing.
    if (!verify) clear_fault(etb);
}

// Stage-2 FF fill — engine-stopped, requires a trusted stage-1 cal. Resets the per-bin crossing
// accumulators and enters the sweep (prep -> down -> up -> write into etb[etb].ff_table).
void ElectronicThrottle::start_fillff(uint8_t etb, uint8_t row) noexcept {
    if (etb >= N || !calibrated_[etb] || routine_busy()) return;   // need stage-1 cal + an idle module
    // NOT WITH THE ENGINE TURNING. Every one of these sweeps the plate to a mechanical stop, and doing
    // that to a running engine is the disaster this interlock exists to prevent. The comments here have
    // always said "engine-stopped" and nothing enforced it: the only thing that stopped it was the drive
    // arbitration aborting the routine on the NEXT tick, which is a rollback, not a refusal. Answer the
    // caller so the studio is not left waiting on a reply that will never come.
    if (!engine_stopped_) { set_command_state(cmdstate::etb_op(cmdstate::ETB0_FILLFF, etb), cmdstate::FAIL); return; }
    const cmdstate::Op op = cmdstate::etb_op(cmdstate::ETB0_FILLFF, etb);
    ac_op_[etb] = op; set_command_state(op, cmdstate::RUNNING);
    ff_etb_ = etb;
    ff_row_target_ = row;                         // client-chosen Y row; clamped to the live bin count at write
    for (unsigned k = 0; k < FF_BINS; k++) { ff_down_[k]=0.0f; ff_up_[k]=0.0f; ff_dn_ok_[k]=false; ff_up_ok_[k]=false; }
    ff_prev_pos_ = 0.0f; ff_row_ = 0;
    ac_phase_[etb] = AC_FF_PREP;
    ac_t_[etb] = platform_get_tick_ms();
    manual_until_ms_[etb] = 0; duty_cmd_[etb] = 0.0f;
}

// PID autotune — engine-stopped relay-feedback at each FF X breakpoint. Requires a trusted cal (FF map
// supplies the per-point hold-duty bias). Starts at the first breakpoint; run_autotune walks the rest.
void ElectronicThrottle::start_autotune(uint8_t etb, uint8_t rule) noexcept {
    if (etb >= N || !calibrated_[etb] || routine_busy()) return;
    // NOT WITH THE ENGINE TURNING. Every one of these sweeps the plate to a mechanical stop, and doing
    // that to a running engine is the disaster this interlock exists to prevent. The comments here have
    // always said "engine-stopped" and nothing enforced it: the only thing that stopped it was the drive
    // arbitration aborting the routine on the NEXT tick, which is a rollback, not a refusal. Answer the
    // caller so the studio is not left waiting on a reply that will never come.
    if (!engine_stopped_) { set_command_state(cmdstate::etb_op(cmdstate::ETB0_AUTOTUNE, etb), cmdstate::FAIL); return; }
    const cmdstate::Op op = cmdstate::etb_op(cmdstate::ETB0_AUTOTUNE, etb);
    ac_op_[etb] = op; set_command_state(op, cmdstate::RUNNING);
    at_etb_   = etb;
    at_rule_  = (rule < static_cast<uint8_t>(TR_COUNT)) ? rule : static_cast<uint8_t>(TR_TYREUS_LUYBEN);
    at_point_ = 0;
    at_ku_worst_ = 0.0f; at_tu_worst_ = 0.0f; at_any_ = false;
    at_cyc_ = 0; at_tu_sum_ = 0.0f; at_a_sum_ = 0.0f; at_settle_i_ = 0.0f;
    ac_phase_[etb] = AT_CHECK;     // key-on sanity check (plate responds) before the relay sweep
    ac_t_[etb] = platform_get_tick_ms();
    manual_until_ms_[etb] = 0; duty_cmd_[etb] = 0.0f; integ_[etb] = 0.0f;
}

// THE FAULT LATCH IS NOT SESSION STATE. reset_state() runs on every engine stop and on the key-on edge,
// and it used to clear fault_latched_ with everything else — so a stall, or turning the key, forgave a
// latched P2135/P2111/P2112 and put a throttle that had just failed back into closed loop. A genuine
// fault demands a reset of the ECU (init), exactly as the comments at the key-on edge say. Only that
// clears it; a stop or a key cycle carries it.
void ElectronicThrottle::reset_state(bool clear_faults) {
    for (unsigned i = 0; i < N; i++) {
        pos_primed_[i]=false; disagree_ms_[i]=0;
        manual_pct_[i]=0.0f; manual_until_ms_[i]=0; duty_cmd_[i]=0.0f;
        ac_phase_[i]=AC_OFF; ac_t_[i]=0; ac_still_ms_[i]=0;
        ac_rest_a_[i]=ac_rest_b_[i]=ac_relax_a_[i]=ac_relax_b_[i]=ac_last_a_[i]=0;
        target_cmd_[i]=0.0f; integ_[i]=0.0f; last_err_[i]=0.0f;
        stall_ms_[i]=0; stall_pos_[i]=0.0f;
        if (clear_faults) { fault_latched_[i]=false; active_dtc_[i]=0; }
    }
    last_ms_ = 0;
}

// L2 latch + edge-triggered DTC raise. Sets the fail-safe latch (-> DIS in update) and raises `code`
// ONCE (the table interaction must be edge-triggered, not per-frame). A different code supersedes.
void ElectronicThrottle::latch_fault(unsigned i, uint16_t code, uint32_t now) noexcept {
    fault_latched_[i] = true;
    // Re-raised every frame while latched, not once on the edge: raise() only counts the
    // inactive->active transition, and calling it is what keeps last-seen fresh. Edge-guarding froze
    // last_ms at the moment of the fault, so a latched code was indistinguishable from a stale one.
    if (dtc_) {
        if (active_dtc_[i] != code && active_dtc_[i] != 0) dtc_->heal(active_dtc_[i]);
        dtc_->raise(code, DtcSource::THROTTLE, S_ETB_FAULT, now, dtc_ttl());
        active_dtc_[i] = code;
    }
}
// Clear the latch + heal its DTC (called by autocal, which re-establishes trusted feedback).
void ElectronicThrottle::clear_fault(unsigned i) noexcept {
    fault_latched_[i] = false;
    if (dtc_ && active_dtc_[i] != 0) { dtc_->heal(active_dtc_[i]); active_dtc_[i] = 0; }
}


// Is this ETB's feedback pair actually calibrated? Asked of the STORED sensor cal, not of whether a
// findlimits happened to run since boot.
//
// calibrated_ used to be pure session state: set by findlimits, wiped by reset_state() -- which init()
// calls at boot and on_engine_stop() calls on every transition into STOPPED. The calibration itself
// lives in the tune and survives both, so a throttle that was calibrated, burned and reset came back
// with a perfectly good cal in flash and the loop still refusing to close, reporting UNCAL. It had to be
// re-swept after every key cycle. Deriving the flag from the thing that actually persists fixes that,
// and cannot go stale: write a new cal and it follows, clear one and it follows that too.
static bool feedback_calibrated(const EtbConfig& e) {
    const int ia = find_sensor(static_cast<SignalId>(e.tps_a_src));
    const int ib = find_sensor(static_cast<SignalId>(e.tps_b_src));
    if (ia < 0 || ib < 0) return false;
    if (g_config.sensors.sensor[ia].cal_n < 2 || g_config.sensors.sensor[ib].cal_n < 2) return false;
    // cal_n >= 2 IS NOT EVIDENCE THAT findlimits RAN. Every sensor in the catalog ships with a generic
    // two-point cal (cal_n defaults to 2 across the board), so that test alone was true on a factory
    // ECU that had never swept anything -- and the loop would close on a plate whose 0 % and 100 % were
    // whatever the generic default says, not where the stops actually are. relax_pct is the marker
    // built for this: findlimits measures the de-energised spring rest and writes it, and the schema
    // defines it as 0 until calibrated. It is per-ETB and lives in the tune, so it still survives the
    // key cycle this function exists to survive.
    //
    // Edge case, stated rather than hidden: a throttle whose limp-home rest is EXACTLY the closed stop
    // would store relax_pct = 0 and read as uncalibrated. Every DBW body holds a limp-home crack open
    // by design, so that plate does not really exist -- and failing closed (refuse to drive, ask for a
    // findlimits) is the safe way to be wrong about it.
    return e.relax_pct != 0;
}

// KEY-ON = VERIFY, NEVER RE-LEARN. A stored cal makes this ETB a CANDIDATE, not trusted: the loop stays
// open and the bridge disabled until update() has actually seen the feedback and checked it.
void ElectronicThrottle::adopt_stored_cal() {
    for (unsigned i = 0; i < N; i++) {
        const bool have = cfg_ && feedback_calibrated(cfg_->etb[i]);
        cal_pending_[i] = have;      // verify it before trusting it
        calibrated_[i]  = false;     // ... so nothing drives on an unchecked cal
    }
}
// key_prev_ SEEDS FROM THE CURRENT KEY, not from false. Starting it false makes the very first frame
// look like a rising edge even when the key was already on at boot -- and that edge calls reset_state(),
// which sets ac_phase_ to AC_OFF and so silently cancelled a findlimits started before the module's
// first update. init() already arms the verify, so a boot with the key on needs no edge at all; what the
// edge is for is the key being turned on LATER, which is the case reset could never cover.
static bool key_now() { extern bool g_system_active; return g_system_active; }
void ElectronicThrottle::init(const ElectronicThrottleConfig& cfg) { cfg_ = &cfg; reset_state(true); adopt_stored_cal(); key_prev_ = key_now(); }
void ElectronicThrottle::on_config_change(const ElectronicThrottleConfig& cfg) { cfg_ = &cfg; reset_state(); adopt_stored_cal(); key_prev_ = key_now(); }
// An engine stop resets the LOOP (integrator, slew, timers) — it is not a reason to forget a calibration
// that is sitting in the tune. reset_state() no longer touches calibrated_, so it simply carries over.
void ElectronicThrottle::on_engine_stop() { reset_state(); }

// Autocal sub-state machine (engine-stopped, bipolar). Bidirectional: drive CLOSED to the closed stop,
// then OPEN to the open stop, recording the true mechanical span. OC-safe: gentle creep + back off the
// instant the plate stalls at a stop (raw TPS stops changing) — never dwells against a stop.
void ElectronicThrottle::run_autocal(unsigned i, const EtbConfig& e, SignalBus& /*bus*/, float pos, uint32_t now, float dt_ms, bool& dis) {
    const uint32_t elapsed = now - ac_t_[i];
    const SignalId a_src = static_cast<SignalId>(e.tps_a_src);
    const SignalId b_src = static_cast<SignalId>(e.tps_b_src);
    const float    cap   = static_cast<float>(e.ac_duty_cap_pct) * 0.1f;
    const float    ff_rate = 18.0f;                       // %/s — slow, quasi-static sweep for FF mapping
    // Stage-2 crossing recorder: snapshot the hold duty as the plate crosses each CONFIGURED X breakpoint
    // (the client laid the bins; firmware just fills whatever's there). Down + up -> a friction-cancelling
    // midpoint per bin. xb is the live ff_table X axis (uint16 %, read as float).
    const uint16_t* xb = e.ff_table_x_axis;
    const int       xn = static_cast<int>(e.ff_table_x_axis_n);
    auto record = [&](float duty, bool up) {
        for (int k = 0; k < xn; k++) {
            const float xk = static_cast<float>(xb[k]) * 0.1f;
            const bool crossed = up ? (ff_prev_pos_ < xk && pos >= xk) : (ff_prev_pos_ > xk && pos <= xk);
            if (crossed) {
                (up ? ff_up_[k] : ff_down_[k]) = duty;
                (up ? ff_up_ok_[k] : ff_dn_ok_[k]) = true;
            }
        }
        ff_prev_pos_ = pos;
    };
    switch (ac_phase_[i]) {
    case AC_SETTLE:                                   // de-energize: spring settles toward rest
        dis = true; duty_cmd_[i] = 0.0f;
        if (elapsed >= 400) {
            // Capture the de-energized SPRING REST raw now (no power) — resolved to relax_pct once the
            // closed/open span is known (AC_OPEN).
            ac_relax_a_[i] = raw_adc(a_src); ac_relax_b_[i] = raw_adc(b_src);
            ac_last_a_[i] = ac_relax_a_[i]; ac_still_ms_[i] = 0; duty_cmd_[i] = 0.0f;
            ac_phase_[i] = AC_CLOSE; ac_t_[i] = now;
        }
        break;
    case AC_CLOSE: {                                  // drive CLOSED (negative duty) to the closed stop
        dis = false;
        const float rate = e.ac_ramp_pct_s ? static_cast<float>(e.ac_ramp_pct_s) : 50.0f;   // dedicated cal ramp
        duty_cmd_[i] = std::max(-cap, duty_cmd_[i] - rate * dt_ms / 1000.0f);
        const uint16_t a = raw_adc(a_src);
        if (iabs(static_cast<int>(a) - static_cast<int>(ac_last_a_[i])) > 15) { ac_last_a_[i]=a; ac_still_ms_[i]=0; }
        else ac_still_ms_[i] += dt_ms;
        // At the closed stop once the plate is FULLY driven (duty ~cap => past spring breakaway) and has
        // stopped. Requiring ~cap (not cap/2) is what makes the ramp rate irrelevant: at a slow ramp the
        // duty crosses cap/2 while still below breakaway and not-yet-moving, which used to false-latch the
        // "stop" at the rest position. At cap the plate is genuinely pushed, so "not moving" = a real stop.
        if (std::fabs(duty_cmd_[i]) >= cap * 0.95f && ac_still_ms_[i] >= 300) {
            ac_rest_a_[i] = a; ac_rest_b_[i] = raw_adc(b_src);     // CLOSED reference (-> 0%)
            ac_closed_pct_[i] = pos;                               // ... and where the STORED cal says that is
            ac_last_a_[i] = ac_rest_a_[i]; ac_still_ms_[i] = 0; duty_cmd_[i] = 0.0f;
            ac_phase_[i] = AC_OPEN; ac_t_[i] = now;
        } else if (elapsed >= e.ac_timeout_ms) {
            g_text_log.printf("ETB%u findlimits FAILED: no closed stop found (timeout)\n", i);
            // THE KEY-ON SWEEP FAILING IS A FAULT, not a quiet UNCAL. A plate that cannot be driven to
            // a stop at key-on is the seized plate / dead motor / dead bridge this sweep exists to find,
            // and it used to leave no code at all: the body sat at UNCAL with nothing in the table to
            // say why. Unable to reach closed = stuck open.
            if (ac_verify_[i]) latch_fault(i, P_TAC_STUCK_OPEN, now);
            ac_phase_[i] = AC_FAIL; ac_t_[i] = now; dis = true; duty_cmd_[i] = 0.0f;
        }
        break;
    }
    case AC_OPEN: {                                   // drive OPEN (positive duty) to the open stop
        dis = false;
        const float rate = e.ac_ramp_pct_s ? static_cast<float>(e.ac_ramp_pct_s) : 50.0f;    // dedicated cal ramp
        duty_cmd_[i] = std::min(cap, duty_cmd_[i] + rate * dt_ms / 1000.0f);
        const uint16_t a = raw_adc(a_src);
        if (iabs(static_cast<int>(a) - static_cast<int>(ac_last_a_[i])) > 15) { ac_last_a_[i]=a; ac_still_ms_[i]=0; }
        else ac_still_ms_[i] += dt_ms;
        const int moved = iabs(static_cast<int>(a) - static_cast<int>(ac_rest_a_[i]));   // travel from closed
        if (duty_cmd_[i] >= cap * 0.95f && ac_still_ms_[i] >= 300 && moved >= static_cast<int>(e.ac_move_min)) {
            const uint16_t open_b = raw_adc(b_src);
            // Validate the SECOND feedback sensor before trusting it: the sweep is A-driven (stop detection +
            // move-min are on A), so a dead / stuck / miswired B would otherwise be written a degenerate cal
            // and reported OK, only to fail later as an A/B disagreement. Gate B on the same span the plate
            // demonstrably travelled, so a bad B fails findlimits HERE, named.
            const int b_moved = iabs(static_cast<int>(open_b) - static_cast<int>(ac_rest_b_[i]));
            if (b_moved < static_cast<int>(e.ac_move_min)) {
                g_text_log.printf("ETB%u findlimits FAILED: TPS-B span too small (%d counts) - check the B sensor/wiring\n",
                                  i, b_moved);
                if (ac_verify_[i]) latch_fault(i, P_TPS_CORRELATION, now);   // key-on: track B is dead
                ac_phase_[i] = AC_FAIL; ac_t_[i] = now; dis = true; duty_cmd_[i] = 0.0f;
                break;
            }
            // VERIFY MODE — the key-on safety check. Identical sweep, opposite conclusion: instead of
            // defining the stops, compare where they turned out to be against where the stored cal
            // already says they are. Both measurements are in that cal's own units, so a healthy
            // throttle reads ~0 % closed and ~100 % open; anything else means the cal no longer
            // describes this hardware -- a shifted stop, a swapped body, a plate binding short of
            // travel. Nothing is written: findlimits remains the only thing that mutates a cal.
            if (ac_verify_[i]) {
                const float closed_err = std::fabs(ac_closed_pct_[i]);
                const float open_err   = std::fabs(pos - 100.0f);
                if (closed_err <= SWEEP_STOP_TOL_PCT && open_err <= SWEEP_STOP_TOL_PCT) {
                    calibrated_[i] = true;                       // mechanically proven -> loop may close
                    g_text_log.printf("ETB%u key-on sweep OK: closed %d.%d%% open %d.%d%% - cal verified\n", i,
                                      static_cast<int>(ac_closed_pct_[i]),
                                      iabs(static_cast<int>(ac_closed_pct_[i] * 10)) % 10,
                                      static_cast<int>(pos), iabs(static_cast<int>(pos * 10)) % 10);
                    ac_phase_[i] = AC_DONE; ac_t_[i] = now; dis = true; duty_cmd_[i] = 0.0f;
                } else {
                    // Stuck/shifted at whichever end failed: unable to sit at closed = stuck open (P2111),
                    // unable to reach open = stuck closed (P2112). Both already carry that exact meaning.
                    g_text_log.printf("ETB%u key-on sweep FAILED: stops moved - closed %d%% open %d%% "
                                      "(expected ~0/~100) — run findlimits\n",
                                      i, static_cast<int>(ac_closed_pct_[i]), static_cast<int>(pos));
                    latch_fault(i, (closed_err > SWEEP_STOP_TOL_PCT) ? P_TAC_STUCK_OPEN : P_TAC_STUCK_CLOSED, now);
                    ac_phase_[i] = AC_FAIL; ac_t_[i] = now; dis = true; duty_cmd_[i] = 0.0f;
                }
                break;
            }
            write_cal(a_src, ac_rest_a_[i], a);                  // CLOSED stop -> 0%, OPEN stop -> 100%
            write_cal(b_src, ac_rest_b_[i], open_b);
            // Record the spring-rest (limp-home) position as a % of the now-known closed->open span.
            g_config.electronic_throttle.etb[i].relax_pct =
                relax_pct_x10(ac_relax_a_[i], ac_rest_a_[i], a, ac_relax_b_[i], ac_rest_b_[i], open_b);
            ++g_config_generation;                               // Sensors rebuilds with the new cal
            calibrated_[i] = true;                               // feedback trusted -> closed-loop unlocked
            {   // relax_pct is stored x10; emit fixed-point (no float printf on the target).
                const unsigned rx = g_config.electronic_throttle.etb[i].relax_pct;
                g_text_log.printf("ETB%u findlimits OK: closed=%u open=%u (raw A), travel=%d, relax=%u.%u%%\n",
                                  i, static_cast<unsigned>(ac_rest_a_[i]), static_cast<unsigned>(a), moved,
                                  rx / 10u, rx % 10u);
            }
            ac_phase_[i] = AC_DONE; ac_t_[i] = now; dis = true; duty_cmd_[i] = 0.0f;   // limits+relax done
        } else if (elapsed >= e.ac_timeout_ms) {
            g_text_log.printf("ETB%u findlimits FAILED: no open stop / plate stuck (timeout, travel=%d)\n", i, moved);
            if (ac_verify_[i]) latch_fault(i, P_TAC_STUCK_CLOSED, now);   // key-on: cannot open = stuck closed
            ac_phase_[i] = AC_FAIL; ac_t_[i] = now; dis = true; duty_cmd_[i] = 0.0f;  // stuck/dead/miswired
        }
        break;
    }
    case AC_FF_PREP: {                               // drive to the OPEN stop, then start the down-sweep
        dis = false;
        duty_cmd_[i] = std::min(cap, duty_cmd_[i] + ff_rate * dt_ms / 1000.0f);
        if (pos >= 98.0f || duty_cmd_[i] >= cap) {
            // Fill the Y ROW the client chose, clamped to the live bin count (a Y-keyed row can't be
            // reached by "being at that temperature" — the throttle won't track there until it's filled —
            // so the row is picked manually). Y disabled -> the only row is 0.
            const int yn = (e.ff_table_y_en != 0) ? static_cast<int>(e.ff_table_y_axis_n) : 1;
            ff_row_ = static_cast<uint8_t>(std::clamp<int>(ff_row_target_, 0, yn > 0 ? yn - 1 : 0));
            ff_prev_pos_ = pos; ac_phase_[i] = AC_FF_CLOSE; ac_t_[i] = now;
        } else if (elapsed >= e.ac_timeout_ms) {
            g_text_log.printf("ETB%u fillff FAILED: could not reach open stop (timeout)\n", i);
            ac_phase_[i] = AC_FAIL; ac_t_[i] = now; dis = true; duty_cmd_[i] = 0.0f;
        }
        break;
    }
    case AC_FF_CLOSE: {                               // down-sweep: open -> closed, record each X crossing
        dis = false;
        duty_cmd_[i] = std::max(-cap, duty_cmd_[i] - ff_rate * dt_ms / 1000.0f);
        record(duty_cmd_[i], false);
        if (pos <= 2.0f || duty_cmd_[i] <= -cap || elapsed >= 9000) {
            if (xn > 0 && !ff_dn_ok_[0]) { ff_down_[0] = duty_cmd_[i]; ff_dn_ok_[0] = true; }  // closed endpoint
            ff_prev_pos_ = pos; ac_phase_[i] = AC_FF_OPEN; ac_t_[i] = now;
        }
        break;
    }
    case AC_FF_OPEN: {                                // up-sweep: closed -> open, record, then WRITE the row
        dis = false;
        duty_cmd_[i] = std::min(cap, duty_cmd_[i] + ff_rate * dt_ms / 1000.0f);
        record(duty_cmd_[i], true);
        if (pos >= 98.0f || duty_cmd_[i] >= cap || elapsed >= 9000) {
            if (xn > 0 && !ff_up_ok_[xn - 1]) { ff_up_[xn - 1] = duty_cmd_[i]; ff_up_ok_[xn - 1] = true; }  // open endpoint
            write_ff_row(xn);
            g_text_log.printf("ETB%u fillff OK: ff_table row %u filled (%d bins)\n", i, ff_row_, xn);
            ac_phase_[i] = AC_DONE; ac_t_[i] = now; dis = true; duty_cmd_[i] = 0.0f;
        }
        break;
    }
    case AC_DONE:  dis = true; duty_cmd_[i] = 0.0f; if (elapsed >= 400) ac_phase_[i] = AC_OFF; break;
    case AC_FAIL:  dis = true; duty_cmd_[i] = 0.0f; if (elapsed >= 400) ac_phase_[i] = AC_OFF; break;
    default:       dis = true; duty_cmd_[i] = 0.0f; break;
    }
}

// PID tuning rules: each maps the measured (Ku, Tu) to PARALLEL gains Kp/Ki/Kd via Kp=cKp*Ku, Ti=cTi*Tu,
// Td=cTd*Tu (Ki=Kp/Ti, Kd=Kp*Td). All five are tiny, so the autotune reports every candidate; the command's
// `rule` arg selects which one is written. Indices match enum TuneRule.
static void at_tune_coeffs(uint8_t rule, float ku, float tu, float& kp, float& ki, float& kd) {
    struct R { float ckp, cti, ctd; };
    static const R RULES[5] = {
        { 1.0f / 2.2f, 2.2f, 1.0f / 6.3f },   // 0 Tyreus-Luyben (robust, low overshoot)
        { 0.6f,        0.5f, 0.125f       },   // 1 Ziegler-Nichols classic
        { 0.2f,        0.5f, 0.33f        },   // 2 ZN no-overshoot
        { 0.33f,       0.5f, 0.33f        },   // 3 ZN some-overshoot
        { 0.7f,        0.4f, 0.15f        },   // 4 Pessen integral rule
    };
    const R& r = RULES[rule < 5 ? rule : 0];
    kp = r.ckp * ku;
    const float ti = r.cti * tu, td = r.ctd * tu;
    ki = (ti > 0.0f) ? kp / ti : 0.0f;
    kd = kp * td;
}

// Engine-stopped relay-feedback autotune at each FF X breakpoint. Per point: settle to the breakpoint
// (FF hold + gentle P), then bang-bang a relay of +/-D about the FF hold-duty bias and measure the limit
// cycle (amplitude a, period Tu) over a few cycles -> Ku = 4D/(pi*a). Walks every breakpoint and keeps the
// worst case (smallest Ku); finalize_autotune() applies the chosen rule + writes the gains.
void ElectronicThrottle::run_autotune(unsigned i, const EtbConfig& e, SignalBus& bus, float pos,
                                      uint32_t now, float dt_ms, bool& dis) {
    const uint32_t elapsed = now - ac_t_[i];
    const float    dt_s = dt_ms / 1000.0f;
    const float    cap = static_cast<float>(e.ac_duty_cap_pct) * 0.1f;
    // Autotune knobs — all config (plant-specific; retune for a different throttle with no reflash).
    const float    D           = static_cast<float>(e.at_relay_pct) * 0.1f;     // relay amplitude (% duty)
    const float    settle_band = e.at_settle_band_pct * 0.1f;            // park tolerance (%)
    const uint32_t settle_dw   = e.at_settle_dwell_ms;                   // in-band dwell before relay
    const int      cyc_target  = static_cast<int>(e.at_cycles) + 2;      // + 2 transient discarded
    const float    settle_kp   = e.at_settle_kp * 0.001f;               // settle P gain
    const float    settle_ki   = e.at_settle_ki * 0.001f;               // settle integrator rate
    const float    slew_rate   = static_cast<float>(e.at_slew_pct_s);    // settle setpoint slew (%/s)
    const float    check_duty  = static_cast<float>(e.at_check_duty_pct) * 0.1f;// response-check open duty
    const float    check_min   = static_cast<float>(e.at_check_min_pct) * 0.1f; // response-check motion threshold
    const uint32_t relay_to    = e.at_relay_timeout_ms;                  // relay-phase timeout
    const uint16_t* xb = e.ff_table_x_axis;
    const int       xn = static_cast<int>(e.ff_table_x_axis_n);
    if (xn <= 0) {                                            // no breakpoints -> nothing to tune
        g_text_log.printf("ETB%u autotune FAILED: ff_table has no breakpoints (run fillff first)\n", i);
        ac_phase_[i] = AC_FAIL; ac_t_[i] = now; dis = true; duty_cmd_[i] = 0.0f; return;
    }
    at_target_ = static_cast<float>(xb[at_point_ < xn ? at_point_ : xn - 1]) * 0.1f;
    at_bias_   = tbl::tableresolve(etb_ff_table_desc(&e), bus, at_target_,
                                   static_cast<SignalId>(e.ff_table_y_src));

    // Advance to the next breakpoint, or finalize when the grid is exhausted. Reset the settle integrator
    // so each point finds its own hold duty afresh.
    auto next_point = [&] {
        // Carry at_settle_i_ ACROSS points: the hold duty is continuous along the travel, so the previous
        // bin's value is a near-right starting guess — the high-% bins then settle fast instead of timing
        // out trying to build their (larger) hold duty from zero. Only start_autotune resets it.
        at_point_++; at_cyc_ = 0; at_band_ms_ = 0; target_cmd_[i] = pos;   // slew starts at the current pos
        if (at_point_ >= xn) { finalize_autotune(i); ac_phase_[i] = AC_DONE; ac_t_[i] = now; dis = true; duty_cmd_[i] = 0.0f; }
        else                 { ac_phase_[i] = AT_SETTLE; ac_t_[i] = now; }
    };

    // Key-on sanity check: drive OPEN and confirm the plate actually swings past spring-rest. Catches a
    // dead/unpowered/miswired ETB (no H-bridge, EN not asserting, wrong dir_invert, lost feedback) up front
    // with one clear abort, instead of a flood of per-point "skipped" lines.
    if (ac_phase_[i] == AT_CHECK) {
        dis = false;
        duty_cmd_[i] = std::min(check_duty, cap);             // test open drive
        if (elapsed >= 800) {
            if (pos >= check_min) {
                g_text_log.printf("ETB%u autotune: plate responds (open->%d%%), relay-tuning %d points\n",
                                  i, static_cast<int>(pos), xn);
                at_settle_i_ = 0.0f; at_band_ms_ = 0; target_cmd_[i] = pos;   // slew starts at the current position
                ac_phase_[i] = AT_SETTLE; ac_t_[i] = now;
            } else {
                g_text_log.printf("ETB%u autotune ABORT: plate not responding (%d%% on open) - check "
                                  "H-bridge/EN/wiring/dir_invert/feedback\n", i, static_cast<int>(pos));
                ac_phase_[i] = AC_FAIL; ac_t_[i] = now; dis = true; duty_cmd_[i] = 0.0f;
            }
        }
        return;
    }

    if (ac_phase_[i] == AT_SETTLE) {
        dis = false;
        // The plate is twitchy (small duty -> big swing), so don't command the breakpoint outright — SLEW a
        // setpoint toward it and run a GENTLE P + slow integral. The integral absorbs the steady hold duty
        // (robust to an imperfect FF map); the small P + slew keep the error tiny so it never slams. Only
        // start the relay after the plate has actually PARKED in-band for a dwell (not just passed through).
        const float step = slew_rate * dt_s;                                // setpoint slew, %/s
        target_cmd_[i] += std::clamp(at_target_ - target_cmd_[i], -step, step);
        const float err = target_cmd_[i] - pos;
        at_settle_i_ = std::clamp(at_settle_i_ + settle_ki * err * dt_s, -cap, cap);
        duty_cmd_[i] = std::clamp(at_bias_ + settle_kp * err + at_settle_i_, -cap, cap);   // FF + gentle P + I
        const bool parked = std::fabs(at_target_ - target_cmd_[i]) < 0.5f             // slew has arrived
                         && std::fabs(at_target_ - pos) < settle_band;                // ...and plate is there
        at_band_ms_ = parked ? at_band_ms_ + dt_ms : 0;
        if (at_band_ms_ >= settle_dw) {                                    // parked & steady -> begin relay
            at_above_ = (pos >= at_target_);
            at_pmax_ = pos; at_pmin_ = pos; at_cross_t_ = now;
            at_cyc_ = 0; at_tu_sum_ = 0.0f; at_a_sum_ = 0.0f;
            ac_phase_[i] = AT_RELAY; ac_t_[i] = now;
        } else if (elapsed >= e.ac_timeout_ms) {
            g_text_log.printf("ETB%u autotune @%d%%: could not settle (skipped)\n", i, static_cast<int>(at_target_));
            next_point();
        }
        return;
    }

    // AT_RELAY: relay about the measured hold duty (at_settle_i_ frozen) + limit-cycle measurement.
    dis = false;
    duty_cmd_[i] = std::clamp(at_bias_ + at_settle_i_ + (pos < at_target_ ? D : -D), -cap, cap);
    at_pmax_ = std::max(at_pmax_, pos);
    at_pmin_ = std::min(at_pmin_, pos);
    const bool above = (pos >= at_target_);
    if (above != at_above_) {                                // setpoint crossing (half cycle)
        if (!above) {                                        // downward crossing = one full period
            const float period = static_cast<float>(now - at_cross_t_) / 1000.0f;
            const float ampl   = 0.5f * (at_pmax_ - at_pmin_);
            at_cross_t_ = now; at_pmax_ = pos; at_pmin_ = pos;
            if (period > 0.02f && ampl > 0.2f) {
                if (at_cyc_ >= 2) { at_tu_sum_ += period; at_a_sum_ += ampl; }   // discard first 2 (transient)
                at_cyc_++;
            }
        }
        at_above_ = above;
    }
    if (at_cyc_ >= cyc_target) {                             // 2 transient + at_cycles measured
        const int   n  = at_cyc_ - 2;
        const float tu = at_tu_sum_ / static_cast<float>(n);
        const float a  = at_a_sum_  / static_cast<float>(n);
        if (a > 0.05f && tu > 0.0f) {
            const float ku = 4.0f * D / (3.14159265f * a);
            // No float printf on the target — emit fixed-point: Ku as x.xx, Tu in ms.
            g_text_log.printf("ETB%u autotune @%d%%: Ku=%d.%02d Tu=%dms\n",
                              i, static_cast<int>(at_target_),
                              static_cast<int>(ku), static_cast<int>(ku * 100.0f) % 100,
                              static_cast<int>(tu * 1000.0f + 0.5f));
            if (!at_any_ || ku < at_ku_worst_) { at_ku_worst_ = ku; at_tu_worst_ = tu; at_any_ = true; }
        }
        next_point();
    } else if (elapsed >= relay_to) {                        // must gather its cycles before this timeout
        g_text_log.printf("ETB%u autotune @%d%%: no clean oscillation (skipped)\n", i, static_cast<int>(at_target_));
        next_point();
    }
}

void ElectronicThrottle::finalize_autotune(unsigned i) {
    if (!at_any_) {
        g_text_log.printf("ETB%u autotune FAILED: no breakpoint produced a stable oscillation\n", i);
        return;
    }
    static const char* const NAMES[5] =
        { "Tyreus-Luyben", "ZN-classic", "ZN-no-overshoot", "ZN-some-overshoot", "Pessen" };
    // Fixed-point (no float printf on the target): a gain g prints as %d.%03d via its x1000 value (which
    // is also exactly what's stored). Tu in ms.
    auto milli = [](float g) { return static_cast<int>(g * 1000.0f + 0.5f); };
    g_text_log.printf("ETB%u autotune: worst-case Ku=%d.%02d Tu=%dms ->\n",
                      i, static_cast<int>(at_ku_worst_), static_cast<int>(at_ku_worst_ * 100.0f) % 100,
                      static_cast<int>(at_tu_worst_ * 1000.0f + 0.5f));
    for (int r = 0; r < 5; r++) {
        float kp, ki, kd; at_tune_coeffs(static_cast<uint8_t>(r), at_ku_worst_, at_tu_worst_, kp, ki, kd);
        const int mp = milli(kp), mi = milli(ki), md = milli(kd);
        g_text_log.printf("  [%d] %-18s kp=%d.%03d ki=%d.%03d kd=%d.%03d%s\n", r, NAMES[r],
                          mp / 1000, mp % 1000, mi / 1000, mi % 1000, md / 1000, md % 1000,
                          r == at_rule_ ? "  <- applied" : "");
    }
    float kp, ki, kd; at_tune_coeffs(at_rule_, at_ku_worst_, at_tu_worst_, kp, ki, kd);
    // WRITTEN AS FLOATS, exactly as computed. These were uint16 x0.001 clamped to 60000, i.e. a ceiling
    // of 60.0 and a thousandth of resolution — and this plate's own autotune asks for ki in the hundreds,
    // so the integral gain was silently pinned at the clamp and the plate could never be tuned to what
    // the measurement called for. The fields are float now; nothing is scaled or clipped on the way in.
    EtbConfig& w = g_config.electronic_throttle.etb[i];
    w.kp = kp; w.ki = ki; w.kd = kd;
    ++g_config_generation;                                   // studio re-reads etb[i] -> the new gains
}

// Stage-2 write: fuse the down/up crossings into the friction-free MIDPOINT per X bin, linearly fill any
// bin the sweep missed (hold the ends), and write the row into this ETB's ff_table (cells int16 x0.1).
// Stride is row-major at the LIVE x count: cell[ff_row_ * xn + k] (Z absent -> z=0).
void ElectronicThrottle::write_ff_row(int xn) {
    if (xn <= 0) return;
    EtbConfig& w = g_config.electronic_throttle.etb[ff_etb_];
    float v[FF_BINS]; bool ok[FF_BINS];
    for (int k = 0; k < xn; k++) {
        ok[k] = ff_dn_ok_[k] || ff_up_ok_[k];
        if (ff_dn_ok_[k] && ff_up_ok_[k]) v[k] = 0.5f * (ff_down_[k] + ff_up_[k]);
        else if (ff_dn_ok_[k])            v[k] = ff_down_[k];
        else if (ff_up_ok_[k])            v[k] = ff_up_[k];
        else                              v[k] = 0.0f;
    }
    int prev = -1;
    for (int k = 0; k < xn; k++) {
        if (!ok[k]) continue;
        if (prev < 0) for (int j = 0; j < k; j++) v[j] = v[k];                                   // head
        else for (int j = prev + 1; j < k; j++)
            v[j] = v[prev] + (v[k] - v[prev]) * static_cast<float>(j - prev) / static_cast<float>(k - prev);
        prev = k;
    }
    if (prev >= 0) for (int j = prev + 1; j < xn; j++) v[j] = v[prev];                           // tail
    for (int k = 0; k < xn; k++)
        w.ff_table[ff_row_ * xn + k] =
            static_cast<int16_t>(std::lround(std::clamp(v[k], -100.0f, 100.0f) * 10.0f));
    ++g_config_generation;
}

// Closed-loop position control (Stage 3, BIPOLAR). Slews the target, runs a SIGNED PID — output > 0
// drives the plate OPEN, < 0 drives it CLOSED — so the plate is positioned by active torque in both
// directions (no reliance on the spring / brake). Trips the fail-safe latch on a debounced post-cal A/B
// mismatch or a sustained position runaway. Returns the SIGNED duty published on etb_duty_N.
float ElectronicThrottle::closed_loop(unsigned i, const EtbConfig& e, SignalBus& bus, float pos, bool armed, float demand,
                                   bool match_fault, float dt_s, uint32_t now, bool& dis) {
    // Feedback integrity (L2): calibrated A/B disagreement OR lost/stale feedback -> we can't trust the
    // position, so we can't safely close the loop. Latch + raise the correlation/feedback code.
    if (match_fault) latch_fault(i, P_TPS_CORRELATION, now);
    // A LATCHED FAULT CUTS THIS FRAME, not the next. Returning a live duty with dis=false after latching
    // drove the plate one more frame on feedback just declared untrustworthy.
    if (fault_latched_[i]) { integ_[i] = 0.0f; loop_live_[i] = false; dis = true; return 0.0f; }

    const float min_tps = static_cast<float>(e.min_tps_pct) * 0.1f;       // scale 0.1
    const float max_tps = static_cast<float>(e.max_tps_pct) * 0.1f;
    // Target = the arbitrated throttle_demand (App+idle+caps via the Throttle module), clamped to the
    // plate's authority. A bench `throttle` nudge overrides it while armed. NOTE: no seat-release — the
    // loop HOLDS the target with torque (incl. fully closed), so it never fights the spring into a
    // relaxation oscillation. Idle hold-current at the closed/idle target is small and intentional.
    const float target  = std::clamp(armed ? manual_pct_[i] : demand, min_tps, max_tps);
    const float srate   = (target > target_cmd_[i]) ? static_cast<float>(e.open_rate_pct_s)
                                                     : static_cast<float>(e.close_rate_pct_s);
    if (srate <= 0.0f || dt_s <= 0.0f) target_cmd_[i] = target;
    else {
        const float step = srate * dt_s;
        target_cmd_[i] = (target > target_cmd_[i]) ? std::min(target, target_cmd_[i] + step)
                                                   : std::max(target, target_cmd_[i] - step);
    }

    // Spring FEED-FORWARD from this ETB's own ff_table (per-instance: etb[i] IS the table, no Z). The
    // static hold duty at the TARGET position neutralises the compound limp-home spring (incl. its preload
    // step) BEFORE the PID runs, so the error starts ~0. Typed coords: X = slewed target (value),
    // Y = CLT (bus channel, off by default).
    const float ff = tbl::tableresolve(etb_ff_table_desc(&e), bus, target_cmd_[i],
                                       static_cast<SignalId>(e.ff_table_y_src));
    // Gains are FLOATS, used as stored. They were uint16 x0.001 (hence the x0.001 that used to be here)
    // and that field clamped at 60.0 — while this plate's own autotune asks for ki in the hundreds, so
    // the integral gain could never be applied as measured. Reading them scaled would divide every gain
    // by a thousand and leave effectively no loop at all.
    const float kp = e.kp, ki = e.ki, kd = e.kd;
    const float err   = target_cmd_[i] - pos;
    // No derivative on the first frame of a (re)engagement: last_err_ is whatever the loop last saw before
    // a fault, a pedal cut-out or a calibration, and differencing against it is a D-kick into the plate.
    const float deriv = (dt_s > 0.0f && loop_live_[i]) ? (err - last_err_[i]) / dt_s : 0.0f;
    last_err_[i] = err;
    loop_live_[i] = true;
    const float iclamp = static_cast<float>(e.iterm_max_pct) * 0.1f;
    const float base = ff + kp * err + kd * deriv;       // FF + P + D
    // Conditional-integration anti-windup: integrate ONLY when the output isn't saturated — i.e. never
    // wind up while the plate is commanded-but-stuck. The I-term is also hard-clamped to +/-iterm_max.
    if (std::fabs(base + integ_[i]) < 99.0f)
        integ_[i] = std::clamp(integ_[i] + ki * err * dt_s, -iclamp, iclamp);
    float duty = std::clamp(base + integ_[i], -100.0f, 100.0f);   // SIGNED: + opens, - closes

    // Stall detector (L2): a real position error + meaningful drive but the plate isn't moving = stuck
    // or motor disconnected. ETB-agnostic (no current/resistance assumption) and the SAFE response is to
    // CUT (not drive harder) — important now that the 9201's chopper is the only hardware current guard.
    // Direction names the fault: stuck while opening = stuck closed; stuck while closing = stuck open.
    const bool trying = std::fabs(err) > STALL_ERR_PCT && std::fabs(duty) > STALL_DUTY_PCT;
    if (!trying || std::fabs(pos - stall_pos_[i]) > STALL_MOVE_PCT) {
        stall_ms_[i] = 0; stall_pos_[i] = pos;          // moving (or not trying) -> reset
    } else {
        stall_ms_[i] += dt_s * 1000.0f;   // float: a 0.99 ms frame truncated to 0 stretched every debounce
    }
    if (stall_ms_[i] > STALL_MS) {
        latch_fault(i, (err > 0.0f) ? P_TAC_STUCK_CLOSED : P_TAC_STUCK_OPEN, now);
        integ_[i] = 0.0f; loop_live_[i] = false; dis = true;   // stuck plate: cut now, do not drive it again
        return 0.0f;
    }

    dis = false;                                        // closed loop runs the bridge enabled
    return duty;
}

void ElectronicThrottle::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    if (!cfg_) return;

    const uint32_t now   = platform_get_tick_ms();
    // TIMESTEP FROM THE CYCLE COUNTER, not the millisecond tick. The tick is whole milliseconds and this
    // loop runs at 1 kHz, so `now - last_ms_` is an integer nominally equal to 1 — a timestep carrying up
    // to 100 % error, and it is the divisor of the derivative term and the multiplier of the integral.
    // Two frames inside one tick gave dt = 0 and dropped both terms; an overrunning frame still claimed
    // 1 ms. platform_cyccnt() is the free-running CPU cycle counter (4.6 ns at 216 MHz); it wraps every
    // ~20 s and unsigned subtraction handles that. On the host build it is stubbed to 0, so fall back to
    // the tick — without the fallback dt would be permanently zero, silently deleting the integral AND
    // derivative terms while the tests went on passing.
    const uint32_t cyc = platform_cyccnt();
    float dt_ms;
    if (cyc != 0) {
        dt_ms = (last_cyc_ != 0)
              ? static_cast<float>(cyc - last_cyc_) * 1000.0f / static_cast<float>(platform_cpu_hz())
              : 0.0f;
        last_cyc_ = cyc;
    } else {
        dt_ms = (last_ms_ != 0) ? static_cast<float>(now - last_ms_) : 0.0f;
    }
    if (dt_ms > 100.0f) dt_ms = 0.0f;              // a wrap or a long stall is not a timestep
    const float    dt_s  = dt_ms / 1000.0f;
    last_ms_ = now;

    // A throttle body is live iff its own etb[i].enabled is set; the module has no separate enable.
    // Demand arbitration runs whenever ANY instance is enabled (else there is nothing to drive).
    bool any_enabled = false;
    for (unsigned i = 0; i < N; i++) any_enabled |= (cfg_->etb[i].enabled != 0);

    // --- DEMAND ARBITRATION (folded in from the old Throttle module): pedal floored on the idle slice,
    // clamped by traction/torque caps -> throttle_demand. Published as a BASE-priority bus signal so the
    // Lua plane can override it BEFORE the actuator (below) consumes it off the bus. No body enabled or
    // pedal-invalid -> demand collapses to the idle floor (or 0).
    if (!any_enabled) {
        bus.set(SIG_THROTTLE_DEMAND, 0.0f, false, now);     // -> actuator fail-safes (DIS / spring-return)
    } else {
        float pedal = bus.valid(SIG_PEDAL_DEMAND)
                    ? std::clamp(bus.get(SIG_PEDAL_DEMAND, 0.0f), 0.0f, 100.0f) : 0.0f;
        // Cruise sets a throttle floor UNDER the pedal: whichever is higher wins, so the driver always
        // overrides by pressing harder. 0 (or absent) when cruise is inactive -> no effect.
        if (bus.valid(SIG_CRUISE_DEMAND))
            pedal = std::max(pedal, std::clamp(bus.get(SIG_CRUISE_DEMAND, 0.0f), 0.0f, 100.0f));
        const float idle_duty = bus.valid(SIG_IDLE_DUTY)
                              ? std::clamp(bus.get(SIG_IDLE_DUTY, 0.0f), 0.0f, 100.0f) : 0.0f;
        const float idle_floor = idle_duty * (static_cast<float>(cfg_->idle_authority) * 0.1f / 100.0f);
        float demand = idle_floor + (pedal / 100.0f) * (100.0f - idle_floor);   // pedal rides ABOVE the floor
        // The selector is int16 (-1 = unassigned). It was taken as uint8, which truncated any signal id
        // above 255 (there are ~500) to `id & 0xFF` — capping the throttle on an unrelated signal — and only
        // treated "unassigned" right by the accident that -1 truncates to 255.
        auto cap = [&](int16_t src) -> float {
            if (src < 0 || src >= static_cast<int16_t>(SIG_COUNT)) return 100.0f;   // unassigned: no cap
            const SignalId s = static_cast<SignalId>(src);
            return bus.valid(s) ? std::clamp(bus.get(s, 100.0f), 0.0f, 100.0f) : 100.0f;
        };
        demand = std::min(demand, cap(cfg_->traction_cap_src));
        demand = std::min(demand, cap(cfg_->torque_cap_src));
        bus.set(SIG_THROTTLE_DEMAND, std::clamp(demand, 0.0f, 100.0f), true, now, ttl());
    }

    const auto engine_state = static_cast<EngineRunState>(
        static_cast<int>(bus.get(wk::engine_state, 0.0f)));
    const bool engine_stopped = (engine_state == EngineRunState::STOPPED);
    engine_stopped_ = engine_stopped;          // for the command entry points (see the header)

    // THE ENGINE IS HELD DISABLED WHILE ANY ROUTINE DRIVES THE PLATE. These sweeps take the throttle to
    // its OPEN stop -- wide open for a second or so -- and 12 V switching includes a reset, so a driver
    // who turns the key and goes straight to the starter without pausing would be cranking into a fully
    // open throttle. Requiring engine-stopped to START says nothing about the next half-second, so the
    // routine asserts fuel + spark cut for its whole duration (validity-OR, the same way the rev limiter
    // and flat shift request one) and keeps holding it briefly after the routine ends OR aborts, giving
    // the spring time to take the plate home before the engine is allowed to fire at all.
    if (routine_busy()) sweep_cut_until_ = now + SWEEP_CUT_HOLD_MS;
    if (sweep_cut_until_ != 0 && static_cast<int32_t>(now - sweep_cut_until_) < 0) {
        bus.set_bool(wk::ign_cut,  true, now, ttl());
        bus.set_bool(wk::fuel_cut, true, now, ttl());
    }

    // Pedal trust, read from the App module's OWN published state rather than inferred from the demand
    // value -- 0 is a perfectly legitimate demand and is exactly what App publishes when it faults, so
    // the value cannot distinguish "foot off" from "cannot be believed". app_state 2 = correlation
    // fault (P2138), 3 = A missing.
    //
    // Gated on the APP module being enabled: with no pedal configured there is nothing to distrust, and
    // a bench must still be able to drive the plate. An ENABLED pedal that has gone silent counts as
    // untrusted -- where the throttle's authority is concerned, silence is not consent.
    const bool pedal_untrusted =
        g_config.app.enabled &&
        (!bus.valid(SIG_APP_STATE) || bus.get(SIG_APP_STATE, 0.0f) >= 2.0f);

    // --- KEY-ON GATES THE THROTTLE, and it is the KEY EDGE that arms the verify, not reset ---
    // With the key off, Sensors::update runs ONLY the bootstrap battery sensor -- every other analog
    // front-end hangs off unpowered 5 V followers and would read garbage -- so the TPS pair publishes
    // nothing at all. That is indistinguishable, to this loop, from feedback that has died: have_pos
    // stays false, disagree_ms_ accumulates every frame, and tps_match_ms later the key-on verify takes
    // its failure branch and LATCHES P2135. A latch clears only via findlimits, so an ECU powered up
    // with the key off had a dead throttle for the rest of that power cycle -- caused entirely by
    // checking feedback that could not physically be there yet.
    //
    // So: park while the key is off (nothing to read, nothing that can drive), and re-arm the stored-cal
    // verify on the rising edge, when the sensors are live and the answer means something. The latch
    // itself is NOT cleared here -- a genuine fault still demands a reset, not a key cycle.
    extern bool g_system_active;               // key-on: battery over threshold, owned by EngineTask
    const bool key_on = g_system_active;
    if (key_on && !key_prev_) { reset_state(); adopt_stored_cal(); }
    key_prev_ = key_on;

    for (unsigned i = 0; i < N; i++) {
        const EtbConfig& e = cfg_->etb[i];

        if (!key_on) {
            // Park: bridge off, loop state cleared, no fault accumulated on absent feedback. Any routine
            // running when the key dropped is ANSWERED rather than left hanging, exactly as the disabled
            // branch below does -- the studio is waiting on a reply that can no longer come.
            pos_primed_[i]=false; disagree_ms_[i]=0; manual_until_ms_[i]=0; duty_cmd_[i]=0.0f;
            target_cmd_[i]=0.0f; integ_[i]=0.0f; stall_ms_[i]=0;
            if (ac_phase_[i] != AC_OFF) {
                ac_phase_[i] = AC_OFF;
                if (cmdstate::phase_of(g_command_state) == cmdstate::RUNNING &&
                    cmdstate::op_of(g_command_state) == ac_op_[i])
                    set_command_state(static_cast<cmdstate::Op>(ac_op_[i]), cmdstate::FAIL);
            }
            bus.set_bool(EN_SIG[i], false, now, ttl());   // HBridge drives only while high AND fresh
            continue;
        }

        if (!e.enabled) {
            pos_primed_[i]=false; disagree_ms_[i]=0; manual_until_ms_[i]=0; duty_cmd_[i]=0.0f; ac_phase_[i]=AC_OFF;
            // A routine started on a DISABLED body must still be ANSWERED. Setting AC_OFF is not enough:
            // the code that turns AC_OFF into FAIL lives at the FOOT of this loop, and `continue` skips
            // it — so command_state sat at RUNNING for ever and the studio waited on a reply that could
            // not come. Exactly the hang pedalcal had, for the same reason: the module that runs the
            // routine is the module that is switched off.
            if (cmdstate::phase_of(g_command_state) == cmdstate::RUNNING &&
                cmdstate::op_of(g_command_state) == ac_op_[i])
                set_command_state(static_cast<cmdstate::Op>(ac_op_[i]), cmdstate::FAIL);
            continue;
        }

        // --- read the two analog TPS, resolve A/B plate position (uses the LIVE cal) ---
        const SignalId a = static_cast<SignalId>(e.tps_a_src);
        const SignalId b = static_cast<SignalId>(e.tps_b_src);
        // Claim the rate this loop needs on the channels it reads, made HERE by the code doing the
        // reading so it exists exactly while this ETB is enabled. A 1 kHz position loop on a 200 Hz
        // sensor sees the same value five frames running, which leaves the derivative differentiating
        // a staircase.
        sigrate::need(a, cadence::ElectronicThrottle);
        sigrate::need(b, cadence::ElectronicThrottle);
        const bool  va = bus.valid(a);
        const bool  vb = bus.valid(b);
        const float ta = bus.get(a, 0.0f);
        const float tb = bus.get(b, 0.0f);
        // Position IS tps_a (the blade angle, 0..100%). tps_b is the correlation CHECK only — it never
        // contributes to the value. If A is lost, fall back to B as a usable reading but leave it
        // unmatched so it faults to limp (we can't trust an uncorrelated single sensor).
        float resolved = 0.0f; bool have_pos = false; bool matched = false;
        if (va)      { resolved = ta; have_pos = true;
                       matched  = vb && std::fabs(ta - tb) <= static_cast<float>(e.tps_match_err_pct) * 0.1f; }
        else if (vb) { resolved = tb; have_pos = true; matched = false; }
        if (have_pos && matched) disagree_ms_[i]  = 0;
        else                     disagree_ms_[i] += dt_ms;
        // DEBOUNCED UNIFORMLY. This was `(!have_pos) || (disagree_ms_ >= tps_match_ms)`, so a single
        // frame without valid feedback latched P2135 for good while an A/B disagreement got 200 ms of
        // grace. The TPS is a 200 Hz sensor and the loop runs at 1 kHz, so there is no position at all
        // until its first publish — harmless only while calibrated_ was false at boot and the loop never
        // ran. The moment the calibration was adopted from the tune, every boot latched on frame 0.
        // disagree_ms_ already accumulates when have_pos is false, so one test covers both.
        const bool match_fault = (disagree_ms_[i] >= e.tps_match_ms);

        // PLATE POSITION IS SERVOED AND PUBLISHED RAW. A configurable EMA sat here (tps_filter_scale,
        // 10 ms by default) and it is gone, along with the 50 ms per-sensor EMA and the 3 ms ADC IIR
        // underneath it. Together they put ~158 degrees of phase into the feedback at the plant's own
        // ultimate frequency, which is what made this plate look stick-slip-dominated and prompted a
        // friction table, break-loose probing and dither to compensate for it. None of that is here,
        // and none of it is needed: the loop just needs to see the plate when it moves.
        const float out_pos = resolved;
        pos_primed_[i] = have_pos;
        bus.set(POS_SIG[i], out_pos, have_pos, now, ttl());

        // --- key-on verify: validate the STORED cal before the loop is allowed to close ---
        // Passive. No movement, so it needs neither a feed-forward table nor tuned gains to be correct,
        // and it cannot fling the plate. What it proves is that the cal in the tune still describes THIS
        // hardware: both tracks live, agreeing through the cal, and reading inside the span the cal
        // defines. That catches a swapped or dead track, a throttle body changed under a stale cal, and a
        // cal written for different wiring -- the cases where closing a loop would drive somewhere wrong.
        //
        // Deliberately NOT checked: at-rest position against relax_pct. The doc asks for it, but this
        // plate is friction-dominated -- ca12de6 measured it holding wherever it is left across 92 % of
        // travel at any duty from 15 to 25 -- so a de-energised plate does not return to spring rest and
        // the check would false-fail on a perfectly good cal.
        if (cal_pending_[i]) {
            if (have_pos && matched) {
                const bool in_span = (out_pos > -5.0f && out_pos < 105.0f);   // railed => track dead/miswired
                cal_pending_[i] = false;
                if (in_span) {
                    g_text_log.printf("ETB%u key-on verify OK: stored cal accepted (A=%d.%d%% B=%d.%d%%)\n", i,
                                      static_cast<int>(ta), iabs(static_cast<int>(ta * 10)) % 10,
                                      static_cast<int>(tb), iabs(static_cast<int>(tb * 10)) % 10);
                    // The SENSING half is verified. Now prove the MECHANICAL half: run the findlimits
                    // sweep in COMPARE mode, which only grants calibrated_ if the plate still reaches
                    // both stops where the cal says they are. A passive check cannot see a seized plate,
                    // a broken return spring, a shifted stop or a dead bridge -- all of which pass it and
                    // are then found by the engine.
                    if (engine_stopped && !routine_busy()) {
                        start_autocal(static_cast<uint8_t>(i), /*verify=*/true);
                        g_text_log.printf("ETB%u key-on sweep: checking travel to both stops\n", i);
                    } else {
                        // Cannot sweep (engine alive, or another routine holds the plate). Take the
                        // passive result: a running engine is worse off with a dead throttle than with
                        // an unswept one.
                        calibrated_[i] = true;
                    }
                } else {
                    latch_fault(i, P_TPS_CORRELATION, now);
                    g_text_log.printf("ETB%u key-on verify FAILED: position %d%% outside the stored cal span "
                                      "- run findlimits\n", i, static_cast<int>(out_pos));
                }
            } else if (match_fault) {      // never became valid / never agreed within the debounce
                cal_pending_[i] = false;
                latch_fault(i, P_TPS_CORRELATION, now);
                g_text_log.printf("ETB%u key-on verify FAILED: feedback %s - run findlimits\n", i,
                                  have_pos ? "tracks disagree" : "not readable");
            }
        }

        // --- drive arbitration: autocal > latched fault > pedal-untrusted > closed-loop > open nudge ---
        //
        // THERE IS NO "ENGINE RUNNING" CUT HERE ANY MORE, and there must not be. A branch sitting second
        // in this chain zeroed duty, target and the integrator and dropped the bridge whenever the engine
        // was not STOPPED — so the throttle was dead in the one state a drive-by-wire throttle exists for,
        // and the ETB could only ever be exercised on a static bench. Found on the rig: the pedal drove
        // the plate, the trigger was connected, rpm appeared, and the throttle went dead.
        //
        // What it was REACHING for is real — a calibration sweep drives the plate to its open stop, and
        // that must never happen with an engine turning. But that belongs at the door of the routines,
        // not across normal driving, and it is enforced in three places that have nothing to do with this
        // branch: start_autocal/start_fillff/start_autotune refuse outright while the engine runs, the
        // AC_OFF branch below aborts a routine the instant the engine starts, and routine_busy() holds
        // fuel + spark cut for the whole sweep plus a hold-off after it.
        // THE MANUAL NUDGE IS BENCH-ONLY, and this is where the engine-running interlock belongs. The
        // nudge drives the plate to a commanded duty open-loop, ignoring the pedal — the same class of
        // thing as a calibration sweep, and just as unwelcome with an engine turning. Gating it here
        // rather than across the whole chain is the difference between "no bench commands while
        // running" and "no throttle while running"; the branch this replaces did the latter.
        const bool armed = (manual_until_ms_[i] != 0) &&
                           (static_cast<int32_t>(now - manual_until_ms_[i]) < 0) &&
                           engine_stopped;
        if (!armed) manual_until_ms_[i] = 0;
        bool dis;
        bool ran_loop = false;                              // closed_loop() ran this frame (D-term continuity)
        if (ac_phase_[i] != AC_OFF) {                       // autocal/fillff/autotune owns the ETB while running
            if (!engine_stopped) { ac_phase_[i] = AC_OFF; duty_cmd_[i] = 0.0f; dis = true; }
            else if (ac_phase_[i] == AT_CHECK || ac_phase_[i] == AT_SETTLE || ac_phase_[i] == AT_RELAY)
                run_autotune(i, e, bus, out_pos, now, dt_ms, dis);   // PID autotune (relay feedback)
            else run_autocal(i, e, bus, out_pos, now, dt_ms, dis);
        } else if (fault_latched_[i]) {                     // L2 fail-safe latch -> DIS (clear via autocal)
            duty_cmd_[i] = 0.0f; integ_[i] = 0.0f; dis = true;
            // Re-raise from INSIDE the latch. The detection paths that call latch_fault() all live in
            // sibling branches, so once latched nothing reached them again: last-seen froze, and a
            // Mode-04 clear wiped a fault the bridge was still tripped on -- it stayed cleared while the
            // ETB sat dead, with the table saying everything was fine. The latch is the condition here,
            // so the latch is what must keep asserting it.
            if (dtc_ && active_dtc_[i] != 0)
                dtc_->raise(active_dtc_[i], DtcSource::THROTTLE, S_ETB_FAULT, now, dtc_ttl());
        } else if (pedal_untrusted) {
            // THE PEDAL IS NOT TRUSTWORTHY -> DROP THE BRIDGE. App raises P2138 (correlation) or the
            // missing-A code and publishes pedal_demand = 0 as VALID, which is a sane number and exactly
            // the problem: the throttle went on driving, energised and servoing, on the strength of an
            // input the ECU had just declared untrustworthy. A demand of 0 only commands the plate
            // CLOSED; it does not de-energise anything, and it is indistinguishable from a driver with
            // their foot off. Cutting DIS lets the spring take the plate home with no motor authority at
            // all, which is what the pedal being untrustworthy actually warrants.
            //
            // NOT latched. This is not a fault in the throttle -- the plate, its feedback and its cal are
            // all fine -- so it recovers the moment the pedal is trustworthy again, rather than demanding
            // a findlimits to clear something that was never about the throttle.
            duty_cmd_[i] = 0.0f; target_cmd_[i] = 0.0f; integ_[i] = 0.0f; stall_ms_[i] = 0; dis = true;
        } else if (calibrated_[i] && !have_pos) {
            // Calibrated, but no position THIS frame — at boot before the first TPS publish, or feedback
            // genuinely lost. Hold the bridge off rather than servo to a position we do not have (an
            // unpublished channel reads 0, which as an error is "plate fully closed" and commands full
            // open). Still latch once the debounce expires: gone for 200 ms is gone.
            duty_cmd_[i] = 0.0f; integ_[i] = 0.0f; dis = true;
            if (match_fault) latch_fault(i, P_TPS_CORRELATION, now);
        } else if (calibrated_[i]) {                        // CLOSED-LOOP position control
            // Servo to the arbitrated throttle_demand (bench `throttle` nudge overrides while armed).
            // No seat-release: hold the target with torque even fully closed — that's what kills the
            // spring-vs-stop relaxation oscillation. DIS stays for genuine faults/disable only.
            const float demand = bus.get(SIG_THROTTLE_DEMAND, 0.0f);
            duty_cmd_[i] = closed_loop(i, e, bus, out_pos, armed, demand, match_fault, dt_s, now, dis);
            ran_loop = true;
        } else {                                            // OPEN-LOOP manual nudge (pre-cal, Stage 1)
            const float target = armed ? std::clamp(manual_pct_[i], 0.0f, 100.0f) : 0.0f;
            const float rate = (target > duty_cmd_[i]) ? static_cast<float>(e.open_rate_pct_s)
                                                       : static_cast<float>(e.close_rate_pct_s);
            if (rate <= 0.0f || dt_s <= 0.0f) duty_cmd_[i] = target;
            else {
                const float step = rate * dt_s;
                duty_cmd_[i] = (target > duty_cmd_[i]) ? std::min(target, duty_cmd_[i] + step)
                                                      : std::max(target, duty_cmd_[i] - step);
            }
            dis = !(armed || duty_cmd_[i] > 0.5f);
        }
        if (!ran_loop) loop_live_[i] = false;               // any other branch breaks the loop's history

        // Publish with a frame-cadence TTL (5 × the 1 kHz period, headroom for frame-rate degradation under
        // load): if this ETB ever stops running (disabled, engine-stop, dropped), duty/enable EXPIRE within
        // ~5 ms so a downstream HBridge sees them invalid and fail-safes, instead of latching the last value.
        bus.set(DUTY_SIG[i], duty_cmd_[i], true, now, ttl());
        bus.set_bool(EN_SIG[i], !dis, now, ttl());   // fail-safe enable: HBridge drives only while high AND fresh
        bus.set(TGT_SIG[i],   target_cmd_[i], true, now, ttl());
        bus.set(ITERM_SIG[i], integ_[i],      true, now, ttl());
        // State: AUTOCAL while sweeping; FAULT on latch / failed autocal / uncalibrated A/B mismatch;
        // READY once calibrated (closed-loop); UNCAL pre-cal.
        State st;
        if (ac_phase_[i] >= AC_SETTLE && ac_phase_[i] <= AT_RELAY) st = ST_AUTOCAL;   // incl. FF sweep + autotune
        else if (fault_latched_[i] || ac_phase_[i] == AC_FAIL)     st = ST_FAULT;
        else if (ac_phase_[i] == AC_DONE || calibrated_[i])        st = ST_READY;
        else                                                       st = match_fault ? ST_FAULT : ST_UNCAL;
        bus.set(STATE_SIG[i], static_cast<float>(st), true, now, ttl());

        // command_state: latch THIS ETB's routine result once, on the running->terminal edge, so the studio
        // re-reads the cal it wrote. Guarded on the global still showing OUR op RUNNING → fires exactly once,
        // and ETB1 idling can't clear ETB0's result. AC_OFF while still RUNNING = an abort (engine started).
        if (cmdstate::phase_of(g_command_state) == cmdstate::RUNNING &&
            cmdstate::op_of(g_command_state) == ac_op_[i]) {
            if      (ac_phase_[i] == AC_DONE) set_command_state(static_cast<cmdstate::Op>(ac_op_[i]), cmdstate::OK);
            else if (ac_phase_[i] == AC_FAIL) set_command_state(static_cast<cmdstate::Op>(ac_op_[i]), cmdstate::FAIL);
            else if (ac_phase_[i] == AC_OFF)  set_command_state(static_cast<cmdstate::Op>(ac_op_[i]), cmdstate::FAIL);
        }
    }
}
