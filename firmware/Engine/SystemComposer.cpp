#include "SystemComposer.h"
#include "../../generated/module_cadence.h"  // cadence::<Module> — per-module scheduler rate
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "../../generated/ecu_config.h"

void SystemComposer::compose(EngineTask& task, CanBroker& can_broker,
                             SoftPwm& soft_pwm, uint32_t soft_pwm_tps,
                             PinArbiter& pin_arbiter, Comms::CommsManager& comms,
                             IHBridge* hbridge_a, IHBridge* hbridge_b) {
    hb_a_ = hbridge_a; hb_b_ = hbridge_b; comms_ = &comms;
    // --- init the concrete managers (the former EngineTask::start() body) ---
    engine_protection_.init(g_config.engine_protection);
    rev_limiter_.init(g_config.rev_limiter);
    alternator_.init(g_config.alternator);
    boost_.init(g_config.boost);
    launch_.init(g_config.launch);
    dfco_.init(g_config.dfco);
    lambda_protect_.init(g_config.lambda_protect);
    vehicle_speed_.init(g_config.vehicle_speed);
    gear_detect_.init(g_config.gear_detect);
    flat_shift_.init(g_config.flat_shift);
    traction_.init(g_config.traction_control);
    vvt_.init(g_config.vvt_control);
    pit_limiter_.init(g_config.pit_limiter);
    nitrous_.init(g_config.nitrous);
    wmi_.init(g_config.wmi);
    start_control_.init(g_config.start_control, &electronic_throttle_);   // waits on the throttle's key-on check
    anti_lag_.init(g_config.anti_lag);
    vvl_.init(g_config.vvl);
    egt_protect_.init(g_config.egt_protect);
    cruise_control_.init(g_config.cruise_control);
    torque_model_.init(g_config.torque_model);
    lambda_.init(g_config.lambda);
    lambda_.set_dtc(&diag_.table());                     // "enabled but nothing reading" is a fault, not silence
    transient_throttle_.init(g_config.transient_throttle);
    fuel_calc_.init(g_config.fuel_calculator);
    ignition_.init(g_config.ignition);
    ignition_trim_.init(g_config.ignition);   // same tune; produces the slow advance trims
    fuel_trim_.init(g_config.fuel_calculator);           // same tune; produces the slow fuel corrections
    knock_.init(g_config.knock, &ignition_);   // hands retard to ignition_
    misfire_.init(g_config.misfire);           // its segment source is assigned by main (set_timer)
    idle_.init(g_config.idle);
    stepper_.init(g_config.stepper);                     // publishes step_demand_a/b (HBridge consumes) — dormant unless enabled
    app_.init(g_config.app);                             // DBW pedal -> pedal_demand
    app_.set_dtc(&diag_.table());                        // a pedal cut names its cause (correlation vs missing)
    extern App* g_app; g_app = &app_;                    // the `pedalcal` CLI targets this instance
    electronic_throttle_.init(g_config.electronic_throttle);   // DBW arbitration + ETB actuator
    extern ElectronicThrottle* g_electronic_throttle;          // the `throttle` CLI nudge targets this instance
    g_electronic_throttle = &electronic_throttle_;
    extern HBridge* g_h_bridge;                                // ... and the `hbridge` nudge, this one
    g_h_bridge = &hbridge_control_;
    // HBridge: the generic actuator-driver. Bound to the board's two H-bridges (IHBridge,
    // SoftPwm-backed PD6/PD3 + DIR/DIS). Consumes its demand_sig (e.g. idle_duty) and renders it as a
    // stepper/DC actuator. Default-disabled -> bridges stay off (DIS high). Hardware-agnostic here.
    hbridge_control_.init(g_config.h_bridge, hbridge_a, hbridge_b, &comms);   // shadow region-watch
    sensors_.set_generic_can(&can_broker.generic_mut());   // before init(): CAN inputs bind at boot
    sensors_.init(g_config.sensors);
    // …and the other way round, so generic CAN can see which channels already have a producer. A
    // receive field claiming a channel a sensor publishes is a config fault, and it has to be SAID —
    // at equal priority the two simply alternate, and the bus then carries whichever wrote last.
    can_broker.set_sensors(&sensors_);
    diag_.init(/*boot_id*/ 0);                            // main's SdDtcStore.boot_restore() reloads post-start
    extern DtcManager* g_dtc_table;                       // 'G' DTC-read command (CommsManager) reads the table
    g_dtc_table = &diag_.table();
    script_engine_.set_can_broker(&can_broker);
    script_engine_.init(g_config.lua);
    // OutputManager: the Generic output rows (g_config.outputs.output[i] IS pin i). Claims each
    // row's pin via the shared PinArbiter (PinOwner::AUX), rebuilds region-scoped on the Outputs shadow
    // bit (comms) or when the scheduler re-binds, trips P1650 (diag table) on a pin the scheduler still
    // holds. Builds lazily on first update(). PWM rows claim from the shared SoftPwm pool.
    output_manager_.init(soft_pwm, soft_pwm_tps, pin_arbiter, comms, diag_.table());
    output_test_.bind(&pin_arbiter, &g_config.outputs);
    extern OutputTest* g_output_test; g_output_test = &output_test_;   // the `test` CLI targets this

    // --- cross-wiring (kept as explicit injections; the bus-ify-these targets) ---
    sensors_.set_dtc(&diag_.table());                     // sensors raise P-codes into the one table
    electronic_throttle_.set_dtc(&diag_.table());            // ETB L2 supervisor raises throttle P-codes
    start_control_.set_dtc(&diag_.table());                  // P0087: a start that cranked on a low rail
    engine_protection_.set_sensors(&sensors_);            // protection consumes sensor health/severity
    engine_protection_.set_dtc(&diag_.table());           // reactor: cuts from the table's worst severity
    egt_protect_.set_dtc(&diag_.table());                 // an EGT cut names the probe that caused it
    fuel_calc_.set_dtc(&diag_.table());
    boost_.set_dtc(&diag_.table());
    transient_throttle_.set_dtc(&diag_.table());
    launch_.set_dtc(&diag_.table());
    knock_.set_dtc(&diag_.table());
    misfire_.set_dtc(&diag_.table());
    hbridge_control_.set_dtc(&diag_.table());             // missing demand/enable signal -> HBRIDGE_* P-codes
    stepper_.set_dtc(&diag_.table());                     // missing position-demand input signal -> STEP_IN P-code
    cruise_control_.set_dtc(&diag_.table());              // why cruise will not engage, in codes a scan tool reads
    traction_.set_dtc(&diag_.table());                    // why the car is not being held, and whether the axles agree
    can_broker.set_signal_bus(&task.bus());
    can_broker.set_dtc(&diag_.table());                   // Mode 03/04 read/clear the unified table

    // --- inject the one cross-cutting store; everything else flows via the bus ---
    task.set_dtc(&diag_.table());
    // OUTPUT-side pin-conflict probe -> the P1650 merge (heals only when input+output both clear).
    task.set_output_conflict(output_manager_.conflict_ptr());

    task.set_power_on(&SystemComposer::power_on, this);   // key-off = power-off (see power_on_khz1)

    // Engine run-state thresholds (schema Engine group) -> the STOPPED/CRANKING/RUNNING machine.
    task.set_engine_state_params({
        static_cast<float>(g_config.engine.cranking_rpm),
    });
    // Arm the lost-trigger watchdog in the decoder (teeth stop -> forced sync loss). No RPM knob: the
    // timeout scales with the wheel's tooth count off a fixed low-RPM floor. Re-applied on (re)configure.
    task.configure_stall_watchdog();

    // --- register participants in execution-phase order (name = the participant's label) ---
    task.add_participant(&hardware_input_,    "hwin",      EngineTask::Phase::INPUT);   // raw pins -> bus, first
    task.add_participant(&sensors_,           "sensors",   EngineTask::Phase::INPUT,
                         EngineTask::Cadence::KHZ_1, cadence::Sensors);
    task.add_participant(&vehicle_speed_,     "vss",       EngineTask::Phase::INPUT, EngineTask::Cadence::KHZ_1, cadence::VehicleSpeed);  // pickups -> kph, BEFORE gear (which divides by it)
    task.add_participant(&gear_detect_,       "gear",      EngineTask::Phase::INPUT, EngineTask::Cadence::KHZ_1, cadence::GearDetect);   // gear axis producer, before modules
    task.add_participant(&traction_,          "traction",  EngineTask::Phase::INPUT, EngineTask::Cadence::KHZ_1, cadence::TractionControl);   // throttle cap, before the ETB
    task.add_participant(&engine_protection_, "protect",   EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::EngineProtection);
    task.add_participant(&rev_limiter_,       "revlim",    EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::RevLimiter);
    // Launch (MODULE): an input-armed 2nd rev limiter — runs right after revlim so its fuel/ign
    // cut lands before per-cycle fuel/ignition consume the cut flags.
    task.add_participant(&launch_,    "launch",    EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::Launch);
    // Boost (1 kHz, MODULE): PID on (boost_target - map) -> publishes wastegate_duty before the
    // OUTPUT phase; its overboost backstop also sets fuel/ign cut, so it runs in the MODULE phase too.
    task.add_participant(&boost_,     "boost",     EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::Boost);
    // Dfco (MODULE): overrun fuel cut. Runs after the other cut producers (revlim/launch/boost) so it
    // ORs its cut into the accumulated frame/wk::fuel_cut without clobbering theirs, before FuelCalculator.
    task.add_participant(&dfco_,      "dfco",      EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::Dfco);
    task.add_participant(&lambda_protect_, "lambdaprot", EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::LambdaProtect);   // lean-protection cut
    task.add_participant(&flat_shift_,     "flatshift", EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::FlatShift);   // upshift cut
    task.add_participant(&vvt_,            "vvt",       EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::VvtControl);   // cam phasing PID -> vvt_duty_1
    task.add_participant(&pit_limiter_,    "pitlimit",  EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::PitLimiter);   // speed limiter cut
    task.add_participant(&nitrous_,       "nitrous",   EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::Nitrous);   // nitrous retard before ignition
    task.add_participant(&wmi_,           "wmi",       EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::Wmi);   // water/meth pump duty
    task.add_participant(&anti_lag_,      "antilag",   EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::AntiLag);   // ALS retard before ignition
    task.add_participant(&vvl_,           "vvl",       EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::VVL);   // high-lift cam solenoid
    task.add_participant(&egt_protect_,   "egtprot",   EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::EgtProtect);   // EGT enrich + cut (before fuel)
    task.add_participant(&cruise_control_,"cruise",    EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::CruiseControl);   // road-speed hold (before ETB)
    task.add_participant(&torque_model_,  "torque",    EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::TorqueModel);   // torque/power estimate
    // Alternator (1 kHz, MODULE): PI on battery voltage -> publishes alternator_duty before the
    // OUTPUT phase, where a generic output realises it on a pin. Hardware-agnostic — no pin handling here.
    task.add_participant(&alternator_, "alt",      EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::Alternator);
    task.add_participant(&knock_,  "knock",     EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::Knock);   // sets retard before ignition runs
    task.add_participant(&misfire_, "misfire",  EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::Misfire);   // drains crank segment times
    task.add_participant(&lambda_,    "lambda",    EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::Lambda);   // 1 kHz: publishes STFT/LTFT trims to the bus
    // DBW chain (1 kHz, MODULE): App (pedal -> pedal_demand) -> ElectronicThrottle (arbitrates pedal+idle+
    // caps -> throttle_demand, then servos the ETB to it). throttle_demand stays a bus seam inside
    // ElectronicThrottle so the Lua plane can sit between the arbitration and the actuator.
    task.add_participant(&app_,               "app",       EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::App);   // pedal_demand
    // ElectronicThrottle: arbitrate throttle_demand, then resolve A/B plate position + servo it via the
    // position PID; owns autocal + limp. No pin handling — publishes a duty HBridge realises.
    task.add_participant(&electronic_throttle_,  "throttle",  EngineTask::Phase::MODULE,
                         EngineTask::Cadence::KHZ_1, cadence::ElectronicThrottle);
    task.add_participant(&start_control_, "start",     EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::StartControl);   // after the throttle (start_proven is this frame's answer): crank_req / start_pump for the starter and pump outputs
    // TransientThrottle on 1 kHz: throttle/MAP-movement transient (enrich + disenrich) needs a fast,
    // un-windowed source; publishes fuel_corr_accel + ign_corr_transient, which the per-cycle
    // FuelCalculator and Ignition aggregate off the bus.
    task.add_participant(&transient_throttle_, "transient", EngineTask::Phase::MODULE,
                         EngineTask::Cadence::KHZ_1, cadence::TransientThrottle);
    // Idle (1 kHz, MODULE): reads rpm/clt/tps, publishes idle_duty BEFORE the OUTPUT phase, where
    // OutputManager's idle_valve PWM output consumes it. Hardware-agnostic — no pin handling here.
    task.add_participant(&idle_,      "idle",      EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::Idle);
    // Stepper (MODULE): maps a configured position-demand signal to two coil duties, publishing
    // step_demand_a/b BEFORE the OUTPUT phase, where the HBridge slots consume them. Dormant (no bus
    // writes) unless enabled in config. Hardware-agnostic — no pin handling here.
    task.add_participant(&stepper_,   "stepper",   EngineTask::Phase::MODULE, EngineTask::Cadence::KHZ_1, cadence::Stepper);
    // FuelCalculator runs on the PER_CYCLE cadence (E-2b): the heavy charge/VE/correction base needs
    // computing only once per engine cycle, off the 1 kHz critical path. It reads its corrections
    // (STFT/LTFT/protection) and fuel-cut from the bus, so cross-cadence ordering is bus-mediated.
    task.add_participant(&fuel_calc_,         "fuel",      EngineTask::Phase::MODULE, EngineTask::Cadence::PER_CYCLE);
    // Ignition also on PER_CYCLE (E-2 ign→per-cycle): spark/dwell recompute per engine cycle, off the
    // 1 kHz loop. Reads cut/retard from the bus (wk::ign_cut / wk::prot_ign_retard). Now ALL
    // per-cylinder shadow writes (fuel + ignition) happen on the per-cycle task.
    task.add_participant(&ignition_,          "ignition",  EngineTask::Phase::MODULE, EngineTask::Cadence::PER_CYCLE);
    // IgnitionTrim on the 1 kHz MODULE phase: computes the slow advance corrections one-table-per-frame
    // and publishes wk::ign_advance_trim, which the per-cycle Ignition above reads. Cross-cadence
    // via the bus — ordering doesn't matter (the consumer reads the latest published trim).
    task.add_participant(&ignition_trim_,     "igntrim",   EngineTask::Phase::MODULE);
    // FuelTrim on the 1 kHz MODULE phase: the slow fuel corrections (warmup/iat/fuelcomp/baro/gear/
    // generic) one-table-per-frame, re-published every frame; the per-cycle FuelCalculator reads them
    // off the bus in its m() aggregation. Cross-cadence — ordering doesn't matter.
    task.add_participant(&fuel_trim_,         "fueltrim",  EngineTask::Phase::MODULE);
    // script_engine_ is NOT a participant — it runs on its own low-priority thread (see main.cpp's
    // script task), driven by script_engine().tick(bus). It only reads/writes the bus, so it needs
    // no engine-task ordering.
    // HBridge (OUTPUT): consumes the demand idle/etc published in the MODULE phase and actuates
    // the bridge. Runs alongside the other output actuators; no pin handling above the board edge.
    task.add_participant(&hbridge_control_,   "hbridge",   EngineTask::Phase::OUTPUT,
                         EngineTask::Cadence::KHZ_1, cadence::HBridge);
    task.add_participant(&output_manager_,    "outputs",   EngineTask::Phase::OUTPUT);
    // The bench output test sits in the OUTPUT phase beside the manager, so a test and the
    // normal owners see the same frame — and its engine-stopped cancel lands on the very
    // pass the engine starts turning.
    task.add_participant(&output_test_,      "outtest",   EngineTask::Phase::OUTPUT);
    task.add_participant(&diag_,              "diag",      EngineTask::Phase::OUTPUT,
                         EngineTask::Cadence::KHZ_1, cadence::Diagnostics);
}

// THERE IS NO CENTRAL RECONFIGURE HOOK, deliberately. This file used to carry a
// SystemComposer::on_config_change() that re-bound every module and re-inited the sensor pool, and
// nothing ever called it — not main, not comms, not a test. It read exactly like the mechanism, which
// is worse than not existing: work aimed at "what happens on a config change" landed in a function
// that never runs.
//
// What actually happens: a module holds a POINTER into g_config (compose() binds it once via init()),
// so scalars are live with nothing to do. A module with DERIVED state — a bound pin, a composed
// pipeline, a scaled constant — watches g_config_generation itself and re-derives from update(). See
// Sensors::update, OutputManager::update, ScriptEngine and CommsManager, each of which says so.

void SystemComposer::power_on(void* self, EngineTask::Cadence cadence) {
    auto* c = static_cast<SystemComposer*>(self);
    if (cadence == EngineTask::Cadence::KHZ_1) c->power_on_khz1();
    else                                       c->power_on_per_cycle();
}

// KEY-OFF IS A POWER-OFF, for the 1 kHz task's modules. On a car the ECU loses power at key-off and the
// next key-on starts from nothing; with USB holding it up it used to start from whatever the last key
// cycle left. That is how a start press frozen mid-crank (the battery dropped out under the starter)
// came back at key-on as a fresh press and cranked the engine with nobody on the key.
//
// Each module is constructed in place and given the SAME init() and wiring compose() gives it, in the
// same order. Anything meant to survive a power loss is outside the module and init() maps it back in:
// the learned region (LTFT, idle/boost/VVT long-term trims), the DTC store, the tune.
//
// NOT REBUILT, each for a reason:
//   sensors_          skipped while the key is off, so nothing in them drifts; they re-seed themselves
//                     on the key-ON edge, before any gated sensor runs (Sensors::reset_runtime).
//   output_manager_   already releases every pin at key-off and rebuilds from scratch at key-on, with
//                     every conditioned gate born OFF (OutputManager::update) — the same thing, its own way.
//   diag_             the DTC table IS the record of what happened; a power loss keeps it too (SD store).
//   knock_            its measurement ring is written by the knock worker task; rebuilding it here would
//                     race the producer. Its state is per-cylinder knock classification, which only means
//                     anything with the engine running, and its noise floor is in the learned region.
//   misfire_          the segment timer it reads is fed from the capture ISR and bound by main; its state
//                     is per-cylinder classification, meaningful only while running.
//   output_test_      bench-only, and cancels itself on its own rules.
//   script_engine_    Lua runs on its own thread and owns its interpreter's lifetime.
void SystemComposer::power_on_khz1() {
    EngineModule::power_on(hardware_input_);
    EngineModule::power_on(vehicle_speed_);   vehicle_speed_.init(g_config.vehicle_speed);
    EngineModule::power_on(gear_detect_);     gear_detect_.init(g_config.gear_detect);
    EngineModule::power_on(traction_);        traction_.init(g_config.traction_control);
    traction_.set_dtc(&diag_.table());
    EngineModule::power_on(engine_protection_);
    engine_protection_.init(g_config.engine_protection);
    engine_protection_.set_sensors(&sensors_);
    engine_protection_.set_dtc(&diag_.table());
    EngineModule::power_on(rev_limiter_);     rev_limiter_.init(g_config.rev_limiter);
    EngineModule::power_on(launch_);          launch_.init(g_config.launch);
    launch_.set_dtc(&diag_.table());
    EngineModule::power_on(boost_);           boost_.init(g_config.boost);
    boost_.set_dtc(&diag_.table());
    EngineModule::power_on(dfco_);            dfco_.init(g_config.dfco);
    EngineModule::power_on(lambda_protect_);  lambda_protect_.init(g_config.lambda_protect);
    EngineModule::power_on(flat_shift_);      flat_shift_.init(g_config.flat_shift);
    EngineModule::power_on(vvt_);             vvt_.init(g_config.vvt_control);
    EngineModule::power_on(pit_limiter_);     pit_limiter_.init(g_config.pit_limiter);
    EngineModule::power_on(nitrous_);         nitrous_.init(g_config.nitrous);
    EngineModule::power_on(wmi_);             wmi_.init(g_config.wmi);
    EngineModule::power_on(anti_lag_);        anti_lag_.init(g_config.anti_lag);
    EngineModule::power_on(vvl_);             vvl_.init(g_config.vvl);
    EngineModule::power_on(egt_protect_);     egt_protect_.init(g_config.egt_protect);
    egt_protect_.set_dtc(&diag_.table());
    EngineModule::power_on(cruise_control_);  cruise_control_.init(g_config.cruise_control);
    cruise_control_.set_dtc(&diag_.table());
    EngineModule::power_on(torque_model_);    torque_model_.init(g_config.torque_model);
    EngineModule::power_on(alternator_);      alternator_.init(g_config.alternator);
    EngineModule::power_on(lambda_);          lambda_.init(g_config.lambda);
    lambda_.set_dtc(&diag_.table());
    EngineModule::power_on(app_);             app_.init(g_config.app);
    app_.set_dtc(&diag_.table());
    EngineModule::power_on(electronic_throttle_);
    electronic_throttle_.init(g_config.electronic_throttle);
    electronic_throttle_.set_dtc(&diag_.table());
    EngineModule::power_on(start_control_);   start_control_.init(g_config.start_control, &electronic_throttle_);
    start_control_.set_dtc(&diag_.table());
    EngineModule::power_on(transient_throttle_);
    transient_throttle_.init(g_config.transient_throttle);
    transient_throttle_.set_dtc(&diag_.table());
    EngineModule::power_on(idle_);            idle_.init(g_config.idle);
    EngineModule::power_on(stepper_);         stepper_.init(g_config.stepper);
    stepper_.set_dtc(&diag_.table());
    EngineModule::power_on(ignition_trim_);   ignition_trim_.init(g_config.ignition);
    EngineModule::power_on(fuel_trim_);       fuel_trim_.init(g_config.fuel_calculator);
    EngineModule::power_on(hbridge_control_); hbridge_control_.init(g_config.h_bridge, hb_a_, hb_b_, comms_);
    hbridge_control_.set_dtc(&diag_.table());
}

// …and the per-cycle task's, on that task.
void SystemComposer::power_on_per_cycle() {
    EngineModule::power_on(fuel_calc_);       fuel_calc_.init(g_config.fuel_calculator);
    fuel_calc_.set_dtc(&diag_.table());
    EngineModule::power_on(ignition_);        ignition_.init(g_config.ignition);
}
