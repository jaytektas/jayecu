#include "FuelTrim.h"
#include "well_known_signals.h"                 // wk:: fuel_corr_* roles
#include "../TableEval.h"                       // tbl::table_eval
#include "../../../generated/table_descs.h"     // <slow>_table_desc(cfg)
#include "../../Signal/SignalBus.h"
#include "../../Signal/EnginePosition.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"        // platform_get_tick_ms

void FuelTrim::init(const FuelCalculatorConfig& cfg)             { cfg_ = &cfg; }
void FuelTrim::on_config_change(const FuelCalculatorConfig& cfg) { cfg_ = &cfg; }

void FuelTrim::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    if (!cfg_) return;

    using DescFn = tbl::TableDesc (*)(const FuelCalculatorConfig*);
    static const DescFn SLOW[N] = {
        clt_corr_table_desc, iat_corr_table_desc, fuel_comp_corr_table_desc,
        baro_corr_table_desc, fuel_gear_table_desc,
        generic1_corr_table_desc, generic2_corr_table_desc,
        generic3_corr_table_desc, generic4_corr_table_desc,
    };

    // Each correction carries its own enable, in the SAME order as SLOW[] — the table is not evaluated
    // at all when its correction is off, and the publish below then reports exactly 1.000. "Off" used to
    // mean a table tuned to 0%, which still cost this evaluation every visit and still read its axis
    // channels; the flag says what a neutral table only implies.
    const uint8_t on[N] = {
        cfg_->enable_warmup,   cfg_->enable_iat,      cfg_->enable_fuelcomp,
        cfg_->enable_baro,     cfg_->enable_gear,
        cfg_->enable_generic1, cfg_->enable_generic2, cfg_->enable_generic3, cfg_->enable_generic4,
    };

    // Distribute the load: re-evaluate ONE table this frame, never the whole stack. At 1 kHz the full
    // set refreshes every N ms — far faster than coolant/IAT/baro/gear physically move.
    pct_[rr_] = on[rr_] ? tbl::table_eval(SLOW[rr_](cfg_), bus) : 0.0f;
    rr_ = static_cast<uint8_t>((rr_ + 1) % N);

    // Re-publish ALL corrections every frame from the cache so each stays fresh at 1 kHz (the per-cycle
    // m() aggregation reads them with a neutral 1.0 default; a stale signal would drop the correction).
    // Each cell stores % (table_eval applies the schema scale), so the multiplier is 1 + %/100. warmup
    // also exposes the raw CLT-enrichment % as wk::clt_corr telemetry (matches the old FuelCalc publish).
    // A disabled correction reads 1.000 from THIS frame, not from the next visit of the round-robin:
    // the cached percentage is up to N frames old, and a correction switched off on the studio should
    // stop correcting when it is switched off, not up to nine milliseconds later.
    const uint32_t now = platform_get_tick_ms();
    auto mult = [&](int i) { return on[i] ? 1.0f + pct_[i] / 100.0f : 1.0f; };
    bus.set(wk::clt_corr,           on[0] ? pct_[0] : 0.0f, true, now, ttl());
    bus.set(wk::fuel_corr_warmup,   mult(0), true, now, ttl());
    bus.set(wk::fuel_corr_iat,      mult(1), true, now, ttl());
    bus.set(wk::fuel_corr_fuelcomp, mult(2), true, now, ttl());
    bus.set(wk::fuel_corr_baro,     mult(3), true, now, ttl());
    bus.set(wk::fuel_corr_gear,     mult(4), true, now, ttl());
    bus.set(wk::fuel_corr_generic1, mult(5), true, now, ttl());
    bus.set(wk::fuel_corr_generic2, mult(6), true, now, ttl());
    bus.set(wk::fuel_corr_generic3, mult(7), true, now, ttl());
    bus.set(wk::fuel_corr_generic4, mult(8), true, now, ttl());
}
