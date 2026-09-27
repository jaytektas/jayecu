#pragma once

#include "../EngineModule.h"
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "../../../generated/modules/transient_throttle_config.h"

// ---------------------------------------------------------------------------
// TransientThrottle — an empirical transient fuel model (replaces the old scalar
// AccelEnrich). On a load (TPS or MAP) MOVEMENT it adds enrichment for an opening throttle (the
// manifold momentarily goes lean while pressure/airflow catch up) or disenrichment for a closing
// throttle (momentarily rich). It is the empirical sibling of FuelCalculator's physical wall-film
// (X-τ) model — run ONE or the other, never both (a codegen validator warns if both are enabled).
//
// Runs on the 1 kHz cadence (NOT per-cycle) so it actually catches a throttle stab — per-cycle is
// ~75 ms at idle, far too slow. Detection:
//   load     = the selected source (load_source 0=TPS via tps_src, 1=MAP via map_src), units = % or kPa
//   rate     = d(load)/dt          [units/s]  — published as wk::tt_load_rate, drives the rate tables
//   accel    = d(rate)/dt          [units/s²] — the MAINTENANCE gate (a load-accel dead band)
//
//   ENRICH triggers when rate > enr_load_rate_db held for enr_detect_ms; it stays topped-up while
//   accel > enr_load_accel_db and otherwise bleeds off by the Enrich Decay Rate table (%/engine-cycle,
//   applied on cycle_count boundaries). Magnitude = EnrichRate(startLoad,rate) × SyncAmount(rpm)
//   × CLTcorr(clt) × (1+Overall). DISENRICH mirrors on the closing side (Rate × Amount).
//
// Publishes the SYNC correction as a fuel-mass multiplier on wk::fuel_corr_accel (>1 enrich, <1
// disenrich) — FuelCalculator aggregates it off the bus, unchanged. The ASYNC portion (Async Amount
// table) is delivered on the tip-in edge as a burst of extra all-injector squirts via the frame
// (async_inj_pw_us/pulses → EngineTask → EnginePositionHal → EventScheduler's async train), spread
// between the sequential events. Also publishes wk::ign_corr_transient (deg, summed by
// Ignition) plus telemetry: wk::transient_active / transient_enrich_pct / tt_start_load.
// ---------------------------------------------------------------------------

class DtcManager;

class TransientThrottle : public EngineModule {
public:
    void init(const TransientThrottleConfig& cfg);
    void on_config_change(const TransientThrottleConfig& cfg);
    void on_engine_stop() override;
    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

private:
    enum State : uint8_t { IDLE = 0, ENRICH = 1, DISENRICH = 2 };

    const TransientThrottleConfig* cfg_ = nullptr;
    DtcManager*                    dtc_ = nullptr;
    bool was_enabled_ = false;   // enabled->disabled edge: heal our DTCs once (DtcManager::heal)

    float    last_load_   = 0.0f;    // previous load sample (for the rate)
    float    rate_        = 0.0f;    // filtered load rate [units/s]
    float    last_rate_   = 0.0f;    // previous rate (for the accel)
    uint32_t last_ms_     = 0;

    // A SLIDING WINDOW TO DIFFERENTIATE OVER, because frame-to-frame cannot work here.
    //
    // The rate was (load - last_load) / (now_ms - last_ms) taken every control frame. Two things go
    // wrong at 1 kHz. The denominator is whole milliseconds, so it is 0 (skipped) or 1 — about one
    // bit of precision, and every wobble in the frame period lands straight in the quotient. And the
    // numerator moves SLOWER than the denominator: TPS publishes at 200 Hz and MAP at 100 Hz, so four
    // frames in five the load has not changed and the rate reads 0, then the fifth divides 5 ms of
    // movement by 1 ms and reads five times too high. Measured on the rig, a mathematically exact
    // +20.0 /s ramp came out swinging +2 .. +64 /s.
    //
    // tt_enrich_rate_table is indexed on this, with breakpoints at 0/25/50/100 — so that swing walked
    // across three of them and the enrichment jittered with it.
    //
    // So keep a short history and difference against the sample ~kRateWinMs old: the denominator is
    // real elapsed time (~20 ms, ~5 % quantisation instead of ~100 %) and the numerator spans several
    // genuine sensor updates. It is still recomputed EVERY frame — a slower update would have been
    // simpler, but enr_detect_ms defaults to 1 ms and transient fuel that arrives late is the stumble
    // people chase elsewhere, so the answer stays fresh and only the baseline lengthens.
    static constexpr uint8_t  kRateHist   = 48;   // ≥ kRateWinMs of frames at 1 kHz, with headroom
    static constexpr uint32_t kRateWinMs  = 20;   // ≥ 2 MAP updates (100 Hz) and ≥ 4 TPS (200 Hz)
    float    hist_load_[kRateHist] = {};
    float    hist_rate_[kRateHist] = {};   // the rate computed at that sample — the accel's baseline
    uint32_t hist_ms_[kRateHist]   = {};
    uint8_t  hist_n_   = 0;          // samples written (saturates at kRateHist)
    uint8_t  hist_i_   = 0;          // next write slot
    uint32_t last_cycle_  = 0;       // EnginePosition.cycle_count at the last decay step

    State    state_       = IDLE;
    float    start_load_  = 0.0f;    // load captured at event start (drives the rate/ign axes)
    float    enrich_pct_  = 0.0f;    // current transient %, signed (+enrich / -disenrich)
    float    detect_ms_   = 0.0f;    // accumulated time the trigger condition has held
    float    ign_corr_    = 0.0f;    // current ignition correction [deg], decays over ign_corr_decay_ms
    uint32_t last_async_ms_ = 0;     // tick of the last async burst (async_holdoff_ms gate)
};
