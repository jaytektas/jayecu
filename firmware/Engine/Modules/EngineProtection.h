#pragma once

#include "../EngineModule.h"
#include "../../../generated/modules/engine_protection_config.h"
#include "../../Diagnostics/DtcManager.h"        // the one error table — detection raises into it, the reactor reads severity from it

class Sensors;   // Tier-3 sensor runtime — source of per-sensor health + severity

// ---------------------------------------------------------------------------
// EngineProtection — protection DETECTION + the 3-tier REACTION policy.
//
// Runs early in the EngineTask pipeline so cuts are visible to RevLimiter and
// all downstream modules.
//
// There is no named-fault state machine any more (the old FaultId / active_faults
// / fault_actions path is deleted). Each check raises its OBD P-code straight into
// the DTC table with an intrinsic severity (level 1 / 2 / 3) — exactly the
// way the Sensors module raises sensor codes. The table is the single source of
// truth; the reactor below reads its worst ACTIVE severity and applies the matching
// protection level. Healing is immediate (condition clears → heal()); the table
// keeps the stored history + persists to SD.
// ---------------------------------------------------------------------------

class EngineProtection : public EngineModule {
public:
    void init(const EngineProtectionConfig& cfg);
    void on_config_change(const EngineProtectionConfig& cfg);
    void on_engine_stop() override;

    // Inject the Sensors module (the producer). Protection consumes its per-sensor
    // health/severity instead of re-deriving sensor faults from the bus. Optional —
    // if unset, the legacy bus-age timeout sweep runs unchanged.
    void set_sensors(const Sensors* s) { sensors_ = s; }

    // The unified DTC table. Detection raises/heals P-codes into it; the reactor
    // reads the worst active severity and applies the cut/limp policy.
    void set_dtc(DtcManager* d) { dtc_ = d; }

    void update(const EnginePosition& pos,
                SignalBus&      bus,
                      EngineFrame&    frame) override;

    // Re-evaluate all checks from scratch next frame — call after an external wipe
    // of the table (OBD Mode 04 clear_all) so still-true conditions re-raise.
    void reset_edges();

private:
    // Edge-guarded detect: only touch the DTC table on a condition TRANSITION, so a
    // steady true/false condition costs nothing (no per-frame find() scan). `idx` is
    // this check's slot in prev_cond_ — stable across frames. raise on false→true,
    // heal on true→false; the table does the dedup/count/persist bookkeeping.
    void detect(uint8_t idx, uint16_t code, uint8_t severity, bool condition, uint32_t now_ms);

    const EngineProtectionConfig* cfg_     = nullptr;
    const Sensors*                sensors_ = nullptr;
    DtcManager*                   dtc_     = nullptr;

    // True once the engine achieves first CRANK sync — prevents startup
    // crank-searching from triggering the sync-loss code.
    bool was_synced_ = false;
    float synced_rpm_   = 0.0f;    // speed at the last update that still had sync
    bool  lost_running_ = false;   // sync was lost while running — P0335 until sync returns

    // THE LEVEL'S OWN RELEASE, which is a different question from whether the fault is still being
    // reported. A code goes inactive the moment its judge stops saying so; a limp level should not,
    // because a marginal fault that flickers would otherwise flicker the engine in and out of limp —
    // which is worse to drive than staying in it, and is exactly what `auto_reset_s` says in the tune.
    //
    // Two settings, both declared in the schema and neither read by anything until now:
    //   auto_reset_s   seconds after the condition clears before this level lets go. 0 = it does not
    //                  self-release: it holds for the rest of the key cycle, or until the codes are
    //                  cleared.
    //   dtc_condition  whether the level watches CURRENT faults or also STORED ones — "the fault
    //                  indicates damage that healing does not undo"
    //
    // With these here, the DTC table's freshness stops being safety-critical: it decides what is being
    // REPORTED, and this decides what the engine is being PROTECTED from. They were conflated while
    // this was missing, which is why the ttl was creeping towards carrying both.
    uint32_t level_since_ms_[3] = {};   // when each level last saw its severity present (0 = never)
    bool     level_held_[3]     = {};   // …and whether it is currently holding
    uint32_t clear_gen_seen_    = 0;    // the table's clear counter, to let go when the codes are wiped
    bool     key_was_on_        = false;   // …and the key state, to let go on a key cycle

    // Previous condition per check (the edge guard). One slot per detect() call site.
    static constexpr uint8_t CHECK_COUNT = 16;   // battery low/high detection moved to the sensor; 15 = a disarmed monitor
    bool prev_cond_[CHECK_COUNT] = {};
    uint32_t last_raise_ms_[CHECK_COUNT] = {};   // when each true condition last refreshed its code

    // Trigger NOISE is reported off a free-running counter, so the condition is "the counter moved
    // recently" rather than a level. Held briefly, or it would raise and heal at 1 kHz for as long
    // as noise kept arriving and the fault table would record a storm instead of a fault.
    static constexpr uint32_t NOISE_HOLD_MS = 1000;
    uint32_t prev_noise_total_    = 0;
    uint32_t noise_hold_until_ms_ = 0;
    // Phase loss (a cam not where the crank says, or overdue), reported the same way off its own count.
    uint32_t prev_phase_lost_total_   = 0;
    uint32_t phase_lost_hold_until_ms_ = 0;

    // Threshold-monitor programs that failed validation at the last config change. A bad program is
    // DISARMED — it never trips — because a condition that cannot be evaluated must not be allowed to
    // cut the engine. Checked once per config load, not per frame.
    void validate_monitors();
    bool mon_bad_[8] = {};
    // on_config_change is never called (SystemComposer.cpp: there is no central reconfigure hook), so
    // the module watches g_config_generation itself, like Launch and Traction: a monitor typed or edited
    // live is validated before it is ever run.
    uint32_t cfg_gen_seen_ = 0xFFFFFFFFu;
};
