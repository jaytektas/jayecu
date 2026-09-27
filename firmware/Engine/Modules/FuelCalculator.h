#pragma once

#include "../EngineModule.h"
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "../../../generated/modules/fuel_calculator_config.h"

class DtcManager;

// ---------------------------------------------------------------------------
// FuelCalculator — speed-density fuel equation.
//
// Algorithm:
//   1. Interpolate VE from 16×16 table [rpm_axis × load_axis].
//   2. Compute base fuel mass from ideal gas law (MAP × VE × displacement).
//   3. Convert mass to pulse width using injector flow rate and dead time.
//   4. Apply CLT enrichment correction from 1D table.
//   5. Apply fuel cut from frame (rev limiter, overtemp, etc.).
//
// Displacement and cylinder count are live tune fields (g_config.engine.displacement /
// .cylinder_count) — read each frame, so they retune without a reset like everything else.
// ---------------------------------------------------------------------------

class FuelCalculator : public EngineModule {
public:
    void init(const FuelCalculatorConfig& cfg);
    void on_config_change(const FuelCalculatorConfig& cfg);
    void on_engine_start() override;
    void on_engine_stop() override;
    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

private:
    float ethanol_pct_  = 0.0f;     // flex: the ethanol content fuelling uses (see update())
    bool  ethanol_seen_ = false;    // a valid reading has been had since power-up
    const FuelCalculatorConfig* cfg_ = nullptr;
    DtcManager*                 dtc_ = nullptr;

    // Wall-film (X-τ) state: the film mass held in fuel-PW units, and the last update tick for dt.
    float    film_pw_    = 0.0f;
    uint32_t wf_last_ms_ = 0;
    // MAP prediction: the throttle-rate estimate it triggers on, and when the hold expires. Kept here
    // rather than recomputed from the bus because the rate is a DERIVATIVE — it only exists if
    // something remembers the last sample and when it was taken.
    float    tps_last_    = 0.0f;
    float    tps_rate_    = 0.0f;      // %/s, lightly filtered (a raw derivative of a noisy channel is noise)
    uint32_t tps_last_ms_ = 0;
    uint32_t predict_until_ms_ = 0;
    float    predict_wt_  = 0.0f;      // how much of the predicted value the trigger asked for (0..1)

    // Engine run-time for post-start enrichment: tick captured at the cranking->running edge by
    // on_engine_start() (the engine caught). wk::run_time = now - run_start_ms_; the post-start
    // table decays from that real start, not from boot.
    //
    // running_ is what makes it RUN time rather than UP time. The anchor starts at 0, so before the
    // engine had ever caught the subtraction below was now-0 — seconds since boot, published under a
    // name that promises otherwise. It read as an engine that had been running all along: the studio
    // showed a climbing Run Time on a stationary engine, the post-start table decayed while nothing
    // had started, and idle's long-term-trim gate (run_time >= ltt_min_runtime_s) opened on a bench.
    // Not running -> 0, which is the true answer to "how long has it been running".
    uint32_t run_start_ms_ = 0;
    bool     running_      = false;

    // Prime pulse: fired once per power-up on the first CRANKING observation. Latched so it never
    // re-primes (only the very first pulse; not used after that has occurred).
    // One prime per power-up, and only once the decoder has SYNC while cranking — see update(). Keyed
    // on/off without cranking never primes, which is the point: that is what was building fuel in the
    // ports during a tuning session.
    bool primed_ = false;
};
