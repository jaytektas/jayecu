#pragma once

#include "../EngineModule.h"
#include "well_known_signals.h"
#include "../PiController.h"
#include "../../../generated/modules/vvt_control_config.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// VvtControl — closed-loop cam phasing for up to 4 cams (Intake/Exhaust × banks 1-2). A `mode`
// (Intake / Exhaust / both) and `num_banks` select which loops run. Each active loop drives its
// solenoid duty (vvt_duty_1..4) = base(CLT table) + LTT(learned) + PID trim on the error between a
// slew-limited target advance (per-type rpm×load table + overall correction) and the measured cam
// angle (vvt_angle_1..4); gains come from per-type Coolant-temp tables; the output is scaled by the
// shared warm-up Target Scalar. Long Term Trim per cam lives in battery-backed RAM: it slowly absorbs
// the PID hold-trim into a per-CLT learned duty (like Lambda's LTFT) → vvt_ltt_1..4.
// ---------------------------------------------------------------------------

class VvtControl : public EngineModule {
public:
    void init(const VvtControlConfig& cfg);                 // maps the LTT learned region
    void on_config_change(const VvtControlConfig& cfg) { cfg_ = &cfg; }
    void on_engine_stop() override                     { reset(); }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    [[nodiscard]] float duty(int loop) const { return (loop >= 0 && loop < 4) ? loops_[loop].duty : 0.0f; }
    [[nodiscard]] float ltt(int loop, float clt) const { return ltt_ ? ltt_[ltt_cell(loop, clt)] : 0.0f; }

    static constexpr int CLT_BINS  = 8;
    static constexpr int LTT_CELLS = 4 * CLT_BINS;          // 4 cams × per-CLT learned duty

private:
    struct Loop { PiController pi; float ramped = 0.0f; float last_err = 0.0f; float duty = 0.0f; };
    void reset() { for (Loop& l : loops_) { l.pi.reset(); l.ramped = 0.0f; l.last_err = 0.0f; l.duty = 0.0f; } last_ms_ = 0; }
    int  ltt_cell(int loop, float clt) const;

    const VvtControlConfig* cfg_ = nullptr;
    Loop     loops_[4];   // 0 Intake B1 · 1 Exhaust B1 · 2 Intake B2 · 3 Exhaust B2
    float*   ltt_ = nullptr;   // [LTT_CELLS] learned duty %, in the persistent (battery-backed) region
    uint32_t last_ms_ = 0;
};
