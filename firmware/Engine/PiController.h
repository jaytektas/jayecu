#pragma once
#include <algorithm>

// Reusable PI controller with output clamp + back-calculation anti-windup. The integrator is the
// persistent state (e.g. the short-term fuel trim). First user: Lambda (STFT); boost/idle/
// e-throttle reuse it. Gains + limits are caller-owned (live config). Pure, no allocation.
struct PiController {
    float kp = 0.0f, ki = 0.0f;
    float out_min = -1.0f, out_max = 1.0f;
    float integ = 0.0f;                 // integral accumulator == the controller's persistent state

    // Step the loop. error in the caller's units; dt_s seconds since the last step. `extra` is any term
    // the caller adds to the output (a D term) — passed in so saturation is judged on what is actually
    // applied. Returns the clamped output.
    //
    // ANTI-WINDUP BY CONDITIONAL INTEGRATION. The integrator stops growing INTO a limit the output is
    // already against, and on its own may never exceed the controller's authority. This used to be
    // back-calculation — integ = out - p — which is wrong the moment P ALONE saturates: p = +30 against a
    // ceiling of 10 drove the integrator to -20, so when the error came back to zero the output was that
    // -20, slammed against the opposite limit. Every loop sharing this (idle, boost, VVT, alternator,
    // cruise, traction, lambda) overshot the wrong way after every large disturbance.
    float step(float error, float dt_s, float extra = 0.0f) {
        const float p  = kp * error;
        const float di = ki * error * dt_s;
        const float trial = p + integ + di + extra;
        const bool into_high = trial > out_max && di > 0.0f;
        const bool into_low  = trial < out_min && di < 0.0f;
        if (!into_high && !into_low) integ += di;
        integ = std::clamp(integ, out_min, out_max);
        return std::clamp(p + integ + extra, out_min, out_max);
    }

    // Move a fraction of the current integral OUT of the loop (e.g. bled into LTFT) so the
    // short-term trim recenters toward zero while the total correction is preserved elsewhere.
    void bleed(float amount) { integ -= amount; }

    void reset() { integ = 0.0f; }
};
