#pragma once

#include "EngineTask.h"
#include "Modules/EngineProtection.h"
#include "Modules/RevLimiter.h"
#include "Modules/Alternator.h"
#include "Modules/Boost.h"
#include "Modules/Launch.h"
#include "Modules/Dfco.h"                // Dfco (deceleration/overrun fuel cut)
#include "Modules/LambdaProtect.h"        // LambdaProtect (lean-protection fuel cut)
#include "Modules/VehicleSpeed.h"
#include "Modules/GearDetect.h"           // GearDetect (rpm/speed ratio -> gear)
#include "Modules/FlatShift.h"            // FlatShift (upshift ignition/fuel cut)
#include "Modules/TractionControl.h"      // TractionControl (slip -> throttle cap, retard, cut)
#include "Modules/VvtControl.h"           // VvtControl (closed-loop cam phasing)
#include "Modules/PitLimiter.h"           // PitLimiter (vehicle-speed limiter)
#include "Modules/Nitrous.h"               // Nitrous (single-stage nitrous control)
#include "Modules/Wmi.h"                   // Wmi (water/methanol injection)
#include "Modules/AntiLag.h"               // AntiLag (ALS ignition retard)
#include "Modules/Vvl.h"                   // Vvl (variable valve lift / VTEC)
#include "Modules/EgtProtect.h"            // EgtProtect (EGT over-temp enrich + cut)
#include "Modules/CruiseControl.h"         // CruiseControl (road-speed hold)
#include "Modules/TorqueModel.h"           // TorqueModel (brake-torque estimate)
#include "Modules/Lambda.h"
#include "Modules/TransientThrottle.h"
#include "Modules/FuelCalculator.h"
#include "Modules/Ignition.h"
#include "Modules/IgnitionTrim.h"
#include "Modules/FuelTrim.h"
#include "Modules/Knock.h"
#include "Modules/Misfire.h"
#include "Modules/Misfire.h"
#include "Modules/Idle.h"
#include "Modules/App.h"                 // App (drive-by-wire accelerator-pedal input -> pedal_demand)
#include "Modules/ElectronicThrottle.h"  // ElectronicThrottle (DBW demand arbitration + ETB actuator)
#include "Modules/HBridge.h"      // HBridge (generic H-bridge actuator-driver)
#include "Modules/Stepper.h"      // Stepper (position demand -> step_demand_a/b, consumed by HBridge)
#include "../Scripting/ScriptEngine.h"
#include "../Sensors/Sensors.h"
#include "../Integration/HardwareInput.h"   // HardwareInput (raw physical inputs -> bus, ahead of Sensors)
#include "../Diagnostics/Diagnostics.h"
#include "../Integration/OutputManager.h"
#include "Modules/OutputTest.h"
#include "../Scheduler/SoftPwm.h"
#include "../Can/CanBroker.h"

// ---------------------------------------------------------------------------
// SystemComposer — the composition layer. It OWNS the concrete engine managers, runs their
// init, performs the cross-wiring (the explicit set_*() injections), and registers each with
// the EngineTask scheduler in execution-phase order. EngineTask stays composition-agnostic;
// this is the one place that knows the concrete set.
//
// Wiring order = the historical run_frame() order:
//   INPUT  : hardware_input, sensors
//   MODULE : engine_protection, rev_limiter, fuel_calc, ignition, script_engine
//   OUTPUT : diagnostics
// ---------------------------------------------------------------------------

class SystemComposer {
public:
    // Create + init + cross-wire + register the managers into `task`. Call once at boot,
    // BEFORE task.start(). `can_broker` is wired to the bus/sensors/DTC table here too.
    void compose(EngineTask& task, CanBroker& can_broker,
                 SoftPwm& soft_pwm, uint32_t soft_pwm_tps,
                 PinArbiter& pin_arbiter, Comms::CommsManager& comms,
                 IHBridge* hbridge_a, IHBridge* hbridge_b);

    // Re-apply tune constants to the concrete modules (called on a burn from the host).

    // The unified DTC table (owned by the Diagnostics module).
    DtcManager& dtc() { return diag_.table(); }

    // The Lua engine — owned here, but driven by the script task on its own thread (not a
    // pipeline participant). main.cpp calls tick(bus) on it periodically.
    ScriptEngine& script_engine() { return script_engine_; }

    // The knock controller — the knock worker task POSTS per-cylinder measurements into its ring
    // (post_measurement); Knock::update() drains and classifies them on the engine-module task.
    Knock& knock() { return knock_; }
    // The misfire classifier — consumes SegmentTimer's per-cylinder crank segment times.
    Misfire& misfire() { return misfire_; }

private:
    HardwareInput      hardware_input_;   // INPUT phase, runs before sensors_ (publishes raw pins to the bus)
    Sensors            sensors_;
    EngineProtection   engine_protection_;
    RevLimiter         rev_limiter_;
    Alternator  alternator_;
    Boost       boost_;
    Launch      launch_;
    Dfco        dfco_;             // deceleration/overrun fuel cut (ORs into wk::fuel_cut)
    LambdaProtect lambda_protect_;  // lean-protection fuel cut
    VehicleSpeed vehicle_speed_;   // wheel/shaft pickups -> road speed (kph producer)
    GearDetect  gear_detect_;      // rpm/speed ratio -> gear (table axis producer)
    FlatShift   flat_shift_;       // clutchless upshift cut
    TractionControl traction_;     // wheel slip -> throttle cap (ETB), timing retard (Ignition), cut
    VvtControl  vvt_;              // closed-loop cam phasing -> vvt_duty_1
    PitLimiter  pit_limiter_;      // vehicle-speed limiter
    Nitrous     nitrous_;          // single-stage nitrous (solenoid + timing retard)
    Wmi         wmi_;              // water/methanol injection
    AntiLag     anti_lag_;         // anti-lag (ALS) ignition retard
    Vvl         vvl_;              // variable valve lift (VTEC) cam engagement
    EgtProtect  egt_protect_;      // EGT over-temp enrichment + fuel cut
    CruiseControl cruise_control_; // closed-loop road-speed hold
    TorqueModel torque_model_;     // table-based engine torque/power estimate
    Lambda      lambda_;
    TransientThrottle  transient_throttle_;
    FuelCalculator     fuel_calc_;
    Ignition  ignition_;
    IgnitionTrim        ignition_trim_;   // slow advance corrections, produced off the per-cycle path
    FuelTrim            fuel_trim_;        // slow fuel corrections, produced off the per-cycle path
    Knock     knock_;
    Misfire   misfire_;
    Idle        idle_;
    App                app_;             // DBW pedal input -> pedal_demand
    ElectronicThrottle electronic_throttle_;   // DBW demand arbitration + ETB actuator
    Stepper     stepper_;          // bipolar stepper controller; publishes step_demand_a/b for HBridge
    HBridge     hbridge_control_;
    ScriptEngine       script_engine_;
    OutputManager      output_manager_;
    OutputTest         output_test_;
    Diagnostics        diag_;
};
