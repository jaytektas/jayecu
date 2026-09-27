#include "Sensors.h"
#include "../Signal/SignalRates.h"
#include "../Platform/platform_hal.h"
#include "../Scheduler/EngineSyncSampler.h"   // engine-sync angle-window pin registration
#include "../Signal/Expr.h"                   // op-window precondition expression VM
#include "../Diagnostics/ReconfigCost.h"
#include "../Storage/Crc32.h"                 // crc32_buf — each element's fingerprint (recompute_live)
#include "../../generated/well_known_signals.h"  // wk::battery — bootstrap-sensor role (rename-safe)
#include <cstddef>                            // offsetof — guard the inline precond array view

#include "../Signal/EnginePosition.h"   // full type for the update() signature (EngineModule param)
#include "../Can/GenericCan.h"   // CAN-interface sensors read a generic-CAN receive field
#include "../../generated/table_registry.h"   // expr_table_* — the tables an expression may read

// The Input pool.  VALUE PRODUCTION + CONDITIONING + PUBLISH is the per-input pipeline
// (pipe::build_input: Acquire -> Decode -> Condition* -> Publish, per the design doc).  This
// manager applies the cross-input policy: rate decimation, pin arbitration, and DTC POLICY —
// mapping the diagnostic slots the pipeline's Conditions reported (Ctx.diag_tripped) to
// catalog P-codes + severity in the unified DtcManager.  See
// docs/polymorphic-pipeline-architecture.md.

namespace {

// Source sentinel — an unassigned board resource.
constexpr uint8_t SOURCE_NONE = 255;

// A sensor raises under its own catalog index, so the subsystem ids must sit above every one of them.
static_assert(SENSOR_COUNT <= DtcSource::SUBSYS_BASE, "sensor DTC sources collide with the subsystem ids");

// The 5 V sensor references' DTCs (power-good low), one per follower, in board_read_power_good order.
constexpr uint16_t P_SENSOR_SUPPLY_1 = 0x0641;   // SAE: sensor reference voltage "A"
constexpr uint16_t P_SENSOR_SUPPLY_2 = 0x0651;   // SAE: sensor reference voltage "B"
// How long a power-good line must stay low before its code is raised. The sensors stop trusting their
// readings on the FIRST low sample (Sensors::update); this only decides when it becomes a stored fault,
// so a reference dipping for a few milliseconds as the key comes up is not written down as one.
constexpr uint32_t SUPPLY_DTC_DELAY_MS = 100;

// The raw hardware SignalId a sensor's PRIMARY interface consumes off the bus (SIG_NONE when it owns
// no external pin — on-board / CAN). This is the arbitration KEY: two enabled sensors consuming the
// same channel is the "one input, one consumer" conflict. Analog and engine-sync share the analog
// channel for their pin (engine-sync reads the windowed value, but still owns the physical pin).
SignalId primary_hw_sig(uint8_t iface, uint8_t source) {
    if (source == SOURCE_NONE) return SIG_NONE;
    if (iface & (IFACE_ANALOG_VOLTAGE | IFACE_ENGINE_SYNC_VOLTAGE))
        return (source < HW_POOL_COUNT) ? HW_POOL_SIG[source] : SIG_NONE;
    if (iface & IFACE_DIGITAL_FREQ) return pipe::dig_hw_sig(HW_DIG_FREQ_SIG,  source);
    if (iface & IFACE_PULSE_WIDTH)  return pipe::dig_hw_sig(HW_DIG_PULSE_SIG, source);
    if (iface & IFACE_SENT)         return pipe::dig_hw_sig(HW_DIG_SENT_SIG,  source);
    if (iface & IFACE_DIGITAL)       return pipe::dig_hw_sig(HW_DIG_LEVEL_SIG, source);
    return SIG_NONE;   // IFACE_ON_BOARD / IFACE_CAN_DEVICE own no external pin
}

} // namespace

// Give b the same phase as a, so the two run on the same tick. Looked up by the channel each sensor
// PUBLISHES (its primary channel), which is what a module's *_src selector names.
void Sensors::align_phase(SignalId a, SignalId b)
{
    if (a == SIG_NONE || b == SIG_NONE || a == b) return;
    int ia = -1, ib = -1;
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        if (SENSOR_CATALOG[i].primary_channel == a) ia = i;
        if (SENSOR_CATALOG[i].primary_channel == b) ib = i;
    }
    if (ia >= 0 && ib >= 0) phase_[ib] = phase_[ia];
}

void Sensors::init(const SensorsConfig& cfg) {
    cfg_ = &cfg;
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        fault_[i] = false;
        sev_[i]   = 0;
        dtc_[i]   = 0;
        last_slot_[i] = 0; ran_once_[i] = false;   // first frame: every sensor is due
        if (SENSOR_CATALOG[i].primary_channel == wk::battery) boot_idx_ = i;   // runs FIRST, sources key_on
        // Default phase = the sensor's own index, which spreads a large pool across each period
        // instead of bunching every same-rate input onto one engine frame. align_phase() overrides it
        // for pairs that are cross-checked and must coincide.
        phase_[i] = static_cast<uint16_t>(i);
        last_run_[i] = 0;
    }
    recompute_live(/*all=*/true);
}

// One input's pipeline and its secondary outputs, from the live config. An unbuilt pipeline
// (disabled, unassigned source, unbound CAN) has n==0 and the update loop skips it.
void Sensors::rebuild_one(uint8_t i) {
    const SensorDescriptor& d = SENSOR_CATALOG[i];
    const SensorConfig&     c = cfg_->sensor[i];
    // Secondary outputs (e.g. flex fuel temp): each declared sensor_type output is its own full
    // pipeline, sharing the parent sensor's source pin. Built from the catalog spec; the parent's
    // element is what they read, so they are rebuilt exactly when it is.
    for (uint8_t a = 0; a < SENSOR_AUX_COUNT; a++) {
        const SensorAuxOutput& ao = SENSOR_AUX_OUTPUTS[a];
        if (ao.sensor_index != i) continue;
        if (!pipe::build_aux_output(ao, d, c, aux_pools_[a], aux_pipelines_[a]))
            aux_pipelines_[a].n = 0;
    }
    // A DISABLED sensor gets no pipeline. build_input() does not check `enabled` — it composes
    // anything with a configured type — so every switched-off sensor in the catalogue was built in
    // full and then skipped by the update loop below. On a bench with three sensors in use that is
    // 118 pipelines composed for nothing. Torn down rather than merely skipped, so a sensor that has
    // just been switched off stops publishing instead of running on its last-built pipeline.
    if (!c.enabled) { pipelines_[i].n = 0; return; }
    if (pipe::build_input(d, c, pools_[i], pipelines_[i])) return;
    // build_input declined → CAN-interface (or unassigned). A CAN sensor reads the generic-CAN receive
    // FIELD it names by FRAME and START BIT — one decode, shared with the transmit side and editable
    // in the tune. The field supplies the reading and its own TTL; everything the sensor adds on top
    // (calibration, operating window, per-sensor DTCs, the enable) is what makes this a sensor rather
    // than a channel the frame could have written itself.
    //
    // RESOLVED HERE, on every rebuild, rather than stored. The pool index is not durable — the studio
    // repacks the whole field pool on any structural edit — so the tune holds the frame's id and the
    // field's start bit, and the index is derived from them each time the sensor is built.
    pipelines_[i].n = 0;
    if (!gcan_ || pipe::active_iface(d, c) != IFACE_CAN_DEVICE) return;
    const int32_t resolved = gcan_->find_field(c.can_frame, c.can_bit);
    if (resolved < 0) return;               // no field named, or the frame it named is gone
    const uint16_t fi = static_cast<uint16_t>(resolved);
    const GenericCanFieldValue* fv = gcan_->value(fi);
    if (!fv) return;
    // The field's own Valid For is the sensor's staleness rule too, so the two cannot disagree about
    // when a sender has gone quiet.
    pipe::build_can_input(fv, gcan_->field_ttl_ms(fi), d, c, pools_[i], pipelines_[i]);
}

// The elements a write actually landed in, rebuilt — then everything recompute_live() derives from the
// whole array re-derived once. Split out rather than parameterised so the full sweep keeps reading as
// the simple thing it is, and so the CRC (which exists to catch a write that changed nothing) still
// guards each element it touches.
// AN INPUT THAT HAS JUST BEEN SWITCHED OFF takes its codes with it. Called once, by the rebuild that
// turned it off — not by every frame for the rest of time, which is what the per-frame walk used to do
// for every disabled input on the board. The ttl on the DTC table is the backstop if a path is missed.
void Sensors::retire_codes(uint8_t i) {
    if (i >= SENSOR_COUNT) return;
    fault_[i] = false; sev_[i] = 0; dtc_[i] = 0;
    if (!dtc_mgr_) return;
    const SensorDescriptor& d = SENSOR_CATALOG[i];
    dtc_mgr_->heal(d.dtc_raw_min); dtc_mgr_->heal(d.dtc_raw_max);
    dtc_mgr_->heal(d.dtc_op_min);  dtc_mgr_->heal(d.dtc_op_max);
    dtc_mgr_->heal(d.dtc_stuck);   dtc_mgr_->heal(d.dtc_max_deriv);
    dtc_asserted_[i] = 0;
    for (uint8_t k = 0; k < d.aux_count; k++) {
        const SensorAuxOutput& ao = SENSOR_AUX_OUTPUTS[d.aux_off + k];
        dtc_mgr_->heal(ao.dtc_op_min); dtc_mgr_->heal(ao.dtc_op_max);
        aux_asserted_[d.aux_off + k] = 0;
    }
}

void Sensors::recompute_range(uint8_t first, uint8_t last) {
    if (!cfg_) return;
    const reconfig::Time _t(reconfig::SENSORS);
    bool any = false;
    for (uint8_t i = first; i <= last && i < SENSOR_COUNT; i++) {
        const SensorConfig& c = cfg_->sensor[i];
        const bool on = c.enabled != 0;
        const uint32_t crc = on ? crc32_buf(reinterpret_cast<const uint8_t*>(&c), sizeof(c)) : 0;
        if (on == built_on_[i] && (!on || crc == built_crc_[i])) continue;   // written, but unchanged
        rebuild_one(i);
        if (!on && built_on_[i]) retire_codes(i);   // it was on a moment ago: take its codes down with it
        built_on_[i] = on; built_crc_[i] = crc;
        any = true;
    }
    if (any) rederive_shared();
}

// THE GENERIC CAN FRAME POOLS AS ONE NUMBER. Read off the LIVE config rather than off a value
// GenericCan cached at its last configure(): the two run on different tasks, and a fingerprint that
// lagged by one config write would leave a CAN sensor aimed at the old field for exactly as long as
// nobody wrote the tune again.
uint32_t Sensors::can_pool_crc() const {
    if (!gcan_) return 0;
    const CanConfig* cc = gcan_->config();
    if (!cc) return 0;
    const uint32_t a = crc32_buf(reinterpret_cast<const uint8_t*>(cc->gc_frame), sizeof(cc->gc_frame));
    const uint32_t b = crc32_buf(reinterpret_cast<const uint8_t*>(cc->gc_field), sizeof(cc->gc_field));
    return a ^ ~b;
}

void Sensors::recompute_live(bool all) {
    if (!cfg_) return;
    const reconfig::Time _t(reconfig::SENSORS);   // what this cost, for `cpu` to report
    // Which inputs' own settings changed since their pipelines were built. See Sensors.h: a pipeline
    // rebuild zeroes its stage state, so only an input whose element actually changed is rebuilt, and
    // every other input keeps running undisturbed through a write to anything else.
    //
    // A CAN SENSOR HAS A SECOND HALF. Its element says which frame and which start bit; the FRAME POOL
    // says where that is, and whether it is still there at all. Moving a field or deleting its frame
    // changes not one byte of the sensor, so the per-element CRC alone would leave it running on a
    // pipeline pointed at the pool slot the field used to occupy — the silent wrong-bits failure that
    // naming the field structurally exists to prevent. Only CAN-interface sensors are rebuilt for it.
    const uint32_t can_crc  = can_pool_crc();
    const bool     can_moved = (can_crc != built_can_crc_);
    built_can_crc_ = can_crc;
    bool any = all;
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        const SensorConfig& c = cfg_->sensor[i];
        const bool on = c.enabled != 0;
        uint32_t crc = 0;
        if (on) crc = crc32_buf(reinterpret_cast<const uint8_t*>(&c), sizeof(c));
        const bool can_here = can_moved && on
                           && pipe::active_iface(SENSOR_CATALOG[i], c) == IFACE_CAN_DEVICE;
        if (!all && !can_here && on == built_on_[i] && (!on || crc == built_crc_[i])) continue;   // unchanged
        rebuild_one(i);
        if (!on && built_on_[i]) retire_codes(i);   // it was on a moment ago: take its codes down with it
        built_on_[i] = on; built_crc_[i] = crc;
        any = true;
    }
    if (!any && static_cast<uint16_t>(cfg_->engine_sync_window_deg) == built_sync_window_)
        return;                                 // nothing the rest of this derives from moved
    rederive_shared();
}

// EVERYTHING THAT IS DERIVED FROM THE WHOLE ARRAY, not from one element: the engine-sync pin list, the
// enabled-channel mask, the pin-conflict verdict. Any element changing can change all three, so both
// rebuild paths end here.
void Sensors::rederive_shared() {
    // Engine-sync angle-window sampler: register the source pins of enabled engine-sync sensors
    // (MAP is locked engine-sync) so the grid ISR crank-angle-averages their ADC. MAP-focused in
    // practice; pin-driven so it follows the config. Non-engine-sync pins fall through to a direct
    // read in engine_sync_read_mv(), so this only affects sensors the tune marks engine-sync.
    uint8_t es_pins[EngineSyncSampler::MAX_PINS]; uint8_t es_n = 0;
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        const SensorConfig& sc = cfg_->sensor[i];
        if (!sc.enabled) continue;
        if (pipe::active_iface(SENSOR_CATALOG[i], sc) != IFACE_ENGINE_SYNC_VOLTAGE) continue;
        if (es_n < EngineSyncSampler::MAX_PINS) es_pins[es_n++] = sc.source;
    }
    // The window is the module's own scalar, read here rather than passed: both rebuild paths end up
    // in this function and only one of them had it in hand.
    const uint16_t window = static_cast<uint16_t>(cfg_->engine_sync_window_deg);
    built_sync_window_ = window;
    g_engine_sync.configure(es_pins, es_n, static_cast<uint16_t>(window * 10));

    // THE INPUTS THAT ACTUALLY RUN, listed once instead of rediscovered 134 times a millisecond.
    //
    // The per-frame loop walked every catalogue slot and skipped the ones that were off — 134 iterations
    // to service the two an untuned ECU has. Measured: 64 us of every 1 kHz frame with two sensors
    // enabled and 110 us with thirty, so about 62 us of it — six percent of the chip — was the walk
    // itself, on every ECU, whatever its tune.
    //
    // The bootstrap sensor (the battery, which decides key-on) stays FIRST: the ordering is what lets
    // the pass in which the key turns also be the pass in which everything it gates publishes. A
    // board-managed input is always live, enable flag or not, so it is active by definition.
    active_n_ = 0;
    if (boot_idx_ < SENSOR_COUNT) active_[active_n_++] = boot_idx_;
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        if (i == boot_idx_) continue;
        const bool board_managed = (SENSOR_CATALOG[i].locked_interface == IFACE_ON_BOARD);
        if (cfg_->sensor[i].enabled || board_managed) active_[active_n_++] = i;
    }

    // Enabled-channel mask — an enabled sensor feeds its primary channel (and any aux outputs).
    // O(1) for channel_enabled(), which the protection sweep hits 5x/frame.
    for (uint16_t s = 0; s < SIG_COUNT; s++) ch_enabled_[s] = false;
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        if (!cfg_->sensor[i].enabled) continue;
        const uint16_t ch = SENSOR_CATALOG[i].primary_channel;
        if (ch < SIG_COUNT) ch_enabled_[ch] = true;
    }
    for (uint8_t a = 0; a < SENSOR_AUX_COUNT; a++) {
        const SensorAuxOutput& ao = SENSOR_AUX_OUTPUTS[a];
        if (cfg_->sensor[ao.sensor_index].enabled && ao.signal < SIG_COUNT)
            ch_enabled_[ao.signal] = true;
    }
    compute_conflicts();   // cross-input (one pin, one consumer), so any changed input re-runs it all
}

void Sensors::compute_conflicts() {
    // Input arbitration — inverted from the old pin-pool model to CONSUMPTION of a raw hardware signal.
    // Since HardwareInput reads each pin exactly once and publishes it, there is no physical contention
    // any more; the real invariant is that each raw SIG_HW_* channel has ONE consuming sensor. The FIRST
    // enabled sensor (lowest catalog index) to consume a channel OWNS it; a later consumer of the SAME
    // channel is REJECTED (arb_rejected_, skipped in update()) and the conflict flagged (input_conflict_
    // -> P1650). Because the key is the (pin, mode) CHANNEL, freq and pulse on one DIG pin are DIFFERENT
    // channels and legitimately coexist (flex fuel: freq=ethanol + pulse=fuel temp), which the pin-pool
    // model wrongly rejected. on-board/CAN sensors own no channel. Re-derived live (recompute_live).
    input_conflict_ = false;
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) { arb_rejected_[i] = false; cfg_invalid_[i] = false; }

    // Validate every precondition program ONCE per config change. The bytes can arrive from an SD
    // card or a partial write, so "the studio compiled it" is not evidence; exec() must never be
    // handed bytecode nothing has checked. Cost is paid on a 'w' write, not per frame.
    {
        extern EcuConfig g_config;
        for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
            const auto& pe = cfg_->sensor[i].precond_expr;
            expr_bad_[i] = !expr::is_empty(pe, sizeof(pe)) &&
                           expr::validate(pe, sizeof(pe), sizeof(g_config), /*tables=*/EXPR_TABLE_COUNT)
                               != expr::Invalid::None;
        }
    }
    if (!cfg_) return;
    // Owner (catalog index, -1 = free) per raw hardware SignalId. static: keeps ~0.5 KB off the stack —
    // compute_conflicts runs only at init / config change, never on the hot path.
    static int16_t owner[SIG_COUNT];
    for (uint16_t s = 0; s < SIG_COUNT; s++) owner[s] = -1;
    uint16_t used_cap = 0;   // DIG pins an enabled freq/pulse/SENT sensor is bound to (for capture teardown)
    auto claim = [&](SignalId sig, uint8_t i) -> bool {
        if (sig == SIG_NONE || sig >= SIG_COUNT) return true;              // no channel → nothing to arbitrate
        if (owner[sig] >= 0 && owner[sig] != i) { input_conflict_ = true; return false; }  // taken by another
        owner[sig] = static_cast<int16_t>(i);
        return true;
    };
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        const SensorDescriptor& d = SENSOR_CATALOG[i];
        const SensorConfig&     c = cfg_->sensor[i];
        if (!c.enabled) continue;
        const uint8_t iface = pipe::active_iface(d, c);
        // Interfaces that REQUIRE a physical input pin. ON_BOARD / CAN_DEVICE own no external pin, so a
        // missing `source` there isn't an error.
        const bool needs_pin = iface & (IFACE_ANALOG_VOLTAGE | IFACE_ENGINE_SYNC_VOLTAGE
                                        | IFACE_DIGITAL_FREQ | IFACE_DIGITAL | IFACE_SENT | IFACE_PULSE_WIDTH);
        // Enabled, the active interface needs a pin, but none is assigned -> a config error (the pipeline
        // builds empty and silently produces nothing). Flagged here, raised as SENSOR_CATALOG[i].dtc_config.
        if (needs_pin && static_cast<uint8_t>(c.source) == SOURCE_NONE) cfg_invalid_[i] = true;   // int8 -1 -> 255
        // AN INTERFACE THIS SENSOR IS NOT BUILT TO BE READ THROUGH. The catalogue declares what each
        // input may bind (`interfaces:`) and codegen ORs it into interface_mask — which nothing had
        // ever read, so the declaration constrained nothing on either side of the link. Set a switch
        // to Pulse Width and the pipeline is built over a DIG pin measured as a duty cycle: no error,
        // no reading, nothing to say why. Still built (a working tune does not stop working over a
        // catalogue opinion), but no longer silent. Skipped where the catalogue LOCKS the interface —
        // active_iface() ignores the selector there, so validating it would report a field that has
        // no effect.
        if (!d.locked_interface && !(iface & d.interface_mask)) cfg_invalid_[i] = true;
        // A CAN SENSOR WHOSE FIELD IS NOT THERE. Naming nothing (start bit -1) is a sensor not set up
        // yet; naming a frame or a start bit the tune no longer holds is a REFERENCE THAT BROKE — the
        // frame was deleted, or the field was moved to other bits — and the two must not look alike.
        // Both build nothing, so without this the sensor goes quiet with nothing to say why, which is
        // the whole failure that storing a frame id instead of a pool index exists to prevent: it turns
        // "silently reading somebody else's bits" into "reading nothing", and this is what then says so.
        if ((iface & IFACE_CAN_DEVICE) && c.can_bit >= 0
            && (!gcan_ || gcan_->find_field(c.can_frame, c.can_bit) < 0)) cfg_invalid_[i] = true;
        // A MULTI-POSITION SWITCH WHOSE BANDS CANNOT BE DECODED SAFELY — odd point count, bands touching
        // or out of order, or a band down at the 0 V floor. build_input() refuses to build it, so without
        // this it would simply go quiet; the config DTC says which input and that it is the calibration.
        // (A multi-position switch on a counting interface builds nothing either, and is caught above
        // only if the catalogue forbids the interface — so it is named here too.)
        if (effective_type(d, c.type) == SENSOR_TYPE_MULTI_SWITCH) {
            if (iface & (IFACE_ANALOG_VOLTAGE | IFACE_ENGINE_SYNC_VOLTAGE)) {
                if (pipe::multi_switch_bands(c) == 0) cfg_invalid_[i] = true;
            } else if (iface & (IFACE_DIGITAL_FREQ | IFACE_SENT | IFACE_PULSE_WIDTH)) {
                cfg_invalid_[i] = true;
            }
        }

        // Which DIG pins carry a live edge-capture (freq/pulse/SENT). Switch is a polled level, analog is
        // ADC — neither uses a capture EXTI, so they don't count. The teardown below disables the rest.
        if ((iface & (IFACE_DIGITAL_FREQ | IFACE_SENT | IFACE_PULSE_WIDTH))
            && static_cast<uint8_t>(c.source) < 8u)
            used_cap = static_cast<uint16_t>(used_cap | (1u << static_cast<uint8_t>(c.source)));

        bool ok = claim(primary_hw_sig(iface, c.source), i);
        // Secondary outputs consume their OWN (pin, mode) channel from the parent's pin (flex fuel: the
        // primary is freq, the aux is pulse — same sensor claims both, no self-conflict).
        for (uint8_t k = 0; k < d.aux_count; k++) {
            const SensorAuxOutput& ao = SENSOR_AUX_OUTPUTS[d.aux_off + k];
            const SignalId asig = (ao.acquire == 1) ? pipe::dig_hw_sig(HW_DIG_FREQ_SIG,  c.source)
                                                    : pipe::dig_hw_sig(HW_DIG_PULSE_SIG, c.source);
            if (!claim(asig, i)) ok = false;
        }
        if (!ok) arb_rejected_[i] = true;
    }

    // Disable-on-unassign: tear down capture on any DIG pin no longer bound to a freq/pulse/SENT sensor.
    // The acquire step re-enables assigned pins idempotently each frame, but nothing turned a dropped
    // pin's EXTI back off — a leaked interrupt kept firing its handler and stealing decode cycles until a
    // reset. Runs only here (config change), off the hot path. platform_capture_disable no-ops for a pin
    // this path never enabled, so a DIG pin used as a TRIGGER (different enable mechanism) is untouched.
    for (uint8_t p = 0; p < 8u; p++)
        if (!(used_cap & (1u << p))) platform_capture_disable(p);
}

// Give the sensors that are CROSS-CHECKED against each other the same publish phase, so they land on
// the same tick. Which pairs those are is not a property of the sensors — it is wherever a module has
// been POINTED — so it is re-derived from those modules' own selectors on every config change: the
// throttle's tps_a/tps_b (aim it at a generic aux and that aux is aligned with whatever it is compared
// against) and the pedal's fixed app_1/app_2.
//
// It lives here, in the sensors' own re-derive, because that is what runs: on_config_change() is not
// wired in this firmware and every module self-watches g_config_generation instead. Putting it in the
// composer looked tidier and would silently never have run.
void Sensors::align_crosschecked() {
    for (unsigned i = 0; i < ELECTRONIC_THROTTLE_ETB_COUNT; i++) {
        const auto& e = g_config.electronic_throttle.etb[i];
        align_phase(static_cast<SignalId>(e.tps_a_src), static_cast<SignalId>(e.tps_b_src));
    }
    align_phase(SIG_APP_1, SIG_APP_2);
}

// The two 5 V sensor references, from their power-good lines. Key off, there is nothing to judge — USB
// back-feeds the followers through a diode — so the state is forgotten, the channels go stale, and any
// code raised is already healed by the DTC table's own key-off edge. Key on, a low line makes EVERY
// pin-read sensor invalid on the same sample (supply_lost()), and becomes a stored fault only once it has
// stayed low for SUPPLY_DTC_DELAY_MS.
void Sensors::update_supplies(SignalBus& bus, uint32_t now) {
    static constexpr SignalId SIG[2]  = {SIG_SENSOR_SUPPLY_1, SIG_SENSOR_SUPPLY_2};
    static constexpr uint16_t CODE[2] = {P_SENSOR_SUPPLY_1, P_SENSOR_SUPPLY_2};
    bool lost = false;
    for (uint8_t k = 0; k < 2; k++) {
        if (!key_on_) { supply_low_since_[k] = 0; supply_dtc_raised_[k] = false; continue; }
        extern volatile int g_pg_override[2];   // 'pg' bench command: simulate a failed follower
        const bool ok = g_pg_override[k] > 0 ? true
                      : g_pg_override[k] < 0 ? false
                      : platform_read_power_good(k);
        bus.set(SIG[k], ok ? 1.0f : 0.0f, true, now, 200u);
        if (ok) {
            supply_low_since_[k] = 0;
            if (supply_dtc_raised_[k]) { if (dtc_mgr_) dtc_mgr_->heal(CODE[k]); supply_dtc_raised_[k] = false; }
            continue;
        }
        lost = true;
        if (supply_low_since_[k] == 0) supply_low_since_[k] = now ? now : 1u;   // 0 = "not low"
        // ASSERTED EVERY PASS while the rail is down, not once on the edge: a code stays CURRENT only
        // while its judge keeps saying so (DtcManager::raise, ttl). raise() is idempotent — it refreshes
        // the last-seen stamp and counts only the inactive->active edge — so this costs one table lookup
        // per FAULT, not per sensor.
        if ((now - supply_low_since_[k]) >= SUPPLY_DTC_DELAY_MS) {
            if (dtc_mgr_) dtc_mgr_->raise(CODE[k], DtcSource::SUPPLY, DTC_SEV_LEVEL2, now, dtc_ttl());
            supply_dtc_raised_[k] = true;
        }
    }
    supply_lost_ = lost;
}

void Sensors::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    if (!cfg_) return;
    // The pipelines read cfg_ LIVE (their cfg structs point into it), but the pipeline
    // composition + masks are derived. on_config_change() is not wired, so re-derive whenever
    // g_config changed (a 'w' write bumps the generation).
    extern volatile uint32_t g_config_generation;
    extern volatile uint32_t g_config_dirty_lo, g_config_dirty_hi;
    const bool cfg_changed = (g_config_generation != cfg_gen_seen_);
    if (cfg_changed) {
        cfg_gen_seen_ = g_config_generation;
        // WHICH BYTES MOVED decides whether any of this is our business. The sensors are one
        // contiguous block of g_config, so a write that misses it cannot have changed a pipeline —
        // and this used to CRC all 134 elements to discover that, on every write to anything, for
        // 1.45 ms inside the 1 kHz frame. Tuning a fuel cell paid for the sensor layer to check
        // itself over.
        //
        // The range is taken and CLEARED here: this is its only consumer, and leaving it set would
        // make the next unrelated write look like it had touched us. A write that recorded no range
        // (lo > hi — something that does not go through the comms path) falls back to the full sweep,
        // because an unset range is not evidence of anything.
        const uint32_t lo = g_config_dirty_lo, hi = g_config_dirty_hi;
        g_config_dirty_lo = 0xFFFFFFFFu; g_config_dirty_hi = 0;
        const uint32_t base = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(cfg_) -
                                                    reinterpret_cast<const uint8_t*>(&g_config));
        const bool unknown = (lo > hi);
        bool       ours    = unknown || (lo < base + sizeof(SensorsConfig) && hi > base);
        // A WRITE TO THE GENERIC CAN FRAME POOLS IS OURS TOO. A CAN sensor's pipeline is derived from
        // the frames as much as from its own element: it names its field by frame id and start bit,
        // and moving that field, deleting its frame or repacking the pool changes which bytes it must
        // read without touching a byte of the sensor. The range test was written when a sensor's
        // pipeline depended on the sensor block alone, so a frame edit landed outside it and was
        // skipped entirely — the sensor then ran on a pipeline aimed at the pool slot the field used
        // to occupy. Found on the bench: moving a field through the pool left the sensor reading
        // nothing and the fingerprint that was supposed to notice never got the chance to run.
        bool can_moved = false;
        if (!unknown && gcan_) {
            if (const CanConfig* cc = gcan_->config()) {
                const uint8_t* g0 = reinterpret_cast<const uint8_t*>(&g_config);
                const uint32_t f0 = static_cast<uint32_t>(
                    reinterpret_cast<const uint8_t*>(cc->gc_frame) - g0);
                const uint32_t d0 = static_cast<uint32_t>(
                    reinterpret_cast<const uint8_t*>(cc->gc_field) - g0);
                can_moved = (lo < f0 + sizeof(cc->gc_frame) && hi > f0)
                         || (lo < d0 + sizeof(cc->gc_field) && hi > d0);
                if (can_moved) ours = true;
            }
        }
        if (ours) {
            // …AND WHICH SENSORS. The elements are a contiguous array, so the range names them by
            // arithmetic — no per-sensor watch list to keep, no CRC to compute: first and last are a
            // subtract and a divide. A studio write is one field in one element, so this is one
            // rebuild instead of a hundred and thirty-four fingerprints.
            //
            // An unknown range still sweeps everything, and so does a write that reaches outside the
            // element array (the module's own scalars at the end of the block), because then what
            // moved is something every element derives from.
            const uint32_t arr_lo = base + static_cast<uint32_t>(
                reinterpret_cast<const uint8_t*>(&cfg_->sensor[0]) -
                reinterpret_cast<const uint8_t*>(cfg_));
            const uint32_t stride = static_cast<uint32_t>(sizeof(cfg_->sensor[0]));
            const uint32_t arr_hi = arr_lo + stride * SENSOR_COUNT;
            // A CAN-pool write names no sensor element, so the arithmetic below has nothing to work
            // from: it takes the full sweep, where the frame-pool fingerprint decides which CAN
            // sensors actually have to be rebuilt.
            if (!can_moved && !unknown && lo >= arr_lo && hi <= arr_hi) {
                const uint8_t first = static_cast<uint8_t>((lo - arr_lo) / stride);
                const uint8_t last  = static_cast<uint8_t>((hi - 1u - arr_lo) / stride);
                recompute_range(first, last);
            } else {
                recompute_live();
            }
            align_crosschecked();   // the compared pairs may have been re-pointed by this very write
        }
    }
    const uint32_t now = platform_get_tick_ms();
    // Roll the rate-claim epoch. Here because this runs unconditionally at frame rate and is the side
    // that consumes the answer; a module that stopped claiming is forgotten within two epochs.
    sigrate::service(now);

    // OBD Mode 04 wiped the table under us → forget which slots we hold raised, so still-active
    // faults re-assert on this frame's evaluation. Order-independent (no strobe race with EngineProtection).
    if (dtc_mgr_ && dtc_mgr_->clear_generation() != last_clear_gen_) {
        last_clear_gen_ = dtc_mgr_->clear_generation();
        for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
            dtc_asserted_[i] = 0; cfg_dtc_raised_[i] = false; expr_dtc_raised_[i] = false;
            noband_dtc_raised_[i] = false;
        }
        for (uint8_t a = 0; a < AUX_N; a++)         aux_asserted_[a] = 0;
        supply_dtc_raised_[0] = supply_dtc_raised_[1] = false;
    }

    // Publish the input pin-conflict status to the bus (the scheduler reads it there, not via
    // a back-reference). Derived in compute_conflicts() at recompute_live.
    bus.set_bool(wk::pin_conflict_fault, input_conflict_, now, 0);

    // Config-error DTC: an ENABLED sensor the tune has configured in a way the hardware cannot honour —
    // its interface needs a pin and none is assigned, or the interface is not one this sensor can be
    // read through at all. Raised as
    // DtcSource::CONFIG (a validity code, so it surfaces at the bench/key-off where you configure — not
    // gated by g_system_active like runtime faults). Edge-triggered, healed when a pin is assigned or the
    // sensor is disabled. Runs ahead of the per-sensor pipeline loop, which skips an unassigned sensor.
    if (dtc_mgr_) {
        for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
            const uint16_t code = SENSOR_CATALOG[i].dtc_config;
            if (cfg_invalid_[i]) {
                if (code) dtc_mgr_->raise(code, DtcSource::CONFIG, 1, now, dtc_ttl());   // level 1
                cfg_dtc_raised_[i] = true;
            } else if (cfg_dtc_raised_[i]) {
                if (code) dtc_mgr_->heal(code);
                cfg_dtc_raised_[i] = false;
            }
            // Broken precondition expression — same edge-triggered shape, its own unique code, so a
            // scan tool lands on ONE sensor rather than "something in the tune is wrong".
            const uint16_t pcode = SENSOR_CATALOG[i].dtc_precond;
            if (expr_bad_[i]) {
                if (pcode) dtc_mgr_->raise(pcode, DtcSource::CONFIG, 1, now, dtc_ttl());
                expr_dtc_raised_[i] = true;
            } else if (expr_dtc_raised_[i]) {
                if (pcode) dtc_mgr_->heal(pcode);
                expr_dtc_raised_[i] = false;
            }
        }
    }

    extern bool g_system_active;   // mirrors key_on() for everything downstream of this phase
    // THE BOOTSTRAP SENSOR RUNS FIRST, AND THE KEY IS DECIDED IMMEDIATELY AFTER IT. Iteration order is
    // remapped so index 0 of this walk is the battery; every other sensor keeps its natural order after
    // it. That ordering is the whole fix: the key is known before the sensors it gates are reached, so
    // the pass in which the key turns is also the pass in which they publish. See Sensors::key_on().
    for (uint8_t n = 0; n < active_n_; n++) {
        const uint8_t i = active_[n];
        if (n == 1) {
            // The battery has just been through its pipeline THIS pass, so the value on the bus is this
            // frame's. Decide the key from it before any gated sensor is reached.
            const float vbat = bus.get(wk::battery, 0.0f);
            key_on_ = key_on_ ? (vbat > KEY_OFF_V) : (vbat > KEY_ON_V);
            extern volatile int g_key_override;          // 'key' bench command: force on/off
            if      (g_key_override > 0) key_on_ = true;
            else if (g_key_override < 0) key_on_ = false;
            g_system_active = key_on_;
            // ON THE WIRE. Every channel below reads zero when the key is off and that is CORRECT —
            // but the studio cannot otherwise tell it from a broken sensor, so it has to be said.
            bus.set(SIG_KEY_ON, key_on_ ? 1.0f : 0.0f, true, now, 200u);
            // ...and RETRACT the raw pins the key gates. HardwareInput published every one of them a
            // phase ago and cannot judge this itself — it runs before the key is known, so gating there
            // would cost a frame on the pass the key turns, and that pass publishing everything is a
            // property this module exists to guarantee (see the ordering note above).
            //
            // Retracted rather than ignored, because the sensor pipelines are not the only reader: the
            // studio's sensor page shows the RAW channel beside the calibrated one, and a raw pin left
            // publishing counts against the wrong reference is exactly the plausible-but-wrong number
            // the gate exists to prevent. Invalid, so it reads as absent — 0 in, 0 out. The battery's
            // own pin is left alone; the key is derived from it.
            if (!key_on_) {
                #include "../../generated/hw_input_keygate.inc"
            }
            update_supplies(bus, now);   // before the sensors it gates, same reasoning as the key
        }
        const SensorDescriptor& d = SENSOR_CATALOG[i];
        const SensorConfig&     c = cfg_->sensor[i];

        // Bench/USB (key off): only the BOOTSTRAP sensor runs — the battery, which sources key_on. Its
        // 12 V divider feeds the ADC directly (USB-powered), while every other analog front-end sits
        // behind the 5 V followers.
        //
        // THE 5 V RAIL IS NOT THE SAME RAIL WITH THE KEY OFF. USB feeds those followers through a diode,
        // and the drop across it moves the reference the ADC measures against — so every reading carries
        // an offset that key-on does not have, and a CALIBRATION built or checked against it will not
        // match the running engine. This was removed once on the argument that the rail "is up and the
        // ADC reads real voltages": it is up, the voltages are real, and they are still wrong, which is
        // worse than nothing. A plausible number calibrated against the wrong reference is a number
        // somebody will trust.
        //
        // So the battery keeps publishing (key_on needs it) and the rest stays quiet until key-on. The
        // studio says WHY a value is absent — see its key-off banner — rather than leaving a dead gauge.
        const bool is_bootstrap = (i == boot_idx_);      // the sensor that sources key_on
        if (!key_on_ && !is_bootstrap) continue;

        // Board-managed inputs (locked on-board) are board features — always live, NOT gated by the
        // user enable flag.
        // A DISABLED INPUT IS NOT IN THE LIST, so there is nothing to skip here any more and nothing
        // to clear every frame: retire_codes() takes its fault state and its codes down once, in the
        // rebuild that switched it off. Board-managed inputs are in the list whatever their enable flag
        // says, which is what "always live" meant when this branch had to test for it.

        if (arb_rejected_[i]) continue;           // lost pin arbitration — don't read/publish
        if (pipelines_[i].n == 0) continue;       // no producer (CAN-device / unassigned source)

        // A 5 V reference is down (supply_lost()): every sensor read off an ECU pin publishes invalid and
        // its own diagnostics stand down — the supply's code names the cause once, instead of every sensor
        // on the rail raising a raw-low fault of its own. The battery, on-board and CAN inputs are exempt.
        const uint8_t  iface_now = pipe::active_iface(d, c);
        const bool     gated     = supply_lost_ && !is_bootstrap
                                && !(iface_now & (IFACE_ON_BOARD | IFACE_CAN_DEVICE));

        // Instant (un-windowed) MAP companion — published EVERY frame (1 kHz, AE's read rate), NOT
        // gated by this sensor's decimation (window-rate is too slow for a tip-in) and NOT the ~10 kHz
        // ADC rate (nothing reads that fast). It's the live DMA-FILTERED read (a fast time-filtered
        // value, not a raw sample) through this sensor's cal. wk::map stays the angle-windowed average
        // for the fuel base; wk::imap is the fast tap TransientThrottle reads — no module touches an ADC pin.
        if (d.primary_channel == wk::map && iface_now == IFACE_ENGINE_SYNC_VOLTAGE) {
            // (valid, now) spelled out: this was set(ch, value, now), which passed the TICK as `valid`
            // and left the timestamp at 0.
            // …and only as valid as MAP ITSELF. The fast tap skips the pipeline, diagnostics included, so it
            // stayed valid on a sensor whose raw check had tripped — a faulted MAP still steering the
            // transient fuel through imap. MAP's own channel carries the verdict; imap follows it.
            bus.set(wk::imap, pipe::curve_eval(&pools_[i].curve,
                                               static_cast<float>(platform_read_ain_raw(c.source))),
                    !gated && bus.valid(wk::map), now);
        }

        // --- rate decimation: run once per period SLOT ---
        // The type's rate is a FLOOR and a consumer's claim can only raise it. The type describes how fast
        // this data is worth capturing — a percent input at 200 Hz because that is what a pedal movement
        // deserves — and it is captured whether a module reads it or not: telemetry and the datalog take
        // every channel, and they never claim. So a slow reader must not be able to drag a sensor down to
        // its own cadence and quietly coarsen the logs; a 5 Hz consumer on a 200 Hz input still gets
        // 200 Hz, and only something wanting MORE moves the rate at all.
        //
        // What the claim is for is the other direction. A 1 kHz position loop reading a throttle is not
        // served by 200 Hz however reasonable that number is for a pedal, and the sensor has no way to
        // know what its data is for. Every channel this sensor publishes counts, not just the primary —
        // an aux output can easily be the one somebody is waiting on.
        const uint8_t  vt     = effective_type(d, c.type);
        uint16_t       hz     = (vt < SENSOR_TYPE_COUNT) ? SENSOR_TYPE_CATALOG[vt].update_hz : 0;
        // NO PER-SENSOR OVERRIDE HERE, and that is a decision rather than an omission. Raising one
        // sensor's floor at build time breaks align_crosschecked(): it aligns the PHASE of a
        // compared pair, and same phase only lands on the same tick at the same RATE. A throttle
        // forced to 1 kHz against a partner track running at its own type's cadence can never be
        // sampled together, and which sensor that partner IS comes from the tune (tps_b_src points
        // wherever it is aimed) — so no build-time rule can keep the pair matched.
        //
        // The claim mechanism is the right tool and already does it: a module claims every channel
        // it reads, so the ETB and transient loops raise BOTH tracks together, whichever they are.
        for (unsigned k = 0; k < d.provides_count; k++) {
            const uint16_t r = sigrate::required_hz(static_cast<SignalId>(SENSOR_PROVIDES[d.provides_off + k]));
            if (r > hz) hz = r;
        }
        // Keep the publish ttl on the rate ACTUALLY being used, not the one baked when the pipeline was
        // composed. The ttl is what tells a consumer its data has gone stale, so it has to mean something
        // at the rate the consumer asked for: leave it at the type's leisurely figure and a 1 kHz loop
        // would happily accept a value fifteen milliseconds old as fresh. It has to move the other way
        // too — when a fast consumer goes away and the rate drops back, a ttl still cut for 1 kHz would
        // expire every value the slower sensor publishes and the channel would read invalid forever.
        // Recomputed here because here is where the rate is known, every frame.
        pools_[i].pub.ttl_ms = static_cast<uint16_t>(hz ? (3000u / hz) : 0u);
        // …AND THIS INPUT'S FAULTS GET ITS OWN CLOCK TOO. A 2 Hz sensor evaluates every 500 ms and a
        // 200 Hz one every 5: judging both on one number means it is either too tight for the slow one
        // — where a single late evaluation would put a real fault out — or pointlessly long for the
        // fast one. Eight of its periods, never under a second. Same shape as the publish ttl above,
        // with a fault's tolerance instead of a reading's.
        const uint32_t sensor_dtc_ttl = hz ? ((8000u / hz) < 1000u ? 1000u : (8000u / hz)) : 1000u;
        const uint32_t period = hz ? (1000u / hz) : 0u;     // ms; 0 = every frame
        // Which slot of that period we are in, offset by this sensor's phase. Same rate + same phase =
        // same slot = same tick, however long either sensor has been running; different phases stagger
        // the work across the period. A skipped millisecond lands in the next slot and still runs,
        // where a "now % period == 0" test would have dropped the whole period.
        if (period) {
            const uint32_t slot = (now + phase_[i]) / period;
            if (ran_once_[i] && slot == last_slot_[i]) continue;   // already ran in this slot
            last_slot_[i] = slot;
        }
        ran_once_[i] = true;
        const float    dt_ms  = static_cast<float>(now - last_run_[i]);
        last_run_[i] = now;

        // --- run the input's pipeline (Acquire -> Decode -> Condition* -> Publish) ---
        // The pipeline publishes the value (valid=false if a Condition tripped) and reports
        // the tripped diagnostic slots in ctx.diag_tripped.
        // Operating-window DTC arming: run this sensor's precondition EXPRESSION (Expr.h). An
        // empty program means no preconditions — always armed. A program that failed validation at
        // the last config change also arms: a broken gate must not silently switch detection off,
        // and SENSOR_CATALOG[i].dtc_precond names the sensor whose expression is broken.
        bool armed = true;
        if (!expr_bad_[i] && !expr::is_empty(c.precond_expr, sizeof(c.precond_expr))) {
            extern EcuConfig g_config;
            expr::Ctx ectx;
        // THE TUNE'S OWN TABLES, readable from an expression. expr_table_value() reads one at its
        // configured axes (OP_TABLE); expr_table_at() reads it at an x the program pushed (OP_INTERP).
        // The bus rides in the user pointer because a table's axes ARE bus channels — which keeps the
        // VM kernel free of the table engine, the way it was written to be.
        ectx.table       = expr_table_value;
        ectx.curve       = expr_table_at;
        ectx.curve_user  = &bus;
        ectx.curve_count = EXPR_TABLE_COUNT;
            ectx.bus      = &bus;
            ectx.cfg      = reinterpret_cast<const uint8_t*>(&g_config);
            ectx.cfg_size = sizeof(g_config);
            ectx.now_ms   = now;
            armed = expr::eval_bool(c.precond_expr, sizeof(c.precond_expr), ectx,
                                    /*on_invalid=*/true);
        }
        const pipe::Ctx ctx = pipelines_[i].run(bus, now, dt_ms, armed, gated);
        if (ctx.abort) continue;                  // unassigned source / no data → published nothing

        // Bench/USB: the bootstrap battery published its value above (key_on needs it) but DTC
        // detection is OFF — with no 12 V the battery reads ~0 mV, which looks like a short, so we
        // must not raise a phantom raw-min fault. (Non-bootstrap sensors already `continue`d above.)
        if (!key_on_) { fault_[i] = false; sev_[i] = 0; dtc_[i] = 0; continue; }

        // Supply down: the reading is the rail's, not the sensor's, so it is not evidence either way —
        // no code raised, none healed. Persistence timers restart, so a check that trips the moment the
        // rail returns still has to hold for its full delay before it counts.
        if (gated) {
            for (uint8_t slot = 0; slot < 6; slot++) dtc_since_ms_[i][slot] = 0;
            for (uint8_t k = 0; k < d.aux_count; k++) {   // secondary outputs go invalid with it
                const uint8_t ai = d.aux_off + k;
                if (aux_pipelines_[ai].n) aux_pipelines_[ai].run(bus, now, dt_ms, true, true);
            }
            continue;
        }

        // --- DTC policy: map the reported diagnostic slots to catalog P-codes + severity ---
        // raw_min/raw_max/stuck/derivative checks raise-or-heal every run (an absent/passing
        // check heals); operating-window codes only when its Condition armed (under load).
        const uint16_t codes[6] = { d.dtc_raw_min, d.dtc_raw_max, d.dtc_op_min,
                                    d.dtc_op_max,  d.dtc_stuck,   d.dtc_max_deriv };
        uint8_t  worst_sev = 0;
        uint16_t worst_dtc = 0;
        bool     any = false;
        uint8_t& asserted = dtc_asserted_[i];
        for (uint8_t slot = 0; slot < 6; slot++) {
            const bool is_op = (slot == pipe::DIAG_SLOT_OP_MIN || slot == pipe::DIAG_SLOT_OP_MAX);
            // A CHECK THAT IS SWITCHED OFF HAS NO VERDICT TO WAIT FOR, so it cannot be left pending: its
            // code has to go. The enable bit is `1 << slot` for all six (pipe::DIAG_RAW_MIN … MAX_DERIV).
            //
            // This is the one way a code could be STRANDED. The op-window codes are deliberately left
            // alone whenever their Condition did not evaluate — "not under load" is not evidence that a
            // fault has cleared — and a check the user has just unticked does not evaluate either. With
            // both directions off the stage is not even built, so op_armed never comes true again and the
            // skip below ran for ever: trip ECU Temperature's reading-high check, untick it, and the DTC
            // stayed current with nothing left in the system able to heal it.
            //
            // The other four slots never had the problem — they fall through to raise-or-heal every run,
            // and an absent check reads as "not tripping", which heals. This makes that true of all six.
            const bool slot_on = ((c.diag_enable >> slot) & 1u) != 0;
            if (is_op && slot_on && !ctx.op_armed) continue;   // not under load → leave op codes
            const uint16_t code = codes[slot];
            const bool raw_trip = slot_on && ((ctx.diag_tripped & (1u << slot)) != 0);
            // PERSISTENCE. A check must trip CONTINUOUSLY for diag_delay_ms before it counts as a fault;
            // the first passing sample resets the timer. Healing stays immediate — a sensor that has come
            // good should not keep a light on for another delay — so the asymmetry is deliberate.
            uint32_t& since = dtc_since_ms_[i][slot];
            if (!raw_trip) since = 0;
            else if (since == 0) since = now ? now : 1u;   // 0 is the "not tripping" sentinel
            const bool tripped = raw_trip && (c.diag_delay_ms == 0 || (now - since) >= c.diag_delay_ms);
            const uint8_t cs = static_cast<uint8_t>((c.diag_severity >> (slot * 2)) & 0x3);
            // ASSERTED EVERY PASS while tripped; healed on the falling edge. The table ages out a code
            // its judge has stopped asserting (DtcManager::raise, ttl), so "keep saying it" is what
            // keeps it CURRENT — and the immediate heal below means a fault that clears does not wait
            // out a ttl to go. The cost is one lookup per TRIPPED code, not per sensor per slot.
            if (code && dtc_mgr_) {
                if (tripped) dtc_mgr_->raise(code, i, cs, now, sensor_dtc_ttl);
                else if ((asserted >> slot) & 1u) dtc_mgr_->heal(code);
                if (tripped != static_cast<bool>((asserted >> slot) & 1u))
                    asserted ^= static_cast<uint8_t>(1u << slot);
            }
            if (tripped) {                                  // fault/severity recorded regardless of code (local)
                any = true;
                if (cs > worst_sev)      { worst_sev = cs; worst_dtc = code; }
                else if (worst_dtc == 0) { worst_dtc = code; }
            }
        }
        // NO BAND — a band-decoding type only, and NOT one of the six checks above: there is no
        // threshold to choose and nothing to arm, so it has no tick box and is always reported. Same
        // edge-triggered shape as the precondition code. The channel is already invalid by this point,
        // which is all a consumer needs to shut down; this is what tells the user WHY.
        const uint16_t nbcode = d.dtc_no_band;
        const bool nb = (ctx.diag_tripped & (1u << pipe::DIAG_SLOT_NO_BAND)) != 0;
        if (nbcode && dtc_mgr_ && (nb || nb != noband_dtc_raised_[i])) {
            if (nb) dtc_mgr_->raise(nbcode, i, DTC_SEV_LEVEL1, now, sensor_dtc_ttl);
            else    dtc_mgr_->heal(nbcode);
            noband_dtc_raised_[i] = nb;
        }
        if (nb) { any = true; if (worst_dtc == 0) worst_dtc = nbcode; }

        fault_[i] = any;
        sev_[i]   = worst_sev;
        dtc_[i]   = worst_dtc;

        // --- secondary outputs: each is its own full pipeline (acquire -> decode -> op-window ->
        // ema -> publish), run at the parent's cadence. Map its operating-window trips to the
        // output's own low/high P-codes and fold the worst into this sensor's summary. ---
        for (uint8_t k = 0; k < d.aux_count; k++) {
            const uint8_t ai = d.aux_off + k;
            if (aux_pipelines_[ai].n == 0) continue;
            const pipe::Ctx ax = aux_pipelines_[ai].run(bus, now, dt_ms, /*precond_armed=*/true);
            if (ax.abort) continue;
            const SensorAuxOutput& ao = SENSOR_AUX_OUTPUTS[ai];
            const uint16_t acodes[2] = { ao.dtc_op_min, ao.dtc_op_max };
            uint8_t& aasserted = aux_asserted_[ai];
            for (uint8_t j = 0; j < 2; j++) {
                const uint8_t slot = j ? pipe::DIAG_SLOT_OP_MAX : pipe::DIAG_SLOT_OP_MIN;
                const bool trip = (ax.diag_tripped & (1u << slot)) != 0;
                // Edge-triggered (same as the main loop): scan only on a state flip.
                if (acodes[j] && dtc_mgr_ && (trip || trip != static_cast<bool>((aasserted >> j) & 1u))) {
                    if (trip) dtc_mgr_->raise(acodes[j], i, ao.severity, now, sensor_dtc_ttl);
                    else      dtc_mgr_->heal(acodes[j]);
                    if (trip != static_cast<bool>((aasserted >> j) & 1u))
                        aasserted ^= static_cast<uint8_t>(1u << j);
                }
                if (trip) {
                    fault_[i] = true;
                    if (ao.severity > sev_[i]) { sev_[i] = ao.severity; dtc_[i] = acodes[j]; }
                    else if (dtc_[i] == 0)     { dtc_[i] = acodes[j]; }
                }
            }
        }
    }
}
