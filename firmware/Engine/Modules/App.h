#pragma once

#include "../EngineModule.h"
#include "../../Diagnostics/DtcManager.h"   // the pedal cut raises its cause into the one table
#include "../../../generated/modules/app_config.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// App — accelerator-pedal input for drive-by-wire. The INPUT-side counterpart to the ETB actuator,
// and it shares the same shape: two ANALOG pedal signals are A/B cross-checked, app_a is mapped
// through the pedal-to-throttle table to publish pedal_demand, and a calibrate-mode (engine-stopped)
// learns the pedal's released->pressed span.
//
//   pedal_demand = pedal_to_throttle_table(app_a, rpm[, gear])     (app_a IS the value; 0..100%)
//   app_b        = cross-check ONLY (|a-b| > match_err for match_ms -> fail safe pedal_demand = 0)
//
// Calibrate (CALIBRATION-AWARE, like the ETB autocal): while calibrating, the correlation fault is
// suppressed and pedal_demand is held at 0, so a mid-calibration value never commands throttle. It
// captures the min/max raw of both sensors over a window and writes their 2-point cal. ENGINE-STOPPED
// only — refuses to start, and aborts if the engine starts. Fail-safe pedal-loss keeps the engine
// idling (pedal_demand 0 -> the Throttle module falls to the idle floor).
// ---------------------------------------------------------------------------

class App : public EngineModule {
public:
    void init(const AppConfig& cfg)            { cfg_ = &cfg; reset_state(); }
    void on_config_change(const AppConfig& cfg) { cfg_ = &cfg; reset_state(); }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }

    // Published on app_state so the cut is visible live, not only in the code table afterwards.
    enum State : uint8_t { ST_OK = 0, ST_CALIBRATING = 1, ST_FAULT_CORRELATION = 2, ST_FAULT_NO_SIGNAL = 3 };

    // Pedal calibrate (engine-stopped). Started via the `pedalcal` CLI; the gate is enforced in update().
    void start_calibrate() noexcept;

    // Raw-ADC read seam (ADC counts by analog pool index). Defaults to platform_read_ain_raw; tests override it.
    static void set_raw_reader(uint16_t (*fn)(uint8_t)) noexcept;

private:
    uint16_t latched_code_  = 0;      // pedal fault held until key-off (0 = none)
    uint8_t  latched_state_ = 0;      // the ST_FAULT_* that goes with it
    const AppConfig* cfg_ = nullptr;
    uint32_t last_ms_      = 0;
    float disagree_ms_  = 0;
    // Calibrate working state. The sweep captures each track's raw EXTREMES — order-independent, so the
    // operator cannot get it wrong. Which extreme is "released" comes from the declared track sense, not
    // from the sweep: released->pressed and pressed->released yield the identical set of values, so no
    // procedure can recover direction from the signals.
    DtcManager* dtc_       = nullptr;
    uint16_t active_dtc_   = 0;        // the code currently raised (0 = none) — edge raise/heal
    bool     calibrating_  = false;
    uint32_t cal_t_        = 0;
    uint16_t cal_min_a_=0xFFFF, cal_max_a_=0, cal_min_b_=0xFFFF, cal_max_b_=0;

    void reset_state();
    // Raise `code` (0 = no fault) once, healing whatever this module had raised before it. The table
    // interaction is edge-triggered: re-raising per frame would rewrite the freeze frame every ms.
    void set_fault(uint16_t code, uint32_t now) noexcept;
};

// CLI entry point (defined in App.cpp): forwards to the composed instance.
void app_bench_calibrate() noexcept;
