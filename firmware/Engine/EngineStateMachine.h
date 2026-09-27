#pragma once
#include <cstdint>
#include "../../generated/signal_enums.h"   // EngineRunState (schema-sourced; STOPPED/CRANKING/RUNNING)

// ---------------------------------------------------------------------------
// EngineStateMachine — the engine operating-mode (spinning-state) machine.
//
// Models three states. jayecu's decoder reports rpm=0 until sync
// is acquired (get_rpm_x10() returns 0 while sync_level==NONE), so there is no detectable pre-sync
// "rpm present but unreliable" window, so a spinning-up state is deliberately omitted rather than
// synthesised.
//
// Pure + host-testable: takes a fresh rpm + monotonic tick each call, owns the previous state plus
// the stop-debounce timer, and reports the STOPPED/RUNNING edges so the owner (EngineTask) can drive
// the module lifecycle hooks. All thresholds come from config (the schema Engine group) — no magic
// numbers live here beyond struct defaults that mirror the schema defaults.
// ---------------------------------------------------------------------------

// EngineRunState (STOPPED=0 / CRANKING=1 / RUNNING=2) is generated into signal_enums.h from the
// schema enum `engine_run_state` — the same value-set the TS precondition picker shows. States:
//   STOPPED  — no position: rpm == 0
//   CRANKING — turning but not caught: 0 < rpm < cranking_rpm
//   RUNNING  — caught and running: rpm >= cranking_rpm; held until a real stop (full hysteresis)
//
// There is no "stopped RPM" threshold. rpm is 0 only when the decoder has lost position — a genuine
// desync or the lost-trigger WATCHDOG firing after teeth stop arriving (see VirtualTrigger; its timeout
// scales with the wheel's tooth count). The PLL holds rpm across a single missed tooth, so a momentary
// dropout never zeroes it and the watchdog owns the debounce timing. So a fresh rpm==0 here is already a
// confirmed stop, taken immediately on !turning; any non-zero rpm means the decoder is tracking.

class EngineStateMachine {
public:
    struct Params {
        float cranking_rpm = 400.0f;   // CRANKING -> RUNNING (engine caught; anchors post-start)
    };

    void configure(const Params& p) { p_ = p; }

    // Advance the machine. started_edge fires exactly on the transition into RUNNING; because RUNNING
    // only ever exits to STOPPED (never back to CRANKING), that edge marks a genuine (re)start — which
    // re-anchors the post-start timer. stopped_edge fires exactly on
    // the transition into STOPPED. Returns the new state.
    EngineRunState update(float rpm, bool& started_edge, bool& stopped_edge) {
        started_edge = false;
        stopped_edge = false;

        const bool turning = rpm > 0.0f;   // any position at all; rpm is 0 only on lost sync (see header)

        const EngineRunState prev = state_;
        switch (state_) {
            case EngineRunState::STOPPED:
                if (rpm >= p_.cranking_rpm)      state_ = EngineRunState::RUNNING;   // straight catch (rare)
                else if (turning)                state_ = EngineRunState::CRANKING;
                break;
            case EngineRunState::CRANKING:
                if (rpm >= p_.cranking_rpm)      state_ = EngineRunState::RUNNING;
                else if (!turning)               state_ = EngineRunState::STOPPED;
                break;
            case EngineRunState::RUNNING:
                // Full hysteresis: once running, hold RUNNING through any RPM dip until a real stop
                // (rpm 0 — which the decoder/watchdog only assert when position is genuinely lost).
                if (!turning)                    state_ = EngineRunState::STOPPED;
                break;
        }

        if (state_ != prev) {
            if (state_ == EngineRunState::RUNNING) started_edge = true;
            if (state_ == EngineRunState::STOPPED) stopped_edge = true;
        }
        return state_;
    }

    EngineRunState state() const { return state_; }

private:
    Params         p_{};
    EngineRunState state_ = EngineRunState::STOPPED;
};
