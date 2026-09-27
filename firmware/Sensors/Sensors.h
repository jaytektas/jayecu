#pragma once

#include "../Engine/EngineModule.h"
#include "../Signal/SignalBus.h"
#include "../../generated/ecu_config.h"          // SensorsConfig / SensorConfig
#include "../../generated/sensors_catalog.h"     // SENSOR_CATALOG, SensorDescriptor, SENSOR_COUNT
#include "../Diagnostics/DtcManager.h"           // the one error table — sensors raise P-codes here
#include "../Integration/PipelineBuilder.h"      // value-production pipeline per input (build_input)

class GenericCan;

// ---------------------------------------------------------------------------
// Sensors — the Input pool (Tier-3 runtime). Walks SENSOR_CATALOG[i] (read-only
// Tier-2 catalog) paired with cfg.sensor[i] (Tier-3 tune) each frame.
//
// VALUE PRODUCTION is a polymorphic pipeline per input (pipe::build_input):
//
//   acquire (analog/freq/sent/pulse/switch/on-board) -> decode_curve -> [EMA]
//
// On top of the pipeline's value, this manager applies the cross-cutting Input-pool
// policy: rate decimation, pin arbitration, and the
// diagnostics — raw-window, operating-window (precondition-gated), stuck, and
// rate-of-change checks — raising/healing catalog DTCs (P-code + severity) and
// publishing the value with valid=!faulted. See docs/polymorphic-pipeline-architecture.md.
//
// Runs FIRST in the EngineTask pipeline (it's the producer — it fills the bus
// before any consumer reads it). CAN-device inputs are produced by the CAN RX path,
// not here (their pipeline isn't built). Migrating the diagnostics into Condition
// stages is the documented next refinement.
//
// State arrays are sized by SENSOR_COUNT (from the generated catalog) so they can
// never drift from the config-array dimension.
// ---------------------------------------------------------------------------

class Sensors : public EngineModule {
public:
    void init(const SensorsConfig& cfg);
    // Make the two sensors publishing these channels run on the SAME tick. For a pair that is
    // cross-checked — the ETB's tps_a/tps_b, the pedal's app_a/app_b — sampling them a fraction of a
    // period apart shows up as a disagreement of however far the thing moved in between, which is a
    // scheduling artefact read as a sensor fault. Called by the composer from each module's own
    // config, so the pair that is compared is the pair that is aligned, with nothing to tick and
    // nothing to forget. Harmless for channels no sensor publishes.
    void align_phase(SignalId a, SignalId b);
    // Re-derive every cross-checked pair from the modules that compare them. Called on each config
    // change (see the definition — the composer's on_config_change is not wired).
    void align_crosschecked();

    // The unified DTC table. Each diagnostic check raise()s/heal()s its catalog
    // P-code here (source = the sensor's catalog index). Optional — null = the
    // legacy per-sensor tracking still runs, the table just isn't populated.
    void set_dtc(DtcManager* d) { dtc_mgr_ = d; }

    // Generic CAN (owned by CanBroker). A sensor whose interface is CAN takes its reading from a
    // receive FIELD, named by its `source` byte — the same decode that feeds the signal bus, so the
    // addressing lives in the tune and is shared with the transmit side rather than being a device
    // table compiled in. Null = CAN-interface sensors don't build.
    // Composed before init() (SystemComposer), but if it ever arrives after, every CAN input built
    // without it must be rebuilt now — nothing in its own settings will have changed to trigger it.
    void set_generic_can(const GenericCan* g) { gcan_ = g; if (cfg_) recompute_live(/*all=*/true); }

    // DOES A LIVE SENSOR WRITE THIS CHANNEL? Asked by GenericCan when it builds its receive index, so
    // a frame field and a sensor both claiming one channel is reported as a config fault instead of
    // the two of them alternating on the bus with nothing to show for it. Cheap and called only on a
    // config change, so it walks the pipelines rather than keeping a second table in step with them.
    [[nodiscard]] bool produces(SignalId s) const {
        for (const pipe::Pipeline& p : pipelines_)
            if (p.n > 0 && p.signal == s) return true;
        for (uint8_t i = 0; i < AUX_N; i++)
            if (aux_pipelines_[i].n > 0 && aux_pipelines_[i].signal == s) return true;
        return false;
    }

    // Producer: reads the physical inputs and WRITES them onto the bus.
    // pos/frame unused; now_ms is read internally.
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    // KEY-ON LIVES HERE, because the battery lives here. The battery is the bootstrap sensor -- the one
    // input that runs whether or not the key is on -- so it is the only thing that can tell the rest of
    // the system the key just turned. It used to be read here and DECIDED in EngineTask, one phase
    // later, and that gap was a real fault: on the frame the key came up, INPUT had already run with the
    // gate still false and skipped every non-bootstrap sensor, then MODULE ran and found no TPS. Every
    // module that needs a signal raised "signal missing" for exactly one frame, every key-on and every
    // boot (P1720 was reproducibly counted doing it, once per key cycle). Deciding it mid-pass, right
    // after the battery publishes, lets the remaining sensors run in the SAME pass -- so by the time any
    // module looks at the bus, the signals are there. No waiting, no grace window, no settling timer.
    bool key_on() const noexcept { return key_on_; }

    // EITHER 5 V SENSOR REFERENCE IS DOWN (a power-good line low, key on). While it is, every sensor read
    // from an ECU pin publishes INVALID — not only the ones on the failed rail. The ECU is not told which
    // reference each sensor is wired to, and nobody configures that on any other ECU; running on the half
    // of the sensors that happen to be on the good rail is not a state worth engineering towards. Exempt:
    // the battery (its own divider, and the key is decided from it), on-board inputs and CAN inputs,
    // none of which the followers feed. Each reference raises its own DTC (P0641 / P0651).
    bool supply_lost() const noexcept { return supply_lost_; }

    // Key-on hysteresis, in volts. Public so the one place that used to own them can't drift from them.
    static constexpr float KEY_ON_V  = 8.0f;
    static constexpr float KEY_OFF_V = 7.0f;

    // Per-sensor health: false once an enabled diagnostic check is tripped.
    bool healthy(uint8_t idx) const { return idx < SENSOR_COUNT && !fault_[idx]; }

    // Two enabled sensors claim the same input pin — a config
    // error surfaced as FAULT_PIN_CONFLICT. Computed at init() from the static config.
    bool input_conflict() const { return input_conflict_; }

    // Worst active diagnostic severity across all enabled sensors this frame
    // (0 = none, 1..3 = level 1..3). EngineProtection maps this to a reaction
    // instead of re-deriving sensor faults from the bus. See docs/sensors-design.md.
    // Inline so consumers (EngineProtection) need not link Sensors.cpp.
    uint8_t worst_severity() const {
        uint8_t w = 0;
        for (uint8_t i = 0; i < SENSOR_COUNT; i++) if (sev_[i] > w) w = sev_[i];
        return w;
    }

    // True if some ENABLED sensor produces this channel — lets EngineProtection
    // skip the stale-timeout fault for channels no configured sensor feeds
    // (kills phantom timeouts on unconfigured inputs). O(1): the mask is rebuilt
    // from the static config at init()/config-change, not scanned per frame
    // (it was a 91-sensor loop called 5x/frame from the protection sweep).
    bool channel_enabled(SignalId ch) const {
        return ch < SIG_COUNT && ch_enabled_[ch];
    }

    // Active OBD DTCs this frame — each enabled sensor with a tripped check
    // contributes its worst check's catalog P-code (0 = none). Fills out[] up to
    // max, returns the count. Consumed by ObdResponder Mode 03. Inline so the CAN
    // layer needn't link Sensors.cpp.
    uint8_t active_dtcs(uint16_t* out, uint8_t max) const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < SENSOR_COUNT && n < max; i++)
            if (dtc_[i]) out[n++] = dtc_[i];
        return n;
    }

private:
    const SensorsConfig* cfg_ = nullptr;

    // Per-input value-production pipeline (acquire -> decode -> filter), rebuilt from the
    // live config in recompute_live(). The EMA + switch-debounce state now lives inside the
    // pipeline (pipe::State), so this manager only keeps the diagnostics/decimation state.
    pipe::Pipeline    pipelines_[SENSOR_COUNT];
    pipe::InputCfgPool pools_[SENSOR_COUNT];   // backing store for each pipeline's stage cfgs
    // Secondary outputs (a sensor_type's declared extra outputs, e.g. flex fuel temp): each is its
    // own full pipeline, sharing the parent sensor's source pin + filter. Sized by the catalog.
    static constexpr uint8_t AUX_N = SENSOR_AUX_COUNT ? SENSOR_AUX_COUNT : 1;
    pipe::Pipeline    aux_pipelines_[AUX_N];
    pipe::InputCfgPool aux_pools_[AUX_N];
    // EMA / switch-debounce / stuck / derivative state now lives INSIDE the pipeline
    // (pipe::State per stage); this manager keeps only decimation + arbitration + DTC state.
    bool     key_on_   = false;   // hysteretic key state, decided mid-pass (see key_on())
    // The 5 V references (see supply_lost()): read right after the key is decided, before any sensor that
    // they gate runs, so the pass in which one fails is also the pass in which the sensors go invalid.
    void     update_supplies(SignalBus& bus, uint32_t now);
    bool     supply_lost_ = false;
    uint32_t supply_low_since_[2]  = {};   // tick a power-good line went low (0 = good)
    bool     supply_dtc_raised_[2] = {};   // edge state for P0641 / P0651
    uint8_t  boot_idx_ = 0;       // catalog index of the bootstrap (battery) sensor — run FIRST
    DtcManager* dtc_mgr_ = nullptr;              // unified DTC table (set_dtc); checks raise/heal here
    const GenericCan* gcan_ = nullptr;           // generic CAN, for CAN-interface sensors
    bool     input_conflict_ = false;        // two enabled sensors share an input pin (config error)
    bool     ch_enabled_[SIG_COUNT] = {};    // enabled-channel mask; channel_enabled() reads it
    bool     arb_rejected_[SENSOR_COUNT] = {}; // lost pin arbitration -> must NOT acquire/publish
    // Enabled, but the tune says something the hardware cannot honour: either the active interface
    // needs a pin and none is assigned, or it is an interface this sensor is not built to be read
    // through. One flag, one DTC per sensor — both are "this input is configured wrong", and both
    // leave it publishing nothing useful.
    bool     cfg_invalid_[SENSOR_COUNT] = {};
    bool     cfg_dtc_raised_[SENSOR_COUNT] = {}; // edge state for the SENSOR_CATALOG[i].dtc_config raise/heal
    // Precondition EXPRESSION validity, re-derived on every config change (never per frame — the
    // M7 must not execute unvalidated bytecode, but it need only be checked when the bytes change).
    // A bad program fails ARMED: detection stays on and its own DTC says which sensor is broken.
    bool     expr_bad_[SENSOR_COUNT] = {};
    bool     expr_dtc_raised_[SENSOR_COUNT] = {}; // edge state for SENSOR_CATALOG[i].dtc_precond
    bool     noband_dtc_raised_[SENSOR_COUNT] = {}; // edge state for SENSOR_CATALOG[i].dtc_no_band
    uint32_t cfg_gen_seen_ = 0xFFFFFFFFu;    // last g_config_generation we re-derived from (force first)
    // Re-derive what depends on the live config — each input's pipeline, the enabled-channel mask, pin
    // arbitration, the engine-sync pins. Run at init (`all`) and whenever g_config changes.
    //
    // A PIPELINE IS REBUILT ONLY WHEN ITS OWN SENSOR'S SETTINGS CHANGED. Rebuilding zeroes the stage
    // state — a switch's debounce, a multi-position switch's settled position, the stuck and rate
    // timers — and this used to rebuild all 128 on ANY write, so a VE cell edited mid-drive dropped a
    // held switch to "off" for three samples. The global generation only says something, somewhere,
    // changed; each element's own bytes are fingerprinted (CRC-32) and compared, which catches every
    // writer — the studio, a routine writing its own calibration, a tune load — without any of them
    // having to report what they touched. Only ENABLED elements are hashed: a disabled one has no
    // pipeline to disturb, and switching it on or off shows as its enable flag against built_on_.
    void     recompute_live(bool all = false);
    // Rebuild just the elements a write landed in — the range comes from the comms layer, which knows
    // the offset and length it was given (see update()). One rebuild instead of 134 fingerprints.
    void     recompute_range(uint8_t first, uint8_t last);
    // The derivations that belong to the WHOLE array rather than one element: the engine-sync pin
    // list, the enabled-channel mask, the pin-conflict verdict. Both rebuild paths end here.
    void     rederive_shared();
    // Heal what an input raised, once, when it is switched off (see the .cpp).
    void     retire_codes(uint8_t i);
    void     rebuild_one(uint8_t i);         // pipeline + secondary outputs of one input
    uint32_t built_crc_[SENSOR_COUNT] = {};  // fingerprint of each element as its pipeline was built
    // Set by the rebuild that turned an input OFF, cleared by the frame that retires its codes. One
    // flag per input instead of healing every disabled one on every config write (see update()).
    // The inputs that actually run, in the order the frame must run them (bootstrap first). Rebuilt by
    // rederive_shared(); the frame walks THIS instead of all 134 catalogue slots.
    uint8_t  active_[SENSOR_COUNT] = {};
    uint8_t  active_n_ = 0;

    bool     built_on_[SENSOR_COUNT]  = {};  // …and whether it was built ENABLED
    uint16_t built_sync_window_ = 0xFFFF;    // engine_sync_window_deg the sampler was configured with
    // THE GENERIC CAN FRAME POOLS, fingerprinted. A CAN sensor names its field by frame id and start
    // bit, so a frame edit can move, delete or re-point the field it reads WITHOUT a byte of the
    // sensor's own element changing — and the per-element CRC above would then keep it running on a
    // pipeline aimed at the old pool slot. This is how the rebuild hears about the other half of the
    // question. 0 = no CAN subsystem wired.
    uint32_t built_can_crc_ = 0;
    uint32_t can_pool_crc() const;
    void     compute_conflicts();            // pin arbitration: first claimant wins, losers rejected
    bool     fault_[SENSOR_COUNT]     = {};   // last-frame diagnostic trip
    uint8_t  sev_[SENSOR_COUNT]       = {};   // last-frame worst severity per sensor
    uint16_t dtc_[SENSOR_COUNT]       = {};   // last-frame worst check's P-code (0=none)
    // WHICH PERIOD SLOT this sensor last ran in, and WHERE in the period it sits.
    //
    // Decimation is "have I run in this slot yet", not "is it past my deadline": slot = (now + phase)
    // / period, run when it changes. Two sensors of the same rate and phase therefore run on the same
    // tick whenever each of them started, and a late frame (the engine task is vTaskDelay(1), not a
    // fixed rate, so a millisecond can be skipped) still runs instead of dropping a whole period.
    //
    // The phase SPREADS the load: a sensor's default is its own index, so 120 inputs at 200 Hz land
    // ~24 per millisecond across the 5 ms period rather than 120 on one frame. Sensors that must be
    // COMPARED with each other are given the same phase by align_phase(), so they coincide by
    // construction — the ETB's A/B check and the pedal's are the pairs that need it.
    uint32_t last_slot_[SENSOR_COUNT] = {};   // slot index of the last run (rate decimation)
    bool     ran_once_[SENSOR_COUNT]  = {};   // …distinguishes "slot 0" from "never run"
    uint16_t phase_[SENSOR_COUNT]     = {};   // ms offset within the period (see align_phase)
    uint32_t last_run_[SENSOR_COUNT]  = {};   // tick of this sensor's last run (per-sensor filter dt)
    // Edge-triggered DTC interaction: the bit set is the diagnostic slots this sensor currently
    // has RAISED in the central table. The expensive find()-scan in raise/heal fires ONLY when a
    // slot's state flips — steady-state (healthy or already-faulted) touches the table zero times,
    // so cost is O(1)/sensor/frame regardless of how many sensors are enabled.
    uint8_t  dtc_asserted_[SENSOR_COUNT] = {};        // main 6 slots: bit s = slot s raised
    // PERSISTENCE (Diag Delay ms): the tick a slot STARTED tripping, 0 = not tripping. A check must hold
    // continuously for the delay before its code raises, so a single out-of-range sample — an EMI spike on
    // one ADC read — is not a stored fault. The setting shipped in every tune and the firmware never read
    // it, so detection was one-sample-instant however it was configured.
    uint32_t dtc_since_ms_[SENSOR_COUNT][6] = {};
    uint8_t  aux_asserted_[AUX_N]        = {};        // per aux output: bit0=op_min, bit1=op_max raised
    uint32_t last_clear_gen_ = 0;                     // DtcManager.clear_generation() last seen (Mode 04)
};
