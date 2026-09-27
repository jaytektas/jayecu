#pragma once

#include "../EngineModule.h"
#include "../../../generated/modules/ignition_config.h"
#include "../../Scheduler/SchedulerTypes.h"   // MAX_CYLINDERS (per-cylinder knock retard)

// ---------------------------------------------------------------------------
// Ignition — advance angle and dwell from 2D table + corrections.
//
// Algorithm:
//   1. Interpolate base advance from 16×16 table [rpm × load].
//   2. Apply CLT correction from 1D table.
//   3. Apply knock retard (written by a future Knock module).
//   4. Clamp to [min_adv, max_adv].
//   5. Write per-cylinder spark_btdc and dwell to frame.cyl[].
// ---------------------------------------------------------------------------

class Ignition : public EngineModule {
public:
    void init(const IgnitionConfig& cfg);
    void on_config_change(const IgnitionConfig& cfg);
    void on_engine_stop() override;

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    // Called by Knock (runs after this module) to apply retard. The whole engine at once (an external
    // knock source cannot say which cylinder), or PER CYLINDER — the retard a knocking cylinder earned
    // is that cylinder's; the others keep their timing.
    void apply_knock_retard(float retard_deg);
    void apply_knock_retard(const float* per_cyl, uint8_t n);

private:
    // ign_table / clt_advance lookups go through the generic table engine — tbl::interp() in
    // Engine/TableEngine.h — over float axes; the old per-type helpers (interp2d_i8/_i16, interp1d_i8)
    // were retired with the table-engine migration.
    const IgnitionConfig* cfg_ = nullptr;
    float knock_retard_deg_ = 0.0f;   // the WORST cylinder's knock retard — published, and in the global advance
    float knock_cyl_[MAX_CYLINDERS] = {};   // each cylinder's own knock retard
};
