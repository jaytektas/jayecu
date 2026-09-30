#pragma once

#include "../EngineModule.h"
#include "KnockProfile.h"
#include "../../../generated/modules/knock_config.h"

#include <cstdint>

class Ignition;
class DtcManager;

// ---------------------------------------------------------------------------
// Knock — adaptive knock retard, measured against each cylinder's LEARNED NOISE FLOOR.
//
// THE THRESHOLD IS A RATIO, NOT A LEVEL. What a knock sensor hears when nothing is wrong is engine
// mechanical noise — valve seating, injector actuation, piston slap — and that background rises
// steeply with both rpm and load. An absolute threshold is therefore a hand-fitted noise floor: it
// has to be re-fitted per engine, per sensor, per mounting torque, and it is deaf at one end of the
// range whenever it is right at the other. So this module learns what each cylinder normally sounds
// like at each operating point and thresholds the DIFFERENCE:
//
//     intensity = measured_db - noise_floor[cyl][cell]
//     knock     = intensity > knock_threshold_table(rpm, load)
//
// THE FLOOR IS A MAP, NOT AN AVERAGE. A single running average lags whenever the operating point
// moves — and it moves hardest exactly when knock happens. Open the throttle, the floor jumps, one
// average trails it, and every cycle in the transient reads as knock. A map moves to a cell that
// already knows its own floor. It lives in the persistent learned region, so an engine is not deaf
// for its first minutes after every start.
//
// A KNOCKING CYCLE NEVER TEACHES THE FLOOR. A reference that learns from knock chases it upward and
// goes progressively deaf — the classic failure of this whole approach. Only cycles classified clean,
// with the learn gate open, are allowed to move a cell.
//
// THREADING. on_knock_sense() reaches a verdict and mutates controller + learned state, so it runs in
// the ENGINE-MODULE task and nowhere else. The knock worker is a different task: it calls
// post_measurement(), which is a lock-free single-producer ring that update() drains. The previous
// version's header promised exactly this mailbox while main.cpp called on_knock_sense() straight from
// the worker — retard, level, count and now a learned map, all mutated from two tasks at once.
// ---------------------------------------------------------------------------

class Knock : public EngineModule {
public:
    // Noise-floor grid. Matches the knock_noise learned block (see the schema for why 8x8 is right for
    // a floor: it is a smooth function of rpm and load, with none of a VE map's structure, so finer
    // cells would only split the same evidence and slow learning).
    static constexpr uint8_t  RPM_BINS  = 8;
    static constexpr uint8_t  LOAD_BINS = 8;
    static constexpr uint16_t CELLS     = RPM_BINS * LOAD_BINS;
    static constexpr uint8_t  MAX_CYL   = 12;   // == KNOCK_CYL_BANK_COUNT / the per-cylinder block count

    void init(const KnockConfig& cfg, Ignition* ignition);
    void on_config_change(const KnockConfig& cfg) { cfg_ = &cfg; }
    void on_engine_stop() override;
    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    // Hand a completed measurement over from the KNOCK WORKER TASK. Lock-free, single-producer:
    // update() is the only consumer. Returns false when the ring is full — the measurement is dropped
    // rather than blocking a task that has a DMA burst to service.
    //
    // The whole PROFILE crosses, not a summary of it. Pre-ignition is decided by where the energy sat
    // relative to the SPARK, and the spark angle is a module-task fact (it comes off the bus) that the
    // burst worker has no business knowing — so the phase evidence travels and the verdict stays in
    // one place.
    bool post_measurement(uint8_t cyl, float db, const KnockProfile& profile) noexcept;

    // Classify one measurement. ENGINE-MODULE TASK ONLY: update() calls it while draining, the
    // external-source path calls it directly, and host tests drive it. Applies the per-cylinder gain
    // trim, compares against the learned floor, and either accumulates retard or teaches the floor.
    //
    // `profile` is optional: null means "no phase information", which disables the pre-ignition
    // conjunction for that measurement rather than guessing at it. The external-intensity source has
    // no phase by construction and passes null.
    void on_knock_sense(uint8_t cyl, float db, const KnockProfile* profile = nullptr);

    // ---- Introspection (tests, telemetry, the bench CLI) ----
    [[nodiscard]] float    current_retard() const { return retard_deg_; }
    [[nodiscard]] float    cylinder_retard(uint8_t c) const { return c < MAX_CYL ? cyl_retard_[c] : 0.0f; }
    [[nodiscard]] uint32_t knock_count()    const { return count_; }
    [[nodiscard]] float    last_intensity() const { return last_intensity_; }   // dB over floor
    [[nodiscard]] uint32_t dropped()        const { return dropped_; }          // ring overflows
    // The learned floor a cylinder would be measured against right now, and how many clean samples
    // taught it. samples == 0 means the cell has never been learned and the seed is in use.
    [[nodiscard]] float    noise_floor(uint8_t cyl) const;
    [[nodiscard]] float    noise_samples(uint8_t cyl) const;
    // Cylinders currently cut for pre-ignition (bit c), and the confirmed event tally per cylinder.
    [[nodiscard]] uint16_t preign_cuts()  const { return preign_cut_mask_; }
    [[nodiscard]] uint8_t  preign_events(uint8_t cyl) const {
        return cyl < MAX_CYL ? preign_events_[cyl] : 0;
    }
    // Fraction of the last classified profile's energy that sat BEFORE the spark (0..1), or -1 when
    // that measurement carried no phase information.
    [[nodiscard]] float    last_pre_frac() const { return last_pre_frac_; }

    // THE LAST MEASUREMENT, KEPT WHOLE — the profile plus everything needed to read it.
    //
    // A bucket row on its own is unreadable: whether it means anything depends on where the floor
    // was, where the spark was, and what the threshold demanded at that operating point. Those are
    // resolved per tick and would be gone by the time anyone looked, so the verdict is recorded WITH
    // its evidence rather than leaving a viewer to re-derive conditions that have since moved.
    enum Verdict : uint8_t { Clean = 0, Bootstrapped = 1, Knocking = 2, PreIgnition = 3 };
    struct Shot {
        KnockProfile profile;
        float    db          = 0.0f;   // measured level, gain-trimmed
        float    floor_db    = 0.0f;   // what it was measured against
        float    intensity   = 0.0f;   // db - floor
        float    threshold   = 0.0f;   // what it had to beat, at that cell
        float    spark_deg   = 0.0f;   // advance BTDC when it was judged
        float    pre_frac    = -1.0f;  // -1 = no phase information
        uint32_t seq         = 0;      // increments per classified measurement; 0 = nothing yet
        uint8_t  cyl         = 0;
        uint8_t  verdict     = Clean;
    };
    [[nodiscard]] const Shot& last_shot() const { return shot_; }
    // THE LAST SHOT THAT WAS ACTUALLY SOMETHING — knock or pre-ignition, latched until replaced.
    //
    // "Latest" is the wrong thing to show a person. At 50 windows/sec an event spanning one or two
    // windows is gone before any poll or any pair of eyes reaches it, so a scope showing the newest
    // measurement shows the 49 quiet ones and never the one that fired. Latching the last non-clean
    // verdict is what makes the evidence reviewable at human speed.
    [[nodiscard]] const Shot& last_event() const { return event_; }

private:
    float quiet_ms_[2] = { 0.0f, 0.0f };   // how long each sensor has read below quiet_db
    float judge_ms_    = 0.0f;             // how long the sensors have been judgeable (combustion, over the floor)
    [[nodiscard]] uint16_t cell_index(float rpm, float load) const;
    // Teach the live cell that this cylinder sounded like `db` on a clean cycle.
    void learn(uint8_t cyl, float db);
    // Energy fraction of `p` lying before `spark_deg` (signed, BTDC negative). -1 without phase.
    [[nodiscard]] float pre_spark_fraction(const KnockProfile& p, float spark_deg) const;
    // A confirmed pre-ignition event on `cyl`: tally, DTC, and the protective cut.
    void on_preignition(uint8_t cyl, uint32_t now);

    const KnockConfig* cfg_      = nullptr;
    Ignition*          ignition_ = nullptr;
    DtcManager*        dtc_      = nullptr;
    bool was_enabled_ = false;   // enabled->disabled edge: heal our DTCs once (DtcManager::heal)

    // Learned region slices — [CELLS] each, per cylinder. Null until init() maps them.
    float* noise_[MAX_CYL] = {};   // dB floor
    float* nsamp_[MAX_CYL] = {};   // clean-sample count that taught it

    float    retard_deg_ = 0.0f;     // the WORST cylinder's knock retard [deg] (telemetry; = max of cyl_retard_)
    float    cyl_retard_[MAX_CYL] = {};   // each cylinder's own accumulated retard (decays back to 0)
    void     clear_retard() { retard_deg_ = 0.0f; for (float& r : cyl_retard_) r = 0.0f; }
    void     recover(float dt);           // decay every cylinder at the recovery rate, refresh retard_deg_
    float    level_db_   = -50.0f;   // peak knock level [dB] (decays) — telemetry
    // Total knock events since boot. It STOPPED at 65535 (a 16-bit count that saturated), and a bench that
    // had knocked that often read every later knock as none — "before/after" differences of zero.
    uint32_t count_      = 0;
    uint32_t last_ms_    = 0;        // tick of the last update (decay dt)
    bool     suppressed_ = true;     // TPS light-load suppression (set by update())

    // The operating point + table values update() resolved this tick. on_knock_sense reads them
    // rather than touching the bus, so every measurement in one tick is judged against ONE operating
    // point — the one that was true when the window was armed, not whatever the bus says mid-drain.
    uint16_t cell_        = 0;
    float    threshold_db_ = 12.0f;
    float    max_retard_   = 8.0f;
    bool     learn_ok_     = false;
    float    last_intensity_ = 0.0f;

    // Cell dwell — the operating point must settle before a cell is allowed to learn, or a transient
    // sweeping across cells smears one cell's floor into its neighbours.
    uint16_t dwell_cell_     = 0xFFFFu;
    uint32_t dwell_since_ms_ = 0;

    // Pre-ignition state. The cut mask is what the scheduler is told; the tally is how many confirmed
    // events a cylinder has had, so a lone false positive does not cut a cylinder on its own.
    uint16_t preign_cut_mask_ = 0;
    uint8_t  preign_events_[MAX_CYL] = {};
    uint32_t preign_last_ms_[MAX_CYL] = {};
    float    last_pre_frac_ = -1.0f;
    Shot     shot_{};
    Shot     event_{};   // last Knocking / PreIgnition shot, latched
    float    spark_deg_     = 0.0f;   // live advance (BTDC positive), resolved by update()

    // Cross-task measurement ring (worker -> update). Power of two; each slot carries a whole profile.
    // SIZED FOR THE WORST CASE: drained at 100 Hz, and a 12-cylinder at 8000 rpm makes 800 windows/s —
    // eight per drain. Eight slots (seven usable) dropped windows there, the highest-load ones. Sixteen
    // is twice the worst case for ~1.2 KB.
    static constexpr uint8_t RING = 16;
    struct Meas { uint8_t cyl; float db; KnockProfile profile; };
    // The SLOTS are plain (a volatile struct cannot be copied); the INDICES are volatile and the
    // publish is ordered by a compiler barrier. Single core, one producer, one consumer: the slot
    // must be fully written before head_ advertises it, which is exactly what the barrier buys.
    Meas              ring_[RING] = {};
    volatile uint8_t  head_ = 0;      // producer (knock worker)
    volatile uint8_t  tail_ = 0;      // consumer (engine-module task)
    volatile uint32_t dropped_ = 0;
};
