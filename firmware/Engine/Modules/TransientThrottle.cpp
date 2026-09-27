#include "TransientThrottle.h"
#include "well_known_signals.h"
#include "../../Diagnostics/DtcManager.h"
#include "../../../generated/module_dtc.h"
#include "../../Diagnostics/Dtc.h"
#include "../TableEval.h"                       // tbl::table_eval
#include "../../../generated/table_descs.h"     // <table>_desc(cfg) builders
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"        // platform_get_tick_ms
#include <algorithm>
#include <cmath>

void TransientThrottle::init(const TransientThrottleConfig& cfg)            { cfg_ = &cfg; }
void TransientThrottle::on_config_change(const TransientThrottleConfig& cfg){ cfg_ = &cfg; }

void TransientThrottle::on_engine_stop() {
    // A stopped engine has no transient — drop everything so a restart starts clean.
    state_ = IDLE; enrich_pct_ = 0.0f; ign_corr_ = 0.0f; detect_ms_ = 0.0f; rate_ = 0.0f;
    last_async_ms_ = 0;
}

void TransientThrottle::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) {
    const uint32_t now = platform_get_tick_ms();
    // Disabled → publish neutral so the FuelCalculator aggregation and Ignition see no effect.
    if (!cfg_ || !cfg_->enabled) {
        bus.set(wk::fuel_corr_accel,      1.0f, true, now, ttl());
        bus.set(wk::ign_corr_transient,   0.0f, true, now, ttl());
        bus.set(wk::transient_active,     0.0f, true, now, ttl());
        bus.set(wk::transient_enrich_pct, 0.0f, true, now, ttl());
        state_ = IDLE; enrich_pct_ = 0.0f; ign_corr_ = 0.0f;
        // Heal what we raised: this return skips the heal() below — see DtcManager::heal.
        if (was_enabled_ && dtc_) dtc_->heal(ModuleDtc::TT_LOAD);
        was_enabled_ = false;
        return;
    }
    was_enabled_ = true;

    // --- Load + derivatives ------------------------------------------------------------------
    const SignalId load_src = static_cast<SignalId>(cfg_->load_source == 0 ? cfg_->tps_src
                                                                           : cfg_->map_src);
    if (dtc_) {
        const uint32_t now_ms = platform_get_tick_ms();
        if (!bus.valid(load_src))
            dtc_->raise(ModuleDtc::TT_LOAD, DtcSource::MODULE, ModuleDtc::TT_LOAD_SEV, now_ms, dtc_ttl());
        else
            dtc_->heal(ModuleDtc::TT_LOAD);
    }
    const float    load   = bus.get(load_src, 0.0f);
    const uint32_t now_ms = platform_get_tick_ms();
    const float    dt_s   = (last_ms_ != 0) ? (now_ms - last_ms_) / 1000.0f : 0.0f;

    // Push this frame's sample, then difference against the oldest one still inside the window. See
    // kRateWinMs in the header for why this cannot be a frame-to-frame difference.
    hist_load_[hist_i_] = load;
    hist_ms_[hist_i_]   = now_ms;
    hist_i_ = static_cast<uint8_t>((hist_i_ + 1) % kRateHist);
    if (hist_n_ < kRateHist) ++hist_n_;

    // Walk back for the NEWEST sample that is at least kRateWinMs old — so the baseline is as short as
    // the window allows and the answer is never staler than it has to be. Before the history has filled
    // (the first ~20 ms after a reset or a re-enable) the oldest sample stands in, and if even that is
    // too recent the rate simply holds rather than dividing by a near-zero interval.
    // k < hist_n_, not <=: the newest slot is the sample just written, and the loop must stop at the
    // oldest one actually WRITTEN. Running one past it read an unused slot whose timestamp is 0, which
    // then failed every subsequent guard and left the rate at zero for good.
    const float* base_load = nullptr;
    uint32_t     base_ms   = 0;
    for (uint8_t k = 1; k < hist_n_; ++k) {
        const uint8_t idx = static_cast<uint8_t>((hist_i_ + kRateHist - 1 - k) % kRateHist);
        base_load = &hist_load_[idx];
        base_ms   = hist_ms_[idx];
        if (now_ms - base_ms >= kRateWinMs) break;                  // far enough back; stop here
    }
    // …and if nothing is that old yet, the loop leaves the OLDEST sample there and we use it. Taking
    // the best window available beats reporting no rate at all: the full window exists within
    // kRateWinMs of starting, and a module called on a slower cadence than the control frame (the host
    // test drives it at 100 Hz) would otherwise never accumulate one. The 2 ms floor is the only hard
    // requirement — below that the millisecond quantisation is back and the quotient means nothing.
    if (base_load) {
        const uint32_t win_ms = now_ms - base_ms;
        if (win_ms >= 2 && win_ms < 1000) {
            const float raw_rate = (load - *base_load) / (win_ms / 1000.0f);   // units/s
            // The window does the smoothing now, so the single pole is only there to take the edge off
            // a genuine step. It was 0.5 against a spike train, where it could not have helped.
            rate_ = rate_ + 0.35f * (raw_rate - rate_);
        }
    }
    // The ACCEL is the derivative of that rate, so it gets the SAME window. Differencing it frame to
    // frame would put the 1 ms quantisation straight back one level up — and a derivative of a
    // derivative squares the error, which is the worst place to leave it.
    float accel = 0.0f;
    if (base_load) {
        const uint32_t win_ms = now_ms - base_ms;
        if (win_ms >= 2 && win_ms < 1000) {
            const uint8_t bi = static_cast<uint8_t>(base_load - hist_load_);
            accel = (rate_ - hist_rate_[bi]) / (win_ms / 1000.0f);  // units/s²
        }
    }
    // Record the rate against THIS sample, so a later frame differencing back to it gets the rate as
    // it stood then rather than as it stands now.
    hist_rate_[(hist_i_ + kRateHist - 1) % kRateHist] = rate_;
    last_load_ = load; last_rate_ = rate_; last_ms_ = now_ms;

    // Publish the rate now (and the held start-load) — the rate/ign tables index on these bus signals.
    if (state_ == IDLE) start_load_ = load;                         // track current load until an event latches one
    bus.set(wk::tt_load_rate,  rate_, true, now, ttl());
    bus.set(wk::tt_start_load, start_load_, true, now, ttl());

    // --- Detection state machine -------------------------------------------------------------
    // Detection runs from ANY state: from IDLE either direction triggers, and from an active event a
    // strong OPPOSITE movement FLIPS direction — a fast tip-in→tip-out blip must register the tip-out
    // even while the enrich is still decaying (else rapid throttle modulation only ever enriches). A
    // same-direction re-trigger is suppressed (state_ != that dir) — the maintenance/decay path owns it.
    const float rate_db = cfg_->enr_load_rate_db * 0.1f;            // %/s (or kPa/s)
    const float dis_db  = cfg_->dis_load_rate_db * 0.1f;            // negative
    bool enrich_triggered = false;                                  // ENRICH trigger edge this tick (arms async)
    const bool enr_trip = rate_ > rate_db && state_ != ENRICH;
    const bool dis_trip = cfg_->enable_disenrich && rate_ < dis_db && state_ != DISENRICH;
    if (enr_trip || dis_trip) {
        detect_ms_ += (dt_s > 0.0f && dt_s < 1.0f) ? dt_s * 1000.0f : 0.0f;
        const float need = (enr_trip ? cfg_->enr_detect_ms : cfg_->dis_detect_ms);
        if (detect_ms_ >= need) {
            state_      = enr_trip ? ENRICH : DISENRICH;
            enrich_triggered = enr_trip;
            enrich_pct_ = 0.0f;                                     // flipping direction → start the new event fresh
            start_load_ = load;                                     // latch the load at event start
            bus.set(wk::tt_start_load, start_load_, true, now, ttl());
            detect_ms_  = 0.0f;
        }
    } else {
        detect_ms_ = 0.0f;
    }

    // --- Magnitude (target %) for the active event -------------------------------------------
    float target = 0.0f;
    if (state_ == ENRICH) {
        const float rate_pct = tbl::table_eval(tt_enrich_rate_table_desc(cfg_), bus);    // %
        const float sync_amt = tbl::table_eval(tt_enrich_sync_table_desc(cfg_), bus);    // %  (100 = full sync)
        const float clt_corr = tbl::table_eval(tt_clt_corr_table_desc(cfg_), bus);       // %  ADD-on cold boost (0 = none)
        const float overall  = cfg_->enable_overall_corr ? (1.0f + cfg_->overall_corr_pct * 0.001f) : 1.0f;
        // sync_amt is a fraction of full (100→×1); CLT is additive enrichment (0% warm → ×1, 220% cold → ×3.2)
        // so a fully-warm engine still gets its base transient fuel.
        target = rate_pct * (sync_amt * 0.01f) * (1.0f + clt_corr * 0.01f) * overall;    // signed +

        // ASYNC delivery — a one-shot burst on the tip-in edge (gated by enable_async + holdoff). The
        // async portion uses the SAME rate × CLT × overall, scaled by the Async Amount table, and is
        // delivered as max_async_pulses extra all-injector squirts of base_pw worth of fuel, spread
        // across the cycle by the scheduler (NOT added to the sync PW). Per-pulse PW = total / pulses.
        if (enrich_triggered && cfg_->enable_async && cfg_->max_async_pulses > 0 &&
            (last_async_ms_ == 0 || (now_ms - last_async_ms_) >= cfg_->async_holdoff_ms)) {
            const float async_amt = tbl::table_eval(tt_enrich_async_table_desc(cfg_), bus);   // %
            const float async_pct = rate_pct * (async_amt * 0.01f) * (1.0f + clt_corr * 0.01f) * overall;
            // FUEL, THEN OPENING TIME PER PULSE. base_pw already carries the injector dead time, so the
            // percentage is taken of the fuel part alone — and every async pulse is a separate opening of
            // the injector, so each needs its OWN dead time. Without it a burst of short pulses could each
            // be shorter than the dead time and deliver almost nothing: a tip-in that stayed lean however
            // the table was tuned.
            const float dead      = std::max(0.0f, bus.get(wk::pw_add_deadtime, 0.0f));
            const float fuel_pw   = std::max(0.0f, bus.get(wk::base_pw, 0.0f) - dead);           // µs/cyl fuel
            const float total_us  = (async_pct * 0.01f) * fuel_pw;
            if (total_us > 0.0f) {
                frame.async_inj_pw_us  = static_cast<uint32_t>(total_us / cfg_->max_async_pulses + dead + 0.5f);
                frame.async_inj_pulses = cfg_->max_async_pulses;
                last_async_ms_         = now_ms;
            }
        }
    } else if (state_ == DISENRICH) {
        const float rate_pct = tbl::table_eval(tt_disenrich_rate_table_desc(cfg_), bus);   // % (negative)
        const float amt      = tbl::table_eval(tt_disenrich_amount_table_desc(cfg_), bus); // % (RPM scale)
        target = rate_pct * (amt * 0.01f);                                                 // signed -
    }

    // --- Accumulate / maintain / decay -------------------------------------------------------
    if (state_ == ENRICH || state_ == DISENRICH) {
        const float accel_db = (state_ == ENRICH) ? cfg_->enr_load_accel_db * 0.1f
                                                  : cfg_->dis_load_decel_db * 0.1f;
        const bool maintaining = (state_ == ENRICH) ? (accel >  accel_db)
                                                    : (accel <  accel_db);
        if (!cfg_->enable_decay) {
            enrich_pct_ = target;                                  // no decay system → follow the rate table directly
        } else if (maintaining) {
            // Keep the peak (tip-in sets the magnitude); decay is paused while the load keeps accelerating.
            enrich_pct_ = (state_ == ENRICH) ? std::max(enrich_pct_, target)
                                             : std::min(enrich_pct_, target);
        }
        // Per-engine-cycle bleed-off (the Decay Rate tables are %/ECyc), applied on cycle boundaries.
        if (cfg_->enable_decay && !maintaining && pos.cycle_count != last_cycle_) {
            const float decay_pct = (state_ == ENRICH)
                ? tbl::table_eval(tt_enrich_decay_table_desc(cfg_), bus)
                : tbl::table_eval(tt_disenrich_decay_table_desc(cfg_), bus);            // %/ECyc
            int steps = static_cast<int>(pos.cycle_count - last_cycle_);
            steps = std::clamp(steps, 1, 8);                                             // bound a big jump

            for (int i = 0; i < steps; ++i) enrich_pct_ *= std::max(0.0f, 1.0f - decay_pct * 0.01f);
        }
        if (std::fabs(enrich_pct_) < 0.1f) { enrich_pct_ = 0.0f; state_ = IDLE; }       // event finished
    }
    last_cycle_ = pos.cycle_count;
    enrich_pct_ = std::clamp(enrich_pct_, -100.0f, 300.0f);

    // --- Ignition correction (deg) — applied while enriching, decays over ign_corr_decay_ms ---
    if (state_ == ENRICH) {
        ign_corr_ = tbl::table_eval(tt_ign_corr_table_desc(cfg_), bus);                 // deg target
    } else if (ign_corr_ != 0.0f && dt_s > 0.0f && dt_s < 1.0f) {
        const float tau = std::max(1.0f, static_cast<float>(cfg_->ign_corr_decay_ms)) / 1000.0f;
        ign_corr_ *= std::exp(-dt_s / tau);
        if (std::fabs(ign_corr_) < 0.05f) ign_corr_ = 0.0f;
    }

    // --- Publish -----------------------------------------------------------------------------
    bus.set(wk::fuel_corr_accel,      1.0f + enrich_pct_ * 0.01f, true, now, ttl());  // mass-mult: >1 enrich, <1 disenrich
    bus.set(wk::ign_corr_transient,   ign_corr_, true, now, ttl());
    bus.set(wk::transient_active,     static_cast<float>(state_), true, now, ttl());
    bus.set(wk::transient_enrich_pct, enrich_pct_, true, now, ttl());
}
