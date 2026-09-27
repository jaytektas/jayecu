#pragma once
//
// Pipeline builder — composes an Input Pipeline from the existing sensor catalog
// descriptor (Tier 2) + the per-sensor tune (Tier 3, SensorConfig).  See
// docs/polymorphic-pipeline-architecture.md (codegen emits the toolbox + default
// compositions; the runtime selects + wires them).
//
// DESIGN CHOICE: reuse the existing SensorConfig as the per-stage config — the cal curve,
// filter tau, source and interface ALREADY hold a pipeline's worth of config; they were
// just consumed by Sensors.cpp's switch(iface).  The builder points cfg structs INTO that
// config (no copy of the heavy arrays).  No new config encoding, no config-version bump.
//
// SCOPE: build_input composes the FULL pipeline per the design doc
// (Acquire -> Decode -> Condition* -> Publish), in the original Sensors order:
//
//   acquire_* -> [cond_raw_window] -> decode_curve
//             -> [cond_op_window] -> [cond_stuck] -> [cond_derivative] -> publish
//
// switch:  acquire_switch (state IS the value, no Decode) -> [cond_stuck] -> ... -> publish
// switch on an analog pin:
//          acquire_analog -> decode_switch_thresh (two trip points + deadband) -> [cond_stuck] -> …
// multi-position switch (analog pin):
//          acquire_analog -> [cond_raw_window] -> decode_bands (settle + gap + stuck) -> …
//          (the raw window is the ordinary optional one here — no forced floor; see multi_switch_bands)
// onboard: acquire_onboard_* (value direct, no Decode) -> [conditions] -> publish
//
// Conditions clear Ctx.valid and report the tripped DiagSlot in Ctx.diag_tripped; the Input
// MANAGER (Sensors) reads that after run() and applies DTC POLICY (raise/heal P-codes +
// severity) — diagnostics are conditions, DTC emission is policy.  Decimation + pin
// arbitration also stay in the manager (cross-input concerns).
//
// CAN-device inputs return false (need the device library's frame binding); the CAN pipeline
// itself already works via acquire_can_field.
//
// CAN-device inputs need extraction params (frame id / offset / len / endian) the
// SensorConfig does NOT carry — that binding is the CAN device library (a later sub-step);
// the CAN pipeline itself already works (acquire_can_field, proven in test_pipeline).
//
#include "../Pipeline/Pipeline.h"
#include "well_known_signals.h"   // wk:: roles — rename-safe signal bindings
#include "../Pipeline/Stages.h"
#include "../Platform/AcquireHal.h"
#include "../../generated/sensors_catalog.h"
#include "../../generated/modules/sensors_config.h"
#include "../../generated/hw_input_map.h"        // HW_POOL_SIG[]: analog source pool -> raw bus channel

namespace pipe {

// Wiring-flag + diag-enable bits in SensorConfig (mirrored from the schema Sensors element).
constexpr uint8_t FLAG_INVERT   = 0x01;   // switch: invert the raw level
constexpr uint8_t DIAG_RAW_MIN  = 0x01;
constexpr uint8_t DIAG_RAW_MAX  = 0x02;
constexpr uint8_t DIAG_OP_MIN   = 0x04;
constexpr uint8_t DIAG_OP_MAX   = 0x08;
constexpr uint8_t DIAG_STUCK    = 0x10;
constexpr uint8_t DIAG_MAX_DERIV = 0x20;

// Storage for one input's pipeline stage configs.  The builder fills the used members and
// points the Pipeline's stages at them; lives in a static pool alongside the Pipeline.
struct InputCfgPool {
    PinAcquireCfg    analog;
    FreqAcquireCfg   freq;
    PulseAcquireCfg  pulse;
    SentAcquireCfg   sent;
    SwitchAcquireCfg sw;
    // ONE DECODE PER INPUT, so the decode configs share storage: an input is a curve, a two-point switch
    // or a band switch, never two of them. The pool is multiplied by 128 sensors, which is why this is a
    // union rather than a member apiece.
    union {
        CurveCfg         curve;
        SwitchThreshCfg  thresh;   // a switch read through an analog pin: the two trip points
        BandCfg          bands;    // a multi-position switch: one voltage band per position
    };
    RawWindowCfg     raw_win;
    OpWindowCfg      op_win;
    StuckCfg         stuck;
    DerivCfg         deriv;
    PublishCfg       pub;
    CanFieldAcquireCfg can_acq;   // CAN-interface inputs: a generic-CAN field
    // A linear raw->engineering decode. Named for what it does rather than for who first needed it:
    // this was `can_lin`, and the aux-output path had quietly borrowed the CAN slot for its own
    // decode, which is why removing the CAN one broke a pipeline that has nothing to do with CAN.
    LinearCfg          lin;
    SentinelCfg        sentinel;
};

// Resolve the active interface BIT. The catalog lock wins (board-managed sensors, e.g. on_board);
// otherwise the explicit per-sensor selector (a SensorIfaceSel index, no 'Default' — it's defaulted
// to the sensor's primary at codegen). No silent fallback: an out-of-range index resolves to none.
inline uint8_t active_iface(const SensorDescriptor& d, const SensorConfig& c) {
    if (d.locked_interface) return d.locked_interface;
    if (c.interface < IFSEL_COUNT) return SENSOR_IFACE_SEL_TO_BIT[c.interface];
    return 0;
}

// A multi-position switch's bands, checked before anything decodes them: whole [low, high] pairs, each
// low <= high, and a real GAP between one band's high and the next band's low (bands that touch leave a
// voltage that is two positions at once). Returns the band count, or 0 for a calibration that cannot be
// decoded safely — which builds no pipeline and raises the sensor's config DTC
// (Sensors::compute_conflicts), rather than guessing what was meant.
//
// THERE IS NO FLOOR HERE ANY MORE. This type used to refuse a band reaching below a fixed 0 V floor, on
// the reasoning that 0 V must never read as a press; that reasoning did not survive (see build_input's
// raw-window note — a stalk that GROUNDS the line is a real position, and where a fault starts is the
// user's own Raw Min). So these are the STRUCTURAL rules only, and they are the ones the studio's band
// editor states back to the user while they are still looking at it.
inline uint8_t multi_switch_bands(const SensorConfig& c) {
    if (c.cal_n < 2 || c.cal_n > SENSOR_CAL_POINTS || (c.cal_n & 1u)) return 0;
    for (uint8_t k = 0; k < c.cal_n; k += 2) {
        if (c.cal_raw[k] > c.cal_raw[k + 1]) return 0;                    // low above high
        if (k && c.cal_raw[k] <= c.cal_raw[k - 1]) return 0;              // touches/overlaps the band below
    }
    return static_cast<uint8_t>(c.cal_n / 2);
}

// Look up a DIG source pin's raw hardware channel for one capture mode (HW_DIG_*_SIG map), guarding
// the pool bound — an out-of-range source yields SIG_NONE (Acquire then reads an invalid/absent slot).
inline SignalId dig_hw_sig(const SignalId* map, uint8_t source) {
    return (source < HW_DIG_COUNT) ? map[source] : SIG_NONE;
}

// Compose the pipeline for one input into `p`, with stage configs in `pool`.  Returns
// false if the input produces nothing this config (unassigned / CAN-device not yet bound).
inline bool build_input(const SensorDescriptor& d, const SensorConfig& c,
                        InputCfgPool& pool, Pipeline& p) {
    p.signal = d.primary_channel;
    p.n = 0;
    p.reset_state();
    auto add = [&](StageFn fn, const void* cfg) {
        if (p.n < MAX_PIPELINE_STAGES) p.stages[p.n++] = Stage{fn, cfg};
    };

    // The type this input is actually read as, and the engineering scale that follows from it.
    // For a catalogued sensor that is the catalog's type; for a `generic` input it is the type the
    // tune selected, which is what makes a generic take on that type's units, precision and range.
    const uint8_t vt = effective_type(d, c.type);
    // No type means NOT CONFIGURED, and an unconfigured input builds nothing. There is no scale to
    // decode with and no units to publish in, so the honest output is silence — the same rule the
    // on-board dispatch below follows. (It also keeps the sentinel out of SENSOR_TYPE_CATALOG[].)
    if (vt >= SENSOR_TYPE_COUNT) return false;
    const float vscale = SENSOR_TYPE_CATALOG[vt].val_scale;

    const uint8_t iface = active_iface(d, c);
    bool numeric_raw = false;   // raw scalar feeds Decode (curve) + the raw-window
    // A raw-window check is meaningful where the raw value is a NUMBER IN THE INPUT'S OWN UNITS: ADC counts
    // on a voltage pin, Hz on a frequency input, µs on a pulse width, the SENT value. The threshold is in
    // those same units (the studio shows mV for a voltage pin and the native unit otherwise). NOT on the
    // on-board sensors: their raw is already a float in engineering units, and comparing it (as the bit
    // pattern of .u, no less) with a counts threshold faulted on every run.
    bool raw_ranged  = false;
    const bool is_switch = (iface & IFACE_DIGITAL) != 0;
    // WHAT IT IS versus HOW IT IS READ. Those are different questions, and everything below used to
    // ask only the second: a switch-type sensor wired to an analog pin (start_sw, ac_request and the
    // generic switches all offer that interface) took the analog branch and was then decoded through
    // the cal CURVE, publishing a calibrated analogue number where every consumer expects 0 or 1 —
    // and skipping the debounce and stuck checks that make a switch a switch.
    const bool switch_type = (vt == SENSOR_TYPE_SWITCH);
    // A multi-position switch is decoded from a VOLTAGE, so it is only meaningful on an analog pin. On a
    // digital pin it is an ordinary two-state switch (acquire_switch below); on a counting interface
    // (frequency / pulse / SENT) there is no voltage to band, so it builds nothing.
    const bool analog_in   = (iface & (IFACE_ANALOG_VOLTAGE | IFACE_ENGINE_SYNC_VOLTAGE)) != 0;
    const bool multi_type  = (vt == SENSOR_TYPE_MULTI_SWITCH);
    if (multi_type && !analog_in && !is_switch) return false;

    // --- Acquire --- (config source is int8; -1 = unassigned -> uint8 255 = SOURCE_NONE / out of pool range)
    const uint8_t src = static_cast<uint8_t>(c.source);
    if (iface & (IFACE_ANALOG_VOLTAGE | IFACE_ENGINE_SYNC_VOLTAGE)) {
        const bool es = (iface & IFACE_ENGINE_SYNC_VOLTAGE) != 0;                // MAP: windowed, off-bus
        const SignalId hw = (src < HW_POOL_COUNT) ? HW_POOL_SIG[src] : SIG_NONE;
        pool.analog = {src, es, hw}; add(acquire_analog, &pool.analog); numeric_raw = raw_ranged = true;
    } else if (iface & IFACE_DIGITAL_FREQ) {
        pool.freq = {src, dig_hw_sig(HW_DIG_FREQ_SIG, src)};   add(acquire_freq, &pool.freq);   numeric_raw = raw_ranged = true;
    } else if (iface & IFACE_PULSE_WIDTH) {
        pool.pulse = {src, dig_hw_sig(HW_DIG_PULSE_SIG, src)}; add(acquire_pulse, &pool.pulse); numeric_raw = raw_ranged = true;
    } else if (iface & IFACE_SENT) {
        pool.sent = {src, dig_hw_sig(HW_DIG_SENT_SIG, src)};
        add(acquire_sent, &pool.sent); numeric_raw = raw_ranged = true;
    } else if (is_switch) {
        pool.sw = {src, (c.flags & FLAG_INVERT) != 0, 0, dig_hw_sig(HW_DIG_LEVEL_SIG, src)};
        add(acquire_switch, &pool.sw);          // value is set directly — no Decode
    } else if (iface & IFACE_ON_BOARD) {
        // Dispatch on the TYPE, and refuse a type we do not handle. The previous `else` sent
        // everything that was not a voltage to the barometric acquire, so ecu_temp — an on-board
        // TEMPERATURE — read pressure and published 94.60 through a temperature curve. It had never
        // worked. A sensor that cannot be built publishes nothing, which is visible; a silent
        // fallthrough publishes a confident wrong number, which is not.
        switch (vt) {
            case SENSOR_TYPE_VOLTAGE:     add(acquire_onboard_voltage, nullptr); break;
            case SENSOR_TYPE_TEMPERATURE: add(acquire_onboard_temp,    nullptr); break;
            case SENSOR_TYPE_PRESSURE:    add(acquire_onboard_baro,    nullptr); break;
            default: return false;
        }
    } else {
        return false;   // IFACE_CAN_DEVICE (needs the device library) / IFACE_NONE
    }

    // --- raw-window Condition (on the raw scalar; analog/freq/sent/pulse + on-board voltage) ---
    // A MULTI-POSITION SWITCH IS NOT SPECIAL HERE. It used to get a raw-window forced on at a
    // hard-coded 200 mV floor, on the reasoning that 0 V must never read as a press. That reasoning does
    // not survive: a dead 5 V reference is already caught separately by the power-good inputs
    // (Sensors::supply_lost), and a button that GROUNDS the line is a real position on most cruise
    // stalks — Toyota's grounds it for MAIN with no resistor at all. What is left is a short to ground
    // being indistinguishable from that press, which is the same ambiguity a temperature sensor has at
    // 0 V, and which every other sensor on this ECU settles the same way: the user says where the fault
    // line is with Raw Min. So this type takes the ordinary path, and its Detect Raw Low tick box means
    // what it says instead of being the one control in the studio that cannot change anything.
    if (raw_ranged && (c.diag_enable & (DIAG_RAW_MIN | DIAG_RAW_MAX))) {
        pool.raw_win = {c.diag_raw_min, c.diag_raw_max,
                        (c.diag_enable & DIAG_RAW_MIN) != 0, (c.diag_enable & DIAG_RAW_MAX) != 0};
        add(cond_raw_window, &pool.raw_win);
    }

    // --- Decode (raw scalar -> engineering via the cal curve) ---
    // The PRIMARY value only. Secondary outputs a type derives from the same input (e.g. a flex
    // sensor's fuel temp from pulse width) are SEPARATE full pipelines — see build_aux_output.
    if (multi_type && analog_in) {
        // A MULTI-POSITION SWITCH: one voltage band per position (see BandCfg). A calibration that cannot
        // be decoded safely builds nothing at all — the sensor goes silent and its config DTC names it.
        const uint8_t nb = multi_switch_bands(c);
        if (nb == 0) return false;
        pool.bands = {c.cal_raw, c.cal_val, nb, (c.diag_enable & DIAG_STUCK) != 0, c.diag_stuck_ms, vscale};
        add(decode_bands, &pool.bands);
    } else if (numeric_raw && switch_type) {
        // A SWITCH ON AN ANALOG PIN: two trip points, not a curve. They are the first two breakpoints
        // of the cal axis — already allocated for every sensor, already in the acquire's raw units,
        // and already ascending by the axis's own rule, which fixes which is which: [0] is the lower
        // (off) and [1] the upper (on). No field of its own, on a config where 128 slots make every
        // added byte cost half a kilobyte.
        pool.thresh = { c.cal_raw[1], c.cal_raw[0], (c.flags & FLAG_INVERT) != 0 };
        add(decode_switch_thresh, &pool.thresh);
    } else if (numeric_raw) {
        // Resizable N-point curve: cal_raw/cal_val are the breakpoint + value arrays; cal_n is the
        // live point count (clamped to the SENSOR_CAL_POINTS max), so the cal edits as a 1D curve.
        const uint8_t cal_pts = c.cal_n < 2 ? 2 : (c.cal_n > SENSOR_CAL_POINTS ? SENSOR_CAL_POINTS : c.cal_n);
        pool.curve = {c.cal_raw, c.cal_val, cal_pts, vscale};
        add(decode_curve, &pool.curve);
    }

    // --- EMA filter ---

    // --- operating-window Condition (precondition-gated; on the engineering value) ---
    if (c.diag_enable & (DIAG_OP_MIN | DIAG_OP_MAX)) {
        pool.op_win = {c.diag_op_min * vscale, c.diag_op_max * vscale,
                       (c.diag_enable & DIAG_OP_MIN) != 0, (c.diag_enable & DIAG_OP_MAX) != 0};
        add(cond_op_window, &pool.op_win);
    }

    // --- stuck Condition (a switch, however it is read) ---
    // Gated on the TYPE rather than the interface: a switch that never changes state is the same
    // fault whether it was read off a digital pin or off a threshold, and gating it on the interface
    // meant an analog-wired switch silently had no stuck detection at all.
    if ((is_switch || switch_type) && (c.diag_enable & DIAG_STUCK)) {
        pool.stuck = {c.diag_stuck_ms, true};
        add(cond_stuck, &pool.stuck);
    }

    // --- rate-of-change Condition ---
    if (c.diag_enable & DIAG_MAX_DERIV) {
        pool.deriv = {static_cast<float>(c.diag_max_deriv), true};
        add(cond_derivative, &pool.deriv);
    }

    // --- Publish (ttl = ~3 sample periods, matching Sensors freshness) ---
    // The ttl comes from the TYPE's rate, not from what consumers are currently claiming. Claims move at
    // runtime and this is baked when the pipeline is composed, so a ttl derived from a fast claim would
    // outlive the claim and start calling perfectly good values stale the moment the fast consumer went
    // away. The type rate is the slow bound: sampling faster than it only ever makes values fresher than
    // the ttl demands, which is the safe direction for a staleness guard.
    const uint16_t hz = SENSOR_TYPE_CATALOG[vt].update_hz;
    pool.pub = { static_cast<uint16_t>(hz ? 3000u / hz : 0u) };
    add(publish, &pool.pub);
    return true;
}

// Compose a CAN-interface input pipeline. The reading comes from a generic-CAN receive FIELD, which
// has already done the bit surgery and the scale — that decode is shared with the transmit side and
// is editable in the tune, rather than a device table compiled into the firmware.
//
// What the sensor adds is everything a direct bus write cannot: its own calibration curve if it has
// one, the operating-window and derivative diagnostics, its per-sensor DTCs, and its enable. That is
// the reason a CAN-sourced sensor goes through here at all instead of the field simply naming the
// channel.
//
//   acquire_can_field -> decode (identity today) -> [cond_op_window]
//                     -> [cond_derivative] -> publish
//
inline bool build_can_input(const GenericCanFieldValue* field, uint16_t stale_ms,
                            const SensorDescriptor& d, const SensorConfig& c,
                            InputCfgPool& pool, Pipeline& p) {
    if (!field) return false;
    p.signal = d.primary_channel;
    p.n = 0;
    p.reset_state();
    auto add = [&](StageFn fn, const void* cfg) {
        if (p.n < MAX_PIPELINE_STAGES) p.stages[p.n++] = Stage{fn, cfg};
    };

    const uint8_t vt = effective_type(d, c.type);
    if (vt >= SENSOR_TYPE_COUNT) return false;          // unconfigured — nothing to decode into
    const float vscale = SENSOR_TYPE_CATALOG[vt].val_scale;

    pool.can_acq = { field, stale_ms };
    add(acquire_can_field, &pool.can_acq);
    // AN IDENTITY DECODE, and it is not optional. The field's own multiplier and offset already
    // produced the engineering value, so there is no second transform to apply — but Acquire fills
    // `raw` and Publish writes `value`, and a pipeline with no Decode at all publishes zero. Saying
    // it as decode_linear{1,0} also leaves the obvious place for a CAN sensor that needs a real
    // calibration curve: this stage becomes decode_curve, exactly as an analog sensor's does.
    pool.lin = {1.0f, 0.0f};
    add(decode_linear, &pool.lin);

    if (c.diag_enable & (DIAG_OP_MIN | DIAG_OP_MAX)) {
        pool.op_win = {c.diag_op_min * vscale, c.diag_op_max * vscale,
                       (c.diag_enable & DIAG_OP_MIN) != 0, (c.diag_enable & DIAG_OP_MAX) != 0};
        add(cond_op_window, &pool.op_win);
    }
    if (c.diag_enable & DIAG_MAX_DERIV) {
        pool.deriv = {static_cast<float>(c.diag_max_deriv), true};
        add(cond_derivative, &pool.deriv);
    }
    const uint16_t hz = SENSOR_TYPE_CATALOG[vt].update_hz;
    pool.pub = { static_cast<uint16_t>(hz ? 3000u / hz : 0u) };
    add(publish, &pool.pub);
    return true;
}

// Compose a SECONDARY output's pipeline (e.g. flex fuel temp from the same input's pulse width).
// It's a full, ordinary pipeline reusing the standard stages — acquire -> decode_linear ->
// cond_op_window (always armed) -> publish — built entirely from the catalog spec
// (transform + range + codes), with the source pin + filter taken from the parent SensorConfig.
inline bool build_aux_output(const SensorAuxOutput& a, const SensorDescriptor& parent,
                             const SensorConfig& c, InputCfgPool& pool, Pipeline& p) {
    p.signal = a.signal;
    p.n = 0;
    p.reset_state();
    const uint8_t src = static_cast<uint8_t>(c.source);   // int8 -1 (unassigned) -> uint8 255 = SOURCE_NONE
    if (src == SOURCE_NONE) return false;
    auto add = [&](StageFn fn, const void* cfg) {
        if (p.n < MAX_PIPELINE_STAGES) p.stages[p.n++] = Stage{fn, cfg};
    };
    // Acquire: read the SAME pin in the secondary's capture mode (pulse width / frequency).
    if (a.acquire == 1) { pool.freq  = {src, dig_hw_sig(HW_DIG_FREQ_SIG,  src)}; add(acquire_freq,  &pool.freq); }
    else                { pool.pulse = {src, dig_hw_sig(HW_DIG_PULSE_SIG, src)}; add(acquire_pulse, &pool.pulse); }
    // Decode: linear raw -> engineering (scale/offset derived from the type's two cal points).
    pool.lin = {a.scale, a.offset};
    add(decode_linear, &pool.lin);
    // EMA — shares the parent sensor's filter time constant (before the window, like the primary).
    // Operating-range Condition — always armed (min_rpm 0, no precondition); out-of-range trips
    // DIAG_OP_MIN/MAX, which the manager maps to this output's low/high P-codes.
    pool.op_win = {a.range_min, a.range_max, true, true};   // aux outputs are always armed (no precond)
    add(cond_op_window, &pool.op_win);
    const uint16_t hz = SENSOR_TYPE_CATALOG[parent.type].update_hz;
    pool.pub = { static_cast<uint16_t>(hz ? 3000u / hz : 0u) };
    add(publish, &pool.pub);
    return true;
}

} // namespace pipe
