#include "EngineProtection.h"
#include "../../../generated/module_dtc.h"   // ModuleDtc::MONITOR_EXPR
#include "../../../generated/ecu_config.h"   // g_config.engine.cranking_rpm

#include "../../Signal/Expr.h"   // the monitors are expressions now
#include "well_known_signals.h"   // wk:: roles — rename-safe signal bindings
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Sensors/Sensors.h"        // per-sensor health + channel_enabled()
#include "../../Platform/platform_hal.h"  // platform_get_tick_ms
#include "../../../generated/table_registry.h"   // expr_table_* — the tables an expression may read

namespace {
// Real OBD P-codes per protection check (the nibbles ARE the displayed digits, e.g.
// 0x0217 = P0217) — flashable on the LED and recognised by any scan tool. Severity is
// intrinsic to the check: 1..3 = level 1..3 (selects the protection level).
constexpr uint16_t P_CLT_TIMEOUT     = 0x0117;  constexpr uint8_t S_CLT_TIMEOUT     = 2;
constexpr uint16_t P_MAP_TIMEOUT     = 0x0107;  constexpr uint8_t S_MAP_TIMEOUT     = 2;
constexpr uint16_t P_TPS_TIMEOUT     = 0x0122;  constexpr uint8_t S_TPS_TIMEOUT     = 2;
constexpr uint16_t P_LAMBDA_TIMEOUT  = 0x0131;  constexpr uint8_t S_LAMBDA_TIMEOUT  = 1;
constexpr uint16_t P_BATTERY_TIMEOUT = 0x0560;  constexpr uint8_t S_BATTERY_TIMEOUT = 1;
constexpr uint16_t P_OVERTEMP_CLT    = 0x0217;  constexpr uint8_t S_OVERTEMP_CLT    = 3;  // P0217 (SAE: Engine Coolant Over Temp)
// No SAE J2012DA code exists for a coolant *warning* (P0216 is "Injection Timing Control",
// unrelated) nor for intake over-temp (P0098 is "IAT Sensor 2 Circuit Low") — use our
// manufacturer P1xxx space deliberately. Both are bucketed into coolant/iat categories.
constexpr uint16_t P_CLT_WARNING     = 0x1217;  constexpr uint8_t S_CLT_WARNING     = 1;  // mfr: Coolant Temp Warning
constexpr uint16_t P_OVERTEMP_IAT    = 0x1098;  constexpr uint8_t S_OVERTEMP_IAT    = 3;  // mfr: Intake Air Over-Temp
constexpr uint16_t P_OVERBOOST       = 0x0234;  constexpr uint8_t S_OVERBOOST       = 3;
// Battery low/high (P0562/P0563) is DETECTED by the battery sensor's operating-range check now —
// EngineProtection only PROTECTS, so the old battery threshold checks were removed.
constexpr uint16_t P_SYNC_LOSS       = 0x0335;  constexpr uint8_t S_SYNC_LOSS       = 3;
constexpr uint16_t P_TRIGGER_TOOTH   = 0x0336;  constexpr uint8_t S_TRIGGER_TOOTH   = 1;
constexpr uint16_t P_TRIGGER_GAP     = 0x0337;  constexpr uint8_t S_TRIGGER_GAP     = 2;
// The trigger is GONE — no teeth at all, past one crank revolution at the RPM floor. Severity 3:
// it IS a sync loss, and it must cut. Distinct from P0335 on purpose: "sync lost" and "there is no
// signal on the wire" send an operator to two completely different places, and until the decoder
// owned a clock the second one could not be said at all.
constexpr uint16_t P_TRIGGER_ABSENT  = 0x0338;  constexpr uint8_t S_TRIGGER_ABSENT  = 3;
// Edges arriving too soon to be teeth — a shield, a ground, a VR threshold. Severity 1: the edges
// are rejected before they reach the decoder, so this reports a wiring problem rather than a
// position problem. It is the difference between a noisy trigger and a dead one.
constexpr uint16_t P_TRIGGER_NOISE   = 0x0339;  constexpr uint8_t S_TRIGGER_NOISE   = 1;
// A CAM THAT IS NOT WHERE THE CRANK SAYS, or that stopped arriving. The decoder already acts on it —
// phase drops to crank sync, or for another bank's cam it is only recorded — so this REPORTS, at
// Level 1, and names the fault on a scan tool. Until now it existed only as a counter nobody was
// told to watch, and a jumped timing chain on a VVT engine could come and go without a code.
// SAE P0341: camshaft position sensor A, range/performance.
constexpr uint16_t P_PHASE_LOST      = 0x0341;  constexpr uint8_t S_PHASE_LOST      = 1;
}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

extern volatile uint32_t g_config_generation;   // bumped by every config write (CommsManager.cpp)

void EngineProtection::init(const EngineProtectionConfig& cfg) {
    cfg_        = &cfg;
    was_synced_ = false; synced_rpm_ = 0.0f; lost_running_ = false;
    validate_monitors();
}

void EngineProtection::on_config_change(const EngineProtectionConfig& cfg) {
    cfg_ = &cfg;
    validate_monitors();
}

// Validate every monitor program ONCE per config change. The bytes can arrive from an SD tune or a
// client write, so they are not trusted; a program that fails is DISARMED and its slot raises the
// config code, because a protection whose condition cannot be evaluated must not be given authority
// to cut. (Sensors.cpp does the same job for precond_expr and chooses the other default — see the
// note at the sweep.)
void EngineProtection::validate_monitors() {
    extern EcuConfig g_config;
    if (!cfg_) return;
    for (uint8_t i = 0; i < 8u; ++i) {
        const auto& c = cfg_->threshold_monitors[i].condition;
        mon_bad_[i] = !expr::is_empty(c, sizeof(c)) &&
                      expr::validate(c, sizeof(c), sizeof(g_config), /*tables=*/EXPR_TABLE_COUNT)
                          != expr::Invalid::None;
    }
}

void EngineProtection::on_engine_stop() {
    // Nothing to clear here any more. The engine is declared stopped in the SAME frame sync drops
    // (rpm reads 0 without sync), so clearing the sync gate here erased every sync loss before
    // update() could see it — P0335 could never be raised. Whether a loss was a stop or a fault is
    // now decided from the speed it happened at (see update()).
}

void EngineProtection::reset_edges() {
    for (auto& p : prev_cond_) p = false;   // force a fresh raise/heal next frame
}

// ---------------------------------------------------------------------------
// Detection, cheap in steady state but NOT edge-only.
//
// A code is raised with an expiry (dtc_ttl()) and stays ACTIVE only while it keeps being raised — that
// is how a fault the module stops reporting clears itself. This used to raise on the false→true edge
// ONLY, so a condition that simply stayed true (boost held over the limit, coolant over the cut) went
// inactive a second or so later while the engine was still over the limit; it also never reached
// CONFIRMED, which takes holding active for DTC_CONFIRM_MS; and a condition that began while the key
// was off (the table refuses runtime raises then) was never raised at all once the key came on.
//
// So a TRUE condition refreshes its code every quarter of the expiry — a few table touches a second,
// not one per frame — and a FALSE one is healed once, on its edge.
// ---------------------------------------------------------------------------
void EngineProtection::detect(uint8_t idx, uint16_t code, uint8_t severity,
                              bool condition, uint32_t now_ms) {
    if (!dtc_ || idx >= CHECK_COUNT) return;
    const bool edge = condition != prev_cond_[idx];
    prev_cond_[idx] = condition;
    if (!condition) {
        if (edge) dtc_->heal(code);
        return;
    }
    const uint32_t refresh = dtc_ttl() / 4u;
    if (edge || static_cast<uint32_t>(now_ms - last_raise_ms_[idx]) >= refresh) {
        dtc_->raise(code, DtcSource::PROTECTION, severity, now_ms, dtc_ttl());
        last_raise_ms_[idx] = now_ms;
    }
}

// ---------------------------------------------------------------------------
// Main update cycle
// ---------------------------------------------------------------------------

void EngineProtection::update(const EnginePosition& pos,
                               SignalBus&      bus,
                                     EngineFrame&    /*frame*/) {
    if (!cfg_) return;
    const uint32_t now_ms  = platform_get_tick_ms();
    const uint16_t timeout = cfg_->sensor_timeout_ms;

    // A tune edit may have changed a monitor's program: validate before anything runs it.
    if (g_config_generation != cfg_gen_seen_) { cfg_gen_seen_ = g_config_generation; validate_monitors(); }

    // OBD-clear strobe (set by the scheduler when a CAN Mode 04 lands): forget the latched
    // edges so still-true faults re-raise this frame, then consume the one-shot strobe.
    if (bus.get_bool(wk::obd_clear_cmd)) {
        reset_edges();
        bus.set_bool(wk::obd_clear_cmd, false, now_ms, 0);
    }

    // -----------------------------------------------------------------------
    // 1. Sensor age sweep. A channel only times out if a CONFIGURED sensor feeds
    //    it — an unconfigured input never publishes, so without this gate it would
    //    phantom-fault. No Sensors module injected → gate open (legacy behaviour).
    // -----------------------------------------------------------------------
    auto stale = [&](SignalId s) -> bool {
        const bool fed = !sensors_ || sensors_->channel_enabled(s);
        return fed && bus.age_ms(s, now_ms) > timeout;
    };

    detect(0, P_CLT_TIMEOUT,     S_CLT_TIMEOUT,     stale(wk::clt),      now_ms);
    detect(1, P_MAP_TIMEOUT,     S_MAP_TIMEOUT,     stale(wk::map),    now_ms);
    detect(2, P_TPS_TIMEOUT,     S_TPS_TIMEOUT,     stale(wk::tps),    now_ms);
    detect(3, P_LAMBDA_TIMEOUT,  S_LAMBDA_TIMEOUT,  stale(wk::lambda), now_ms);
    detect(4, P_BATTERY_TIMEOUT, S_BATTERY_TIMEOUT, stale(wk::battery),  now_ms);

    // -----------------------------------------------------------------------
    // 2. Value threshold checks — only when the signal is fresh
    // -----------------------------------------------------------------------
    const bool clt_fresh  = bus.age_ms(wk::clt,     now_ms) <= timeout;
    const bool map_fresh  = bus.age_ms(wk::map,   now_ms) <= timeout;
    const bool iat_fresh  = bus.age_ms(wk::iat,     now_ms) <= timeout;

    const float clt_c   = bus.get(wk::clt,     0.0f);
    const float iat_c   = bus.get(wk::iat,     0.0f);
    const float map_kpa = bus.get(wk::map,   0.0f);

    detect(5, P_OVERTEMP_CLT, S_OVERTEMP_CLT,
           clt_fresh && (clt_c   >= static_cast<float>(cfg_->clt_cut_c)),   now_ms);
    detect(6, P_CLT_WARNING,  S_CLT_WARNING,
           clt_fresh && (clt_c   >= static_cast<float>(cfg_->clt_warn_c)),  now_ms);
    detect(7, P_OVERTEMP_IAT, S_OVERTEMP_IAT,
           iat_fresh && (iat_c   >= static_cast<float>(cfg_->iat_cut_c)),   now_ms);
    detect(8, P_OVERBOOST,    S_OVERBOOST,
           map_fresh && (map_kpa >= static_cast<float>(cfg_->map_cut_kpa)), now_ms);

    // -----------------------------------------------------------------------
    // 3. Sync loss — only after first sync (prevents startup crank-search noise)
    // -----------------------------------------------------------------------
    // A LOSS AT SPEED, NOT A STOP. Every stop ends in lost sync, so "was synced, is not" alone either
    // fires on every stop or — as it did, cleared by on_engine_stop() in the same frame — never. What
    // tells them apart is the speed the engine was doing while it still had sync: at or above the
    // Cranking Threshold it was running, and losing position then is a fault; below it, it was
    // winding down or cranking. The code holds until sync returns, and heals then (no latch).
    // KEY OFF IS A STOP. The decoder takes no edges with the key off, so sync drops then at whatever
    // speed the crank is doing — on a bench still turning, which read as a loss at speed and raised
    // P0335 the moment the key came back. With the key off, nothing about sync is a fault.
    extern bool g_system_active;
    const bool currently_synced = (pos.sync_level >= SyncLevel::CRANK);
    if (!g_system_active) {
        was_synced_   = false;
        lost_running_ = false;
    } else if (currently_synced) {
        was_synced_   = true;
        synced_rpm_   = pos.rpm;
        lost_running_ = false;
    } else if (was_synced_) {
        was_synced_   = false;
        lost_running_ = synced_rpm_ >= static_cast<float>(g_config.engine.cranking_rpm);
    }
    const bool sync_lost = lost_running_;
    detect(9, P_SYNC_LOSS, S_SYNC_LOSS, sync_lost, now_ms);

    // Trigger sync-health (REPORTING, default no-cut at severity 1/2). EVERY tolerated tooth-miss is
    // recorded — a miss the decoder rode through (sync kept) must not be silently forgotten; the
    // DtcManager stores the code even after it heals. trigger_error_pct_limit is a NOISE-TOLERANCE
    // floor: 0 (default) records ANY single miss; >0 ignores up to that per-rev error % before
    // recording. Keyed by the decoder's last error kind so the table separates noise from a bad wheel.
    const bool trig_err = (cfg_->trigger_error_pct_limit == 0)
                              ? (pos.errors_last_cycle > 0)
                              : (pos.trigger_error_pct > cfg_->trigger_error_pct_limit * 0.1f);
    // P0336 covers a tooth that came at the wrong TIME and one that never came at all — both are
    // "the tooth count is not what the wheel says". It used to be gated on TOOTH_WINDOW alone, a
    // kind only a SEQUENCE stream could ever produce, so on every missing-tooth wheel there is
    // (60-2, 36-1) this branch was unreachable and the code was dead.
    detect(10, P_TRIGGER_TOOTH, S_TRIGGER_TOOTH,
           trig_err && (pos.last_error_kind == static_cast<uint8_t>(TriggerErrorKind::TOOTH_WINDOW) ||
                        pos.last_error_kind == static_cast<uint8_t>(TriggerErrorKind::MISSED_TOOTH)),
           now_ms);
    detect(11, P_TRIGGER_GAP, S_TRIGGER_GAP,
           trig_err && pos.last_error_kind == static_cast<uint8_t>(TriggerErrorKind::GAP_MISMATCH),
           now_ms);

    // ---- Absence. Deliberately NOT gated on trig_err ------------------------
    // trig_err comes from errors_last_cycle / trigger_error_pct, both snapshotted at a cycle
    // boundary counted in TEETH. When the teeth stop, those freeze — so every fault downstream of
    // them can only be reported about an engine that is still turning, which excludes the one fault
    // an operator most needs to see. This reads the decoder's live absence flag instead.
    detect(12, P_TRIGGER_ABSENT, S_TRIGGER_ABSENT, pos.trigger_absent, now_ms);

    // ---- Noise. A free-running counter, held briefly so it reports as a fault, not a storm ----
    if (pos.noise_edges_total != prev_noise_total_) {
        prev_noise_total_    = pos.noise_edges_total;
        noise_hold_until_ms_ = now_ms + NOISE_HOLD_MS;
    }
    detect(13, P_TRIGGER_NOISE, S_TRIGGER_NOISE,
           noise_hold_until_ms_ != 0 &&
           static_cast<int32_t>(noise_hold_until_ms_ - now_ms) > 0,
           now_ms);

    // ---- Phase lost. Same shape as noise: a free-running count, held so it reads as a fault ----
    if (pos.phase_lost_total != prev_phase_lost_total_) {
        prev_phase_lost_total_    = pos.phase_lost_total;
        phase_lost_hold_until_ms_ = now_ms + NOISE_HOLD_MS;
    }
    detect(14, P_PHASE_LOST, S_PHASE_LOST,
           phase_lost_hold_until_ms_ != 0 &&
           static_cast<int32_t>(phase_lost_hold_until_ms_ - now_ms) > 0,
           now_ms);

    // -----------------------------------------------------------------------
    // 4. Reactor — the 3-tier reaction policy. The worst ACTIVE DTC severity (1/2/3)
    //    selects the matching protection level; its limp profile is written to the
    //    EngineFrame and applied by the control modules (rev limit, enrichment,
    //    retard, boost, throttle). Sync-loss is a HARD safety on top: no sync means
    //    spark can't be timed, so it always cuts ignition regardless of the levels.
    // -----------------------------------------------------------------------
    // worst_severity() is a 64-slot scan — compute it ONCE here and reuse for both the
    // reaction level and prot_status. Sync-loss uses the live local condition (sync_lost),
    // not a table lookup, so it needs no extra scan.
    const uint8_t worst_now = dtc_ ? dtc_->worst_severity() : 0;
    // THE LEVEL HOLDS AFTER THE FAULT GOES, for as long as the tune says. Worked out per level rather
    // than on the worst severity alone, because the levels are configured separately and a level whose
    // fault has cleared may still be holding while a lower one is live.
    //
    // `dtc_condition` picks what counts as present at all: CURRENT (0) is "the fault is being reported
    // now", STORED (1) is "it has been reported since the codes were last cleared", which keeps the
    // engine in limp for a fault whose damage healing does not undo.
    // WHAT RELEASES A LEVEL THAT DOES NOT SELF-RELEASE: clearing the codes, and a key cycle. Both, and
    // for different reasons — the first is the operator saying they have dealt with it, the second is
    // how every production ECU lets a limp mode go, and a fault that is still true simply re-trips on
    // the next pass, so it costs nothing to allow.
    //
    // The hold is RAM, which is why the key cycle has to be here rather than left implicit: a POWER
    // cycle clears it whatever we do, so without this, turning the key off would do nothing while
    // pulling the plug would reset it — a distinction with no explanation behind it. On most
    // installations the ECU is powered through the key and the two are the same event anyway; this
    // makes the bench behave like the car.
    //
    // What is NOT released by either is a level watching STORED faults: that evidence is on the card,
    // so it trips again at boot and holds until somebody clears the codes.
    if (dtc_) {
        const uint32_t cg = dtc_->clear_generation();
        extern bool g_system_active;   // key-on, owned by Sensors (battery over threshold)
        const bool key = g_system_active;
        if (cg != clear_gen_seen_ || (!key && key_was_on_))
            for (uint8_t k = 0; k < 3; ++k) { level_held_[k] = false; level_since_ms_[k] = 0; }
        clear_gen_seen_ = cg;
        key_was_on_     = key;
    }
    uint8_t worst = 0;
    for (uint8_t lv = 1; lv <= 3; ++lv) {
        const ProtectionLevelsConfig& L = cfg_->protection_levels[lv - 1];
        // A LEVEL IS SELECTED, NOT STACKED. The worst active fault picks the ONE level that matches its
        // severity — three separate reactions, not a ladder where a severity-3 fault also drags levels 1
        // and 2 in. (Written as >= first, which held every level below the fault as well and left the
        // engine on level 2's reaction after level 3 let go.)
        const uint8_t sev_now = (L.dtc_condition == 1 && dtc_) ? dtc_->worst_stored_severity() : worst_now;
        const bool present = dtc_ && (sev_now == lv);
        if (present) {
            level_since_ms_[lv - 1] = now_ms ? now_ms : 1u;
            level_held_[lv - 1]     = true;
        } else if (level_held_[lv - 1]) {
            // Released only after auto_reset_s of quiet. 0 means it does not self-release at all: the
            // level stands until the codes are cleared or the key is cycled, which is the conservative
            // choice and the schema's default.
            const uint32_t hold_ms = static_cast<uint32_t>(L.auto_reset_s) * 1000u;
            if (L.auto_reset_s && (now_ms - level_since_ms_[lv - 1]) >= hold_ms)
                level_held_[lv - 1] = false;
        }
        if (level_held_[lv - 1] && lv > worst) worst = lv;
    }
    // Reaction outputs go on the BUS (not the per-frame EngineFrame) so EngineProtection / RevLimiter / Boost
    // can run at independent cadences. Cuts are validity-OR (publish true only while asked). rev-limit +
    // boost-corr are published only WHILE a level asks (they expire otherwise → RevLimiter/Boost see "none").
    // enrich/retard are locals, published every frame below so they return to neutral when no level is active.
    float prot_enrich = 0.0f, prot_retard = 0.0f;
    if (dtc_) {
        // Sync loss: spark can't be timed -> hard-cut fuel AND ignition, above the levels.
        if (sync_lost) {
            bus.set_bool(wk::fuel_cut, true, now_ms, ttl());
            bus.set_bool(wk::ign_cut,  true, now_ms, ttl());
        }
        const uint8_t sev = worst;   // 0 = none, 1..3
        if (sev >= 1 && sev <= 3) {
            const ProtectionLevelsConfig& L = cfg_->protection_levels[sev - 1];
            if (L.enabled) {
                if (L.rev_limit_rpm > 0) {
                    bus.set(wk::prot_rev_limit, static_cast<float>(L.rev_limit_rpm), true, now_ms, ttl());
                    bus.set_bool(wk::prot_rev_cut_fuel, (L.rev_limit_type == 1), now_ms, ttl());   // 0=ign,1=fuel
                }
                prot_enrich = static_cast<float>(L.enrich_pct);
                prot_retard = static_cast<float>(L.ign_retard_deg);
                if (L.boost_corr_pct != 0)
                    bus.set(wk::prot_boost_corr, static_cast<float>(L.boost_corr_pct), true, now_ms, ttl());
            }
        }
    }

    // -----------------------------------------------------------------------
    // 5. Threshold monitor sweep (user-configurable, any SignalBus signal)
    // -----------------------------------------------------------------------
    // Each slot is an EXPRESSION now (Signal/Expr.h), not a signal and a band: "one signal outside a
    // fixed range" is a special case of what the VM already says, and the interesting protections are
    // the ones it could not — a pressure limit that only applies above an RPM, a differential between
    // two channels, a bit of a status word. A stale or absent channel reads as invalid inside the VM,
    // so a dead sensor still cannot trip a cut.
    uint8_t mon_bits = 0;
    for (uint8_t i = 0; i < 8u; ++i) {
        const ThresholdMonitorsConfig& m = cfg_->threshold_monitors[i];
        if (m.action == 0 || mon_bad_[i]) continue;        // off, or a program that failed validation
        if (expr::is_empty(m.condition, sizeof(m.condition))) continue;   // empty = watches nothing

        extern EcuConfig g_config;
        expr::Ctx ectx;
        ectx.bus      = &bus;
        ectx.cfg      = reinterpret_cast<const uint8_t*>(&g_config);
        ectx.cfg_size = sizeof(g_config);
        ectx.now_ms   = now_ms;
        // on_invalid = FALSE: a condition that cannot be evaluated does not get to cut the engine.
        // (The sensor preconditions choose true for the opposite reason — there a broken gate would
        // switch DETECTION off, and silence is the dangerous answer. Here silence is the safe one.)
        const bool triggered = expr::eval_bool(m.condition, sizeof(m.condition), ectx,
                                               /*on_invalid=*/false);

        if (triggered) {
            mon_bits |= static_cast<uint8_t>(1u << i);
            if (m.action == 2 || m.action == 4) bus.set_bool(wk::fuel_cut, true, now_ms, ttl());
            if (m.action == 3 || m.action == 4) bus.set_bool(wk::ign_cut,  true, now_ms, ttl());
        }
    }
    bus.set_u32(wk::monitor_flags, static_cast<uint32_t>(mon_bits), true, now_ms, ttl());
    // A monitor that is switched on but whose condition failed validation is DISARMED, and says so.
    bool any_bad = false;
    for (uint8_t i = 0; i < 8u; ++i) any_bad |= (cfg_->threshold_monitors[i].action != 0 && mon_bad_[i]);
    detect(15, ModuleDtc::MONITOR_EXPR, ModuleDtc::MONITOR_EXPR_SEV, any_bad, now_ms);

    // -----------------------------------------------------------------------
    // 6. Protection status bits (telemetry): cut active / any DTC / severe (sev 3)
    // -----------------------------------------------------------------------
    bus.set(SIG_PROT_LEVEL, static_cast<float>(worst), true, now_ms, ttl());   // 0..3, held levels included
    // Flashers for output conditions: 1 Hz and 4 Hz, 50 % (a lamp that flashes the protection level).
    bus.set(SIG_BLINK_SLOW, ((now_ms / 500u) & 1u) ? 1.0f : 0.0f, true, now_ms, ttl());
    bus.set(SIG_BLINK_FAST, ((now_ms / 125u) & 1u) ? 1.0f : 0.0f, true, now_ms, ttl());
    bus.set_u32(wk::prot_status, static_cast<uint32_t>(
        ((bus.get_bool(wk::fuel_cut) || bus.get_bool(wk::ign_cut)) ? 0x01u : 0u) |
        ((worst >= 1)                      ? 0x02u : 0u) |
        ((worst >= 3)                      ? 0x04u : 0u)), true, now_ms, ttl());

    // Protection-level enrichment as a fuel correction factor (1.0 = none). FuelCalculator
    // aggregates it off the bus; published every frame so it returns to neutral when cleared.
    bus.set(wk::fuel_corr_protection, 1.0f + prot_enrich / 100.0f, true, now_ms, ttl());
    // Ignition retard reaction onto the bus too — Ignition runs on the per-cycle cadence and reads it here.
    // 0 when no level active (published every frame so it returns to neutral).
    bus.set(wk::prot_ign_retard, prot_retard, true, now_ms, ttl());
}
