#include "RevLimiter.h"
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include <algorithm>
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"   // platform_get_tick_ms

void RevLimiter::init(const RevLimiterConfig& cfg) {
    cfg_    = &cfg;
    in_cut_ = false;
}

void RevLimiter::on_config_change(const RevLimiterConfig& cfg) {
    cfg_ = &cfg;
}

void RevLimiter::on_engine_stop() {
    in_cut_ = false;
}

void RevLimiter::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& /*frame*/) {
    // rpm is the decoder's channel — consumed here (pos.rpm), never published.
    const uint32_t now = platform_get_tick_ms();
    const uint32_t t   = ttl();
    // Cuts are validity-OR: publish wk::fuel_cut/ign_cut=true only WHILE cutting; expiry releases them.
    // soft_cut_pct + rpm_to_limit are always published (the rev-limit trim's approach indicators).

    // DTC protection rev limit — read off the bus from EngineProtection (valid + >0 => a level is asking
    // for a rev limit). Enforced even when the user rev limiter is off; cut fuel or ign per prot_rev_cut_fuel.
    const float prot_rev = bus.valid(wk::prot_rev_limit) ? bus.get(wk::prot_rev_limit, 0.0f) : 0.0f;
    if (prot_rev > 0.0f && pos.rpm >= prot_rev) {
        if (bus.get_bool(wk::prot_rev_cut_fuel)) bus.set_bool(wk::fuel_cut, true, now, t);
        else                                     bus.set_bool(wk::ign_cut,  true, now, t);
    }

    if (!cfg_ || !cfg_->enabled) {
        bus.set(wk::soft_cut_pct, 0.0f, true, now, t);
        bus.set(wk::rpm_to_limit, 99999.0f, true, now, t);   // no limiter -> "infinitely far" -> trim neutral
        return;
    }

    const float hard = static_cast<float>(cfg_->hard_limit_rpm);
    // Headroom to the hard cut — the X axis of the rev-limiter fuel trim (pull fuel as rpm nears the cut).
    bus.set(wk::rpm_to_limit, std::max(0.0f, hard - pos.rpm), true, now, t);
    const float soft = static_cast<float>(cfg_->soft_limit_rpm);
    const float resume = hard - static_cast<float>(cfg_->resume_band_rpm);

    // --- Hysteresis: stay in cut until RPM drops below resume threshold ---
    if (in_cut_ && pos.rpm < resume) in_cut_ = false;
    if (pos.rpm >= hard)             in_cut_ = true;

    const uint8_t method = cfg_->cut_method;          // 0 = Fuel, 1 = Ignition, 2 = Both
    if (in_cut_) {
        // Hard cut — every frame, renewed, released by the TTL when the rpm comes back.
        if (method == 0 || method == 2) bus.set_bool(wk::fuel_cut, true, now, t);
        if (method == 1 || method == 2) bus.set_bool(wk::ign_cut,  true, now, t);
        bus.set(wk::soft_cut_pct, 100.0f, true, now, t);
        soft_duty_.reset();
    } else if (pos.rpm >= soft && (hard > soft)) {
        // SOFT CUT, AND NOW ACTUALLY SOFT. This was `if (pct > 0.5) cut` — nothing at all across the
        // bottom half of the band and a continuous 100% cut across the top, which is not a progressive
        // limiter but a SECOND HARD ONE at the band's midpoint. It only ever cut fuel, too, ignoring the
        // cut method the hard limiter beside it honours, and soft_cut_pct reported where the rpm sat in
        // the band rather than what was being cut — so a datalog said 30% while nothing was cut at all.
        //
        // The duty is the position in the band, spread frame by frame (CutDuty), published with
        // frame_ms() so one decision is one frame: ttl() is period PLUS grace, which would hold each cut
        // into the following frame and roughly double every duty.
        const float band = hard - soft;
        const float duty = (pos.rpm - soft) / band * 100.0f;   // 0 .. 100 across the band
        if (soft_duty_.step(duty)) {
            const uint32_t pulse = frame_ms();
            if (method == 0 || method == 2) bus.set_bool(wk::fuel_cut, true, now, pulse);
            if (method == 1 || method == 2) bus.set_bool(wk::ign_cut,  true, now, pulse);
        }
        bus.set(wk::soft_cut_pct, duty, true, now, t);         // what is being cut, not where the rpm is
    } else {
        bus.set(wk::soft_cut_pct, 0.0f, true, now, t);
        soft_duty_.reset();
    }
}
