#pragma once

// TriggerWheel — the pure-logic core of the trigger-wheel designer. A "wheel" (36-1, 60-2+cam, …)
// is the generic stream encoding the firmware decoder runs, expressed for authoring. This file has
// NO dependency on Cache / MetaModel / the UI — it is the designer's engine and is unit-tested
// headless against the firmware semantics.
//
// The wheel → config mapping mirrors the bench script's gen_rebench.configure() field-for-field
// (bench-proven on 22 wheels)
// and deliberately OMITS capture_index (which physical pin the crank/cam land on = board wiring,
// assigned separately). The designer's Apply to ECU writes the pairs through applyWheelToConfig.

#include <functional>
#include <j/config/Json.h>   // jf::JJson
#include "TriggerFit.h"

#include <string>
#include <utility>
#include <vector>

// One decode stream. rate: 0 = crank (360°), 1 = cam (720°).
// prim: 0 = GAP (missing-tooth), 1 = SEQUENCE (inter-edge spans, 0.1°), 2 = WIDTH (cam pulse window).
struct WheelStream {
    // What this stream IS: 0 Unused, 1 Crank Primary, 2 Crank Secondary, 3 Cam Intake B1,
    // WHICH SLOT of trigger.streams[] this is, and the slot IS the role: 0 Crank Primary,
    // 1 Crank Secondary, 2 Cam Intake B1, 3 Cam Exhaust B1, 4 Cam Intake B2, 5 Cam Exhaust B2.
    //
    // -1 means the wheel did not say, and wheelPairs places it at the next free slot of its rate.
    // NOT 0: zero is a real slot now. Carrying the role as a value that had to be unique is what
    // this replaced — two streams could claim one role and the assignment resolver silently kept
    // the last, and a role seeded from array position could not describe a V engine with phasers on
    // the intakes alone (Intake B1 and Intake B2 are slots 2 and 4, never 2 and 3).
    int role  = -1;
    // HOW MANY TIMES this stream's pattern repeats per ENGINE CYCLE; its angular period is
    // cycle/repeats. 0 means "take the slot's default" — 2 for a crank slot on a four-stroke, 1 for
    // a cam slot — which is every ordinary wheel.
    //
    // Stated only when the wheel is neither: a symmetrical crank pattern recurring twice per
    // revolution (Renix, 4 and 6), or a distributor on an ODD-cylinder engine whose pulses are 1.5
    // per revolution and therefore not a crank-rate wheel at any tooth count. Dropping it on a round
    // trip does not lose a label — it silently rewrites the wheel's angular period.
    int repeats = 0;
    int rate  = 0;
    int prim  = 0;
    int slots = 0;                 // GAP: nominal tooth count
    int ratio = 0;                 // GAP: gap ratio (missing teeth + 1); 0 → treated as 2 on emit
    std::vector<int> cell;         // GAP: gap present-tooth indices · SEQUENCE: inter-edge spans (0.1°)
    int wMin = 0, wMax = 7200, wTgt = 0;   // WIDTH window (0.1°)
    int edge = 0;                  // 0 = Rising, 1 = Falling, 2 = Both (decoder: Capture Edge)
    int windowPct = 25;            // match tolerance ±% — GAP position, SEQUENCE ratio, WIDTH window
    int windowCrankPct = 75;       // the same tolerance while CRANKING; blended into windowPct by rpm
    int nominalAngle = 0;          // VVT reference (0.1°): where the cam sits at 0 advance; 0 = none/crank
    // WHAT THE CAM PHYSICALLY IS, which the pattern cannot say. A wheel that does not carry these
    // cannot describe a VVT engine, and — worse — applying it used to LEAVE WHATEVER THE ECU HAD:
    // a fixed-cam wheel dropped over a config with phased=1, authority=60° kept the phaser's band,
    // which the firmware's own comment calls blinding the check. wheelPairs() emits every slot
    // precisely so applying a wheel REPLACES the trigger setup; these were the fields it forgot.
    int phased = 0;                // 1 = a phaser can move this cam relative to the crank
    int phaseAuthority = 600;      // phased: how far it may travel from nominal (0.1° crank)
    int phaseAllowance = 0;        // FIXED: mechanical slop (chain stretch, lash) to tolerate (0.1°)
};

// A trigger wheel: crank + 0..N cam streams, plus authoring metadata. Cam identity (Intake/Exhaust,
// bank 1/2) is assigned by stream order in wheelPairs (role = Cam base + order) — the VVT-relevant
// per-cam data (edge, nominal angle) rides on each WheelStream.
struct Wheel {
    std::string name;
    std::string id;                // stable slug (survives renames); empty until saved
    std::string sync = "CRANK";    // "CRANK" or "PHASE" (a cam stream present)
    double tdcOffset = 0.0;        // crank° from the sync reference to TDC #1 (→ trigger_offset_btdc)
    // Pickup bearing, degrees from vertical, positive ANTICLOCKWISE (to the left), front of the
    // engine. VISUAL ONLY:
    // it is saved with the wheel and drawn, and it never reaches the ECU — the firmware's whole
    // knowledge of the wheel-to-engine relationship is tdcOffset. It exists so the dial can show
    // the gear the way the user sees it, and so the offset can be derived from that picture
    // instead of copied from someone else's number. A 2JZ's pickup is +105 (105 anticlockwise).
    double sensorAngle = 0.0;
    std::vector<WheelStream> streams;
};

// Stream constructors — identical encoding to gen_rebench.
WheelStream gapStream(int rate, int slots, int ratio, std::vector<int> cell);
WheelStream seqStream(int rate, std::vector<int> cell);
WheelStream widStream(int rate, int wMin, int wMax, int wTgt);

// The wheel → config (path, value) pairs. Paths are studio-jf config paths
// (trigger.trigger_offset_btdc, trigger.streams[slot].<field>, streams[slot].cell[k].v) — the stream
// INDEX is the role, so there is no count and no role field;
// values are ENGINEERING units. The Cache does NOT apply a field's scale — setConfigValue takes RAW and
// writes bytes — so the pairs go through applyWheelToConfig, which converts. The comment here used to
// claim the Cache did it, which is exactly the belief that put 15.5° in an ECU told 155.
// The config a wheel describes, as (path, value) pairs in ENGINEERING units — the same units
// wheelFromConfig reads back, so the two are inverses. Nearly every field is scale 1.0 and therefore
// identical either way; the angles are not, and the caller must divide by the field's scale before
// writing (Cache::setConfigValue takes RAW). See onApplyToEcu in main.cpp.
std::vector<std::pair<std::string, double>> wheelPairs(const Wheel& w);

// Write a wheel into the live config, converting each pair from the engineering units wheelPairs speaks
// into the RAW bytes the Cache stores. THE place that conversion happens: it lived in the apply button's
// lambda and simply did not exist, so a 2JZ's 155.0° trigger offset was written as raw 155 and read back
// as 15.5°. Returns how many settings were written.
int applyWheelToConfig(const Wheel& w, class Cache& cache);

// The inverse: reconstruct the Wheel currently in the config, via a reader that returns the
// DISPLAY-unit value at a config path (e.g. `[](auto& p){ return Cache::instance().configValue(p); }`).
// Lets the designer render whatever wheel the ECU/tune currently holds. Crank-vs-cam rate is inferred
// from each stream's role (1,2 = crank; ≥3 = cam); camEdge from the cam stream's edge.
Wheel wheelFromConfig(const std::function<double(const std::string&)>& get);

// ---- authoring params ⇄ wheel ------------------------------------------------------------------
// Human-facing form values; buildWheel(params) → wheel, decodeWheel(wheel) → params (lossless
// round-trip for every shape the seed set uses).
// One authored cam (VVT-aware). Identity (Intake/Exhaust B1/B2) follows its order in `cams`.
// One CRANK stream's geometry. The decoder allows two (Crank Primary + Crank Secondary), and this is
// what the second one is described with. The primary keeps its fields flat on WheelParams for now —
// the form is built around them — so this exists to describe the SECOND without duplicating the
// editor. Same four primitives are available either way: the decoder does not treat them differently.
struct CrankParams {
    int    type      = 0;                        // WheelParams::CRANK_* (MISSING / EVEN / SEQ / WIDTH)
    int    teeth     = 36;
    int    missing   = 1;
    std::vector<int>    gapPos    = {0};
    int    evenTeeth = 4;
    std::vector<double> seqAngles = {0.0, 135.0};
    double cwMin = 0.0, cwMax = 360.0, cwTgt = 0.0;
    int    edge      = 0;                        // 0 Rising · 1 Falling · 2 Both
    // Pattern repeats per engine cycle; 0 = the slot default (2 on a four-stroke crank). 4 and 6 are
    // the symmetrical Renix wheels, whose pattern recurs twice and three times per revolution.
    int    repeats   = 0;
};

// WHICH SLOT of trigger.streams[] a stream is. The slot IS the role (8bcc148), 0-based, and it is what
// WheelStream::role holds. Not to be confused with StreamGeometry::displayRole, which is 1-based for the
// dial's labels — the designer wrote one into the other, and every cam it authored came out one slot too
// far: the first cam landed on Cam Exhaust B1, and a fourth would have been dropped for exceeding the
// count. Public so the UI can name a slot by the same constant the emitter places it with.
constexpr int kSlotCrankPrimary = 0;
constexpr int kSlotCrankSecond  = 1;
constexpr int kSlotCamBase      = 2;
constexpr int kSlotCount        = 6;

struct CamParams {
    // Which cam this is — slot 2..5 (Intake/Exhaust x bank 1/2). -1 = take it from the cam's position,
    // which is only the default for a freshly added cam. Explicit because position cannot express
    // "phasers on both intakes and nothing on the exhausts", which is an ordinary V-engine layout.
    int    role    = -1;
    int    type    = 0;                          // CAM_PULSE / CAM_PATTERN / CAM_EVEN
    int    evenTeeth = 1;                        // CAM_EVEN: teeth per cam period (1 = one pulse)
    double wMin = 0.0, wMax = 720.0, wTgt = 0.0; // pulse window (deg)
    std::vector<double> angles = { 0.0, 360.0 }; // pattern edge angles (deg)
    int    edge    = 2;                          // 2 = Both, 0 = Rising
    double nominal = 0.0;                         // VVT nominal/reference angle (deg) — 0-advance cam pos
    // Pattern repeats per engine cycle; 0 = the cam-slot default of 1 (the pattern spans the cycle).
    int    repeats = 0;
};

struct WheelParams {
    // The decoder offers three primitives on ANY stream, crank included: Gap, Sequence and Width.
    // The editor previously offered the crank only the first two.
    enum CrankType { CRANK_MISSING = 0, CRANK_EVEN = 1, CRANK_SEQ = 2, CRANK_WIDTH = 3 };
    // CAM_EVEN is a GAP-primitive cam: N evenly spaced teeth over the cam's period, most often ONE.
    // A single pulse per engine cycle is exactly that, and it is NOT expressible as a pattern — a
    // one-cell SEQUENCE has no ratio to match and does not lock, measured on the bench. Without this
    // type the designer could not represent the cam on any wheel that uses one, and a round trip
    // silently converted it into something that does not decode.
    enum CamType   { CAM_PULSE = 0, CAM_PATTERN = 1, CAM_EVEN = 2 };

    std::string name;
    int    crankType = CRANK_MISSING;
    int    teeth     = 36;
    int    missing   = 1;
    std::vector<int>    gapPos = {0};
    int    evenTeeth = 4;
    std::vector<double> seqAngles = {0.0, 135.0};
    double cwMin = 0.0, cwMax = 360.0, cwTgt = 0.0;   // CRANK_WIDTH reference-pulse window (deg)
    int    crankEdge = 0;                             // 0 Rising · 1 Falling · 2 Both
    // The crank pattern's repeats per engine cycle; 0 = the slot default. See WheelStream::repeats —
    // it has to live on the params too or the designer's decode->edit->build round trip drops it and
    // silently rewrites a symmetrical wheel into an ordinary one.
    int    crankRepeats = 0;
    int    windowPct = 25;                            // match tolerance ±%, all streams
    // Where the PICKUP sits: degrees from vertical, positive ANTICLOCKWISE (to the left), seen from the front of the
    // engine. Studio-side only — the firmware never needs it, because the wheel-to-engine
    // relationship reaches it as tdcOffset alone. It is here so the designer can draw the gear as
    // the user actually sees it, and so an offset can be DERIVED from what they see rather than
    // typed from a number someone else measured:
    //
    //     bearing(D) = sensorAngle + tdcOffset + crankAngle - D
    //
    // (D = a tooth's decoder angle, crankAngle = 0 at TDC #1.) Rearranged, that is the setup
    // procedure: put the engine at TDC, look at where a known feature sits, and the offset falls
    // out. A 2JZ's pickup is +105 (105 anticlockwise).
    double sensorAngle = 0.0;
    // A SECOND crank stream (slot 1, Crank Secondary). The decoder has always had the role and
    // wheelPairs has always been able to emit it; there was simply no way to author one, because
    // WheelParams described exactly one crank. Engines with a second crank pickup — a separate
    // reference tooth wheel alongside the main one — could not be described at all.
    // NOT EVERY TRIGGER HAS A CRANK. A distributor is one pickup on a shaft turning at cam speed,
    // and on an odd-cylinder engine it cannot be a crank-rate stream at any tooth count. buildWheel
    // used to emit a crank unconditionally, so reading such a wheel and writing it back grew a
    // second stream out of nothing — describing a sensor the engine does not have.
    bool        hasCrank  = true;
    bool        hasCrank2 = false;
    CrankParams crank2{};
    std::vector<CamParams> cams;                 // 0..4 cams; each states its own role
    double tdcOffset = 0.0;
};

Wheel       buildWheel(const WheelParams& p);
WheelParams decodeWheel(const Wheel& w);

// ---- is this wheel decodable, and if not, why? ---------------------------------------------------
//
// A wheel that the firmware cannot sync on looks EXACTLY like one that works: the dial draws it, the
// trace draws it, describe() names it, and the only way to find out was to apply it to an ECU and
// watch the engine not start. These are the conditions the decoder cannot get past, stated where a
// designer can read them while drawing.
//
// Pure logic, on the built Wheel rather than the authoring params, so it judges what would actually
// be SENT — an authoring field that never reaches a stream cannot make a wheel undecodable, and one
// that does is caught whichever form filled it in.
struct WheelIssue {
    enum Level { Note = 0, Warn = 1, Error = 2 };
    Level       level = Note;
    std::string text;      // what is wrong, in the words the form uses for the field
};
// Worst first, so a caller showing one line shows the one that matters.
std::vector<WheelIssue> validateWheel(const Wheel& w);

// A MEASUREMENT, TAKEN AS A DESIGN. What the capture saw of the wheel — the crank track and every
// cam — written into the params the designer edits. What a capture cannot see is not touched: the
// pickup angle and TDC are the wheel's relationship to the ENGINE, and nothing on the wire says
// where the pistons are.
//
// Out here rather than in the designer because the designer is a scene graph and this is arithmetic
// over a capture. It was in there, it only ever adopted the crank, and no test could reach it.
bool applyMeasured(const triggerfit::CaptureFit& m, WheelParams& p);

// Angle helpers (SEQUENCE cell encoding).
std::vector<int>    anglesToCell(const std::vector<double>& angles, double period);
std::vector<double> cellToAngles(const std::vector<int>& cell);

// The seed set — the gen_rebench wheel table (25 wheels), the designer's starting content.
// The SHIPPED library, parsed out of the ECU's meta ("trigger_wheels"). The definitions live in the
// schema and travel with the firmware, so the wheels a studio offers are the ones the connected ECU
// can actually decode — not whatever happened to be compiled into this build.
// seedWheels() is gone: the shipped library lives in definition/ecu.schema.yaml and arrives in the
// meta, so it belongs to the firmware rather than to whichever studio build is running.
std::vector<Wheel> wheelsFromMeta(const jf::JJson &root);
// …and the same, reading the file itself. It exists so the app does not have to parse a 1.5 MB meta in
// its own wiring unit, which is compiled with the optimiser turned down (its compile time is the cost of
// every UI change): that parse was half a second of every launch there and is a fraction of it here.
// Returns an empty library if the file is missing or unreadable, as a shipped library legitimately can be.
std::vector<Wheel> wheelsFromMetaFile(const std::string &path);

// USER wheels: authored in the designer and saved alongside the ECU library, so they survive a
// restart and a studio upgrade. Stored as JSON in the studio's own data directory, separate from the
// shipped set — a firmware update replaces the shipped wheels and must never silently drop the
// user's own.
std::string              userWheelsPath();
std::vector<Wheel>       loadUserWheels();
bool                     saveUserWheels(const std::vector<Wheel> &wheels);
// Append or replace by name, then persist. Returns false if the write failed.
bool                     saveUserWheel(const Wheel &w);
// Remove one of the user's own by name. Shipped wheels cannot be deleted — they come from the
// firmware's meta and would simply reappear on the next load, so refusing is honest.
bool                     deleteUserWheel(const std::string &name);

// IMPORT / EXPORT — the same JSON shape as the meta and the user store, so a wheel exported here can
// be pasted straight into a schema's `trigger_wheels` and vice versa. One file format, three homes.
//
// Import MERGES by name rather than replacing the library: someone sending you one wheel should not
// wipe the rest of yours. Returns how many were added or updated, or -1 if the file could not be read.
bool                     exportWheels(const std::string &path, const std::vector<Wheel> &wheels);
int                      importWheels(const std::string &path);
std::vector<Wheel>       readWheelFile(const std::string &path);
