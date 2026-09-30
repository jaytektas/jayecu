#include "Knock.h"
#include "Ignition.h"
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "../TableEval.h"         // tbl::table_eval — one channel-driven path for every table
#include "../../Diagnostics/DtcManager.h"
#include "../../../generated/module_dtc.h"
#include "../../../generated/table_descs.h"     // knock_threshold_table_desc / knock_max_retard_table_desc
#include "../../../generated/learned_layout.h"  // LEARNED_KNOCK_NOISE_* (fixed offsets)
#include "../../Diagnostics/Dtc.h"
#include "../../Signal/EnginePosition.h"
#include "../../../generated/ecu_config.h"      // g_config.engine: which sensor each cylinder is on
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"   // platform_get_tick_ms, platform_learned_block
#include "../../../generated/signal_ids.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
constexpr float kNoKnockDb   = -50.0f;   // floor / "silent" level
constexpr float kLevelDecay  = 20.0f;    // telemetry peak-level decay [dB/s]

static_assert(LEARNED_KNOCK_NOISE_1_CELLS   == Knock::CELLS, "learned_layout knock_noise dims != module");
static_assert(LEARNED_KNOCK_NOISE_N_1_CELLS == Knock::CELLS, "learned_layout knock_noise_n dims != module");

// Fixed per-cylinder offsets, in cylinder order. Append-only in the schema, so these never move.
constexpr uint32_t kNoiseOff[Knock::MAX_CYL] = {
    LEARNED_KNOCK_NOISE_1_OFFSET,  LEARNED_KNOCK_NOISE_2_OFFSET,  LEARNED_KNOCK_NOISE_3_OFFSET,
    LEARNED_KNOCK_NOISE_4_OFFSET,  LEARNED_KNOCK_NOISE_5_OFFSET,  LEARNED_KNOCK_NOISE_6_OFFSET,
    LEARNED_KNOCK_NOISE_7_OFFSET,  LEARNED_KNOCK_NOISE_8_OFFSET,  LEARNED_KNOCK_NOISE_9_OFFSET,
    LEARNED_KNOCK_NOISE_10_OFFSET, LEARNED_KNOCK_NOISE_11_OFFSET, LEARNED_KNOCK_NOISE_12_OFFSET };
constexpr uint32_t kSampOff[Knock::MAX_CYL] = {
    LEARNED_KNOCK_NOISE_N_1_OFFSET,  LEARNED_KNOCK_NOISE_N_2_OFFSET,  LEARNED_KNOCK_NOISE_N_3_OFFSET,
    LEARNED_KNOCK_NOISE_N_4_OFFSET,  LEARNED_KNOCK_NOISE_N_5_OFFSET,  LEARNED_KNOCK_NOISE_N_6_OFFSET,
    LEARNED_KNOCK_NOISE_N_7_OFFSET,  LEARNED_KNOCK_NOISE_N_8_OFFSET,  LEARNED_KNOCK_NOISE_N_9_OFFSET,
    LEARNED_KNOCK_NOISE_N_10_OFFSET, LEARNED_KNOCK_NOISE_N_11_OFFSET, LEARNED_KNOCK_NOISE_N_12_OFFSET };
}

void Knock::init(const KnockConfig& cfg, Ignition* ignition) {
    cfg_        = &cfg;
    ignition_   = ignition;
    clear_retard();
    level_db_   = kNoKnockDb;
    count_      = 0;
    last_ms_    = platform_get_tick_ms();
    head_ = tail_ = 0;
    dropped_ = 0;
    // Null when the region cannot hold the block — the module then falls back to the seed floor and
    // simply never learns, rather than writing through a null.
    auto map = [](uint32_t off) -> float* {
        auto* base = platform_learned_block(off, CELLS * sizeof(float));
        return base ? reinterpret_cast<float*>(base) : nullptr;
    };
    for (uint8_t c = 0; c < MAX_CYL; ++c) {
        noise_[c] = map(kNoiseOff[c]);
        nsamp_[c] = map(kSampOff[c]);
    }
}

void Knock::recover(float dt) {
    const float give = (cfg_ ? cfg_->retard_reapply_rate * 0.01f : 0.0f) * dt;
    retard_deg_ = 0.0f;
    for (float& r : cyl_retard_) { r = std::max(0.0f, r - give); retard_deg_ = std::max(retard_deg_, r); }
}

void Knock::on_engine_stop() {
    clear_retard();
    level_db_   = kNoKnockDb;
    if (ignition_)
        ignition_->apply_knock_retard(0.0f);

    // RELEASE THE PRE-IGNITION CUTS. preign_clear_s documents 0 as "latch until engine stop", and
    // without this it meant latch until MCU RESET: a cut cylinder stayed cut across a stop/start with
    // nothing short of a reboot to restore it, and no indication that was the reason. Found on the
    // bench, where two cylinders arrived at the next run already cut.
    //
    // The stored DTC is what survives — that is the record of what happened. The CUT is a live
    // protective action, and holding one across a restart the operator deliberately performed is
    // asserting a fault we are no longer measuring.
    preign_cut_mask_ = 0;
    for (uint8_t c = 0; c < MAX_CYL; ++c) preign_events_[c] = 0;
}

// The learned cell for (rpm, load). The store IS a table with two declared axes, so both bins come
// from those axes through the one lookup.
//
// This function existed verbatim in BOTH Lambda and Knock, each against its own RPM_SPAN/LOAD_SPAN
// pair holding the same two numbers — 8000 and 250 — restating the grid the schema already declared.
// The axes are shared config arrays now (knock_noise_1_x_axis / _y_axis, shared by every per-cylinder noise map), so the grid is one fact in one place and resizing it
// moves the firmware and the studio together instead of silently disagreeing with both.
uint16_t Knock::cell_index(float rpm, float load) const {
    if (!cfg_) return 0;
    const tbl::TableDesc d = knock_noise_1_desc(cfg_);
    const int xn = tbl::axis_live_n(d.x);
    return static_cast<uint16_t>(tbl::nearest_bin(d.y, load) * (xn > 0 ? xn : 1)
                                 + tbl::nearest_bin(d.x, rpm));
}

float Knock::noise_floor(uint8_t cyl) const {
    if (cyl >= MAX_CYL || !noise_[cyl] || !nsamp_[cyl] || !cfg_)
        return cfg_ ? cfg_->noise_seed_db * 0.1f : kNoKnockDb;
    // A cell nothing has ever taught reads the SEED, not whatever the region happened to contain. The
    // sample count is the only thing that can tell those apart — a fresh region is zeroes, and 0 dB is
    // a perfectly plausible measured floor.
    return (nsamp_[cyl][cell_] > 0.0f) ? noise_[cyl][cell_] : cfg_->noise_seed_db * 0.1f;
}

float Knock::noise_samples(uint8_t cyl) const {
    if (cyl >= MAX_CYL || !nsamp_[cyl]) return 0.0f;
    return nsamp_[cyl][cell_];
}

bool Knock::post_measurement(uint8_t cyl, float db, const KnockProfile& profile) noexcept {
    const uint8_t h = head_;
    const uint8_t nxt = static_cast<uint8_t>((h + 1u) & (RING - 1u));
    if (nxt == tail_) { dropped_++; return false; }   // full -> drop, never block the burst worker
    ring_[h].cyl     = cyl;
    ring_[h].db      = db;
    ring_[h].profile = profile;
    // Publish only once the slot is whole: the consumer reads head_ to decide a slot exists.
    std::atomic_signal_fence(std::memory_order_seq_cst);
    head_ = nxt;
    return true;
}

// How much of the window's energy lies BEFORE the spark.
//
// This is the pre-ignition discriminator, and it is the only reason the profile exists. Normal
// combustion puts almost nothing here: the charge has not lit yet. Knock puts its energy after the
// spark, during expansion. Pre-ignition lights the charge early, so the ringing starts at or before
// the spark and a real fraction of the window's energy lands on the wrong side of it.
//
// Energies are summed in LINEAR power, not dB — averaging decibels would weight a quiet bucket as
// heavily as a loud one and understate exactly the concentration being looked for.
float Knock::pre_spark_fraction(const KnockProfile& p, float spark_deg) const {
    if (!p.hasPhase()) return -1.0f;
    float pre = 0.0f, total = 0.0f;
    for (unsigned i = 0; i < p.count; ++i) {
        const float lin = powf(10.0f, p.db[i] * 0.1f);
        total += lin;
        if (p.angleOf(i) < spark_deg) pre += lin;
    }
    return (total > 0.0f) ? (pre / total) : 0.0f;
}

void Knock::on_preignition(uint8_t cyl, uint32_t now) {
    if (cyl >= MAX_CYL || !cfg_) return;
    preign_last_ms_[cyl] = now;
    if (preign_events_[cyl] < 0xFFu) preign_events_[cyl]++;
    if (preign_events_[cyl] < cfg_->preign_events_to_act) return;   // not confirmed yet

    // Name the cylinder. Without this a stored code leaves every piston a suspect, which is the whole
    // argument the EGT probes already won.
    if (dtc_)
        // HOW LONG A STRIKE STANDS IS ALREADY A TUNE SETTING. preign_clear_s is the quiet period after
        // which this module releases the cut, resets the tally and heals this very code (see update()),
        // so the code's freshness is that same number — the light and the cut go out together, and
        // neither outlives the other because somebody changed one of them.
        //
        // It was marked LATCHING on the reasoning that a strike is an event nothing re-asserts. True,
        // and it does not follow: an event that the tune says expires is a ttl, not a latch.
        dtc_->raise(static_cast<uint16_t>(ModuleDtc::PREIGN_CYL_1 + cyl), DtcSource::PROTECTION,
                    ModuleDtc::PREIGN_CYL_1_SEV, now,   // level 3, declared in the schema
                    cfg_->preign_clear_s ? static_cast<uint32_t>(cfg_->preign_clear_s) * 1000u
                                         : DTC_TTL_LATCH);

    // CUT, never retard. The spark did not light this charge, so retarding the spark does not address
    // the cause and leaves more time for the hot spot to do it again.
    if (cfg_->preign_cut_fuel || cfg_->preign_cut_spark)
        preign_cut_mask_ |= static_cast<uint16_t>(1u << cyl);
}

void Knock::learn(uint8_t cyl, float db) {
    if (cyl >= MAX_CYL || !noise_[cyl] || !nsamp_[cyl] || !cfg_) return;
    float& f = noise_[cyl][cell_];
    float& n = nsamp_[cyl][cell_];

    if (n <= 0.0f) {          // first evidence this cell has ever had — take it whole
        f = db;
        n = 1.0f;
        return;
    }
    // FAST-LEARN then EMA. While a cell is young a 1/n running mean converges in a handful of cycles;
    // an EMA started at the seed would crawl toward the truth and leave the engine mis-thresholded for
    // the whole climb. Once established, the fixed rate takes over so the floor tracks a slowly
    // changing engine without being yanked by one loud cycle.
    if (n < static_cast<float>(cfg_->learn_fast_n)) {
        n += 1.0f;
        f += (db - f) / n;
    } else {
        const float alpha = static_cast<float>(cfg_->learn_rate) * 0.001f;
        f += (db - f) * alpha;
        if (n < 65000.0f) n += 1.0f;   // keep counting (telemetry) without overflowing the float's ints
    }
}

// A completed per-cylinder knock measurement. ENGINE-MODULE TASK ONLY (see the header).
void Knock::on_knock_sense(uint8_t cyl, float db, const KnockProfile* profile) {
    if (!cfg_ || !cfg_->enabled || suppressed_) return;

    if (cyl < KNOCK_CYL_SENSOR_COUNT)
        db += cfg_->cyl_sensor[cyl].gain * 0.1f;     // per-cylinder dB trim (manual override; the learned
                                                   // floor is the real normalisation)
    level_db_ = std::max(level_db_, db);           // peak-hold (decayed for telemetry in update())

    // The whole point: measure against what THIS cylinder normally sounds like HERE, not an absolute.
    const float floor_db  = noise_floor(cyl);
    const float intensity = db - floor_db;
    last_intensity_ = intensity;

    // BOOTSTRAP. A cell nothing has ever taught has no floor — only the configured seed, which is a
    // guess about an engine the firmware has never heard. Judging against it is unfounded in BOTH
    // directions, and one of them is a trap that never clears: if this cylinder genuinely sits above
    // the seed, every cycle reads as knock, a knocking cycle is never allowed to teach, and the cell
    // stays at the seed permanently — deaf, retarding forever, on an engine doing nothing wrong.
    //
    // So an unlearned cell LISTENS instead of judging. It costs the first cycle in each cell, during
    // which that cylinder is unprotected — which is honest, and far better than being confidently
    // wrong about an engine whose noise floor is still unknown. Everything after has real evidence.
    auto record = [&](uint8_t verdict, float pre_frac) {
        shot_.profile   = profile ? *profile : KnockProfile{};
        shot_.db        = db;
        shot_.floor_db  = floor_db;
        shot_.intensity = intensity;
        shot_.threshold = threshold_db_;
        shot_.spark_deg = spark_deg_;
        shot_.pre_frac  = pre_frac;
        shot_.cyl       = cyl;
        shot_.verdict   = verdict;
        ++shot_.seq;
        // Latch anything that was not routine, so it survives the flood of clean windows behind it.
        if (verdict == Knocking || verdict == PreIgnition) event_ = shot_;
    };

    if (noise_samples(cyl) <= 0.0f) {
        record(Bootstrapped, -1.0f);
        if (learn_ok_) learn(cyl, db);
        return;
    }

    if (intensity > threshold_db_) {
        // Something abnormal happened. WHICH abnormal thing decides the response, and the two
        // responses are opposites: knock answers to retard, pre-ignition answers to a cut.
        last_pre_frac_ = -1.0f;
        if (cfg_->preign_enabled) {
            const float pre_frac = profile ? pre_spark_fraction(*profile, -spark_deg_) : -1.0f;
            last_pre_frac_ = pre_frac;

            // EXTREME magnitude is pre-ignition-class whatever the phase says. This is the deliberate
            // fail-toward-protection rule: at this level the event is destroying something, and if we
            // cannot tell which fault it is, cutting is the safe error and retarding is not.
            const bool extreme = intensity > cfg_->preign_extreme_db * 0.1f;

            // Otherwise it takes BOTH: energy genuinely before the spark, AND a magnitude well past
            // the knock threshold. Either alone false-positives — a noisy sensor leaks energy into
            // the pre-spark buckets, and ordinary heavy knock is loud.
            const bool phase_says = pre_frac >= 0.0f
                                 && pre_frac >= static_cast<float>(cfg_->preign_pre_frac) * 0.01f;
            const bool loud_enough = intensity > threshold_db_ + cfg_->preign_margin_db * 0.1f;

            if (extreme || (phase_says && loud_enough)) {
                record(PreIgnition, pre_frac);
                on_preignition(cyl, last_ms_);
                return;   // and NOT a knock event: it never touches the retard
            }
        }
        record(Knocking, last_pre_frac_);
        // THIS cylinder's retard. From an external source the measurement carries no cylinder (it arrives
        // as 0) and is the engine's, so it goes on every cylinder alike.
        const float step = cfg_->retard_step_deg * 0.01f;
        if (cfg_->source != 0) { for (float& r : cyl_retard_) r = std::min(r + step, max_retard_); }
        else if (cyl < MAX_CYL) cyl_retard_[cyl] = std::min(cyl_retard_[cyl] + step, max_retard_);
        retard_deg_ = 0.0f;
        for (float r : cyl_retard_) retard_deg_ = std::max(retard_deg_, r);
        count_++;
        return;   // NEVER teach the floor from a knocking cycle — it would chase the knock and go deaf
    }
    // Anything at or below the floor is not knock by construction, so it lands here too — which is
    // what lets a cell recover if its very first cycle happened to be a knocking one and set the
    // floor too high. Fast-learn then pulls it down over the next few clean cycles.
    record(Clean, last_pre_frac_);
    if (learn_ok_) learn(cyl, db);
}

void Knock::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) {
    (void)frame;
    const uint32_t now = platform_get_tick_ms();
    const float dt = (now - last_ms_) * 0.001f;   // seconds since last update (decay rate)
    last_ms_ = now;

    // Per cylinder to Ignition, except from an external source, which cannot say which cylinder knocked
    // and so retards them all (its measurements arrive as cylinder 0).
    auto apply = [&](float) {
        if (!ignition_) return;
        if (cfg_ && cfg_->source != 0) ignition_->apply_knock_retard(retard_deg_);
        else                           ignition_->apply_knock_retard(cyl_retard_, MAX_CYL);
    };

    if (!cfg_ || !cfg_->enabled) {
        clear_retard();
        suppressed_ = true;
        head_ = tail_ = 0;              // drop anything the worker queued while we were off
        apply(0.0f);
        bus.set(SIG_KNOCK_LEVEL, kNoKnockDb, true, now, ttl());
        // Heal what we raised: this return skips every heal() below — see DtcManager::heal. This is
        // the case that was reported. All four kinds go, including the per-cylinder pre-ignition
        // block: heal() on a code that was never stored is a no-op, so sweeping the block costs
        // nothing and cannot miss a cylinder by reading a mask that the cut-release already cleared.
        if (was_enabled_ && dtc_) {
            dtc_->heal(ModuleDtc::KNOCK_TPS);
            dtc_->heal(ModuleDtc::KNOCK_1);
            dtc_->heal(ModuleDtc::KNOCK_2);
            for (uint8_t c = 0; c < MAX_CYL; ++c)
                dtc_->heal(static_cast<uint16_t>(ModuleDtc::PREIGN_CYL_1 + c));
        }
        was_enabled_ = false;
        return;
    }
    was_enabled_ = true;

    // Resolve the operating point ONCE per tick. Every measurement drained below is judged against
    // this one point rather than re-reading the bus mid-drain, so a burst captured microseconds apart
    // cannot be thresholded against two different cells.
    const float rpm  = pos.rpm;
    const float load = bus.get(wk::fuel_load, 0.0f);
    cell_ = cell_index(rpm, load);
    // Live spark advance (positive = BTDC). The pre-ignition test is "energy before the SPARK", not
    // before TDC — the spark moves with the map, so a fixed angle would mean a different question at
    // every operating point. Profiles are stamped ATDC-positive, so the spark sits at -advance.
    spark_deg_ = bus.get(wk::advance, 0.0f);
    threshold_db_ = tbl::table_eval(knock_threshold_table_desc(cfg_), bus);
    max_retard_   = tbl::table_eval(knock_max_retard_table_desc(cfg_), bus);

    // SUPPRESSION — when there is nothing worth listening to.
    //
    // The precise condition is NO COMBUSTION, and the bus already states it exactly: DFCO (overrun
    // fuel cut) ORs into wk::fuel_cut. The engine is being driven by the wheels, nothing is burning,
    // so there is no knock to find and nothing representative to learn from.
    //
    // Throttle position used to carry this, and it was the wrong proxy twice over. It answered a
    // different question — light load is not the same as no combustion — and on a drive-by-wire
    // engine the ETB also does idle control, so the plate sits at a few percent AT IDLE. A 5%
    // default therefore straddled the idle operating point and would have flickered knock detection
    // on and off there. Worse, the gate silently disabled detection entirely whenever TPS was
    // unconfigured or dropped out.
    //
    // What replaced it is not a looser gate but a more precise one: the per-cell learned noise floor
    // already normalises light-load mechanical noise by construction — that is the whole point of a
    // floor per operating point — so the throttle threshold was doing work nothing needed done.
    // It survives as an OPT-IN override (0 = off, the default) for anyone who wants it.
    const bool cut = bus.valid(wk::fuel_cut);
    const bool tps_gate = cfg_->suppress_min_tps > 0;
    const bool tps_ok   = bus.valid(wk::tps);
    const float tps     = bus.get(wk::tps, 0.0f);
    const bool tps_low  = tps_gate && (!tps_ok || tps < static_cast<float>(cfg_->suppress_min_tps) * 0.1f);
    suppressed_ = cut || tps_low;
    // Only a fault when the tuner ASKED for the TPS gate. Requiring a signal the configuration does
    // not use would be inventing a dependency.
    if (dtc_) {
        if (tps_gate && !tps_ok) dtc_->raise(ModuleDtc::KNOCK_TPS, DtcSource::MODULE, ModuleDtc::KNOCK_TPS_SEV, now, dtc_ttl());
        else                     dtc_->heal(ModuleDtc::KNOCK_TPS);
    }

    // Cell dwell — settle before teaching, so a transient sweeping across cells does not smear one
    // cell's floor into its neighbours.
    if (cell_ != dwell_cell_) { dwell_cell_ = cell_; dwell_since_ms_ = now; }
    const bool dwelt = (now - dwell_since_ms_) >= cfg_->learn_dwell_ms;

    // LEARN GATE — stricter than "not knocking". A floor is only meaningful when the engine is
    // actually running at a representative operating point: above the cranking floor, settled in one
    // cell, not in fuel cut, and not light-load-suppressed (where the measurement is not trusted for
    // detection either, so it must not be trusted to teach).
    // suppressed_ already carries the fuel-cut condition, so this adds only what learning needs
    // beyond detection: a settled cell and a speed where the background is representative.
    // NOT UNDER A SPARK CUT EITHER. A cylinder whose coil is cut (rev limiter, launch, flat shift,
    // traction, a soft cut's pattern) does not burn, so its window is "clean" by construction — and
    // averaging those into the floor drags it down in exactly the high-load cells, after which normal
    // combustion reads as knock and timing is pulled for nothing. Detection may run; teaching may not.
    const bool spark_cut = bus.valid(wk::ign_cut) || bus.get(SIG_SOFT_CUT_PCT, 0.0f) > 0.0f;
    learn_ok_ = !suppressed_ && !spark_cut && dwelt && rpm >= static_cast<float>(cfg_->learn_min_rpm);

    if (suppressed_) {
        // RECOVER AT THE RECOVERY RATE — never snap back. wk::fuel_cut is not only overrun: the rev
        // limiter, launch, flat shift, traction, overboost, EGT and lean protection all cut fuel through
        // it. Zeroing here handed a knocking engine all its timing back the moment it touched the limiter
        // or a protection tripped, to be re-earned by knocking again. Suppressed is "no knock seen", so the
        // retard is given back exactly as it is on any quiet stretch: at Retard Recovery Rate.
        recover(dt);
        apply(retard_deg_);
        level_db_ = std::max(kNoKnockDb, level_db_ - kLevelDecay * dt);   // keep the telemetry peak decaying
        head_ = tail_;                                                    // discard suppressed measurements
        bus.set(SIG_KNOCK_LEVEL, level_db_, true, now, ttl());
        bus.set(SIG_KNOCK_COUNT, static_cast<float>(count_ & 0xFFFFu), true, now, ttl());   // the channel is 16-bit: it WRAPS, so differences stay right
        // The pre-ignition cut is published every tick (see below) — a suppressed tick included, or it
        // expires and the cut cylinder is fuelled the moment the fuel cut ends.
        bus.set(SIG_PREIGN_CUT_MASK, static_cast<float>(preign_cut_mask_), true, now, ttl());
        return;
    }

    // SENSOR HEALTH (onboard sensors publish knock_1/2). Skipped for the external-signal source.
    //  * Judged only for a sensor some cylinder is actually mapped to — a single-sensor engine has no
    //    knock_2 and that is not a fault — and only while running above the learn floor.
    //  * MISSING: the channel expired (KnockDetector now publishes with a lifetime — without one this
    //    check could never trip at all).
    //  * TOO QUIET (opt-in): a disconnected sensor still produces bursts — the ADC samples its own noise —
    //    so "missing" cannot see it. It reads far quieter than a live sensor on a running engine; below
    //    the configured level for a second, it is reported the same way.
    if (dtc_ && cfg_->source == 0) {
        bool used[2] = { false, false };
        const uint8_t ncyl = std::min<uint8_t>(g_config.engine.cylinder_count, MAX_CYL);
        for (uint8_t c = 0; c < ncyl && c < KNOCK_CYL_SENSOR_COUNT; ++c) {
            uint8_t in = cfg_->cyl_sensor[c].input;
            if (in > 1) in = (g_config.engine.cyl[c].bank >= 2) ? 1 : 0;     // Auto: by bank
            used[in] = true;
        }
        // …AND ONLY WITH COMBUSTION. The windows are armed from the spark schedule, so with fuel or spark
        // cut — a protection level holding a cut after a trigger fault, a rev limiter, overrun — no
        // window is sampled and the channel expires on a perfectly good sensor. Judged then, a cut
        // caused by one fault reported a second one (P1750) that was nothing but its consequence.
        const bool combustion = !bus.valid(wk::fuel_cut) && !bus.valid(wk::ign_cut);
        // …AND NOT AT ONCE. The first window is sampled a few firings after spark begins; judged in the
        // frame the engine crosses the floor, the channel has not been published yet. A real start ramps
        // through cranking and hides that, but key-on into a crank already turning (a bench, a bump
        // start) jumps straight over the floor and reported P1750 on a sensor that had not been read.
        const bool running = combustion && rpm >= static_cast<float>(cfg_->learn_min_rpm);
        judge_ms_ = running ? std::min(judge_ms_ + std::min(dt * 1000.0f, 100.0f), 1000.0f) : 0.0f;  // a gap is not time running
        const bool judge = judge_ms_ >= 1000.0f;
        static constexpr SignalId SIG[2]  = { SIG_KNOCK_1, SIG_KNOCK_2 };
        static constexpr uint16_t CODE[2] = { ModuleDtc::KNOCK_1, ModuleDtc::KNOCK_2 };
        static constexpr uint8_t  SEV[2]  = { ModuleDtc::KNOCK_1_SEV, ModuleDtc::KNOCK_2_SEV };
        for (int k = 0; k < 2; ++k) {
            const bool missing = !bus.valid(SIG[k]);
            const bool quiet_now = cfg_->quiet_check_en && !missing
                                && bus.get(SIG[k], 0.0f) < static_cast<float>(cfg_->quiet_db) * 0.1f;
            quiet_ms_[k] = (judge && quiet_now) ? quiet_ms_[k] + dt * 1000.0f : 0.0f;
            if (used[k] && judge && (missing || quiet_ms_[k] >= 1000.0f))
                dtc_->raise(CODE[k], DtcSource::MODULE, SEV[k], now, dtc_ttl());
            else
                dtc_->heal(CODE[k]);
        }
    }

    if (cfg_->source != 0) {
        // External source: read the configured intensity signal and feed it as a global (cyl 0) sense.
        const SignalId esig = static_cast<SignalId>(cfg_->external_intensity_sig);
        if (esig != SIG_NONE && bus.valid(esig))
            on_knock_sense(0, bus.get(esig, kNoKnockDb));
    } else {
        // Onboard: drain everything the knock worker posted since the last tick. This is the ONLY
        // place worker measurements are consumed, which is what keeps the learned map and the retard
        // single-threaded.
        uint8_t t = tail_;
        while (t != head_) {
            const uint8_t       cyl = ring_[t].cyl;
            const float         db  = ring_[t].db;
            const KnockProfile& pf  = ring_[t].profile;
            on_knock_sense(cyl, db, &pf);
            t = static_cast<uint8_t>((t + 1u) & (RING - 1u));
            tail_ = t;
        }
    }

    // Optional recovery: restore a cut cylinder after a quiet interval. Default 0 = latch until the
    // engine stops, which is the conservative choice for a fault that destroys pistons — a cylinder
    // that pre-ignited once will do it again under the same conditions.
    if (cfg_->preign_clear_s && preign_cut_mask_) {
        const uint32_t hold_ms = static_cast<uint32_t>(cfg_->preign_clear_s) * 1000u;
        for (uint8_t c = 0; c < MAX_CYL; ++c) {
            if (!(preign_cut_mask_ & (1u << c))) continue;
            if (now - preign_last_ms_[c] < hold_ms) continue;
            preign_cut_mask_ &= static_cast<uint16_t>(~(1u << c));
            preign_events_[c] = 0;
            if (dtc_) dtc_->heal(static_cast<uint16_t>(ModuleDtc::PREIGN_CYL_1 + c));
        }
    }

    // Decay the accumulated retard toward 0 at the recovery rate (accumulation is in on_knock_sense).
    recover(dt);
    level_db_   = std::max(kNoKnockDb, level_db_ - kLevelDecay * dt);   // telemetry peak decay

    apply(retard_deg_);
    bus.set(SIG_KNOCK_LEVEL, level_db_, true, now, ttl());
    bus.set(SIG_KNOCK_COUNT, static_cast<float>(count_ & 0xFFFFu), true, now, ttl());   // the channel is 16-bit: it WRAPS, so differences stay right
    // The module DECIDES which cylinders are cut; EngineTask ACTUATES it, exactly as it does for
    // every other cut. Published every tick rather than on the transition so a reconfigure — which
    // can move a cylinder's output channels — cannot strand a cut on the wrong channel.
    bus.set(SIG_PREIGN_CUT_MASK, static_cast<float>(preign_cut_mask_), true, now, ttl());
}
