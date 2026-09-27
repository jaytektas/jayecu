#pragma once
#include "ITimerChannel.h"
#include "SchedulerTypes.h"

// ---------------------------------------------------------------------------
// EcuHardwareAssignment — runtime-resolved binding of ECU logical functions
// to concrete hardware channel objects.
//
// This struct contains raw pointers resolved from a BoardProfile by
// descriptor_to_assignment() in the platform layer. The EFI subsystem
// (firmware/src/efi_pos/) receives a fully-populated const reference
// and never consults the BoardProfile or EcuDescriptor directly.
//
// nullptr entries mean the corresponding logical function is not wired
// on this vehicle. EnginePositionHal must guard all accesses.
// ---------------------------------------------------------------------------
struct EcuHardwareAssignment {
    // ---- Crank trigger inputs -----------------------------------------------
    ICaptureChannel* crank_primary;
    CaptureEdge      crank_primary_edge;
    ICaptureChannel* crank_secondary;
    CaptureEdge      crank_secondary_edge;

    // ---- Cam trigger inputs -------------------------------------------------
    ICaptureChannel* cam[MAX_CAM_CHANNELS];
    CaptureEdge      cam_edge[MAX_CAM_CHANNELS];
    AngleDeg10       cam_nominal_angle[MAX_CAM_CHANNELS]; 

    // ---- Power Stage Outputs ------------------------------------------------
    // High-current timing-accurate channels.
    ITimerChannel*   ign[MAX_IGN_CHANNELS]; // IGN1..12
    ITimerChannel*   ls[MAX_INJ_CHANNELS];  // LS1..22 (Injections + Misc)
    IGpioOutput*     hs[8];                 // HS1..8 (High side switches)

    // ---- General Purpose IO -------------------------------------------------
    IGpioInput*      digital_in[8];         // DIGITAL1..8

    // ---- Specialized Controllers --------------------------------------------
    struct ETBController {
        IGpioOutput*  dir;
        IGpioOutput*  dis;
        ITimerChannel* pwm;
    } etb[2]; // ETB1, ETB2

    // ---- Communication Busses -----------------------------------------------
    ICanBus*         can[2];                // CAN1, CAN2

    // ---- System -------------------------------------------------------------
    IGpioOutput*     running_led;
    IGpioOutput*     warning_led;
    IGpioOutput*     error_led;
    IGpioOutput*     comms_led;

    // authorative clock source for absolute tick arithmetic.
    const ITimerChannel* timebase;

    // Firing-layer domain alarms (firing-layer rework): Timer 1 = angle events,
    // Timer 2 = time events (injector close). The scheduler drives these instead
    // of per-channel deadlines. nullptr = not wired (firing disabled).
    IAlarmTimer* angle_alarm;
    IAlarmTimer* time_alarm;

    // The DECODER's clock (Timer 3 = TIM5 CCR4). Nothing to do with firing: it answers "is a tooth
    // overdue, and has the engine stopped", which is a question about the trigger and therefore
    // belongs to the trigger. It used to be answered inside the DCO's match ISR, which meant the
    // detector was switched off by every sync transition (each one resets the DCO) — an ECU could
    // then hold sync and RPM for ever with no wheel connected at all. A watchdog another layer's
    // state machine can disarm is not a watchdog. nullptr = not wired (no liveness detection).
    IAlarmTimer* tooth_alarm;

    // Output-pin ownership arbiter over ign[]/ls[]. The scheduler claims only the
    // pins its cylinders use; spares stay Hi-Z, free for other modules to claim.
    class PinArbiter* pin_arbiter;
};
