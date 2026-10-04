#include "App.h"
#include "well_known_signals.h"                      // wk::engine_state
#include "../EngineStateMachine.h"                   // EngineRunState
#include "../TableEval.h"                            // tbl::table_eval
#include "../../../generated/table_descs.h"          // pedal_to_throttle_table_desc(cfg)
#include "../../../generated/signal_ids.h"           // SIG_PEDAL_DEMAND / SIG_APP_STATE
#include "../../../generated/module_dtc.h"           // ModuleDtc::APP_CORRELATION / APP_A_MISSING
#include "../../../generated/ecu_config.h"           // g_config (pedal sensor cal writeback)
#include "../../../generated/sensors_catalog.h"      // SENSOR_CATALOG / SENSOR_COUNT
#include "../../Platform/platform_hal.h"             // platform_get_tick_ms / platform_read_ain_raw
#include "../../Signal/SignalBus.h"
#include "../../Comms/CommandState.h"                 // command_state telemetry: report the pedalcal result
#include <algorithm>
#include <cmath>
#include "../../Diagnostics/TextLog.h"   // g_text_log — calibration results to the host console

extern uint32_t g_config_generation;   // bump so Sensors re-reads the pedal cal we write

// Raw-ADC read seam (mirrors ElectronicThrottle's): ADC COUNTS by analog source index.
static uint16_t (*s_read_ain)(uint8_t) = platform_read_ain_raw;
void App::set_raw_reader(uint16_t (*fn)(uint8_t)) noexcept { s_read_ain = fn; }

static int find_sensor(SignalId sig) {
    for (int i = 0; i < static_cast<int>(SENSOR_COUNT); i++)
        if (SENSOR_CATALOG[i].primary_channel == sig) return i;
    return -1;
}
static uint16_t raw_adc(SignalId sig) {
    const int idx = find_sensor(sig);
    if (idx < 0) return 0;
    return s_read_ain(g_config.sensors.sensor[idx].source);
}
// 2-point cal: released(lo)->0%, pressed(hi)->100%, slope-agnostic (descending sensor gets 1000->0).
static void write_cal(SignalId sig, uint16_t lo_mv, uint16_t hi_mv) {
    const int idx = find_sensor(sig);
    if (idx < 0) return;
    SensorConfig& s = g_config.sensors.sensor[idx];
    if (lo_mv <= hi_mv) { s.cal_raw[0]=lo_mv; s.cal_val[0]=0;    s.cal_raw[1]=hi_mv; s.cal_val[1]=1000; }
    else                { s.cal_raw[0]=hi_mv; s.cal_val[0]=1000; s.cal_raw[1]=lo_mv; s.cal_val[1]=0;    }
    s.cal_n = 2;
}

// Composed instance for the `pedalcal` CLI command (set in SystemComposer::compose).
App* g_app = nullptr;
void app_bench_calibrate() noexcept { if (g_app) g_app->start_calibrate(); }

static constexpr uint32_t CAL_WINDOW_MS = 5000;   // press + release the pedal fully within this window

void App::reset_state() { last_ms_ = 0; disagree_ms_ = 0; missing_ms_ = 0; calibrating_ = false; }

// One code at a time: a pedal that is both missing and uncorrelated is one fault with one cause, and
// leaving the old code raised beside the new one would leave the table saying two things happened.
void App::set_fault(uint16_t code, uint32_t now) noexcept {
    // RE-RAISE EVERY FRAME while the condition holds. raise() is built for it -- "safe to call every
    // frame; count only bumps on the inactive->active edge" -- and calling it is what refreshes the
    // record's last-seen stamp. This used to return early when the code was unchanged, so a fault that
    // was CONTINUOUSLY true stamped last_ms once and then froze, and the table became unreadable: a
    // stale ACTIVE and a live one looked identical, and last-seen -- the one field that should answer
    // "is this still happening?" -- answered it only for whoever happened to re-raise.
    if (!dtc_) return;
    if (code != active_dtc_ && active_dtc_ != 0) dtc_->heal(active_dtc_);   // superseded by a different code
    if (code != 0)
        dtc_->raise(code, DtcSource::MODULE,
                    code == ModuleDtc::APP_A_MISSING ? ModuleDtc::APP_A_MISSING_SEV
                                                     : ModuleDtc::APP_CORRELATION_SEV, now, dtc_ttl());
    active_dtc_ = code;
}

void App::start_calibrate() noexcept {
    calibrating_ = true;
    cal_t_ = platform_get_tick_ms();
    cal_min_a_ = 0xFFFF; cal_max_a_ = 0; cal_min_b_ = 0xFFFF; cal_max_b_ = 0;
    set_command_state(cmdstate::PEDALCAL, cmdstate::RUNNING);   // studio watches this until OK/FAIL
}

void App::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now   = platform_get_tick_ms();
    const float    dt_ms = (last_ms_ != 0) ? static_cast<float>(now - last_ms_) : 0.0f;
    last_ms_ = now;

    if (!cfg_ || !cfg_->enabled) {
        // A calibration in flight has to be ANSWERED. This used to drop it silently, which left
        // command_state at RUNNING for ever: the CLI said "pedalcal started", the routine never ran
        // because the module is what runs it, and the studio waited on a reply that could not come.
        // A routine that says no is far better than one that never says anything.
        if (calibrating_) set_command_state(cmdstate::PEDALCAL, cmdstate::FAIL);
        calibrating_ = false;
        // Heal what we raised: this return skips the heal() below — see DtcManager::heal.
        if (active_dtc_ && dtc_) { dtc_->heal(active_dtc_); active_dtc_ = 0; }
        return;
    }

    // KEY OFF: PARK, AND JUDGE NOTHING. The sensors stop publishing with the key off (Sensors gates every
    // pin-read input on it), so app_1 is absent for the whole of key-off — and this used to clear the
    // latch and then re-latch NO SIGNAL on that absence in the same frame. The latch only ever cleared
    // while the key was off, so every key-on began with the pedal already faulted: P1780 and a dead
    // throttle until a reset, from a pedal that was fine. Same rule as the ETB's park: no fault is
    // accumulated on feedback that cannot physically be there.
    extern bool g_system_active;                             // key-on: owned by Sensors/EngineTask
    if (!g_system_active) {
        latched_state_ = 0; latched_code_ = 0;               // a new key cycle trusts the pedal again
        disagree_ms_ = 0; missing_ms_ = 0;
        // A cal in flight is ANSWERED, and must not finish: key-off moves the 5 V reference the raw
        // reads are measured against, so a sweep that ran on into key-off would write a wrong cal.
        if (calibrating_) {
            calibrating_ = false;
            set_command_state(cmdstate::PEDALCAL, cmdstate::FAIL);
            g_text_log.printf("pedalcal ABORTED: key off - the pedal cannot be read without the key\n");
        }
        set_fault(0, now);
        return;
    }

    const bool stopped = (static_cast<EngineRunState>(static_cast<int>(bus.get(wk::engine_state, 0.0f)))
                          == EngineRunState::STOPPED);
    // FIXED to the APP sensors: app_1 IS the pedal value, app_2 is the cross-check. There is no selector
    // — nothing to choose means nothing to mis-point, and `pedalcal` writes the calibration of whatever
    // it is aimed at. Where a pedal is WIRED stays configurable, on the sensor (analog / CAN).
    const SignalId a = SIG_APP_1;
    const SignalId b = SIG_APP_2;

    // --- pedal calibrate: sweep for each track's raw EXTREMES, write both sensors' cal ---
    // The sweep supplies the SPAN only. Direction is declared (app1_sense / app2_sense), because it
    // cannot be recovered from the signals: released->pressed and pressed->released give the identical
    // four raw values, so no procedure can tell which extreme was released. Asking the sweep meant
    // trusting the operator's timing with a runaway throttle as the failure mode.
    if (calibrating_) {
        if (!stopped) {                                            // engine started -> abort
            calibrating_ = false;
            set_command_state(cmdstate::PEDALCAL, cmdstate::FAIL);
            g_text_log.printf("pedalcal ABORTED: engine started - calibration is engine-stopped only\n");
        }
        else {
            const uint16_t ra = raw_adc(a), rb = raw_adc(b);
            cal_min_a_ = std::min(cal_min_a_, ra); cal_max_a_ = std::max(cal_max_a_, ra);
            cal_min_b_ = std::min(cal_min_b_, rb); cal_max_b_ = std::max(cal_max_b_, rb);
            bus.set(SIG_PEDAL_DEMAND, 0.0f, true, now, ttl());  // cal-aware: hold demand at 0 (no false fault)
            bus.set(SIG_APP_STATE, static_cast<float>(ST_CALIBRATING), true, now, ttl());
            if (now - cal_t_ >= CAL_WINDOW_MS) {
                const uint16_t span_a = static_cast<uint16_t>(cal_max_a_ - cal_min_a_);
                const uint16_t span_b = static_cast<uint16_t>(cal_max_b_ - cal_min_b_);
                // Direction comes from CONFIGURATION, never from the sweep. A rising track is released
                // at its low raw and pressed at its high; a falling track is the other way about. Passing
                // them to write_cal in released,pressed order reaches its inverted branch (lo > hi) for a
                // falling track, so BOTH tracks end up reading pedal position and the A/B cross-check
                // compares like with like. Inferring this from the sweep is what produced a second track
                // that read 100 % with your foot off.
                const bool fall_a = (cfg_->app1_sense != 0), fall_b = (cfg_->app2_sense != 0);
                const uint16_t rel_a   = fall_a ? cal_max_a_ : cal_min_a_;
                const uint16_t press_a = fall_a ? cal_min_a_ : cal_max_a_;
                const uint16_t rel_b   = fall_b ? cal_max_b_ : cal_min_b_;
                const uint16_t press_b = fall_b ? cal_min_b_ : cal_max_b_;
                if (span_a >= cfg_->cal_min_span && span_b >= cfg_->cal_min_span) {
                    write_cal(a, rel_a, press_a);           // released -> 0%, pressed -> 100%
                    write_cal(b, rel_b, press_b);           // …whichever way the track was declared to run
                    ++g_config_generation;                  // Sensors rebuilds with the new pedal cal
                    set_command_state(cmdstate::PEDALCAL, cmdstate::OK);
                    // Report what was MEASURED, like the throttle routines do. command_state only carries
                    // OK/FAIL, so without this a successful pedal cal is a silent state change and there is
                    // no way to see whether the sweep actually covered the pedal's travel or just caught a
                    // twitch that happened to clear cal_min_span.
                    g_text_log.printf("pedalcal OK: A released=%u pressed=%u span=%u%s | "
                                      "B released=%u pressed=%u span=%u%s (min span %u counts)\n",
                                      rel_a, press_a, span_a, fall_a ? " (falling)" : "",
                                      rel_b, press_b, span_b, fall_b ? " (falling)" : "",
                                      cfg_->cal_min_span);
                } else {
                    set_command_state(cmdstate::PEDALCAL, cmdstate::FAIL);   // span too small — cal rejected
                    // Name WHICH track fell short and by how much — "failed" alone sends you checking both.
                    g_text_log.printf("pedalcal FAILED: span too small - A=%u%s B=%u%s, need %u counts each. "
                                      "Press and release the pedal FULLY within the %u ms window.\n",
                                      span_a, span_a < cfg_->cal_min_span ? " <-- short" : "",
                                      span_b, span_b < cfg_->cal_min_span ? " <-- short" : "",
                                      cfg_->cal_min_span, static_cast<unsigned>(CAL_WINDOW_MS));
                }
                calibrating_ = false;
            }
            return;
        }
    }

    // --- normal: A/B correlation (app_a is the value, app_b cross-checks), then map through the table ---
    const bool  va = bus.valid(a), vb = bus.valid(b);
    const float pa = bus.get(a, 0.0f), pb = bus.get(b, 0.0f);
    const bool  matched = va && vb && std::fabs(pa - pb) <= static_cast<float>(cfg_->match_err_pct) * 0.1f;
    if (matched) disagree_ms_ = 0;
    else         disagree_ms_ += dt_ms;             // float: a ~1 ms frame must not truncate to 0
    if (va) missing_ms_ = 0;
    else    missing_ms_ += dt_ms;
    // Two causes, reported apart: no primary track at all, or a pair that disagreed past the debounce.
    // The missing-signal case is checked first — with no A there is nothing to correlate, so calling it a
    // correlation fault would send someone looking at the wrong wire.
    //
    // A MISSING TRACK HAS TO HOLD FOR match_ms TOO before it LATCHES. It latched on one frame, and a
    // single frame of absence is normal at key-on: the 5 V references can come up a moment after the
    // battery crosses the key threshold, and for that moment Sensors publishes every pin-read input
    // invalid. Latching on it would hold the pedal dead for the key cycle. Demand is still cut on the
    // FIRST frame A is missing (below) — the debounce delays the verdict, never the fail-safe.
    const bool no_signal   = missing_ms_ >= cfg_->match_ms;
    const bool disagreeing = disagree_ms_ >= cfg_->match_ms;
    // A PEDAL FAULT HOLDS UNTIL THE KEY GOES OFF. It used to clear on the first frame the tracks agreed
    // again, and the ETB dropped and re-enabled its bridge in step — so an intermittent track cycled the
    // throttle between limp and driven. What was seen once is not trusted again this key cycle.
    if (no_signal || disagreeing) {
        latched_code_  = no_signal ? ModuleDtc::APP_A_MISSING : ModuleDtc::APP_CORRELATION;
        latched_state_ = static_cast<uint8_t>(no_signal ? ST_FAULT_NO_SIGNAL : ST_FAULT_CORRELATION);
    }
    if (latched_code_ != 0) {
        set_fault(latched_code_, now);
        bus.set(SIG_APP_STATE, static_cast<float>(latched_state_), true, now, ttl());
        bus.set(SIG_PEDAL_DEMAND, 0.0f, true, now, ttl());   // fail safe: 0 -> engine keeps idling
        return;
    }
    set_fault(0, now);                                       // trustworthy again -> heal whatever was raised
    bus.set(SIG_APP_STATE, static_cast<float>(ST_OK), true, now, ttl());
    if (!va) {                                               // missing, not yet a verdict: no throttle on it
        bus.set(SIG_PEDAL_DEMAND, 0.0f, true, now, ttl());
        return;
    }

    const float demand = std::clamp(tbl::table_eval(pedal_to_throttle_table_desc(cfg_), bus), 0.0f, 100.0f);
    bus.set(SIG_PEDAL_DEMAND, demand, true, now, ttl());
}
