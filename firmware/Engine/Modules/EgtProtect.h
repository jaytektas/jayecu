#pragma once
#include "../EngineModule.h"
#include "../../../generated/modules/egt_protect_config.h"
#include "../../Diagnostics/DtcManager.h"   // the cut names the probe that caused it

// EgtProtect — exhaust gas temperature protection on the shared EGT probe. Two-stage: from enrich_c ramp
// protective fuel enrichment linearly up to max_enrich_pct at cut_c (a richer charge runs cooler); at
// cut_c latch a hard fuel cut until EGT recedes below enrich_c. Publishes fuel_corr_egt (FuelCalculator
// multiplies) + egt_protect_active. Guards turbo / exhaust valves / manifold on over-temp.
class EgtProtect : public EngineModule {
public:
    void init(const EgtProtectConfig& cfg)            { cfg_ = &cfg; cut_ = false; }
    void on_config_change(const EgtProtectConfig& cfg) { cfg_ = &cfg; }
    void on_engine_stop() override                     { cut_ = false; }
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;
    [[nodiscard]] bool cutting() const { return cut_; }
    // The one error table. A cut raises the code for the PROBE that caused it: the protection acts on the
    // hottest probe and does not care which, but whoever has to find the cooked cylinder does — and a
    // stored code is all they have when nothing was being logged.
    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }
private:
    const EgtProtectConfig* cfg_ = nullptr;
    bool        cut_ = false;
    DtcManager* dtc_ = nullptr;
    uint16_t    cut_code_ = 0;   // the probe code currently raised (0 = none), so it heals the same one
};
