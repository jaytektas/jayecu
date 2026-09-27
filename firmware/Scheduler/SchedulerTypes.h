#pragma once
#include <cstdint>
#include <cstring>
#include <array>
#include "../../generated/signal_enums.h"   // SyncLevel (schema-sourced; NONE/CRANK/PHASE)
#include "../../generated/boards/board.h"   // BOARD_MAX_CYLINDERS — hardware cylinder ceiling

// ---------------------------------------------------------------------------
// Decidegree angle representation (×10 fixed-point)
// 1 unit = 0.1 degrees. Range: -3276.8 .. +3276.7 degrees (int16_t).
// Full engine cycle: 720 degrees = 7200 units.
// Invariant for absolute crank angles: 0 <= value < ANGLE_720.
// Signed values are used for offsets and deltas.
// ---------------------------------------------------------------------------
using AngleDeg10    = int16_t;
using FractionQ0_15 = uint16_t; // 0..32767 represents 0.0..(1 - 1/32768)

// ---------------------------------------------------------------------------
// System-wide constants
// ---------------------------------------------------------------------------
inline constexpr AngleDeg10 ANGLE_720               = 7200; // 720° = full 4-stroke cycle
inline constexpr AngleDeg10 ANGLE_360               = 3600; // 360° = full 2-stroke cycle
// A Wankel's eccentric shaft turns THREE times per rotor revolution, and each of the three faces is
// its own combustion chamber — its own leading/trailing timing, its own seal condition. The pattern
// therefore repeats every 1080° of eccentric shaft rotation, and that is the span you need to
// address a FACE rather than just "some face". 10800 decidegrees still fits AngleDeg10 (int16).
inline constexpr AngleDeg10 ANGLE_1080              = 10800; // rotary: one rotor rev, in e-shaft deg
inline constexpr AngleDeg10 ANGLE_180               = 1800; // 180°

// THE FIRING CEILINGS ARE HARDWARE FACTS, so they come from the board rather than being
// written here. MAX_CYLINDERS already did; these two did not, and were 12 and 22 — which are
// jaytek_v1's ignition and low-side pin counts. A board with sixteen low-sides tripped the
// output-pool static_assert in its profile, which is the good outcome; what the literal would
// have done unchecked is size every injection mask, row base and per-channel array for pins
// that board does not have.
inline constexpr int  MAX_CYLINDERS                 = BOARD_MAX_CYLINDERS;   // board IGN pins
inline constexpr int  MAX_CAM_CHANNELS              = 4;
inline constexpr int  MAX_IGN_CHANNELS              = BOARD_IGNITION_COUNT;
inline constexpr int  MAX_INJ_CHANNELS              = BOARD_LOW_SIDE_COUNT;
// Virtual teeth per cycle, sized for the LONGEST cycle: a rotary's 1080° at the finest 10° grid =
// 108. A four-stroke uses 72 of them, a two-stroke 36. Sizing this at 72 would have silently
// truncated a rotary's schedule to the first 720° — two thirds of a rotor revolution.
inline constexpr int  MAX_VTEETH                    = 108; // 1080°/10° (rotary); 4-stroke uses 72

// Per-stream ROLE — and it is the stream's INDEX, not a field it carries. trigger.streams[] has one
// slot per role, so the identity of a stream is where it sits and nothing can disagree with it.
//
// Kept as an enum for readable slot constants and to name the schema's element_labels. There is no
// Unused member: absence is `enabled == 0` on the slot, which is one rule for one question. Cam
// slots index cam[0..MAX_CAM_CHANNELS-1] directly, intake/exhaust per bank, so VVT cam identity is
// stable and an engine with phasers on the intakes alone enables slots 2 and 4 and leaves 3 empty.
enum class StreamSlot : uint8_t {
    CrankPrimary = 0, CrankSecondary = 1,
    CamIntakeB1 = 2, CamExhaustB1 = 3, CamIntakeB2 = 4, CamExhaustB2 = 5,
};

// THE STREAM INDEX IS THE ROLE. trigger.streams[] has one slot per role — 0 Crank Primary,
// 1 Crank Secondary, 2..5 the four cams — so a stream's identity is where it sits and there is no
// role field to disagree with it. Two streams can no longer claim one role, which they previously
// did in silence: the assignment resolver simply overwrote the first with the second.
//
// These take the SLOT, not a role value. They are the whole of the mapping that used to exist.
inline constexpr bool role_is_crank_slot(int i) noexcept { return i >= 0 && i < 2; }
inline constexpr bool role_is_cam_slot(int i)   noexcept { return i >= 2 && i < 6; }
inline constexpr int  crank_slot_of(int i) noexcept { return role_is_crank_slot(i) ? i : -1; }
inline constexpr int  cam_slot_of(int i)   noexcept { return role_is_cam_slot(i) ? i - 2 : -1; }

// Tune-selectable injection events per cycle (MULTI_POINT / BATCH only) — the ceiling on a stage's
// inj_stage[].injections_per_cycle, and one of the two terms that size the event-node pool below. 3
// covers a rotary firing once per revolution; 4 leaves room for multi-squirt without growing every board.
inline constexpr int  MAX_INJ_EVENTS_PER_CYCLE      = 4;

// Scheduler node pool — DERIVED from the ceilings, never hardcoded, so raising the cylinder ceiling
// resizes the pool automatically.
//
// IGNITION IS PER CYLINDER; INJECTION IS PER EVENT. A coil belongs to a cylinder no matter how the
// engine is configured, so an ignition node can be addressed by cylinder index. An injection event
// cannot: in MULTI_POINT one event drives every injector, in BATCH one drives half of them, and in
// SEMI_SEQUENTIAL one cylinder's injector needs SEVERAL events per cycle. Indexing injection nodes by
// cylinder made the per-cylinder case the only representable one — which is why batch and
// multi-point could not be expressed at all. The two pools are therefore sized and indexed apart.
//
// Per cylinder (all features on — rotary leading + trailing):
//   STATIC  : IGN_SCHEDULE (re-arms leading + trailing)                       = 1
//   VOLATILE: DWELL(lead)+SPARK(lead) + DWELL(trail)+SPARK(trail)             = 4
//             + KNOCK_WINDOW (per-cyl knock sample window, drives no pin)     = 1
inline constexpr int  SCHED_NODES_PER_CYLINDER      = 6;
// Injection stages (engine.inj_stage[]). Stage 0 is the primary; stages 1..N-1 are the staged
// secondaries, each compiled into its own events from the injector rows that name it. MAX_INJ_STAGES
// mirrors the schema array count; MAX_STAGED_STAGES is how many secondaries an event carries.
inline constexpr int  MAX_INJ_STAGES                = 4;
inline constexpr int  MAX_STAGED_STAGES             = MAX_INJ_STAGES - 1;   // stages 2..4
// Per injection event:
//   STATIC  : INJ_SCHEDULE (arms this stage's open)                           = 1
//   VOLATILE: INJ_OPEN (this stage's channels)                                = 1
inline constexpr int  SCHED_NODES_PER_INJ_EVENT     = 2;

// Each injection STAGE is scheduled independently on its OWN mode, so events SUM over the active
// stages (no longer one bundled primary set with staged sub-nodes). Per-stage worst case =
// groups × events-per-group, maximised over the modes:
//   SEQUENTIAL family — MAX_CYLINDERS groups (one per cylinder) × revs per cycle (semi-seq doubles;
//                       the rotary's 1080° cycle spans the most revolutions).
//   MULTI_POINT/BANK — at most 2 groups × MAX_INJ_EVENTS_PER_CYCLE, far smaller.
// Pool = MAX_INJ_STAGES × the per-stage worst case, so no stage/mode can move without resizing.
inline constexpr int  MAX_REVS_PER_CYCLE            = ANGLE_1080 / ANGLE_360;   // 3 (rotary)
inline constexpr int  MAX_INJ_EVENTS_SEQ            = MAX_CYLINDERS * MAX_REVS_PER_CYCLE;
inline constexpr int  MAX_INJ_EVENTS_GROUPED        = 2 * MAX_INJ_EVENTS_PER_CYCLE;
inline constexpr int  MAX_INJ_EVENTS_PER_STAGE      = (MAX_INJ_EVENTS_SEQ > MAX_INJ_EVENTS_GROUPED)
                                                    ?  MAX_INJ_EVENTS_SEQ : MAX_INJ_EVENTS_GROUPED;
inline constexpr int  MAX_INJ_EVENTS                = MAX_INJ_STAGES * MAX_INJ_EVENTS_PER_STAGE;

inline constexpr int  SCHED_MAX_NODES               = MAX_CYLINDERS * SCHED_NODES_PER_CYLINDER
                                                    + MAX_INJ_EVENTS * SCHED_NODES_PER_INJ_EVENT;

// STATIC schedule-node lead before the cylinder's spark (decidegrees). It only
// needs to exceed the maximum spark ADVANCE — NOT the dwell — because the dwell is
// rescheduled at discharge (the previous spark re-arms the next dwell, giving it a
// full cycle of lead, so dwell length is unconstrained: full-cycle dwelling is
// possible). 90° covers any sane advance (≈88° headroom even at 25k RPM).
inline constexpr AngleDeg10 SCHED_COMPUTE_LEAD      = 900; // 90°

// Bitmask over logical output channel indices (one bit per IGN or INJ channel).
// Width DERIVED from the channel ceiling: a board's low-side count can exceed 16
// (jaytek_v1 has 22), and one SchedNode carries both an ignition and an injection
// mask field, so a single 32-bit type covers either. The static_assert fires loudly rather than
// silently truncating a coil/injector bit if a ceiling ever exceeds 32.
using OutputMask = uint32_t;
static_assert(MAX_IGN_CHANNELS <= 32 && MAX_INJ_CHANNELS <= 32,
              "OutputMask (uint32_t) too narrow for the channel count");
inline constexpr int  MAX_ANOMALIES                 = 8;
// Odd-Fire unit cell: the longest repeating inter-tooth pattern the decoder will hold
// (ONE pattern live at a time — TS keeps the wheel library client-side and pushes the
// active cell). 32 covers any odd-fire (a 12-cyl odd-fire is 12 events) with margin.
// Steady-state decode is O(1) regardless of this; only sync acquisition scans the cell.
inline constexpr int  MAX_PATTERN_TEETH             = 96;
// A Wankel rotor has THREE faces, and each face is a combustion chamber in its own right: it fires
// once per rotor revolution (per 1080 deg of eccentric shaft), takes its own charge, and therefore
// needs its own scheduler slot. The three faces of one rotor SHARE that rotor's leading and trailing
// coils — the plugs are fixed in the housing, not on the faces.
//
// So the rotor ceiling is not an independent number, it is the cylinder-slot pool divided by the
// faces per rotor. Declaring it separately means it agrees with the pool on this board and silently
// disagrees on the next one.
inline constexpr int  FACES_PER_ROTOR              = 3;
inline constexpr int  MAX_ROTORS                   = MAX_CYLINDERS / FACES_PER_ROTOR;
// The other ceiling a rotary leans on: two coils per rotor (leading + trailing). Not binding today
// (12 channels covers 6 rotors, the slot pool only 4), but it fires loudly if either moves.
static_assert(MAX_ROTORS * 2 <= MAX_IGN_CHANNELS,
              "a rotary needs a leading + trailing coil per rotor");
static_assert(MAX_ROTORS >= 1, "the cylinder-slot pool cannot hold a single rotor's three faces");

// Sentinel: channel index meaning "not wired / unused".
inline constexpr uint8_t CHANNEL_UNUSED             = 0xFFu;
inline constexpr int  SYNC_REQUIRED_TEETH           = 3;
inline constexpr int  MAX_CONSECUTIVE_ERRORS        = 4;

// Scheduler trigger window (decidegrees, 1 unit = 0.1°)
inline constexpr AngleDeg10 SCHED_WINDOW_MIN        = 150;  // 15°
inline constexpr AngleDeg10 SCHED_WINDOW_MAX        = 600;  // 60°
inline constexpr AngleDeg10 SCHED_WINDOW_DEF        = 300;  // 30°

// VVT fault threshold: displacements beyond ±60° are sensor faults
inline constexpr AngleDeg10 VVT_FAULT_THRESHOLD     = 600;

// Minimum tick margin to avoid missed-compare race on Cortex-M7 (3-cycle pipeline)
inline constexpr uint32_t   MIN_TICKS_AHEAD         = 5;

// ---------------------------------------------------------------------------
// Compile-time sanity checks
// ---------------------------------------------------------------------------
static_assert(sizeof(AngleDeg10) == 2, "AngleDeg10 must be 16-bit");
static_assert(ANGLE_720 == 7200,   "720 degrees must equal 7200 decidegree units");
static_assert(ANGLE_360 == 3600,   "360 degrees must equal 3600 decidegree units");

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

enum class SyncState : uint8_t {
    UNSYNCHRONIZED = 0, // No crank motion detected
    SYNCHRONIZING  = 1, // Crank pattern found; verifying speed/consistency
    SYNCHRONIZED   = 2  // Position tracking active (at least Crank-level)
};

// Represents the "Confidence Level" of the engine position. SyncLevel (NONE=0 / CRANK=1 / PHASE=2)
// is generated into signal_enums.h from the schema enum `sync_level` — the same value-set the TS
// precondition picker shows.
//   NONE  — No firing possible
//   CRANK — Crank-only sync (360° awareness). Firing: Wasted Spark / Batch.
//   PHASE — Full 720° phase sync. Firing: Sequential / COP / Staged.

enum class EngineCycleType : uint8_t {
    TWO_STROKE  = 0, // 360° cycle — every cylinder fires every revolution
    FOUR_STROKE = 1, // 720° cycle
    ROTARY      = 2  // 1080° cycle (3 eccentric-shaft revs = 1 rotor rev, 3 individually-timed faces)
};

// (SyncStrategy enum removed — the generic-trigger decoder replaces the strategy zoo with a
// pool of stream primitives: GAP / SEQUENCE / WIDTH fused into position. See GenericTrigger.h.)

enum class OutputAction : uint8_t {
    DRIVE_LOW  = 0,
    DRIVE_HIGH = 1,
    TOGGLE     = 2, 
    NO_CHANGE  = 3  
};

enum class EventCategory : uint8_t {
    CYLINDER_RELATIVE = 0, 
    CRANK_ABSOLUTE    = 1  
};

enum class EventAction : uint8_t {
    IGN_DWELL_START = 0,
    IGN_SPARK       = 1,
    INJ_OPEN        = 2,
    INJ_CLOSE       = 3,
    ADC_TRIGGER     = 4,
    CUSTOM_CALLBACK = 5,
    IGN_SCHEDULE    = 6, // STATIC re-arm: re-inserts a cylinder's dwell-start + spark
    INJ_SCHEDULE    = 7  // STATIC re-arm: re-inserts a cylinder's injector-open
};

// Persistent-skeleton node taxonomy (mirrors the reference schedule_header_t):
//   STATIC nodes re-arm (return to READY after firing, stay in the list);
//   VOLATILE nodes are one-shots (removed on fire, re-inserted by a STATIC node).
enum class EventType  : uint8_t { VOLATILE = 0, STATIC = 1 };
enum class EventState : uint8_t { UNSCHEDULED = 0, READY = 1, WAIT = 2, RUN = 3 };

// In-bucket tie-break order (lower fires first when two nodes share a sub_offset).
// SCHEDULE must precede the fire events it inserts.
enum class EventPriority : uint8_t {
    SCHEDULE  = 0,
    DWELL     = 1,
    DISCHARGE = 2,
    INJECT    = 3,
    SAMPLE    = 4
};

// HOW THE COILS ARE WIRED (engine.ign_mode). The studio lays the coil rows out from it; the scheduler
// reads it for one thing only: under WASTED_SPARK a coil row names one cylinder and fires for that
// cylinder's companion too (EventScheduler::resolve_binding). A rotary's trailing plug is the engine
// cycle's business (EngineCycleType::ROTARY), not a value here.
enum class IgnitionCoilMode : uint8_t {
    SINGLE_COIL_DISTRIBUTOR = 0,
    WASTED_SPARK            = 1,
    COIL_ON_PLUG            = 2,
};

// HOW THE FUEL IS DISTRIBUTED OVER THE OUTPUTS, and nothing else. STAGED used to be a fourth value
// how ONE injection STAGE fires its outputs. Each stage (engine.inj_stage[]) carries its own mode; which
// injectors belong to it and which cylinders each serves are the output rows' (OutputMap.h), so a mode
// only decides which of them fire TOGETHER and how many times a cycle. A conventional per-stage
// Injection Mode selector (order preserved so the enum value == the schema option index).
//
// The differences all fall out of build_inj_events(): which outputs an event drives, and how many
// events a cycle holds (injection_events_per_cycle).
enum class InjectionMode : uint8_t {
    SEQUENTIAL          = 0,  // each injector once/cycle at its cylinder's TDC (needs PHASE)
    SEMI_SEQUENTIAL     = 1,  // each injector twice/cycle (companion pair coincides); crank sync
    MULTI_POINT         = 2,  // every output together, N events/cycle
    BANK                = 3,  // outputs grouped by cyl[i].bank; each bank fired together, N events each
    SEQUENTIAL_ANY_SYNC = 4,  // once/cycle at A cylinder's TDC (not necessarily its own); any sync
};


enum class CaptureEdge : uint8_t {
    RISING  = 0,
    FALLING = 1,
    BOTH    = 2
};

// ---------------------------------------------------------------------------
// Configuration structs
// ---------------------------------------------------------------------------

inline constexpr int MAX_SYNC_PATTERNS = 8;

// Trigger-wheel + cylinder-firing configuration now lives in the schema and is
// read DIRECTLY from g_config.trigger (TriggerConfig) and g_config.cylinders
// (CylindersConfig) — one source, no copy. The decoder/scheduler/resolver bind
// references to those generated structs (see generated/modules/{trigger,cylinders}_config.h).
// The hand-written TriggerWheelConfig/CylinderConfig/SyncPattern/AnomalyEntry that
// used to live here are gone; the enums above are the shared value contract.

struct ScheduledEvent {
    EventCategory category;
    EventAction   action;
    AngleDeg10    target_angle;     
    uint32_t      time_domain_us;   
    uint8_t       channel_index;    
    uint8_t       cylinder_index;   
    bool          armed;
    bool          active;
    void (*callback)(void* user_data);
    void*         user_data;
    // Bucket scheduler (filled by EventScheduler::build_buckets): bucket = the
    // virtual-tooth index this event lives in; sub_offset = fraction of a virtual
    // tooth past that tooth's angle (Q0.15) at which it fires.
    uint8_t       bucket;
    FractionQ0_15 sub_offset;
};

// ---------------------------------------------------------------------------
// SchedNode — one node in the persistent intrusive-list scheduler skeleton.
//
// Lives in EventScheduler::node_pool_[SCHED_MAX_NODES]; never heap-allocated.
// STATIC nodes (IGN_SCHEDULE/INJ_SCHEDULE) re-arm every cycle and re-insert the
// VOLATILE fire nodes (IGN_DWELL_START/IGN_SPARK/INJ_OPEN) from live shadow-reg
// state — the self-correcting chain, replacing the per-cycle pool rebuild.
//
// All list mutation runs in the grid/match ISR (priority 2), which cannot
// preempt itself, so the intrusive list needs no lock and no double-buffer.
//
// bucket is the 360°-folded grid index (0..MAX_VTEETH-1) so a node is reachable
// on both CRANK grid passes; target_angle keeps the full 720° value for PHASE
// half-discrimination. crank_mask/phase_mask are the dual Model-A dispatch sets
// (wasted-pair vs configured COP/sequential), selected by phase_known_.
// ---------------------------------------------------------------------------
struct SchedNode {
    SchedNode*    next;
    SchedNode*    prev;
    EventType     type;
    EventState    state;
    EventAction   action;
    EventPriority priority;
    uint8_t       cyl_index;
    uint8_t       bucket;          // 0..MAX_VTEETH-1 (360°-folded for CRANK reach)
    FractionQ0_15 sub_offset;      // fraction into the bucket (Q0.15)
    AngleDeg10    target_angle;    // full 720° angle (PHASE half-discrimination)
    OutputMask    crank_mask;      // channels fired when phase_known_ == false (wasted)
    OutputMask    phase_mask;      // channels fired when phase_known_ == true (COP/seq)
    uint32_t      time_domain_us;  // INJ pulse width in µs (0 for ignition)
    void (*callback)(SchedNode*);  // STATIC schedule callback (nullptr for VOLATILE)
};

// ---------------------------------------------------------------------------
// InjEvent — one compiled injection event: the unit the injection schedule is actually made of.
//
// Each stage's mode is nothing more than a list of these (EventScheduler::build_inj_events), over the
// injector rows that name the stage (OutputMap.h):
//   SEQUENTIAL / SEQUENTIAL_ANY_SYNC — one event per cylinder, mask = that cylinder's own injector.
//   SEMI_SEQUENTIAL — the same, repeated once per crank revolution across the cycle (companion pairs
//                     coincide under the crank fold, firing together on their own outputs).
//   MULTI_POINT     — every injector of the stage in ONE mask, repeated events_per_cycle times.
//   BANK            — one mask per cylinder bank, each at its bank's earliest TDC.
//
// ref_cyl is the cylinder whose shadow (pulse width, opening angle) this event uses. For a grouped
// event that is the lowest-numbered member — the group's outputs all deliver the same commanded pulse,
// because per-cylinder trim can only be honoured when each cylinder has its own independently-timed
// event (full sequential), exactly as conventional Cylinder/Bank Correction gating says.
//
// A staged stage's events are armed only when the fuel calc's duty progression engages that stage.
// ---------------------------------------------------------------------------
struct InjEvent {
    OutputMask mask;           // THIS stage's injector channels this event drives (one stage per event)
    uint8_t    stage;          // 0 = primary (inj_stage[0]); 1..MAX_STAGED_STAGES = staged stage 2..4
    uint8_t    ref_cyl;        // cylinder supplying pulse width + opening angle
    AngleDeg10 base_angle;     // the TDC this event's firing angle is measured back from (full cycle)
};

struct CylinderShadowParams {
    volatile AngleDeg10 spark_btdc;
    volatile uint32_t   dwell_us;
    volatile AngleDeg10 trailing_spark_btdc;
    volatile uint32_t   trailing_dwell_us;
    volatile AngleDeg10 inj_open_btdc;
    volatile uint32_t   inj_pw_us;
    // Staged stages 2..4 (index 0 = stage 2), each its own pulse width + firing angle (own tables) and
    // an engaged flag the fuel calc's duty progression sets.
    volatile uint32_t   inj_secondary_pw_us[MAX_STAGED_STAGES];
    volatile AngleDeg10 inj_secondary_open_btdc[MAX_STAGED_STAGES];
    volatile bool       staged_enabled[MAX_STAGED_STAGES];
};

struct CylinderActiveParams {
    AngleDeg10 spark_btdc;
    uint32_t   dwell_us;
    AngleDeg10 trailing_spark_btdc;
    uint32_t   trailing_dwell_us;
    AngleDeg10 inj_open_btdc;
    uint32_t   inj_pw_us;
    uint32_t   inj_secondary_pw_us;
    bool       staged_enabled;
};

struct VvtResult {
    uint8_t    cam_index;
    AngleDeg10 displacement;
    bool       valid;
};

// What kind of trigger anomaly the decoder last saw (reporting only — the cut
// decision lives in EngineProtection). NONE until the first error.
// The kind names WHAT HAPPENED, not which primitive noticed. It used to name the primitive — GAP
// reported GAP_MISMATCH and SEQUENCE reported TOOTH_WINDOW — so on a missing-tooth wheel (every
// 60-2 and 36-1 there is) TOOTH_WINDOW was unreachable and the P-code gated on it was dead code.
// A fault the operator can act on is a fault described in the wheel's terms, not the decoder's.
enum class TriggerErrorKind : uint8_t {
    NONE          = 0,
    TOOTH_WINDOW  = 1,  // an interval fell outside the predicted window (values 1/2 kept stable:
    GAP_MISMATCH  = 2,  //  telemetry and the DTC map are keyed on them)
    MISSED_TOOTH  = 3,  // a tooth that was DUE never arrived (the deadline fired)
    NOISE_EDGE    = 4,  // an edge arrived too soon to be a tooth — rejected before the matcher
    SIGNAL_ABSENT = 5,  // no teeth at all past the floor bound: the wheel has stopped or the wire is gone
    PHASE_LOST    = 6   // a cam is not where the crank says it should be, or did not arrive at all
};

struct DecoderTelemetry {
    SyncState  sync_state;
    AngleDeg10 current_angle;
    uint32_t   current_rpm_x10;
    uint8_t    tooth_counter;
    uint8_t    sync_tooth_count;
    uint32_t   last_tooth_ticks;
    uint32_t   tooth_period_ticks;
    // Sync-health diagnostics (lean: integer counters from the ISR; the task derives
    // the per-cycle error % and the protection limits from these).
    uint16_t   teeth_last_cycle;     // teeth counted in the just-completed engine cycle
    uint16_t   errors_last_cycle;    // errors in that cycle (→ task computes error %)
    uint32_t   trigger_error_total;  // cumulative tooth-window + gap-mismatch errors
    uint16_t   last_error_tooth;     // tooth_counter at the last error (random vs always-#N)
    uint8_t    last_error_kind;      // TriggerErrorKind of the last error
    uint32_t   noise_total;          // free-running: edges rejected as noise (never cycle-snapshotted)
    uint32_t   missed_total;         // free-running: teeth that were due and did not arrive
    uint32_t   phase_lost_total;     // free-running: cam sync downgrades (watch when tuning authority)
    uint8_t    fault_stream;         // slot index (= role) of the last trigger fault; 255 = none
    bool       trigger_absent;       // the tooth deadline has declared the wheel stopped
    uint32_t   edges_total;          // every capture edge since boot, sync or no sync — the only
                                     // evidence that the wheel is turning BEFORE the decoder locks
};

// ---------------------------------------------------------------------------
// Cycle-span arithmetic
//
// The engine cycle is NOT a constant: a four-stroke completes in 720 crank degrees, a two-stroke in
// 360 (every cylinder fires every revolution). So the span is a PARAMETER, and it is deliberately
// not defaulted — every call site has to say which cycle it means, because "wrap this angle" is
// meaningless without one and a silent 720 is how a two-stroke ends up scheduling into the second
// revolution of a cycle it does not have.
//
// engine_cycle_angle() is the one place the mapping lives; ANGLE_720 remains the four-stroke value.
// ---------------------------------------------------------------------------
[[nodiscard]] inline AngleDeg10 engine_cycle_angle(uint8_t cycle_type) noexcept {
    switch (static_cast<EngineCycleType>(cycle_type)) {
        case EngineCycleType::TWO_STROKE:  return ANGLE_360;   // every cylinder fires every rev
        case EngineCycleType::ROTARY:      return ANGLE_1080;  // 3 e-shaft revs = 1 rotor rev
        case EngineCycleType::FOUR_STROKE:
        default:                           return ANGLE_720;
    }
}

// Injection events per engine cycle, for ONE stage's mode. MULTI_POINT and BATCH take the squirt count
// from the tune (inj_stage[].injections_per_cycle); the others derive it and ignore that field:
//   SEQUENTIAL / SEQUENTIAL_ANY_SYNC — 1, by definition (once per cycle at a cylinder's TDC).
//   SEMI_SEQUENTIAL — once per CRANK REVOLUTION, so the count is how many revolutions a cycle spans:
//                     2 on a four-stroke, 1 on a two-stroke, 3 on a rotary. Hardcoding the
//                     four-stroke's 2 would double a two-stroke's fuel and short-change a rotary's by
//                     a third — the same family as the rev_per_intake bug in the fuel path.
//
// ONE DEFINITION, because two modules need this number and must not disagree: the scheduler builds
// this many events for the stage, and FuelCalculator divides that stage's fuel by the same count. A
// private copy in either is a silent fuelling error of exactly that ratio.
[[nodiscard]] inline uint8_t injection_events_per_cycle(uint8_t inj_mode,
                                                        uint8_t cycle_type,
                                                        uint8_t tune_events) noexcept {
    switch (static_cast<InjectionMode>(inj_mode)) {
    case InjectionMode::SEQUENTIAL:
    case InjectionMode::SEQUENTIAL_ANY_SYNC:
        return 1;
    case InjectionMode::SEMI_SEQUENTIAL: {
        const uint8_t revs = static_cast<uint8_t>(engine_cycle_angle(cycle_type) / ANGLE_360);
        return (revs > 0) ? revs : 1;
    }
    default: {  // MULTI_POINT, BATCH — clamped, not trusted: the event-node pool is
        // sized on this ceiling, so a tune carrying a larger value would build events with nowhere to live.
        if (tune_events < 1) return 1;
        return (tune_events > MAX_INJ_EVENTS_PER_CYCLE)
             ? static_cast<uint8_t>(MAX_INJ_EVENTS_PER_CYCLE) : tune_events;
    }
    }
}

// What each injector ACTUALLY receives per engine cycle at the current sync level — the number the fuel
// must be divided by. With cam phase known that is the event count above. At CRANK sync the scheduler
// cannot tell the revolutions apart, so it walks every revolution's buckets each revolution and every
// event fires once per REVOLUTION: n events spaced cycle/n land on lcm(n, revs) distinct openings per
// cycle (events that fold onto the same angle open the same injector at the same instant and merge).
//   SEQUENTIAL at crank sync: 1 event -> fires every revolution -> revs openings (4-stroke: 2). The fuel
//   used to be divided by 1 regardless: DOUBLE fuel while cranking before the cam was seen, after a cam
//   fault, and for good below min_full_sync_rpm — which the schema suggests setting near 1500 for VR cams.
//   SEQUENTIAL_ANY_SYNC: the scheduler fires it on alternate revolutions (EventScheduler::dispatch_fire),
//   so it stays once per cycle — at A TDC, not necessarily its own, exactly as its help says.
[[nodiscard]] inline uint8_t injection_deliveries_per_cycle(uint8_t inj_mode, uint8_t cycle_type,
                                                            uint8_t tune_events, bool phase_known) noexcept {
    const uint8_t n = injection_events_per_cycle(inj_mode, cycle_type, tune_events);
    if (phase_known) return n;
    const InjectionMode m = static_cast<InjectionMode>(inj_mode);
    // A ROTARY'S per-face events do not multiply: a rotor's three faces are 360 deg apart, so folded to
    // one revolution they land on the SAME angle and merge into one opening a revolution — three a
    // cycle, exactly what the cam-synced schedule gives. The divisor stays as it is.
    if (static_cast<EngineCycleType>(cycle_type) == EngineCycleType::ROTARY &&
        (m == InjectionMode::SEQUENTIAL || m == InjectionMode::SEQUENTIAL_ANY_SYNC ||
         m == InjectionMode::SEMI_SEQUENTIAL))
        return n;
    if (m == InjectionMode::SEQUENTIAL_ANY_SYNC) return 1;
    const uint8_t revs = static_cast<uint8_t>(engine_cycle_angle(cycle_type) / ANGLE_360);
    if (revs <= 1) return n;
    uint8_t a = n, b = revs;                       // lcm(n, revs) = n * revs / gcd
    while (b) { const uint8_t t = static_cast<uint8_t>(a % b); a = b; b = t; }
    return static_cast<uint8_t>((n / a) * revs);
}

[[nodiscard]] inline AngleDeg10 angle_wrap(AngleDeg10 a, AngleDeg10 cycle) noexcept {
    if (cycle <= 0) return a;
    while (a <  0)     a = static_cast<AngleDeg10>(a + cycle);
    while (a >= cycle) a = static_cast<AngleDeg10>(a - cycle);
    return a;
}

// The wasted-spark companion's TDC: half an engine cycle away, whatever the cycle.
//
// One coil, two plugs, both fired together — one cylinder is near TDC and doing the work, the other
// gets the wasted spark. What the companion is DOING there differs by engine, but where it sits does
// not:
//   four-stroke  360 deg away — the companion is at TDC on its exhaust stroke
//   two-stroke   180 deg away — the companion is at BDC
//   rotary       540 deg away — half a rotor revolution in eccentric-shaft degrees
//
// The relation is mutual by construction (companion(companion(x)) == x), which it must be: a coil
// cannot fire two cylinders that disagree about being partners.
[[nodiscard]] inline AngleDeg10 wasted_companion_tdc(AngleDeg10 tdc, AngleDeg10 cycle) noexcept;

[[nodiscard]] inline AngleDeg10 angle_dist_forward(AngleDeg10 from, AngleDeg10 to,
                                                   AngleDeg10 cycle) noexcept {
    AngleDeg10 d = static_cast<AngleDeg10>(to - from);
    if (d < 0) d = static_cast<AngleDeg10>(d + cycle);
    return d;
}

inline AngleDeg10 wasted_companion_tdc(AngleDeg10 tdc, AngleDeg10 cycle) noexcept {
    return angle_wrap(static_cast<AngleDeg10>(tdc + cycle / 2), cycle);
}
