#pragma once
#include "../Engine/EngineModule.h"
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "DtcManager.h"
#include "../Signal/SignalBus.h"              // SIG_DTC_* channels (uint32 cells)
#include "../../generated/dtc_categories.h"   // dtc_indicator_bit(code) — P-code -> category

// ---------------------------------------------------------------------------
// Diagnostics — "the DTC module". A thin EngineModule that owns the passive
// DtcManager table and, each frame, packs its current state into telemetry (the
// ONLY path by which error state reaches the host). Producers (Sensors,
// protection, trigger, conflicts) raise() into table(); consumers (LED, OBD, SD)
// read table(). The table itself stays a pure, standalone-testable structure.
// ---------------------------------------------------------------------------

class Diagnostics : public EngineModule {
public:
    void init(uint16_t boot_id) { dtc_.init(boot_id); }

    DtcManager&       table()       { return dtc_; }
    const DtcManager& table() const { return dtc_; }

    // Publish the table → bus (the ONLY path error state reaches TS). The table is
    // filled by producers via raise()/heal(); we scan it and publish the summary as
    // integer channels (cells are uint32). Cheap (scans 64 slots).
    void update(const EnginePosition&, SignalBus& bus, EngineFrame&) override {
        uint16_t worst_code = 0;
        uint8_t  worst_sev  = 0;
        uint32_t cat_bits   = 0;   // per-category indicator mask (schema dtc_indicators)
        for (uint8_t i = 0; i < DtcManager::SLOTS; i++) {
            const DtcRecord& r = dtc_.slot(i);
            if (!r.code || !(r.status & DTC_ACTIVE)) continue;
            if (r.severity >= worst_sev) { worst_sev = r.severity; worst_code = r.code; }
            const int bit = dtc_indicator_bit(r.code);   // -1 = uncategorised (still in worst/OBD/LED)
            if (bit >= 0) cat_bits |= (1u << bit);
        }
        bus.set_u32(wk::dtc_active,     dtc_.active_count());
        bus.set_u32(wk::dtc_stored,     dtc_.stored_count());
        bus.set_u32(wk::dtc_worst_sev,  worst_sev);
        bus.set_u32(wk::dtc_worst_code, worst_code);
        bus.set_u32(wk::dtc_indicators, cat_bits);
    }

private:
    DtcManager dtc_;
};
