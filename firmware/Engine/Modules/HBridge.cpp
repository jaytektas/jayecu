#include "HBridge.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../EngineStateMachine.h"      // EngineRunState — the bench nudge is engine-stopped only
#include "../../../generated/well_known_signals.h"   // wk::engine_state
#include "../../Comms/CommsManager.h"
#include "../../Diagnostics/Dtc.h"               // DtcSource / DTC_SEV_*
#include "../../Platform/platform_hal.h"         // platform_get_tick_ms (DTC timestamp)
#include "../../../generated/module_dtc.h"       // ModuleDtc::HBRIDGE_* codes + _SEV
#include "../../../generated/shadow_meta.h"
#include "../../../generated/signal_ids.h"       // SIG_HBRIDGE_* — the bridge publishes its own state
#include <algorithm>

namespace {
// Per-half DTC codes + their configured-but-invalid severities (schema module_dtc).
constexpr uint16_t DEMAND_CODE[2] = { ModuleDtc::HBRIDGE_A_DEMAND,     ModuleDtc::HBRIDGE_B_DEMAND };
constexpr uint16_t ENABLE_CODE[2] = { ModuleDtc::HBRIDGE_A_ENABLE,     ModuleDtc::HBRIDGE_B_ENABLE };
constexpr uint8_t  DEMAND_SEV[2]  = { ModuleDtc::HBRIDGE_A_DEMAND_SEV, ModuleDtc::HBRIDGE_B_DEMAND_SEV };
constexpr uint8_t  ENABLE_SEV[2]  = { ModuleDtc::HBRIDGE_A_ENABLE_SEV, ModuleDtc::HBRIDGE_B_ENABLE_SEV };
}  // namespace

void HBridge::init(const HBridgeConfig& cfg, IHBridge* a, IHBridge* b,
                   Comms::CommsManager* comms) noexcept {
    cfg_ = &cfg;
    bridges_[0] = a;
    bridges_[1] = b;
    comms_ = comms;
    apply_carrier();
}

void HBridge::on_config_change(const HBridgeConfig& cfg) noexcept {
    cfg_ = &cfg;
    apply_carrier();
}

void HBridge::apply_carrier() noexcept {
    applied_freq_hz_ = cfg_->pwm_freq_hz;
    for (int i = 0; i < 2; ++i) {
        if (bridges_[i]) {
            bridges_[i]->set_freq(cfg_->pwm_freq_hz);
            bridges_[i]->enable(false);
        }
        was_enabled_[i] = false;
    }
}

// Assert the enable state every frame, in both directions.
//
// This used to enable only on the rising edge of its own bookkeeping, which meant the DIS pin was driven
// once and then never again while the bridge stayed enabled. Anything that changed DIS behind the
// module's back — the `hbridge` bench command, or the driver itself latching a fault — left the pin
// disabled while this module went on happily computing duty, with no path back: the flag still said
// enabled, so the rising edge never came again. The bridge stayed dead until a reset, which is the only
// thing that re-ran the enable. Observed on the bench as an ETB that ignored 45 % duty with the PWM
// verifiably toggling on the pin and DIS reading 1.
//
// Writing a GPIO every frame costs nothing, and enable() is idempotent (the PWM engine's start() already
// is), so the state the hardware is in now always matches the state this module believes in.
void HBridge::apply_bridge(int i, bool en, float signed_cmd) noexcept {
    // SAY WHAT WAS DONE, from the one place that does it. Publishing at the choke point rather than at
    // each caller means the channels cannot drift from the pins: every path that turns a bridge off or
    // drives it — bus demand, bench nudge, disabled half, engine stop — passes through here.
    last_en_[i]  = en;
    last_cmd_[i] = en ? signed_cmd : 0.0f;

    IHBridge* br = bridges_[i];
    if (!br) return;
    if (!en) {
        br->enable(false);
        was_enabled_[i] = false;
        return;
    }
    br->enable(true);
    was_enabled_[i] = true;
    br->drive(signed_cmd);
}

// Demand (the producer's units) -> signed bridge command. Shared by the bus path and the bench nudge,
// which is the point: the nudge is tested through the SAME dc_map / dc_max_pct / dir_invert a real
// producer meets, so a bridge that only moves one way under the bench buttons is telling you the map
// is wrong rather than hiding it behind a special case.
//
// dc_map: 0 = unipolar (a signed -100..+100 demand passes straight through, which is what the throttle
// publishes); 1 = bipolar (0..100 with 50 = stop).
static float map_demand(const HalfConfig& bc, float demand) noexcept {
    demand = std::clamp(demand, -100.0f, 100.0f);
    const float maxp = static_cast<float>(bc.dc_max_pct) * 0.1f;
    float cmd = (bc.dc_map != 0) ? (demand / 50.0f - 1.0f) * maxp
                                 : (demand / 100.0f)        * maxp;
    cmd = std::clamp(cmd, -maxp, maxp);
    return bc.dir_invert ? -cmd : cmd;
}

// The composed instance, for the `hbridge` CLI nudge (set in SystemComposer::compose).
HBridge* g_h_bridge = nullptr;

void hbridge_bench_nudge(uint8_t half, float demand, uint32_t hold_ms) noexcept {
    if (g_h_bridge) g_h_bridge->set_manual(half, demand, hold_ms);
}

void HBridge::set_manual(uint8_t half, float demand, uint32_t hold_ms) noexcept {
    if (half >= 2) return;
    // A zero hold is the Off button: cancel outright rather than computing a deadline. "Expires this
    // instant" is not the same statement, and at tick 0 it is not even the same behaviour — the sentinel
    // below would turn it into a nudge that never expires.
    if (hold_ms == 0) { manual_until_ms_[half] = 0; return; }
    manual_demand_[half] = std::clamp(demand, -100.0f, 100.0f);
    const uint32_t until = platform_get_tick_ms() + hold_ms;
    manual_until_ms_[half] = (until == 0) ? 1u : until;   // 0 is the "no nudge" value, never a deadline
}

void HBridge::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    if (!cfg_) return;
    if (comms_) {
        constexpr uint32_t bit = (1u << JAYECU_SHADOW_HBRIDGE);
        if (comms_->shadow_pending_mask() & bit) {
            comms_->clear_shadow(bit);
            apply_carrier();
        }
    }

    // Re-apply the carrier whenever the configured value stops matching what was last programmed. The
    // shadow bit is set by one comms path only, so a pwm_freq_hz arriving any other way — a raw config
    // write, a tune load — changed the number in the tune and never touched the timer. The two then
    // disagree silently: the studio shows 3 kHz, the hardware runs 20 kHz, and nothing says so. Cheap to
    // check, and it makes the config the single source of truth rather than one of two.
    if (cfg_->pwm_freq_hz != applied_freq_hz_) apply_carrier();

    const uint32_t now_ms = platform_get_tick_ms();
    // Heal `code`, or raise it from the right SOURCE so the key-on gate treats it correctly:
    //   unset (config gap)  -> DtcSource::CONFIG  @ level 1 -> shown even engine-off, so a mis-wired tune
    //                          flags immediately while configuring (DtcManager passes CONFIG when inactive).
    //   configured-invalid  -> DtcSource::MODULE  @ sev   -> key-on gated, so a signal that's legitimately
    //                          absent at rest doesn't flood the table.
    auto report = [&](uint16_t code, bool unset, bool invalid, uint8_t inval_sev) {
        if (!dtc_) return;
        if (unset)        dtc_->raise(code, DtcSource::CONFIG, DTC_SEV_LEVEL1, now_ms, dtc_ttl());
        else if (invalid) dtc_->raise(code, DtcSource::MODULE, inval_sev, now_ms, dtc_ttl());
        else              dtc_->heal(code);
    };

    const auto engine_state = static_cast<EngineRunState>(
        static_cast<int>(bus.get(wk::engine_state, 0.0f)));
    const bool engine_stopped = (engine_state == EngineRunState::STOPPED);

    for (int i = 0; i < 2; ++i) {
        const HalfConfig& bc = cfg_->half[i];

        // A disabled half is not a fault: drop the bridge and clear any stale codes.
        //
        // THE CODES ARE CLEARED ONCE, on the edge. Healing them every frame is two linear scans of the
        // DTC table per half, a thousand times a second, to retire codes that were retired the first
        // time — the same waste the sensor layer was carrying, and it showed up the same way: 15 us of
        // every 1 kHz frame on an ECU with no bridge attached to it. The enable assertion below is NOT
        // edge-triggered and deliberately so (see apply_bridge: the bench command and a latching driver
        // both change DIS behind this module's back, and a GPIO write costs nothing).
        if (!bc.enabled) {
            if (!off_reported_[i]) {
                off_reported_[i] = true;
                report(DEMAND_CODE[i], false, false, 0);
                report(ENABLE_CODE[i], false, false, 0);
            }
            manual_until_ms_[i] = 0;
            apply_bridge(i, false, 0.0f);
            continue;
        }
        off_reported_[i] = false;   // it is on again: the next switch-off has codes to retire

        // A BENCH NUDGE OUTRANKS THE BUS, and only while the engine is stopped: it exists to answer
        // "does this thing move at all", which is a question you ask before any producer is trustworthy
        // — so it deliberately needs neither a demand signal nor an asserted enable. It expires on its
        // own (the console cannot be relied on to send an Off), and a start drops it immediately.
        const bool nudging = manual_until_ms_[i] != 0
                          && static_cast<int32_t>(now_ms - manual_until_ms_[i]) < 0;
        if (!nudging) manual_until_ms_[i] = 0;
        if (nudging) {
            if (!engine_stopped) { manual_until_ms_[i] = 0; apply_bridge(i, false, 0.0f); continue; }
            report(DEMAND_CODE[i], false, false, 0);
            report(ENABLE_CODE[i], false, false, 0);
            apply_bridge(i, true, map_demand(bc, manual_demand_[i]));
            continue;
        }

        // --- Demand: unconfigured (255) -> config level 1; configured-but-invalid -> per-code sev. Either way, off.
        const bool demand_unset = (bc.demand_sig < 0);
        const SignalId dsig = static_cast<SignalId>(bc.demand_sig);
        const bool demand_invalid = !demand_unset && !bus.valid(dsig);
        const bool demand_bad = demand_unset || demand_invalid;
        report(DEMAND_CODE[i], demand_unset, demand_invalid, DEMAND_SEV[i]);

        // --- Enable: fail-safe — the bridge drives ONLY while an enable signal is configured, valid and
        //     asserted. Unconfigured (255) -> config level 1 + off; configured-but-invalid -> per-code sev + off;
        //     valid but de-asserted -> clean commanded-off (NO fault).
        const bool enable_unset = (bc.enable_sig < 0);
        const SignalId esig = static_cast<SignalId>(bc.enable_sig);
        const bool enable_invalid = !enable_unset && !bus.valid(esig);
        const bool enable_fault = enable_unset || enable_invalid;
        report(ENABLE_CODE[i], enable_unset, enable_invalid, ENABLE_SEV[i]);

        const bool enabled = !enable_fault && bus.get(esig, 0.0f) >= 0.5f;
        if (demand_bad || !enabled) { apply_bridge(i, false, 0.0f); continue; }

        apply_bridge(i, true, map_demand(bc, bus.get(dsig, 0.0f)));
    }

    // The bridge's own state, post-mapping: what the pins are doing, not what a producer asked for.
    for (int i = 0; i < 2; ++i) {
        bus.set(i == 0 ? SIG_HBRIDGE_DUTY_1 : SIG_HBRIDGE_DUTY_2, last_cmd_[i], true, now_ms, ttl());
        bus.set(i == 0 ? SIG_HBRIDGE_EN_1   : SIG_HBRIDGE_EN_2,
                last_en_[i] ? 1.0f : 0.0f, true, now_ms, ttl());
    }
}

void HBridge::on_engine_stop() {
    for (int i = 0; i < 2; ++i)
        apply_bridge(i, false, 0.0f);
}
