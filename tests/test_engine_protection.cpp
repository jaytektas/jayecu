#include "test_helpers.h"
#include "../firmware/Engine/Modules/EngineProtection.h"
#include "../firmware/Diagnostics/DtcManager.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"   // wk:: cut/prot signal roles
#include "../generated/signal_ids.h"
#include "../generated/modules/engine_protection_config.h"
#include "../generated/ecu_config.h"      // g_config.engine.cranking_rpm (the stop-vs-loss threshold)
#include "../firmware/Signal/ExprIsa.h"   // the monitors are compiled programs now

#include <algorithm>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// EngineProtection is now pure detection + the 3-tier reactor: each check raises
// its OBD P-code into the DTC table (no FaultId state machine, no active_faults,
// no fault_actions, no latch/heal-timer). These tests assert on the table + the
// EngineFrame prot_* reaction fields, not on a bitmask.
// ---------------------------------------------------------------------------

// A monitor's condition is a compiled PROGRAM now, not a signal id and a pair of limits — so the
// tests hand-assemble the one shape they need, in the same bytes the studio's compiler emits
// (Signal/ExprIsa.h owns the opcode numbers; two copies of them would be a silent-wrong-answer bug).
static std::vector<uint8_t> gt(SignalId s, float v) {
    const uint16_t sel = static_cast<uint16_t>(s + 1);      // 0 = None, so a channel is id+1
    const uint32_t k   = static_cast<uint32_t>(static_cast<int32_t>(v * 100.0f));   // literals are x100
    std::vector<uint8_t> b{expr::OP_PUSH_SIG, uint8_t(sel & 0xFF), uint8_t(sel >> 8), expr::OP_PUSH_F};
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t((k >> (8 * i)) & 0xFF));
    b.push_back(expr::OP_GT);
    b.push_back(expr::OP_END);
    return b;
}

static void set_condition(ThresholdMonitorsConfig& m, const std::vector<uint8_t>& prog) {
    std::memset(m.condition, 0, sizeof(m.condition));       // and the tail stays zero: OP_END is 0
    std::memcpy(m.condition, prog.data(), std::min(prog.size(), sizeof(m.condition)));
}

// P-codes mirrored from EngineProtection.cpp (the values' nibbles are the digits).
static constexpr uint16_t P_CLT_TIMEOUT  = 0x0117;
static constexpr uint16_t P_MAP_TIMEOUT  = 0x0107;
static constexpr uint16_t P_OVERTEMP_CLT = 0x0217;
static constexpr uint16_t P_CLT_WARNING  = 0x1217;   // mfr code (no SAE coolant-warning code)
static constexpr uint16_t P_OVERBOOST    = 0x0234;
static constexpr uint16_t P_SYNC_LOSS    = 0x0335;
static constexpr uint16_t P_PHASE_LOST   = 0x0341;

volatile uint32_t g_config_generation = 0;   // the tune-write counter modules watch
static uint32_t g_tick_ms = 1000u;   // start at 1000 so fresh signals (set at 1000) have age 0
extern "C" uint32_t platform_get_tick_ms() { return g_tick_ms; }
extern "C" void platform_set_led_warning(bool) {}
extern "C" void platform_set_led_error(bool)   {}

// ---------------------------------------------------------------------------
// Config builder — the 3-tier reaction policy (DTC severity 1/2/3 -> a level).
// ---------------------------------------------------------------------------
static EngineProtectionConfig make_cfg(
    uint16_t timeout_ms = 500,
    int16_t  clt_warn   = 95,
    int16_t  clt_cut    = 105,
    int16_t  iat_cut    = 65,
    uint16_t map_cut    = 250)
{
    EngineProtectionConfig cfg{};
    cfg.sensor_timeout_ms = timeout_ms;
    cfg.clt_warn_c        = clt_warn;
    cfg.clt_cut_c         = clt_cut;
    cfg.iat_cut_c         = iat_cut;
    cfg.map_cut_kpa       = map_cut;

    for (auto& m : cfg.threshold_monitors) {   // every slot off, and watching nothing
        m.action = 0;
        std::memset(m.condition, 0, sizeof(m.condition));
    }
    // severity 1/2/3 -> rev limit 6000 (ign) / 4500 (fuel) / 2000 (fuel), staged-severity style.
    cfg.protection_levels[0].enabled = 1; cfg.protection_levels[0].rev_limit_rpm = 6000;
    cfg.protection_levels[0].rev_limit_type = 0;
    cfg.protection_levels[1].enabled = 1; cfg.protection_levels[1].rev_limit_rpm = 4500;
    cfg.protection_levels[1].rev_limit_type = 1;
    cfg.protection_levels[2].enabled = 1; cfg.protection_levels[2].rev_limit_rpm = 2000;
    cfg.protection_levels[2].rev_limit_type = 1;
    return cfg;
}

static EnginePosition make_pos(float rpm = 2000.0f, SyncLevel sync = SyncLevel::CRANK) {
    EnginePosition p{};
    p.rpm             = rpm;
    p.sync_level      = sync;
    p.is_synchronized = (sync >= SyncLevel::CRANK);
    return p;
}

static void set_all_fresh(SignalBus& bus, float clt = 80.0f, float map = 101.3f,
                           float tps = 20.0f, float lam = 1.0f, float batt = 13.0f) {
    bus.set(SIG_CLT,      clt,  true, g_tick_ms);
    bus.set(SIG_MAP,    map,  true, g_tick_ms);
    bus.set(SIG_TPS,    tps,  true, g_tick_ms);
    bus.set(SIG_LAMBDA_1, lam,  true, g_tick_ms);
    bus.set(SIG_BATTERY,  batt, true, g_tick_ms);
    bus.set(SIG_IAT,      25.0f, true, g_tick_ms);
}

bool g_system_active = true;   // key-on, owned by Sensors on target; the reactor reads it to let
                               // a held level go on a key cycle

int main() {
    fprintf(stdout, "=== EngineProtection (DTC-table reactor) ===\n");

    // -------------------------------------------------------------------
    SECTION("no faults — fresh and in range, table empty");
    {
        auto cfg = make_cfg();
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);

        SignalBus bus{}; set_all_fresh(bus);
        EngineFrame frame{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(), bus, frame);

        CHECK(!bus.valid(wk::fuel_cut));
        CHECK(!bus.valid(wk::ign_cut));
        CHECK(dtc.active_count() == 0);
        CHECK(bus.get_u32(SIG_MONITOR_FLAGS) == 0u);
        CHECK(bus.get_u32(SIG_PROT_STATUS) == 0u);
    }

    // -------------------------------------------------------------------
    SECTION("a level HOLDS after its fault clears, and lets go on its own timer");
    {
        // THE TWO CLOCKS ARE DIFFERENT, and this is the one that keeps an engine safe. A code goes
        // inactive the moment its judge stops saying so — which is right, it is a report. A limp level
        // must not, or a marginal fault flickers the engine in and out of limp, which is worse to drive
        // than staying in it. `auto_reset_s` is the tune saying how long to hold; 0 means it does not
        // let go at all, until the codes are cleared or the key is cycled.
        //
        // Both settings have been in the schema, described in the help text, and read by NOTHING.
        auto cfg = make_cfg();
        cfg.protection_levels[2].enabled       = 1;
        cfg.protection_levels[2].rev_limit_rpm = 2000;
        cfg.protection_levels[2].auto_reset_s  = 0;      // never self-release
        cfg.protection_levels[2].dtc_condition = 0;      // watch CURRENT faults
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);
        SignalBus bus{}; EngineFrame frame{};
        set_all_fresh(bus); ep.update(make_pos(), bus, frame);
        CHECK(!bus.valid(SIG_PROT_REV_LIMIT));

        dtc.raise(0x0217, DtcSource::PROTECTION, DTC_SEV_LEVEL3, g_tick_ms);
        bus.invalidate(SIG_PROT_REV_LIMIT);
        set_all_fresh(bus); ep.update(make_pos(), bus, frame);
        CHECK(bus.valid(SIG_PROT_REV_LIMIT) && bus.get(SIG_PROT_REV_LIMIT) == 2000.0f);   // it acts

        dtc.heal(0x0217);                                        // the fault stops being reported
        g_tick_ms += 30000;                                      // half a minute of nothing
        // INVALIDATE FIRST, or this proves nothing: the bus keeps the last value it was given, so a
        // level that had stopped publishing would still read 2000 and the check would pass on a stale
        // number. (It did, until the hold was deliberately removed and the test stayed green.)
        bus.invalidate(SIG_PROT_REV_LIMIT);
        set_all_fresh(bus); ep.update(make_pos(), bus, frame);
        CHECK(dtc.active_count() == 0);
        CHECK(bus.valid(SIG_PROT_REV_LIMIT) && bus.get(SIG_PROT_REV_LIMIT) == 2000.0f);  // LEVEL holds

        cfg.protection_levels[2].auto_reset_s = 3;               // now let it go after 3 s of quiet
        bus.invalidate(SIG_PROT_REV_LIMIT);                      // a level publishes only while it acts
        set_all_fresh(bus); ep.update(make_pos(), bus, frame);
        CHECK(!bus.valid(SIG_PROT_REV_LIMIT));                   // the quiet already elapsed: released
    }

    // -------------------------------------------------------------------
    SECTION("clearing the codes lets a held level go");
    {
        auto cfg = make_cfg();
        cfg.protection_levels[2].enabled       = 1;
        cfg.protection_levels[2].rev_limit_rpm = 2000;
        cfg.protection_levels[2].auto_reset_s  = 0;
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);
        SignalBus bus{}; EngineFrame frame{};
        dtc.raise(0x0217, DtcSource::PROTECTION, DTC_SEV_LEVEL3, g_tick_ms);
        bus.invalidate(SIG_PROT_REV_LIMIT);
        set_all_fresh(bus); ep.update(make_pos(), bus, frame);
        CHECK(bus.valid(SIG_PROT_REV_LIMIT) && bus.get(SIG_PROT_REV_LIMIT) == 2000.0f);
        // One of the two releases for a level that does not self-release: the operator saying they have
        // dealt with it. (The other is a key cycle — see the module.)
        dtc.clear_all();
        bus.invalidate(SIG_PROT_REV_LIMIT);
        set_all_fresh(bus); ep.update(make_pos(), bus, frame);
        CHECK(!bus.valid(SIG_PROT_REV_LIMIT));
    }

    // -------------------------------------------------------------------
    SECTION("a key cycle releases a level that does not self-release");
    {
        // The other release, and the one every production ECU has: switch it off and on. A fault that
        // is still true re-trips on the next pass, so this costs nothing — and without it, a key-off
        // would do nothing while a power cycle cleared the hold anyway, since the hold is RAM.
        auto cfg = make_cfg();
        cfg.protection_levels[2].enabled       = 1;
        cfg.protection_levels[2].rev_limit_rpm = 2000;
        cfg.protection_levels[2].auto_reset_s  = 0;
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);
        SignalBus bus{}; EngineFrame frame{};
        dtc.raise(0x0217, DtcSource::PROTECTION, DTC_SEV_LEVEL3, g_tick_ms);
        bus.invalidate(SIG_PROT_REV_LIMIT);
        set_all_fresh(bus); ep.update(make_pos(), bus, frame);
        CHECK(bus.valid(SIG_PROT_REV_LIMIT) && bus.get(SIG_PROT_REV_LIMIT) == 2000.0f);
        dtc.heal(0x0217);                       // the fault clears; the level holds
        bus.invalidate(SIG_PROT_REV_LIMIT);
        set_all_fresh(bus); ep.update(make_pos(), bus, frame);
        CHECK(bus.valid(SIG_PROT_REV_LIMIT));
        g_system_active = false;                // key off…
        set_all_fresh(bus); ep.update(make_pos(), bus, frame);
        g_system_active = true;                 // …and on
        bus.invalidate(SIG_PROT_REV_LIMIT);
        set_all_fresh(bus); ep.update(make_pos(), bus, frame);
        CHECK(!bus.valid(SIG_PROT_REV_LIMIT));  // released
    }

    // -------------------------------------------------------------------
    SECTION("a level watching STORED faults stays in limp after the fault heals");
    {
        // "The fault indicates damage that healing does not undo" — the schema's words. Nothing read
        // this setting either.
        auto cfg = make_cfg();
        cfg.protection_levels[2].enabled       = 1;
        cfg.protection_levels[2].rev_limit_rpm = 2000;
        cfg.protection_levels[2].auto_reset_s  = 1;      // would let go a second after it cleared…
        cfg.protection_levels[2].dtc_condition = 1;      // …but it watches STORED, which never clears
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);
        SignalBus bus{}; EngineFrame frame{};
        dtc.raise(0x0217, DtcSource::PROTECTION, DTC_SEV_LEVEL3, g_tick_ms);
        dtc.heal(0x0217);                                 // active gone, STORED remains
        g_tick_ms += 10000;
        set_all_fresh(bus); ep.update(make_pos(), bus, frame);
        CHECK(dtc.active_count() == 0 && dtc.stored_count() == 1);
        bus.invalidate(SIG_PROT_REV_LIMIT);
        set_all_fresh(bus); ep.update(make_pos(), bus, frame);
        CHECK(bus.valid(SIG_PROT_REV_LIMIT) && bus.get(SIG_PROT_REV_LIMIT) == 2000.0f);
    }

    // -------------------------------------------------------------------
    SECTION("CLT overtemp — P0217 raised (sev 3), Level 3 reaction");
    {
        auto cfg = make_cfg();
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);

        SignalBus bus{}; set_all_fresh(bus, 110.0f);   // > cut 105 (also > warn 95)
        EngineFrame frame{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(), bus, frame);

        CHECK(dtc.code_severity(P_OVERTEMP_CLT) == 3);
        CHECK(dtc.code_severity(P_CLT_WARNING) == 1);   // both conditions true
        CHECK(dtc.worst_severity() == 3);
        // worst severity 3 -> protection_levels[2]: rev limit 2000, fuel type
        CHECK(bus.get(wk::prot_rev_limit) == 2000.0f);
        CHECK(bus.get_bool(wk::prot_rev_cut_fuel));
        CHECK(bus.get_u32(SIG_PROT_STATUS) & 0x04u);    // severe (sev 3) bit
    }

    // -------------------------------------------------------------------
    SECTION("CLT warning — P0216 only (level 1), no cut");
    {
        auto cfg = make_cfg();
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);

        SignalBus bus{}; set_all_fresh(bus, 97.0f);     // > warn 95, < cut 105
        EngineFrame frame{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(), bus, frame);

        CHECK(!bus.valid(wk::fuel_cut));
        CHECK(!bus.valid(wk::ign_cut));
        CHECK(dtc.code_severity(P_CLT_WARNING) == 1);
        CHECK(dtc.code_severity(P_OVERTEMP_CLT) == 0);  // not over the cut threshold
        CHECK(bus.get_u32(SIG_PROT_STATUS) & 0x02u);    // any-DTC bit
    }

    // -------------------------------------------------------------------
    SECTION("overtemp heals immediately when the condition clears (no heal timer)");
    {
        auto cfg = make_cfg();
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);
        SignalBus bus{};

        set_all_fresh(bus, 110.0f);
        EngineFrame f1{}; bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut); ep.update(make_pos(), bus, f1);
        CHECK(dtc.code_severity(P_OVERTEMP_CLT) == 3);

        set_all_fresh(bus, 80.0f);                      // condition clears
        EngineFrame f2{}; bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut); ep.update(make_pos(), bus, f2);
        CHECK(dtc.code_severity(P_OVERTEMP_CLT) == 0);  // healed at once
        CHECK(dtc.stored_count() >= 1);                 // still STORED as history
        CHECK(!bus.valid(wk::fuel_cut));
    }

    // -------------------------------------------------------------------
    SECTION("sensor timeout — P0117/P0107 raised (level 2), no hard cut at 2000 rpm");
    {
        auto cfg = make_cfg(500);
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);

        SignalBus bus{}; set_all_fresh(bus);
        g_tick_ms += 600u;                              // age past the 500ms timeout
        EngineFrame frame{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(), bus, frame);

        CHECK(dtc.code_severity(P_CLT_TIMEOUT) == 2);
        CHECK(dtc.code_severity(P_MAP_TIMEOUT) == 2);
        CHECK(!bus.valid(wk::fuel_cut));                          // sev 2 -> Level 2 (rev limit), no direct cut
        CHECK(!bus.valid(wk::ign_cut));
        CHECK(bus.get(wk::prot_rev_limit) == 4500.0f);      // Level 2 profile applied
        g_tick_ms = 1000u;
    }

    // -------------------------------------------------------------------
    SECTION("phase lost — P0341 at Level 1 while the count moves, healed after the hold, no cut");
    {
        auto cfg = make_cfg();
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);
        SignalBus bus{}; set_all_fresh(bus);

        EnginePosition pos = make_pos(3000.0f, SyncLevel::PHASE);
        EngineFrame f1{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(pos, bus, f1);
        CHECK(dtc.code_severity(P_PHASE_LOST) == 0);

        pos.phase_lost_total = 1;                        // the decoder dropped phase once
        EngineFrame f2{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(pos, bus, f2);
        CHECK(dtc.code_severity(P_PHASE_LOST) == 1);
        CHECK(!bus.valid(wk::ign_cut));                  // reporting only: nothing is cut

        g_tick_ms += 1500u;                              // past the hold, count unchanged
        set_all_fresh(bus);
        EngineFrame f3{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(pos, bus, f3);
        CHECK(dtc.code_severity(P_PHASE_LOST) == 0);
        g_tick_ms = 1000u;
    }

    // -------------------------------------------------------------------
    SECTION("an engine STOPPING (sync lost below the Cranking Threshold) raises no P0335");
    {
        g_config.engine.cranking_rpm = 400;
        auto cfg = make_cfg();
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);
        SignalBus bus{}; set_all_fresh(bus);
        EngineFrame f1{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(150.0f, SyncLevel::CRANK), bus, f1);    // winding down, still synced
        EngineFrame f2{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.on_engine_stop();                                       // same frame as the loss, as on target
        ep.update(make_pos(0.0f, SyncLevel::NONE), bus, f2);
        CHECK(dtc.code_severity(P_SYNC_LOSS) == 0);
    }

    SECTION("sync lost WHILE RUNNING raises P0335 even though the engine is declared stopped too");
    {
        g_config.engine.cranking_rpm = 400;
        auto cfg = make_cfg();
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);
        SignalBus bus{}; set_all_fresh(bus);
        EngineFrame f1{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(3000.0f, SyncLevel::PHASE), bus, f1);
        EngineFrame f2{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.on_engine_stop();
        ep.update(make_pos(0.0f, SyncLevel::NONE), bus, f2);
        CHECK(dtc.code_severity(P_SYNC_LOSS) == 3);
        EngineFrame f3{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(2500.0f, SyncLevel::CRANK), bus, f3);  // re-synced: heals
        CHECK(dtc.code_severity(P_SYNC_LOSS) == 0);
    }

    // -------------------------------------------------------------------
    SECTION("sync loss — hard fuel+ign cut + P0335; returns -> heals (NO latch)");
    {
        auto cfg = make_cfg();
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);
        SignalBus bus{}; set_all_fresh(bus);

        EngineFrame f1{};                                // achieve sync first
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(2000.0f, SyncLevel::CRANK), bus, f1);
        CHECK(!bus.valid(wk::fuel_cut));

        EngineFrame f2{};                                // sync lost
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(0.0f, SyncLevel::NONE), bus, f2);
        CHECK(bus.valid(wk::fuel_cut));
        CHECK(bus.valid(wk::ign_cut));
        CHECK(dtc.code_severity(P_SYNC_LOSS) == 3);

        EngineFrame f3{};                                // sync returns -> heals (no latch now)
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(2000.0f, SyncLevel::CRANK), bus, f3);
        CHECK(!bus.valid(wk::fuel_cut));
        CHECK(dtc.code_severity(P_SYNC_LOSS) == 0);
    }

    // -------------------------------------------------------------------
    SECTION("overboost — P0234 (level 3), Level 3 reaction, no hard ign cut");
    {
        auto cfg = make_cfg();
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);

        SignalBus bus{}; set_all_fresh(bus, 80.0f, 280.0f);   // MAP 280 > cut 250
        EngineFrame frame{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(), bus, frame);

        CHECK(dtc.code_severity(P_OVERBOOST) == 3);
        CHECK(bus.get(wk::prot_rev_limit) == 2000.0f);
        CHECK(bus.get_bool(wk::prot_rev_cut_fuel));
        CHECK(!bus.valid(wk::ign_cut));                            // overboost ≠ sync loss: no hard ign cut
    }

    // -------------------------------------------------------------------
    SECTION("threshold monitor — fuel cut on high MAP, monitor bit set");
    {
        auto cfg = make_cfg();
        set_condition(cfg.threshold_monitors[0], gt(wk::map, 200.0f));   // "map > 200"
        cfg.threshold_monitors[0].action = 2;            // fuel_cut
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);

        SignalBus bus{}; set_all_fresh(bus, 80.0f, 220.0f);   // MAP 220 > 200
        EngineFrame frame{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(), bus, frame);
        CHECK(bus.valid(wk::fuel_cut));
        CHECK(bus.get_u32(SIG_MONITOR_FLAGS) & 0x01u);
    }

    // -------------------------------------------------------------------
    SECTION("a monitor edited LIVE is validated first: a broken condition is disarmed and raises P17C0");
    {
        // No reconfigure hook reaches this module, so it watches g_config_generation. A program that
        // names a channel this firmware does not have must neither run nor cut, and must say so.
        auto cfg = make_cfg();
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); dtc.set_active(true); ep.set_dtc(&dtc);
        SignalBus bus{}; set_all_fresh(bus, 80.0f, 220.0f);
        EngineFrame frame{};
        ep.update(make_pos(), bus, frame);                               // boots with every slot off

        set_condition(cfg.threshold_monitors[0], {expr::OP_PUSH_SIG, 0xFF, 0xFF, expr::OP_END});   // no such channel
        cfg.threshold_monitors[0].action = 2;                            // fuel cut
        ++g_config_generation;                                           // the tune was written
        bus.invalidate(wk::fuel_cut);
        ep.update(make_pos(), bus, frame);
        CHECK(!bus.valid(wk::fuel_cut));                                 // disarmed: it cannot cut
        CHECK(dtc.code_severity(0x17C0) == 1);                           // …and says so

        set_condition(cfg.threshold_monitors[0], gt(wk::map, 200.0f));   // fixed, live
        ++g_config_generation;
        bus.invalidate(wk::fuel_cut);
        ep.update(make_pos(), bus, frame);
        CHECK(bus.valid(wk::fuel_cut));                                  // MAP 220 > 200: armed and cutting
        CHECK(dtc.code_severity(0x17C0) == 0);                           // healed
    }

    SECTION("threshold monitor — a channel that has gone stale cannot trip it");
    {
        // Freshness is the BUS's job now (each writer declares a ttl; expire_stale invalidates what
        // stopped arriving), not a per-monitor age comparison — so this is the same guarantee proved
        // through the mechanism that actually enforces it. A dead sensor reads as invalid inside the
        // VM, "map > 200" is then unanswerable, and eval_bool's on_invalid=false means the engine is
        // not cut by a question nobody could answer.
        auto cfg = make_cfg(500);
        set_condition(cfg.threshold_monitors[0], gt(wk::map, 200.0f));
        cfg.threshold_monitors[0].action = 2;
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);

        SignalBus bus{};
        bus.set(SIG_MAP, 220.0f, true, 0, /*ttl_ms=*/100);   // last written a second ago
        bus.expire_stale(g_tick_ms);                         // the frame's own freshness sweep
        CHECK(!bus.valid(SIG_MAP));
        EngineFrame frame{};
        bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
        ep.update(make_pos(), bus, frame);
        CHECK(!bus.valid(wk::fuel_cut));
        CHECK(bus.get_u32(SIG_MONITOR_FLAGS) == 0u);
    }

    // -------------------------------------------------------------------
    SECTION("a loss WHILE RUNNING is not healed by the engine stopping — only by sync returning");
    {
        auto cfg = make_cfg();
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);
        SignalBus bus{}; set_all_fresh(bus);

        EngineFrame f1{}; bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut); ep.update(make_pos(2000.0f, SyncLevel::CRANK), bus, f1);
        EngineFrame f2{}; bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut); ep.update(make_pos(0.0f, SyncLevel::NONE), bus, f2);
        CHECK(dtc.code_severity(P_SYNC_LOSS) == 3);

        ep.on_engine_stop();                              // the stop that follows a real loss
        EngineFrame f3{}; bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut); ep.update(make_pos(0.0f, SyncLevel::NONE), bus, f3);
        CHECK(dtc.code_severity(P_SYNC_LOSS) == 3);       // still the fault it was: a stop does not explain it
        EngineFrame f4{}; bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut); ep.update(make_pos(300.0f, SyncLevel::CRANK), bus, f4);
        CHECK(dtc.code_severity(P_SYNC_LOSS) == 0);       // sync back -> healed
    }

    // -------------------------------------------------------------------
    SECTION("edge-guard: reset_edges re-raises a still-true condition after a table clear");
    {
        auto cfg = make_cfg();
        EngineProtection ep; ep.init(cfg);
        DtcManager dtc; dtc.init(1); ep.set_dtc(&dtc);
        SignalBus bus{}; set_all_fresh(bus, 110.0f);          // CLT overtemp, stays true

        EngineFrame f1{}; bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut); ep.update(make_pos(), bus, f1);
        CHECK(dtc.code_severity(P_OVERTEMP_CLT) == 3);        // raised on the false->true edge

        dtc.clear_all();                                      // OBD Mode 04 wipes the table
        EngineFrame f2{}; bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut); ep.update(make_pos(), bus, f2);
        CHECK(dtc.code_severity(P_OVERTEMP_CLT) == 0);        // condition unchanged -> edge-guard skips

        ep.reset_edges();                                     // what EngineTask's Mode-04 path calls
        EngineFrame f3{}; bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut); ep.update(make_pos(), bus, f3);
        CHECK(dtc.code_severity(P_OVERTEMP_CLT) == 3);        // forgets prev -> re-raises the live fault
    }

    return test_summary();
}
