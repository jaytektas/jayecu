#include "EgtProtect.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"
#include "../../../generated/signal_ids.h"
#include "../../../generated/module_dtc.h"
#include <algorithm>

void EgtProtect::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now = platform_get_tick_ms();
    if (!cfg_ || !cfg_->enabled) {
        cut_ = false;
        bus.set(SIG_FUEL_CORR_EGT, 1.0f, true, now, ttl());
        bus.set_bool(SIG_EGT_PROTECT_ACTIVE, false, now, ttl());
        // Heal what we raised: this return skips the heal() below — see DtcManager::heal.
        if (cut_code_ && dtc_) { dtc_->heal(cut_code_); cut_code_ = 0; }
        return;   // disabled -> neutral correction, don't touch wk::fuel_cut
    }

    // Protect on the HOTTEST assigned EGT — any over-temp cylinder trips it. Iterates every per-cylinder
    // EGT sensor (Overall/Bank/Cylinder assignment doesn't matter for a global cut); an absent/unconfigured
    // sensor reads the 0 fallback, so this degrades cleanly to the single-EGT case.
    float egt = 0.0f;
    uint16_t hottest = 0;                       // 1-based probe number, 0 = nothing reading
    for (uint16_t k = 0; k < EGT_SIGNAL_COUNT; ++k) {
        const float v = bus.get(EGT_SIGNALS[k], 0.0f);
        if (v > egt || hottest == 0) { egt = v; hottest = static_cast<uint16_t>(k + 1); }
    }
    const float enrich_c = static_cast<float>(cfg_->enrich_c);
    const float cut_c    = static_cast<float>(cfg_->cut_c);

    // Enrichment ramps 0..max as EGT climbs enrich_c..cut_c (span guarded against misconfig).
    float mult = 1.0f;
    if (egt > enrich_c) {
        const float span = (cut_c > enrich_c) ? (cut_c - enrich_c) : 1.0f;
        const float frac = std::clamp((egt - enrich_c) / span, 0.0f, 1.0f);
        mult = 1.0f + frac * (static_cast<float>(cfg_->max_enrich_pct) / 100.0f);
    }

    // Hard cut latches at cut_c, releases only once EGT falls back below the enrich threshold.
    const bool was_cut = cut_;
    if (egt >= cut_c)         cut_ = true;
    else if (egt < enrich_c)  cut_ = false;

    // NAME THE PROBE. Raised on the cut's rising edge against the probe that was hottest at that moment,
    // and healed on release — so the stored code points at a cylinder instead of leaving twelve to guess.
    if (dtc_) {
        // The twelve codes are one contiguous block, so the probe number indexes it directly — asserted
        // here rather than trusted, since a schema edit that split the block would otherwise raise the
        // code for the wrong cylinder, which is worse than raising none.
        static_assert(ModuleDtc::EGT_OVERTEMP_2  == ModuleDtc::EGT_OVERTEMP_1 + 1,  "EGT codes not contiguous");
        static_assert(ModuleDtc::EGT_OVERTEMP_12 == ModuleDtc::EGT_OVERTEMP_1 + 11, "EGT codes not contiguous");
        static_assert(EGT_SIGNAL_COUNT == 12, "one EGT over-temp code per probe");
        if (cut_ && !was_cut && hottest >= 1 && hottest <= EGT_SIGNAL_COUNT)
            cut_code_ = static_cast<uint16_t>(ModuleDtc::EGT_OVERTEMP_1 + (hottest - 1));
        // RE-ASSERTED EVERY FRAME WHILE THE CUT HOLDS. The probe is chosen on the edge, but a raise carries
        // a lifetime (dtc_ttl) and the table ages a code nobody re-raises out of ACTIVE — so a cut held
        // longer than that showed no active fault while the engine sat cut. ETB and App re-raise for the
        // same reason.
        if (cut_ && cut_code_) {
            dtc_->raise(cut_code_, DtcSource::PROTECTION, ModuleDtc::EGT_OVERTEMP_1_SEV, now, dtc_ttl());   // level 3, declared in the schema
        } else if (!cut_ && cut_code_) {
            dtc_->heal(cut_code_);   // clears ACTIVE, keeps STORED — the history is the point
            cut_code_ = 0;
        }
    }

    bus.set(SIG_FUEL_CORR_EGT, mult, true, now, ttl());
    if (cut_) bus.set_bool(wk::fuel_cut, true, now, ttl());   // validity-OR: publish only while cutting
    bus.set_bool(SIG_EGT_PROTECT_ACTIVE, cut_ || (egt > enrich_c), now, ttl());
}
