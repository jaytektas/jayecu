#include "VehicleSpeed.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../../generated/signal_ids.h"
#include "../../Platform/platform_hal.h"   // platform_get_tick_ms (publish TTL timestamp)

namespace {
// The raw-frequency signal each PICKUP publishes, in Main Source order — which is why the first six
// Main Source values index straight into this and into cfg_->source[]. A GPS speedometer module puts
// out a pulse train like any other pickup, so it is one of them and not a special case.
constexpr SignalId kHzSignal[6] = { SIG_SHAFT_HZ, SIG_WHEEL_HZ_FL, SIG_WHEEL_HZ_FR,
                                    SIG_WHEEL_HZ_RL, SIG_WHEEL_HZ_RR, SIG_GPS_HZ };
// The km/h signal each WHEEL publishes (the shaft has no wheel of its own).
constexpr SignalId kWheelSignal[4] = { SIG_WHEEL_FL, SIG_WHEEL_FR, SIG_WHEEL_RL, SIG_WHEEL_RR };
}   // namespace

bool VehicleSpeed::speed_of_(SignalBus& bus, uint8_t src, float& kph) const {
    if (!cfg_ || src >= VEHICLE_SPEED_SOURCE_COUNT) return false;
    const uint16_t ppk = cfg_->source[src].pulses_per_km;
    if (ppk == 0) return false;                       // uncalibrated: no answer, rather than a wrong one
    if (!bus.valid(kHzSignal[src])) return false;     // no pickup, or it has gone stale
    const float hz = bus.get(kHzSignal[src], 0.0f);
    // pulses/second / pulses/km = km/second; x3600 = km/h.
    kph = hz * 3600.0f / static_cast<float>(ppk);
    // A floating frequency input reads megahertz. A road speed of nine thousand is worse than none at all
    // for anything acting on it, so an implausible reading is dropped rather than published.
    return kph <= static_cast<float>(cfg_->max_kph) * 0.1f;
}

bool VehicleSpeed::median_(uint8_t mask, const bool ok[4], const float w[4], float& out) {
    // THE MIDDLE VALUE, NOT THE MEAN. A road speed has two ways to be lied to and they sit at opposite
    // ends of the list: a wheel spinning under power reads high, a wheel locking under braking reads low
    // (and in a corner the inside wheel genuinely turns slower than the outside). A mean is dragged by
    // both. A median throws both away at once, which is the whole reason there is no choice of method
    // here for a tuner to get wrong.
    float v[4];
    uint8_t n = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        if (!(mask & (1u << i)) || !ok[i]) continue;   // not in the set, or not reading: not in the sort
        // Insertion sort. Four elements, on a 50 Hz path — the shape that reads as what it is beats one
        // that has to be justified.
        uint8_t j = n;
        while (j > 0 && v[j - 1] > w[i]) { v[j] = v[j - 1]; --j; }
        v[j] = w[i];
        ++n;
    }
    if (n == 0) return false;                          // not known, which is not zero
    // Odd: the middle one. Even: the mean of the middle two — with four wheels that is exactly the pair
    // left after the fastest and the slowest are discarded, and with two it is simply their average.
    out = (n & 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) * 0.5f;
    return true;
}

void VehicleSpeed::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    if (!cfg_ || !cfg_->enabled) return;   // no producer -> don't stomp a speed set elsewhere (Lua/CAN)

    const uint32_t now = platform_get_tick_ms();

    // Every wheel that is calibrated and reading, in km/h — published as it is, because traction control
    // and anything else comparing wheels needs the individual speeds, not just the summary.
    float wheel[4];
    bool  ok[4];
    for (uint8_t i = 0; i < 4; ++i) {
        ok[i] = speed_of_(bus, static_cast<uint8_t>(FL + i), wheel[i]);
        if (ok[i]) bus.set(kWheelSignal[i], wheel[i], true, now, ttl());
    }

    // EVERY PICKUP IS A SPEED, published whether or not it is the Main Source — the four wheels above,
    // and these two. The drive train one is what traction control reads on the common cheap install (a
    // gearbox pickup and one undriven wheel); both of them are what the calibration page shows beside
    // each pickup's frequency, so "what does this row actually resolve to" has an answer on the page
    // where the number is typed. Converting a pickup privately and throwing the answer away is what
    // left both of those with nothing to read.
    float pk = 0.0f;
    if (speed_of_(bus, Shaft, pk)) bus.set(SIG_SHAFT_SPD, pk, true, now, ttl());
    // SHAFT RPM, which is a different question from road speed and needs its own calibration: the
    // pulses/km above has the diff ratio and the rolling radius multiplied into it and cannot be
    // taken apart. Unset pulses-per-rev publishes nothing rather than a number derived from a guess.
    if (cfg_->shaft_ppr > 0 && bus.valid(SIG_SHAFT_HZ))
        bus.set(SIG_DRIVESHAFT_RPM, bus.get(SIG_SHAFT_HZ) * 60.0f / cfg_->shaft_ppr,
                true, now, ttl());
    if (speed_of_(bus, Gps,   pk)) bus.set(SIG_GPS_SPD,   pk, true, now, ttl());

    // …and the road speed itself, from whichever source is nominated. The first six are PICKUPS and go
    // through speed_of_() identically; the last three name a set of corners, and every one of those is
    // combined the same way, because the middle value is not a preference — see median_().
    float kph = 0.0f;
    bool  have = false;
    if (cfg_->main_source < kPickupCount) {
        have = speed_of_(bus, cfg_->main_source, kph);
    } else if (cfg_->main_source <= AllWheels) {
        static constexpr uint8_t kMask[3] = { 0b0011, 0b1100, 0b1111 };   // front axle, rear axle, all
        have = median_(kMask[cfg_->main_source - FrontAxle], ok, wheel, kph);
    }
    if (have) bus.set(SIG_VEHICLE_SPD, kph, true, now, ttl());
}
