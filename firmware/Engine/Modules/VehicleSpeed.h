#pragma once

#include "../EngineModule.h"
#include "../../../generated/modules/vehicle_speed_config.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// VehicleSpeed — turn the speed pickups into a road speed.
//
// A speed sensor counts teeth, so what it publishes is a FREQUENCY. Turning that into a road speed needs
// one number the sensor cannot know — how many pulses the car makes per kilometre — and one decision it
// has no business making: which pickup, of up to five, the car believes.
//
// It exists because `vehicle_spd` used to BE that frequency. Launch control, the pit limiter, cruise and
// gear detection each compare it against a threshold named in kph, and nothing anywhere converted it, so
// all four were reading Hz as though it were km/h — wrong by whatever the tyre and tooth count make it.
//
// Publishes nothing at all when disabled. A speed arriving over CAN or set from Lua is a legitimate
// source, and a producer that is switched off must not overwrite it with zero.
// ---------------------------------------------------------------------------

class VehicleSpeed : public EngineModule {
public:
    void init(const VehicleSpeedConfig& cfg)            { cfg_ = &cfg; }
    void on_config_change(const VehicleSpeedConfig& cfg) { cfg_ = &cfg; }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    // Main Source, in schema order. THE SOURCE NAMES ITS SENSORS. There is no "driven pair" here and no
    // Driven Axle to look it up with: an option you have to cross-reference against another setting to
    // find out which wheels it meant is not a source, and the driven/undriven channels it used to
    // publish were read by nothing. Slip is where the axle matters, and traction control owns it — it
    // never reads a road speed at all, only the four wheel channels.
    //
    // The first SIX index the per-pickup arrays directly (kHzSignal, cfg_->source[]) — a GPS speedometer
    // module is a pulse train with its own pulses/km, so it is a pickup like the rest. The last three
    // name a set of corners, and are laid out so (value - FrontAxle) indexes the mask table.
    enum Source : uint8_t {
        Shaft = 0, FL = 1, FR = 2, RL = 3, RR = 4, Gps = 5,
        FrontAxle = 6, RearAxle = 7, AllWheels = 8,
    };
    static constexpr uint8_t kPickupCount = 6;   // Shaft..Gps — the sources that ARE a physical input

private:
    // One pickup, converted: raw Hz x 3600 / pulses-per-km. Returns false when that source has no
    // calibration (pulses_per_km 0) or no live reading — "not known", which is not the same as zero and
    // must not be published as one.
    bool  speed_of_(SignalBus& bus, uint8_t src, float& kph) const;
    // The MIDDLE speed of a set of wheels. `mask` picks the corners (bit 0 = FL … bit 3 = RR), and a
    // wheel that is uncalibrated or not reading is simply not in the set — one fitted wheel IS that
    // set's answer, and none at all is "not known" rather than zero.
    static bool median_(uint8_t mask, const bool ok[4], const float w[4], float& out);

    const VehicleSpeedConfig* cfg_ = nullptr;
};
