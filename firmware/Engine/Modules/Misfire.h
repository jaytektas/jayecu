#pragma once

#include "../EngineModule.h"
#include "../../../generated/modules/misfire_config.h"

#include <cstdint>

class DtcManager;
class SegmentTimer;

// ---------------------------------------------------------------------------
// Misfire — did the cylinder fire at all?
//
// The opposite question to knock, answered by a different instrument. SegmentTimer measures how long
// the crank took to cross each cylinder's expansion stroke, from the REAL teeth; this module decides
// what a long one means. A cylinder that fires accelerates the crank through its segment; one that
// does not, does not.
//
// EVERYTHING HERE IS RELATIVE, and that is the design rather than a shortcut. A segment's absolute
// time is a function of engine speed, so it says nothing on its own. The metric is deviation from the
// mean of the SURROUNDING segments — which makes a whole-engine acceleration cancel, because
// speeding up shortens every segment together. Only a segment that is long relative to its
// neighbours is a misfire. That single choice removes the need for a separate transient inhibit,
// which is the usual source of blind spots in this kind of detector.
//
// THE WHEEL LIES, AND IT LIES CONSISTENTLY. Teeth are not perfectly evenly spaced and TDCs are not
// perfectly known, so one cylinder's segment can read long for no combustion reason — permanently
// indistinguishable from that cylinder always misfiring. The fix is free: during OVERRUN FUEL CUT
// nothing is firing, so every segment SHOULD be identical and whatever spread remains is pure
// geometry. DFCO already publishes that condition (it ORs into wk::fuel_cut). The correction is
// learned there, stored as a speed-independent ratio, and applied thereafter.
//
// Availability is SegmentTimer's to decide: PHASE sync (a segment belongs to a NAMED cylinder, and at
// CRANK level the revolutions are indistinguishable) and enough teeth per segment to time one. Below
// that this module simply never sees a segment and reports nothing, which is the honest answer.
// ---------------------------------------------------------------------------

class Misfire : public EngineModule {
public:
    static constexpr uint8_t MAX_CYL = 12;

    void init(const MisfireConfig& cfg);
    // The segment source is assigned separately, from wherever the scheduler and the composer are
    // both in scope (main). Null = the module never sees a segment and reports nothing, which is
    // exactly what a host test or a platform without a timer should get.
    void set_timer(SegmentTimer* t) noexcept { timer_ = t; }
    void on_config_change(const MisfireConfig& cfg) { cfg_ = &cfg; }
    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }
    void on_engine_stop() override;

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    // ---- Introspection (tests, telemetry, the bench CLI) ----
    // Deviation of this cylinder's last segment from its neighbours, as a fraction (0.08 = 8% long).
    [[nodiscard]] float    roughness(uint8_t cyl)  const { return cyl < MAX_CYL ? rough_[cyl] : 0.0f; }
    [[nodiscard]] uint16_t events(uint8_t cyl)     const { return cyl < MAX_CYL ? events_[cyl] : 0; }
    // Learned geometry correction — the ratio this cylinder's segment reads with nothing firing.
    [[nodiscard]] float    correction(uint8_t cyl) const;
    [[nodiscard]] uint32_t total()                 const { return total_; }
    [[nodiscard]] uint32_t segments_seen()         const { return seen_; }

private:
    void classify(uint8_t cyl, uint32_t ticks, bool overrun, uint32_t now);
    void learn(uint8_t cyl, float ratio);

    const MisfireConfig* cfg_    = nullptr;
    SegmentTimer*        timer_  = nullptr;
    DtcManager*          dtc_    = nullptr;
    float*               corr_[MAX_CYL] = {};   // learned region, 1 cell each

    // The neighbourhood: the last MAX_CYL corrected segment times. The mean is taken BEFORE the
    // current sample joins, so a cylinder is compared against the ones around it rather than against
    // a mean it has already contaminated.
    uint32_t ring_[MAX_CYL] = {};
    uint8_t  rn_ = 0, rhead_ = 0;

    float    rough_[MAX_CYL]  = {};
    uint16_t events_[MAX_CYL] = {};
    uint32_t total_  = 0;
    uint32_t seen_   = 0;
    uint16_t cut_mask_ = 0;
    bool     learning_ = false;
    // Enabled->disabled EDGE, for the heal in update(). Misfire's codes LATCH — they are a tally of
    // events, and no pass re-asserts them once the engine runs cleanly — so the table's ttl cannot
    // retire them and the module has to. Same shape as Boost/Knock/Launch/Stepper/TransientThrottle.
    bool     was_enabled_ = false;
    uint32_t cycles_   = 0;      // segments counted toward the evaluation window
    uint8_t  ncyl_     = 0;
};
