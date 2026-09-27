#pragma once
//
// Pipeline stage library — the composable toolbox.  See
// docs/polymorphic-pipeline-architecture.md (§ stage taxonomy).
//
// Each stage is a small, pure-ish function over the shared Ctx.  They are grouped by
// role and by the type they operate on:
//
//   Acquire   : source            -> raw scalar (Ctx.raw + raw_kind)   [owns addressing]
//   Decode    : raw scalar        -> engineering value (Ctx.value)     [type-uniform]
//   Condition : engineering value -> engineering value + validity      [type-uniform]
//   Publish   : Ctx.value         -> SignalBus
//
// The Decode/Condition math here is LIFTED verbatim from firmware/Sensors/Sensors.cpp
// (curve_cal / raw_diag / EMA / operating-window) — proven, bench-validated logic, now
// expressed as discrete stages instead of branches of one switch(iface).  Step 1 ships
// a test Acquire (acquire_const); the real HAL-bound Acquires (analog/freq/sent/pulse/
// CAN-frame) come in step 2.
//
#include "Pipeline.h"
#include "../Can/CanFieldValue.h"   // the seam: a decoded generic-CAN field
#include "../Engine/TableEngine.h"   // tbl::interp — a cal curve IS a 1-axis table lookup
#include "../../generated/boards/board.h"   // BOARD_ADC_FULL_SCALE — decode_bands' "no reading"
#include <cstdint>

namespace pipe {

// --- per-stage config structs (these become views into the flat ecu_config) ----------

struct ConstAcquireCfg { uint32_t raw; bool present; };          // test/fixed source
struct CurveCfg   { const uint16_t* xs; const int16_t* ys; uint8_t n; float val_scale; };
// A SWITCH READ THROUGH AN ANALOG PIN. Not a curve: a curve interpolates, and a switch does not have
// intermediate states — it has a trip point, and one trip point on a real input chatters. So it is a
// PAIR, exactly as the output gate's two conditions are a pair: above `on_raw` it is on, below
// `off_raw` it is off, and between them it HOLDS. That band is the hysteresis, written as what it is.
// Both are in the raw units the analog acquire produces (ADC counts), which is the same axis the cal
// curve's breakpoints use — so the two numbers ARE cal_raw[0] (off) and cal_raw[1] (on), and a switch
// on an analog pin costs no config of its own.
struct SwitchThreshCfg { uint32_t on_raw; uint32_t off_raw; bool invert; };
// A MULTI-POSITION SWITCH on an analog pin (resistor-ladder buttons, a rotary selector). The cal axis
// holds one voltage BAND per position as a breakpoint PAIR — [2k] the band's low edge, [2k+1] its high
// edge, both raw ADC counts — and cal_val[2k] is that band's position number. Pointers into the live
// config, like CurveCfg, so it costs nothing of its own. Position 0 is REST: the only one that may be
// held indefinitely, so the stuck check exempts it.
struct BandCfg {
    const uint16_t* edges;      // cal_raw: lo0, hi0, lo1, hi1, ...
    const int16_t*  pos;        // cal_val: the position at [2k] (type-scaled)
    uint8_t         bands;      // live band count (cal_n / 2)
    bool            stuck_en;   // DIAG_STUCK: a non-rest position held too long is a fault
    uint16_t        stuck_ms;
    float           val_scale;
};
// How long a reading must stay inside ONE band before it is believed, and how long it may sit in NO band
// before that is a fault. A ladder does not step cleanly: pressing or releasing a button sweeps the
// voltage through every band between the two positions, and a contact makes and breaks as it closes.
// Settling rejects the sweep. The out-of-band allowance lets the reading cross a gap between two bands
// (which is what a gap is for) without calling it a broken wire; past it, the wire is what it is.
constexpr uint32_t BAND_SETTLE_MS = 40;
constexpr uint32_t BAND_GAP_MS    = 100;
struct LinearCfg  { float scale; float offset; };
// Diagnostic windows report which slot tripped (Ctx.diag_tripped) so the Input manager can
// raise the right DTC/severity. The RAW-integrity checks (raw_window/stuck/derivative) ALSO clear
// Ctx.valid — the sensor is AWOL, no trustworthy value (publish-invalid still publishes). The
// operating-window (op_min/op_max) does NOT: the value is real but unhealthy, so it stays valid and
// the DTC alone carries the fault.
struct RawWindowCfg { uint32_t min; uint32_t max; bool en_min; bool en_max; };   // on raw scalar
struct OpWindowCfg  {                                            // on eng value, gated "under load"
    float    min, max;
    bool     en_min, en_max;
    // Arming (the precondition Condition) is resolved by the Input manager and handed in via
    // Ctx.precond_armed — the stage no longer owns the rpm/precond-channel gate.
};
struct StuckCfg   { uint16_t stuck_ms; bool en; };               // switch held one state too long
struct DerivCfg   { float max_deriv; bool en; };                 // |d(value)/dt| over limit (units/s)
struct SentinelCfg { uint32_t value; bool en; };                // raw == value -> invalid (CAN)
struct PublishCfg { uint16_t ttl_ms; };

// A CAN-interface sensor reads a generic CAN FIELD, already unpacked and scaled by GenericCan. The
// bit surgery and the transform live in one place for both directions, so a sensor cannot decode a
// different slice than the same field transmits, and the addressing is editable in the tune rather
// than compiled into a device table.
struct CanFieldAcquireCfg {
    const GenericCanFieldValue* field;   // GenericCan::value(i) — null = nothing to read
    uint16_t stale_ms;                   // 0 = never; else abort if the reading is older than this
};

// The raw scalar as a float, honouring the Acquire's kind tag — this is what makes
// Decode type-uniform (a curve/linear works whether the source was unsigned mV, a signed
// CAN field, or a float frame).
inline float raw_as_float(const Ctx& c) {
    switch (c.raw_kind) {
        case RAW_I32: return static_cast<float>(c.raw.i);
        case RAW_F32: return c.raw.f;
        default:      return static_cast<float>(c.raw.u);
    }
}

// --- Acquire ---------------------------------------------------------------------------

// Test/fixed Acquire: emit a constant raw scalar, or signal "no data" (abort -> nothing
// published).  Stands in for the HAL-bound Acquires (those live in AcquireHal.h).
inline void acquire_const(Ctx& c, const void* cfg, State&) {
    const auto* a = static_cast<const ConstAcquireCfg*>(cfg);
    if (!a->present) { c.abort = true; return; }   // unassigned / no data this run
    c.raw.u   = a->raw;
    c.raw_kind = RAW_U32;
    c.valid   = true;
}

// CAN-field Acquire: take the engineering value GenericCan decoded for this field. No reading yet, or
// one older than the field's TTL, aborts — nothing is published and the slot expires on its own TTL,
// which is what makes a dead sender show as an absent channel rather than a frozen number.
//
// It hands on a FLOAT, because the value has already been through its scale and offset. Decode
// downstream is then the sensor's OWN calibration if it has one, which is the whole reason a CAN
// sensor goes through the pipeline instead of writing the bus directly.
inline void acquire_can_field(Ctx& c, const void* cfg, State&) {
    const auto* a = static_cast<const CanFieldAcquireCfg*>(cfg);
    if (!a->field || a->field->at_ms == 0) { c.abort = true; return; }   // never arrived
    if (a->stale_ms && (c.now_ms - a->field->at_ms) > a->stale_ms) { c.abort = true; return; }
    c.raw.f    = a->field->v;
    c.raw_kind = RAW_F32;
    c.valid    = true;
}


// --- Decode (raw scalar -> engineering) ------------------------------------------------

// A sensor cal curve IS a 1-axis table lookup: a U16 breakpoint axis (xs / cal_raw) + I16 cells
// (ys / cal_val) scaled by val_scale, X = the raw value, Y/Z absent. Resolves through the ONE table
// engine (tbl::interp) — no bespoke interpolator. (Typed axes are what let the U16 axis ride it.)
inline float curve_eval(const CurveCfg* k, float raw) {
    if (k->n == 0) return 0.0f;
    return tbl::interp(k->ys, k->val_scale,
                       k->xs, tbl::CELL_U16, static_cast<int>(k->n), raw,
                       nullptr, tbl::CELL_F32, 0, 0.0f,
                       nullptr, tbl::CELL_F32, 0, 0.0f);
}
inline void decode_curve(Ctx& c, const void* cfg, State&) {
    c.value = curve_eval(static_cast<const CurveCfg*>(cfg), raw_as_float(c));
}

// Decode a switch wired to an ANALOG input: raw counts -> 0/1, with the deadband holding the state
// between the two trip points. It stands in for decode_curve (never beside it) — a switch has no
// engineering value to interpolate, and running a two-point curve over it would report 1.4 for a
// contact that is either made or not.
//
// The state lives in State::sw, the SAME place acquire_switch keeps it, so cond_stuck reads one
// field however the switch was read and a transition resets its timer identically. That is the whole
// point of decoding to a boolean here rather than publishing volts: everything downstream of this
// stage cannot tell an analog-read switch from a digital one.
inline void decode_switch_thresh(Ctx& c, const void* cfg, State& st) {
    const auto* t = static_cast<const SwitchThreshCfg*>(cfg);
    // State::sw.state holds the LOGICAL state — what the sensor is reported as, inversion already
    // applied — because that is what acquire_switch stores and what cond_stuck compares c.value
    // against. Storing the electrical level here instead would make cond_stuck see a mismatch every
    // single frame on an inverted switch, reset the stuck timer forever and overwrite the latch this
    // stage's hysteresis depends on. So invert on the way in as well as out.
    const bool prev_level = t->invert ? !st.sw.state : st.sw.state;
    bool level = prev_level;
    // ON is tested first, so a pair entered the wrong way round (off above on, leaving no band) reads
    // as a plain rising threshold rather than as a state that can never be left.
    if (c.raw.u >= t->on_raw)       level = true;
    else if (c.raw.u <= t->off_raw) level = false;
    // …otherwise: inside the band, and the level stands. That is the hysteresis.
    const bool on = t->invert ? !level : level;
    if (on != st.sw.state) { st.sw.state = on; st.sw.change_ms = c.now_ms; }   // reset the stuck timer
    c.value = on ? 1.0f : 0.0f;
}

// Decode a MULTI-POSITION SWITCH: raw counts -> the position of the band the reading has SETTLED in.
//
// Nothing is reported until a band has been held for BAND_SETTLE_MS — at power-up, and again after any
// fault — so the first value anyone sees is a position the switch is actually in, never one it swept
// through. After that the accepted position stands until another band settles, or until the reading has
// been in NO band for BAND_GAP_MS, which is an open circuit, a short to the reference, or a ladder that
// has drifted out of its calibration: invalid, raw-high slot. A reading already condemned upstream (the
// raw-window floor — 0 V is a dead reference or a short to ground, never a position — or a lost 5 V
// supply) throws the accepted position away, so recovery has to settle afresh rather than resume.
inline void decode_bands(Ctx& c, const void* cfg, State& st) {
    const auto* b = static_cast<const BandCfg*>(cfg);
    auto& s = st.bands;
    c.value = 0.0f;
    if (!c.valid || c.supply_lost) { s.acc = 0; s.init = false; c.valid = false; return; }
    // 0xFFFF is the board HAL's "no conversion" (published valid) — not a voltage, and not any band.
    if (c.raw.u > BOARD_ADC_FULL_SCALE) {
        s.acc = 0; s.init = false; c.valid = false; c.diag_tripped |= (1u << DIAG_SLOT_RAW_MAX); return;
    }
    uint8_t band = 0;                                         // 1-based; 0 = in no band
    for (uint8_t k = 0; k < b->bands; k++)
        if (c.raw.u >= b->edges[2 * k] && c.raw.u <= b->edges[2 * k + 1]) { band = static_cast<uint8_t>(k + 1); break; }
    if (!s.init || band != s.cand) { s.cand = band; s.cand_ms = c.now_ms; s.init = true; }
    const uint32_t in_cand = c.now_ms - s.cand_ms;
    if (band == 0) {
        // NOWHERE AT ALL — not merely crossing a gap. The channel goes INVALID, which is all a consumer
        // needs to shut down. It used to assert Raw High, which is a lie when the pin is sitting at
        // zero; it now says the thing that actually happened, and says it at the SENSOR, because the
        // sensor is the only thing that knows the voltage matched none of its own bands.
        if (in_cand >= BAND_GAP_MS) {
            s.acc = 0; c.valid = false; c.diag_tripped |= (1u << DIAG_SLOT_NO_BAND); return;
        }
    } else if (band != s.acc && in_cand >= BAND_SETTLE_MS) {
        s.acc = band; s.acc_ms = c.now_ms;                    // a new position, settled
    }
    if (s.acc == 0) { c.valid = false; return; }              // nothing settled yet — no claim, no fault
    c.value = static_cast<float>(b->pos[2 * (s.acc - 1)]) * b->val_scale;
    // STUCK: a pressed position (anything but rest) held past the limit — a jammed button or a shorted
    // ladder rung. Rest is where the switch lives, so holding it is not a fault however long it lasts.
    if (b->stuck_en && b->stuck_ms && c.value != 0.0f && (c.now_ms - s.acc_ms) >= b->stuck_ms) {
        c.valid = false; c.diag_tripped |= (1u << DIAG_SLOT_STUCK);
    }
}

// Simple linear transform — e.g. a CAN device's fixed scale (raw / 1024 -> lambda).
inline void decode_linear(Ctx& c, const void* cfg, State&) {
    const auto* k = static_cast<const LinearCfg*>(cfg);
    c.value = raw_as_float(c) * k->scale + k->offset;
}

// --- Condition (engineering -> engineering + validity) ---------------------------------

// Raw-window check: the raw scalar out of range -> invalid + report the slot.  (raw_diag.)
inline void cond_raw_window(Ctx& c, const void* cfg, State&) {
    const auto* w = static_cast<const RawWindowCfg*>(cfg);
    // Compared as a NUMBER, whatever kind the raw is — never as the bit pattern of .u.
    const float r = raw_as_float(c);
    if (w->en_min && r < static_cast<float>(w->min)) { c.valid = false; c.diag_tripped |= (1u << DIAG_SLOT_RAW_MIN); }
    if (w->en_max && r > static_cast<float>(w->max)) { c.valid = false; c.diag_tripped |= (1u << DIAG_SLOT_RAW_MAX); }
}

// Operating-window check: engineering value out of its healthy range -> raise the op DTC, but
// KEEP the signal VALID. The reading is real — the sensor works, the value is just unhealthy (e.g.
// an alternator overcharging to 16 V) — so gauges/telemetry/consumers still get the true value; the
// DTC carries the fault. This is the deliberate split from cond_raw_window, where an out-of-range
// RAW mV means the sensor itself is open/short/AWOL -> no trustworthy value -> invalid.
// Gated "under load" by the precondition Condition (Ctx.precond_armed, e.g. "rpm > 2500 AND map >
// 50"), so e.g. low oil pressure flags only under demand. Sets op_armed when it evaluates so the
// manager knows to raise/heal vs leave the op DTCs.
inline void cond_op_window(Ctx& c, const void* cfg, State&) {
    const auto* w = static_cast<const OpWindowCfg*>(cfg);
    if (!c.precond_armed) return;
    c.op_armed = true;
    if (w->en_min && c.value < w->min) { c.diag_tripped |= (1u << DIAG_SLOT_OP_MIN); }
    if (w->en_max && c.value > w->max) { c.diag_tripped |= (1u << DIAG_SLOT_OP_MAX); }
}

// Stuck check (switch): the debounced state (the value 0/1) held one state longer than the
// timeout -> invalid + report.  Self-contained: tracks the last state + change tick in State.
inline void cond_stuck(Ctx& c, const void* cfg, State& st) {
    const auto* w = static_cast<const StuckCfg*>(cfg);
    if (!w->en || w->stuck_ms == 0) return;
    const bool s = c.value >= 0.5f;
    if (st.sw.change_ms == 0) st.sw.change_ms = c.now_ms;     // seed
    if (s != st.sw.state) { st.sw.state = s; st.sw.change_ms = c.now_ms; }
    if ((c.now_ms - st.sw.change_ms) >= w->stuck_ms) { c.valid = false; c.diag_tripped |= (1u << DIAG_SLOT_STUCK); }
}

// Rate-of-change plausibility: |d(value)/dt| over the limit (units/s) -> invalid + report.
inline void cond_derivative(Ctx& c, const void* cfg, State& st) {
    const auto* w = static_cast<const DerivCfg*>(cfg);
    if (w->en && w->max_deriv > 0.0f && st.deriv.init && c.dt_ms > 0.0f) {
        const float rate = (c.value - st.deriv.last) / (c.dt_ms * 0.001f);
        if ((rate < 0.0f ? -rate : rate) > w->max_deriv) { c.valid = false; c.diag_tripped |= (1u << DIAG_SLOT_DERIV); }
    }
    st.deriv.last = c.value;
    st.deriv.init = true;
}

// Sentinel check: a device's "no reading" raw code (e.g. a WB1 0x7FFF = free-air)
// -> invalid.  Reads the raw scalar (still in Ctx after Decode), so place it anywhere
// after Acquire.
inline void cond_sentinel(Ctx& c, const void* cfg, State&) {
    const auto* s = static_cast<const SentinelCfg*>(cfg);
    if (s->en && c.raw.u == s->value) c.valid = false;
}

// NO FILTER STAGE HERE, DELIBERATELY. An EMA low-pass used to sit at the end of every
// sensor pipeline, driven by the per-sensor `filter_tau_ms` (default 50 ms). It is gone.
// A sensor publishes what it measured; smoothing is the CONSUMER's decision, because only
// the consumer knows whether it is fuelling off the value or closing a 500 Hz loop around
// it. One tau shared by both cannot be right for either — a 50 ms pole inherited from a
// thermistor default is what destabilised the electronic throttle into a full-travel limit
// cycle while every gain in the loop was correct.

// --- Publish ---------------------------------------------------------------------------

// Write the engineering value to the bus, carrying validity + freshness.  Runs even when
// invalid (the publish-invalid rule) — consumers + expire_stale() need the fresh status.
inline void publish(Ctx& c, const void* cfg, State&) {
    const auto* p = static_cast<const PublishCfg*>(cfg);
    if (!c.bus) return;
    if (c.supply_lost) c.valid = false;   // a failed 5 V reference: the number is not the sensor's
    c.bus->set(static_cast<SignalId>(c.signal), c.value, c.valid, c.now_ms, p->ttl_ms);
}

} // namespace pipe
