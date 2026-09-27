#pragma once

#include <cstdint>
#include "../Scheduler/SchedulerTypes.h"

// ---------------------------------------------------------------------------
// EnginePosition — decoder state for the current control frame.
//
// This is NOT a sensor signal.  It comes from the trigger decoder and is
// not routable via the SignalBus.  Every module that needs RPM or crank
// position reads from here.
// ---------------------------------------------------------------------------

struct EnginePosition {
    float     rpm             = 0.0f;   // Engine speed [RPM]
    float     crank_angle_deg = 0.0f;   // Absolute crank angle [0..720 deg]
    uint32_t  cycle_count     = 0;      // ++ once per engine cycle (720° wrap) — lets the heavy
                                        // per-cycle fuel/ign compute self-decimate off the 1 kHz frame
    SyncLevel sync_level      = SyncLevel::NONE;
    bool      is_synchronized = false;
    // Decoder sync-health (lean diagnostics; see TriggerDecoder). The task derives the
    // engine-cycle error % here; EngineProtection reads these for the trigger faults.
    uint8_t   trigger_error_pct = 0;    // errors_last_cycle / teeth_last_cycle [%]
    uint16_t  errors_last_cycle = 0;    // raw tolerated-miss count in the last rev (≥1 = record the DTC)
    uint16_t  last_error_tooth  = 0;    // tooth index at the last decoder error
    uint8_t   last_error_kind   = 0;    // TriggerErrorKind (see SchedulerTypes.h)
    // FREE-RUNNING trigger-health totals, and the one live absence flag.
    //
    // The three fields above are all snapshotted at a cycle boundary that is itself counted in
    // TEETH, so the moment the teeth stop they freeze — which made them useless for reporting the
    // one fault that stops teeth. These do not depend on a cycle completing.
    uint32_t  noise_edges_total = 0;    // edges rejected as too early to be a tooth
    uint32_t  missed_teeth_total= 0;    // teeth that were due and never arrived
    uint32_t  phase_lost_total  = 0;    // cam sync downgrades
    bool      trigger_absent    = false;// the decoder's deadline says the wheel has stopped
};
