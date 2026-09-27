// build_generic() — construct a GenericTrigger from the schema's generic stream config
// (StreamsConfig[] + the shared CellPoolConfig[]). The bridge from tune config → decoder;
// EnginePositionHal calls this at start()/reconfigure() (then binds each stream's capture
// channel by capture_index into the board pool). Pure, header-only, host-testable.
#pragma once
#include "GenericTrigger.h"
#include "modules/trigger_config.h"

// The config can never carry more streams than the decoder core holds — otherwise the trailing
// streams[] would be silently dropped (build_generic caps n at MAX_STREAMS). Derive the config
// count from the generated struct so bumping the schema's streams count without MAX_STREAMS is a
// COMPILE error, not a quiet runtime truncation.
static_assert(sizeof(TriggerConfig::streams) / sizeof(StreamsConfig) <= MAX_STREAMS,
              "MAX_STREAMS must be >= the schema's trigger streams[] count");
// A stream's cell array is sized to the SEQUENCE cap on purpose: every cell the studio can write is
// one the decoder can read. Sizing it larger would let a tune carry a 40-tooth pattern that silently
// decodes as 32 — the kind of quiet truncation that reads as a decoder bug for weeks.
static_assert(sizeof(StreamsConfig::cell) / sizeof(CellSub) == MAX_PATTERN_TEETH,
              "a stream's cell[] must match MAX_PATTERN_TEETH exactly");

// `cycle` is the engine cycle span (engine_cycle_angle of the tune's cycle_type). It must be set
// BEFORE the streams are added: a cam-rate stream's period IS the cycle, so building the streams
// first would configure them against the wrong wheel.
//
// The cells are PER STREAM. They used to be runs inside one shared pool, addressed by a cell_off the
// tune carried — which meant two streams could point at the same cells, describe the same wheel, and
// nothing here could tell. A private array removes both the offset and the failure mode: each stream
// reads its own cells from 0, and no arithmetic can make two of them collide.
inline void build_generic(GenericTrigger& g,
                          const StreamsConfig* streams, uint8_t n,
                          AngleDeg10 cycle = ANGLE_720) noexcept {
    g.reset_all();
    g.set_cycle(cycle);
    if (n > MAX_STREAMS) n = MAX_STREAMS;
    for (uint8_t i = 0; i < n; ++i) {
        const StreamsConfig& c = streams[i];
        if (!c.enabled) continue;                         // a slot whose role is not on this engine
        // THE INDEX IS THE ROLE, and the ROLE gives the DEFAULT repeat count — but only the default.
        // A crank-mounted wheel normally repeats once per revolution and a cam-mounted one once per
        // cycle, and `repeats` states it outright when the wheel is neither: a symmetrical crank
        // pattern, a 12-tooth Honda, or a distributor on an odd-cylinder engine. Placed AT its slot,
        // never appended: the position HAL hands on_edge the config index.
        const StreamRepeats dflt = role_is_cam_slot(i)
            ? REPEATS_PHASE
            : static_cast<StreamRepeats>(cycle / ANGLE_360);   // 2 four-stroke, 1 two-stroke, 3 rotary
        const StreamRepeats rate = c.repeats ? c.repeats : (dflt ? dflt : REPEATS_PHASE);
        // What this slot physically IS, before what it decodes like. A cam's parked reference angle
        // and whether a phaser can move it decide two things the primitive cannot: whether it may
        // anchor a crank that has no feature of its own, and how far its sync is allowed to wander
        // from nominal before that is a mechanical fault rather than the phaser doing its job.
        g.set_phase_meta(i, c.nominal_angle, c.phased != 0, c.phase_authority, c.phase_allowance);
        g.set_window_pcts(i, c.window_pct, c.window_crank_pct);

        if (c.primitive == 0) {                           // GAP (subsumes EVEN/MISSING/multi-gap)
            uint8_t gi[MAX_ANOMALIES] = {}, gr[MAX_ANOMALIES] = {};
            uint8_t ng = (c.cell_len <= MAX_ANOMALIES) ? c.cell_len : MAX_ANOMALIES;
            for (uint8_t k = 0; k < ng; ++k) {
                gi[k] = static_cast<uint8_t>(c.cell[k].v);
                gr[k] = c.gap_ratio ? c.gap_ratio : 2;
            }
            g.add_gap(i, rate, c.slots, gi, gr, ng, c.window_pct);
        } else if (c.primitive == 1) {                    // SEQUENCE (odd-fire / cam-pulse)
            AngleDeg10 cell[MAX_PATTERN_TEETH] = {};
            uint8_t nc = (c.cell_len <= MAX_PATTERN_TEETH) ? c.cell_len : MAX_PATTERN_TEETH;
            for (uint8_t k = 0; k < nc; ++k) cell[k] = static_cast<AngleDeg10>(c.cell[k].v);
            g.add_seq(i, rate, cell, nc, c.window_pct);
        } else {                                          // WIDTH (pulse-width id)
            const bool lead_rising = (c.edge != 1);       // 0 rising / 1 falling / 2 both
            g.add_width(i, rate, c.width_min, c.width_max, c.width_target, lead_rising);
        }
    }
}
