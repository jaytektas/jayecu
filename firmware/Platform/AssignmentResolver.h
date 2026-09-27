#pragma once
#include "Scheduler/BoardProfile.h"
#include "Scheduler/HardwareAssignment.h"
#include "Scheduler/SchedulerTypes.h"
#include "../../generated/modules/trigger_config.h"   // TriggerConfig (g_config.trigger)

// ---------------------------------------------------------------------------
// Trigger-input mapping → hardware assignment.
//
// The crank/cam input mapping is read straight from the tune (g_config.trigger):
// which board capture resource (an index into BoardProfile.capture_resources[])
// and which edge each trigger role uses — no intermediate copy.
//
// descriptor_to_assignment() binds that mapping onto the board's capture pool,
// producing an EcuHardwareAssignment. It is the platform-layer resolver named in
// HardwareAssignment.h, and is re-runnable for no-reset reconfiguration.
// ---------------------------------------------------------------------------

// Clears `out`, sets the timebase, and points crank/cam at the chosen pool
// channels. Output-compare (ignition/injection) binding is a later milestone.
void descriptor_to_assignment(const BoardProfile&    profile,
                              const TriggerConfig&   trig,
                              EcuHardwareAssignment& out) noexcept;
