#include "LambdaProtect.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"      // platform_get_tick_ms
#include "../../../generated/signal_ids.h"

#include <algorithm>

void LambdaProtect::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now   = platform_get_tick_ms();
    const float    dt_ms = (last_ms_ != 0) ? static_cast<float>(now - last_ms_) : 0.0f;
    last_ms_ = now;

    if (!cfg_ || !cfg_->enabled) {
        cut_ = false; bad_ms_ = 0.0f;
        bus.set_bool(SIG_LAMBDA_PROTECT_ACTIVE, false, now, ttl());
        return;   // disabled -> don't touch wk::fuel_cut
    }

    const float rpm    = pos.rpm;
    const float tps    = bus.get(wk::tps, 0.0f);

    // EVERY WIDEBAND THAT HOLDS A JOB, the leanest counting. This read the fixed SIG_LAMBDA_1 — so a
    // wideband wired as sensor 2, or on CAN, or bank 2 of a V engine, was not watched at all, and an
    // absent reading counted as 1.0, "never lean". The Lambda module forwards whichever sensor holds each
    // role; a role nobody holds (or whose sensor is invalid) is simply not there.
    float lambda = 0.0f;
    bool  have   = false;
    for (SignalId r : { SIG_LAMBDA_OVERALL, SIG_LAMBDA_BANK_1, SIG_LAMBDA_BANK_2 })
        if (bus.valid(r)) { lambda = have ? std::max(lambda, bus.get(r)) : bus.get(r); have = true; }
    if (!have && bus.valid(SIG_LAMBDA_1)) { lambda = bus.get(SIG_LAMBDA_1); have = true; }   // unassigned bench setup

    const bool monitored = have
                        && (tps > static_cast<float>(cfg_->min_tps) * 0.1f)
                        && (rpm > static_cast<float>(cfg_->min_rpm));
    // LEAN AGAINST THE TARGET TOO. A fixed limit cannot protect a boosted engine: at a 0.78 target a
    // 1.05 reading is 35 % lean and still under the default 1.10.
    const float target  = bus.get(wk::lambda_target, 1.0f);
    const float margin  = static_cast<float>(cfg_->lean_margin_pct) * 0.001f;   // 0.1 % units -> fraction
    const bool  lean_abs = lambda > static_cast<float>(cfg_->max_lambda) * 0.001f;
    const bool  lean_rel = margin > 0.0f && bus.valid(wk::lambda_target) && lambda > target * (1.0f + margin);
    const bool  lean     = lean_abs || lean_rel;

    if (!monitored) {
        cut_ = false; bad_ms_ = 0.0f;               // out of the load window (lift / low rpm) -> release
    } else {
        if (lean) bad_ms_ += dt_ms;
        else      bad_ms_ = 0.0f;
        if (bad_ms_ >= static_cast<float>(cfg_->timeout_ms)) cut_ = true;   // latches until !monitored
    }

    if (cut_) bus.set_bool(wk::fuel_cut, true, now, ttl());   // validity-OR: publish only while cutting
    bus.set_bool(SIG_LAMBDA_PROTECT_ACTIVE, cut_, now, ttl());
}
