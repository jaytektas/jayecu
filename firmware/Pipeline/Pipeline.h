#pragma once
//
// Polymorphic pipeline — the one runtime primitive.  See
// docs/polymorphic-pipeline-architecture.md.
//
// An Input/Module/Output is a fixed-shape pipeline of polymorphic stages:
//
//     Trigger -> Acquire -> Decode -> Condition* -> Publish
//
// (Output mirrors it: Acquire-from-bus -> arbitrate -> Encode -> Emit.)  Each stage
// is a plain function pointer over a shared POD context; the pipeline is a flat,
// contiguous array of POD `Stage` records plus a parallel per-stage `State` blob.
// No heap, no vtables, no object graph — so the configuration engine can build a new
// pipeline into a spare slot and publish it with one atomic pointer flip (the same
// lock-free double-buffer used by the generic trigger).  Memory contract: install via
// a plain copy of POD inside a short interrupt-guarded critical section.
//
// Step 1 (this file): the engine + the stage CONTRACT.  Acquire normalises the source
// to a raw scalar (a tagged union); everything after Acquire is type-uniform, so
// Decode and Condition compose freely.  The Trigger is uniform-by-default (the caller
// polls run() each tick); event triggers just refresh a source cache that an Acquire
// reads on the next run().
//
#include <cstdint>
#include "../Signal/SignalBus.h"

#ifndef MAX_PIPELINE_STAGES
#define MAX_PIPELINE_STAGES 10   // Acquire + raw-win + Decode + EMA + op-win + deriv/stuck + Publish
#endif

namespace pipe {

// What an Acquire normalised the source into — the contract between Acquire and the
// rest of the chain.  Validated at config-build time (decode.consumes == acquire.produces).
enum RawKind : uint8_t { RAW_NONE = 0, RAW_U32, RAW_I32, RAW_F32 };

// Diagnostic check slots — the bit index in Ctx.diag_tripped, AND the severity-pack slot
// (diag_severity >> slot*2), AND the SensorDescriptor dtc_* order.  A diagnostic Condition
// sets its bit when it trips; the Input manager maps the bits to DTC raise/heal + severity.
enum DiagSlot : uint8_t {
    DIAG_SLOT_RAW_MIN = 0, DIAG_SLOT_RAW_MAX = 1,
    DIAG_SLOT_OP_MIN  = 2, DIAG_SLOT_OP_MAX  = 3,
    DIAG_SLOT_STUCK   = 4, DIAG_SLOT_DERIV   = 5,
    // NOT one of the six user checks, and deliberately outside them: it has no threshold to set and no
    // tick box, because it is not a judgement call. The voltage matched none of the bands the user
    // themselves defined, which is a structural fact about the calibration — the same shape as a
    // precondition expression that will not compile. Reported like that one, always.
    DIAG_SLOT_NO_BAND = 6,
};

// Transient context flowing down ONE pipeline for ONE run.  POD, stack-local per run().
struct Ctx {
    SignalBus* bus      = nullptr;     // set by run(); Publish writes through it
    uint16_t   signal   = SIG_NONE;    // the output SignalId this pipeline produces
    union { uint32_t u; int32_t i; float f; uint8_t bytes[8]; } raw{};
    uint8_t    raw_kind = RAW_NONE;    // how `raw` is populated (RawKind)
    float      value    = 0.0f;        // engineering value (Decode onward)
    bool       valid    = true;        // a Condition may clear it; still PUBLISHED (see below)
    bool       abort    = false;       // Acquire sets this when there is NO data at all
    uint16_t   ttl_ms   = 0;
    uint32_t   now_ms   = 0;           // current tick — filters / staleness / diag timing
    float      dt_ms    = 0.0f;        // ms since this pipeline last ran — EMA etc.
    bool       precond_armed = true;   // operating-window precondition Condition holds (set by run())
    uint8_t    diag_tripped = 0;       // bitmask of DiagSlot checks that tripped this run
    bool       op_armed = false;       // an operating-window Condition evaluated (under load)
    // A 5 V sensor reference is down (set by run()'s caller from the power-good lines). The reading is
    // then not the sensor's at all, so Publish marks it invalid whatever the stages concluded, and a
    // stateful Decode (the multi-position switch) discards what it had settled on.
    bool       supply_lost = false;
};

// Persistent per-stage state (EMA accumulator, switch debounce, derivative history).
// Lives ALONGSIDE the active pipeline buffer, so a live reconfigure (buffer flip) gets
// fresh, zeroed state for the new stage set.
union State {
    float f;
    struct { uint8_t cnt; bool state; uint32_t change_ms; } sw;
    struct { float last; uint32_t t; bool init; }        deriv;
    // Multi-position switch (decode_bands). Bands are held 1-based so the zeroed state is "none":
    // acc = the accepted band, cand = the band the reading is in now (0 = in no band).
    struct { uint8_t acc; uint8_t cand; bool init; uint8_t pad; uint32_t cand_ms; uint32_t acc_ms; } bands;
    uint8_t bytes[12];
    State() : bytes{} {}
};

// A stage: a function pointer + an immutable config pointer (into the flat config struct).
using StageFn = void (*)(Ctx&, const void* cfg, State& st);
struct Stage { StageFn fn = nullptr; const void* cfg = nullptr; };

// One producer's pipeline.  Flat POD: a fixed stage array + a parallel state array.
struct Pipeline {
    uint16_t signal = SIG_NONE;
    uint8_t  n      = 0;
    Stage    stages[MAX_PIPELINE_STAGES];
    State    state[MAX_PIPELINE_STAGES];

    void reset_state() { for (auto& s : state) s = State{}; }

    // Run the pipeline once.  Walk the stages in order; the value lands on the bus via
    // the Publish stage at the tail.
    //
    // PUBLISH-INVALID RULE: a Condition that sets `valid = false` does NOT abort — the
    // context flows all the way to Publish so the invalid status is broadcast to the bus
    // (the output/arbitration side and the TTL/staleness logic need to see a source go
    // dark).  Only `abort` (Acquire found NO data — unassigned source, no frame) stops
    // the walk early, and then nothing is published.
    //
    // Returns the final Ctx so a caller running a value-only pipeline (no Publish stage)
    // can read the produced raw + value (e.g. the Input manager that does its own
    // diagnostics + publish on top of the value-production pipeline).
    Ctx run(SignalBus& bus, uint32_t now_ms, float dt_ms, bool precond_armed = true,
            bool supply_lost = false) {
        Ctx c;
        c.bus = &bus; c.signal = signal; c.now_ms = now_ms; c.dt_ms = dt_ms; c.precond_armed = precond_armed;
        c.supply_lost = supply_lost;
        for (uint8_t i = 0; i < n && i < MAX_PIPELINE_STAGES; i++) {
            if (stages[i].fn) stages[i].fn(c, stages[i].cfg, state[i]);
            if (c.abort) break;   // no data — skip the rest, publish nothing
        }
        return c;
    }
};

} // namespace pipe
