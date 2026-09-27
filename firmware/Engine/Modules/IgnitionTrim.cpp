#include "IgnitionTrim.h"
#include "well_known_signals.h"                 // wk::ign_advance_trim
#include "../TableEval.h"                       // tbl::table_eval
#include "../../../generated/table_descs.h"     // <slow>_advance_table_desc(cfg)
#include "../../Signal/SignalBus.h"
#include "../../../generated/signal_ids.h"    // SIG_IGN_CORR_* — one channel per term
#include "../../Signal/EnginePosition.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"        // platform_get_tick_ms

void IgnitionTrim::init(const IgnitionConfig& cfg)            { cfg_ = &cfg; }
void IgnitionTrim::on_config_change(const IgnitionConfig& cfg) { cfg_ = &cfg; }

void IgnitionTrim::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    if (!cfg_) return;

    using DescFn = tbl::TableDesc (*)(const IgnitionConfig*);
    static const DescFn SLOW[N] = {
        clt_ign_corr_table_desc, iat_ign_corr_table_desc, fuelcomp_ign_corr_table_desc,
        gear_ign_corr_table_desc,
        igngen1_ign_corr_table_desc, igngen2_ign_corr_table_desc, igngen3_ign_corr_table_desc, igngen4_ign_corr_table_desc,
        post_start_ign_corr_table_desc,
    };

    // A correction that is OFF is not evaluated at all — the same rule the fuel side follows. That is a
    // different statement from a table tuned flat: a neutral table still costs the lookup, and still
    // hides a mis-set axis behind a row of zeros. Same order as SLOW[].
    const uint8_t ENABLED[N] = {
        cfg_->enable_clt, cfg_->enable_iat, cfg_->enable_fuelcomp,
        cfg_->enable_gear,
        cfg_->enable_generic1, cfg_->enable_generic2, cfg_->enable_generic3, cfg_->enable_generic4,
        cfg_->enable_poststart,
    };

    // Distribute the load: recompute ONE table this frame, never the whole stack. At 1 kHz the full
    // set refreshes every N ms — far faster than coolant/IAT/gear actually move.
    contrib_[rr_] = ENABLED[rr_] ? tbl::table_eval(SLOW[rr_](cfg_), bus) : 0.0f;
    rr_ = static_cast<uint8_t>((rr_ + 1) % N);

    float sum = 0.0f;
    for (int i = 0; i < N; i++) sum += contrib_[i];
    const uint32_t now = platform_get_tick_ms();
    bus.set(wk::ign_advance_trim, sum, true, now, ttl());

    // EACH TERM, not just the total. The sum answers "how much" and never "which one", and a tuner
    // watching the timing move needs the term that moved it — the fuel side has published its
    // corrections individually since it was written. Same order as SLOW[] above; publishing them from
    // the contribution array means these are the numbers that were actually summed, not a second
    // evaluation that could disagree with the first.
    static constexpr SignalId OUT[N] = {
        SIG_IGN_CORR_CLT, SIG_IGN_CORR_IAT, SIG_IGN_CORR_FUELCOMP,
        SIG_IGN_CORR_GEAR,
        SIG_IGN_CORR_GENERIC1, SIG_IGN_CORR_GENERIC2, SIG_IGN_CORR_GENERIC3, SIG_IGN_CORR_GENERIC4,
        SIG_IGN_CORR_POSTSTART,
    };
    for (int i = 0; i < N; i++) bus.set(OUT[i], contrib_[i], true, now, ttl());
}
