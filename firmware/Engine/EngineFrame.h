#pragma once

#include <cstdint>
#include "../../firmware/Scheduler/SchedulerTypes.h"

// ---------------------------------------------------------------------------
// EngineFrame — per-frame working state, allocated on the stack each cycle.
//
// Modules read from EnginePosition + SignalBus (sensors) and write to EngineFrame
// (computed values, built up through the pipeline).
//
// Execution order in EngineTask::run_frame() makes all dependencies explicit:
//   safety modules write limits first → control modules respect those limits.
//
// Frame is zero-initialised at the start of every cycle. Stale data from
// last cycle cannot contaminate this cycle.
// ---------------------------------------------------------------------------

struct EngineFrame {
    // The fuel/ign cuts and EngineProtection's rev-limit/enrich/retard/boost-corr reaction USED to live here
    // (a per-frame scratchpad), which forced every cut/protection module to run the same 1 kHz frame in
    // lockstep. They now travel on the SignalBus with a ttl (cuts are validity-OR: publish true only while
    // cutting; prot hand-offs are wk::prot_* signals) so those modules can run at independent cadences.
    float    effective_rpm_limit = 0.0f; // 0 = no limit active (telemetry aid; Launch takes the tightest)

    // ---- Fueling computation ----
    float    target_lambda   = 1.0f;     // From fuel target / AFR table
    float    ve_pct          = 100.0f;   // Volumetric efficiency [%]
    float    base_fuel_pw_us = 0.0f;     // Base pulse width before corrections [µs]
    float    clt_fuel_corr   = 0.0f;     // CLT enrichment correction [%]
    uint32_t prime_pw_us     = 0;        // One-shot prime squirt request this cycle (0 = none); fired by EngineTask
    uint32_t async_inj_pw_us = 0;        // Async transient: per-pulse PW (TransientThrottle sets it, EngineTask fires)
    uint8_t  async_inj_pulses = 0;       // Async transient: number of extra squirts to spread across the cycle (0 = none)

    // ---- Ignition computation ----
    float    ign_advance_deg = 0.0f;     // Computed advance before corrections [deg]
    float    knock_retard    = 0.0f;     // Active knock retard [deg]
    uint32_t dwell_us        = 3000;     // Coil dwell [µs]

    // ---- Ancillary ----
    float    boost_target_kpa = 0.0f;    // Boost target [kPa]

    // ---- Per-cylinder shadow write-back (committed to EnginePositionHal) ----
    // Indexed [0..cylinder_count-1]. EngineTask writes these via set_*() after
    // the pipeline completes.
    struct CylOutput {
        AngleDeg10  spark_btdc_x10  = 0;   // Spark timing [decidegrees BTDC]
        // Rotary trailing plug: the leading advance plus the configured offset (negative retards).
        // Same chamber, same coil charge — an angle offset and nothing more, no timing of its own.
        AngleDeg10  trail_btdc_x10  = 0;   // Trailing spark [decidegrees BTDC]
        uint32_t    dwell_us        = 3000;
        AngleDeg10  inj_btdc_x10    = 3550; // Injection open angle [decidegrees BTDC]
        uint32_t    inj_pw_us       = 0;
        // Staged stages 2..4 (index 0 = stage 2) — each its own PW + open angle; enabled per-cylinder by
        // the fuel calc's duty progression (0 / false when below threshold or staging off).
        uint32_t    staged_pw_us[MAX_STAGED_STAGES]    = {};
        AngleDeg10  staged_btdc_x10[MAX_STAGED_STAGES] = { 3550, 3550, 3550 };
        bool        staged_enabled[MAX_STAGED_STAGES]  = {};
    };
    CylOutput cyl[MAX_CYLINDERS] = {};
};
