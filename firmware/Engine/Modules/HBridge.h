#pragma once

#include "../EngineModule.h"
#include "../../Scheduler/IHBridge.h"
#include "../../Diagnostics/DtcManager.h"   // raises HBRIDGE_* P-codes for missing demand/enable signals
#include "../../../generated/modules/h_bridge_config.h"
#include <cstdint>

namespace Comms { class CommsManager; }

// ---------------------------------------------------------------------------
// HBridge — two independent half-bridge slots, each driven by its own demand/dis signal from the bus.
// Any producer (ETB duty, Stepper coil demand, idle duty …) binds via bridge[i].demand_sig. The module
// is deliberately dumb: no integrator, no ramp, no current control — those live in the producer module.
// Runs in the OUTPUT phase after producers have published. See docs/pwm-hal-design.md §5/§7.
// ---------------------------------------------------------------------------

class HBridge : public EngineModule {
public:
    void init(const HBridgeConfig& cfg, IHBridge* a, IHBridge* b,
              Comms::CommsManager* comms = nullptr) noexcept;
    void on_config_change(const HBridgeConfig& cfg) noexcept;
    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;
    void on_engine_stop() override;

    // BENCH NUDGE — drive one half directly for hold_ms, in place of its demand signal, without the
    // enable signal being asserted. This is what "prove it moves" needs and what poking the DIS/DIR
    // GPIOs behind this module's back could never be: update() re-asserts every pin every frame (see
    // apply_bridge), so an external write is gone within a millisecond, and it sets no duty anyway.
    //
    // `demand` is in the PRODUCER's units, not the bridge's: it goes through the same dc_map, dc_max_pct
    // and dir_invert as a real demand would. That is deliberate — a bench test that compensated for the
    // DC map could never reveal a wrong one, and reading the map backwards is the failure this test is
    // mostly for. Engine-stopped only, enforced in update() so a start mid-nudge drops it.
    void set_manual(uint8_t half, float demand, uint32_t hold_ms) noexcept;

private:
    void apply_carrier() noexcept;
    void apply_bridge(int i, bool en, float signed_cmd) noexcept;

    const HBridgeConfig* cfg_          = nullptr;
    Comms::CommsManager* comms_        = nullptr;
    DtcManager*          dtc_          = nullptr;
    IHBridge*            bridges_[2]   = {};
    uint32_t applied_freq_hz_ = 0;   // carrier actually programmed into the timer
    bool                 was_enabled_[2] = {};
    // Whether this half's codes have already been retired since it was switched off — so the retiring
    // happens on the edge instead of on every frame (see update()).
    bool                 off_reported_[2] = {};
    bool                 last_en_[2]  = {};          // what apply_bridge last did — published each frame
    float                last_cmd_[2] = {};
    float                manual_demand_[2]   = {};   // bench nudge: demand in the producer's units
    uint32_t             manual_until_ms_[2] = {};   // 0 = no nudge; else the tick it expires at
};

// CLI seam, mirroring throttle_bench_nudge: the console has no module pointers, and this keeps the
// command a one-liner that cannot reach anything else in the module.
void hbridge_bench_nudge(uint8_t half, float demand, uint32_t hold_ms) noexcept;
