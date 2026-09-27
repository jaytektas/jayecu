// TriggerConfigCheck — CAN THIS CONFIGURATION EVER KNOW WHERE THE ENGINE IS?
//
// Position comes from an ANCHOR: some stream that can state its own position within its pattern, not
// merely count along it. A wheel with a unique feature can; an even one cannot, and no amount of
// counting will turn one into the other. That is a property of the CONFIGURATION, knowable the moment
// a tune is applied — so it is answered here, once, rather than discovered by an engine that will not
// run right and a tuner wondering why.
//
// The useful output is not a yes/no but the HIGHEST SYNC LEVEL this configuration can reach:
//
//   NONE   nothing here can ever fix position. Refuse to arm firing.
//   CRANK  position within a revolution. Wasted spark, batch / semi-sequential.
//   PHASE  position within the whole cycle. Sequential, COP, per-cylinder anything.
//
// so "this wheel can never do sequential" is answered on the bench instead of by a car that runs
// badly on one bank.
//
// Pure and header-only: no decoder state, no hardware. The same function the firmware gates firing on
// can be run by a tune editor against a configuration nobody has flashed yet.
#pragma once
#include <cstdint>
#include "SchedulerTypes.h"
#include "GenericTrigger.h"      // MAX_STREAMS, StreamRepeats, REPEATS_PHASE, SyncLvl
#include "modules/trigger_config.h"

enum class TriggerConfigFault : uint8_t {
    NONE = 0,
    NO_STREAMS,          // nothing enabled at all
    NO_ANCHOR,           // every source is relative: the engine can never be located
    ONLY_PHASED_ANCHOR,  // the only absolute source is a phaser, which is not a reference
    EVEN_WHEEL_COILS,    // a single even wheel, but Ignition Mode is not Distributor (or the teeth do not fit it)
    EVEN_WHEEL_INJECTION,// a single even wheel, but an injection stage times each injector to its cylinder
};

// What the ENGINE is, as far as a single even wheel is concerned. Default-constructed it says nothing,
// and the check behaves exactly as it did before sync-always existed.
struct TriggerEngineShape {
    bool    known     = false;
    uint8_t ign_mode  = 0;       // IgnitionCoilMode
    uint8_t ncyl      = 0;
    bool    even_fire = true;
    bool    rotary    = false;
    // An active injection stage times its injectors to their OWN cylinders: Sequential, Semi-Sequential
    // or Bank. A sync-always wheel cannot say which cylinder is which, so those would run on a timing
    // the engine cannot have. Multi-Point and Sequential (any sync) are the ones that fit.
    bool    cyl_timed_injection = false;
};

struct TriggerConfigCheck {
    SyncLvl            ceiling = SyncLvl::NONE;   // the best this configuration can ever reach
    TriggerConfigFault fault   = TriggerConfigFault::NONE;
    // SYNC ALWAYS: the tune is a single even wheel whose every tooth is a sync tooth (see below). The
    // decoder is told so, and takes full sync on the first tooth instead of waiting for an anchor that
    // this wheel will never produce.
    bool               sync_always = false;
    [[nodiscard]] bool can_fire() const noexcept { return ceiling != SyncLvl::NONE; }
};

// Can a stream of this shape state its own position, or only count along it?
//
//   GAP      a gap is a unique feature -> absolute. No gaps is an even wheel -> relative, and that
//            is every distributor, a flywheel ring gear, and the Nissan CAS fine track.
//   SEQUENCE an asymmetric cell has a unique origin -> absolute. A uniform one does not: it is an
//            even wheel written the long way round.
//   WIDTH    the matched pulse IS the reference -> absolute by construction.
[[nodiscard]] inline bool stream_can_anchor(const StreamsConfig& c) noexcept {
    if (c.primitive == 0) return c.cell_len > 0;               // GAP
    if (c.primitive == 2) return true;                          // WIDTH
    if (c.cell_len < 2) return false;                           // SEQUENCE needs a pattern
    for (uint8_t k = 1; k < c.cell_len && k < MAX_PATTERN_TEETH; ++k)
        if (c.cell[k].v != c.cell[0].v) return true;            // asymmetric -> unique origin
    return false;                                               // uniform -> relative, like an even wheel
}

// Events per ENGINE CYCLE on an even stream (a GAP with no gaps, or a uniform SEQUENCE).
[[nodiscard]] inline uint16_t even_teeth_per_cycle(const StreamsConfig& c, StreamRepeats rate) noexcept {
    const uint16_t per = (c.primitive == 0) ? c.slots : (c.primitive == 1 ? c.cell_len : 0);
    return static_cast<uint16_t>(per * (rate ? rate : 1));
}

// A SINGLE EVEN WHEEL — SYNC ALWAYS, which is DISTRIBUTOR mode. A distributor, or any wheel whose
// teeth are all alike, cannot say which tooth is which, and with one coil and a rotor it does not need
// to: the rotor picks the plug, so every tooth is a sync tooth and the first one syncs the engine. There
// is no half or full sync to earn. It needs only that every tooth stands in the same place relative to a
// TDC — the tooth pitch a whole multiple of the firing interval, i.e. the teeth per cycle divide the
// cylinder count — on an even-fire, non-rotary engine.
[[nodiscard]] inline bool even_wheel_sync_always(uint16_t tpc, AngleDeg10 /*cycle*/,
                                                 const TriggerEngineShape& e) noexcept {
    if (!e.known || !e.even_fire || e.rotary || tpc == 0 || e.ncyl == 0) return false;
    return static_cast<IgnitionCoilMode>(e.ign_mode) == IgnitionCoilMode::SINGLE_COIL_DISTRIBUTOR
        && (e.ncyl % tpc) == 0;
}

// …and the fuel must not need to know either (see TriggerEngineShape::cyl_timed_injection).
[[nodiscard]] inline bool even_wheel_injection_ok(const TriggerEngineShape& e) noexcept {
    return !e.cyl_timed_injection;
}

[[nodiscard]] inline TriggerConfigCheck check_trigger_config(
        const StreamsConfig* streams, uint8_t n, AngleDeg10 cycle,
        const TriggerEngineShape& engine = TriggerEngineShape{}) noexcept
{
    TriggerConfigCheck out;
    if (n > MAX_STREAMS) n = MAX_STREAMS;

    bool any = false;
    bool crank_present = false, crank_anchor = false;
    bool phase_id = false;          // a cam that can identify the cycle (a phaser still can)
    bool phase_anchor_fixed = false;// a cam that can also ANCHOR (fixed phase only)
    uint8_t enabled_n = 0;          // how many streams the tune runs
    uint16_t even_tpc = 0;          // teeth per cycle of the last even (non-anchoring) stream

    for (uint8_t i = 0; i < n; ++i) {
        const StreamsConfig& c = streams[i];
        if (!c.enabled) continue;
        any = true;
        // The slot is the role, and the role gives the default rate; `repeats` states it outright for
        // a wheel that is neither (a symmetrical pattern, a distributor on an odd-cylinder engine).
        const StreamRepeats dflt = role_is_cam_slot(i)
            ? REPEATS_PHASE
            : static_cast<StreamRepeats>(cycle / ANGLE_360);
        const StreamRepeats rate = c.repeats ? c.repeats : (dflt ? dflt : REPEATS_PHASE);
        const bool anchors = stream_can_anchor(c);
        ++enabled_n;
        if (!anchors) even_tpc = even_teeth_per_cycle(c, rate);

        if (rate == REPEATS_PHASE) {
            if (anchors) {
                phase_id = true;
                // A PHASER IS NOT A REFERENCE. It can still say which revolution — its travel is tens
                // of degrees and cannot cross a 360 boundary — but it cannot fix a position, because
                // the position it reports moves whenever the phaser is commanded to move.
                if (!c.phased) phase_anchor_fixed = true;
            }
        } else {
            crank_present = true;
            if (anchors) crank_anchor = true;
        }
    }

    if (!any) { out.fault = TriggerConfigFault::NO_STREAMS; return out; }

    // One stream, and it is even: sync always, if the coils allow it (see even_wheel_sync_always).
    if (enabled_n == 1 && !crank_anchor && !phase_id) {
        if (even_wheel_sync_always(even_tpc, cycle, engine)) {
            if (!even_wheel_injection_ok(engine)) {
                out.fault = TriggerConfigFault::EVEN_WHEEL_INJECTION; return out;
            }
            out.ceiling = SyncLvl::PHASE; out.sync_always = true; return out;
        }
        if (engine.known) { out.fault = TriggerConfigFault::EVEN_WHEEL_COILS; return out; }
    }

    if (!crank_present) {
        // Cam-only (a CAS): the cam is the fine source and the reference both.
        out.ceiling = phase_anchor_fixed ? SyncLvl::PHASE : SyncLvl::NONE;
        if (out.ceiling == SyncLvl::NONE)
            out.fault = phase_id ? TriggerConfigFault::ONLY_PHASED_ANCHOR
                                 : TriggerConfigFault::NO_ANCHOR;
        return out;
    }

    if (crank_anchor) {
        // The crank locates itself. A cam adds which revolution, and a phased one may do that.
        out.ceiling = phase_id ? SyncLvl::PHASE : SyncLvl::CRANK;
        return out;
    }

    // A relative crank: it counts, and something else must tell it where it is counting from. Only a
    // FIXED-phase cam can, and when it does it supplies the revolution in the same breath.
    if (phase_anchor_fixed) { out.ceiling = SyncLvl::PHASE; return out; }

    out.fault = phase_id ? TriggerConfigFault::ONLY_PHASED_ANCHOR
                         : TriggerConfigFault::NO_ANCHOR;
    return out;                                   // ceiling stays NONE: refuse to arm firing
}
