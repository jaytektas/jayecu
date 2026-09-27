#include "Misfire.h"
#include "well_known_signals.h"
#include "../../Diagnostics/DtcManager.h"
#include "../../Diagnostics/Dtc.h"
#include "../../Scheduler/SegmentTimer.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"
#include "../../../generated/learned_layout.h"
#include "../../../generated/signal_ids.h"

#include <algorithm>

namespace {
// Standard OBD misfire codes. `code` is a flat uint16 where 0x0301 == P0301 (see Dtc.h), so the
// standard range needs no manufacturer namespace — these ARE the codes every scan tool knows.
constexpr uint16_t P0300_RANDOM = 0x0300;
constexpr uint16_t P0301_CYL1   = 0x0301;

static_assert(LEARNED_MISFIRE_SEG_1_CELLS == 1, "misfire_seg is one correction per cylinder");

constexpr uint32_t kCorrOff[Misfire::MAX_CYL] = {
    LEARNED_MISFIRE_SEG_1_OFFSET,  LEARNED_MISFIRE_SEG_2_OFFSET,  LEARNED_MISFIRE_SEG_3_OFFSET,
    LEARNED_MISFIRE_SEG_4_OFFSET,  LEARNED_MISFIRE_SEG_5_OFFSET,  LEARNED_MISFIRE_SEG_6_OFFSET,
    LEARNED_MISFIRE_SEG_7_OFFSET,  LEARNED_MISFIRE_SEG_8_OFFSET,  LEARNED_MISFIRE_SEG_9_OFFSET,
    LEARNED_MISFIRE_SEG_10_OFFSET, LEARNED_MISFIRE_SEG_11_OFFSET, LEARNED_MISFIRE_SEG_12_OFFSET };
}

void Misfire::init(const MisfireConfig& cfg) {
    cfg_ = &cfg;
    for (uint8_t c = 0; c < MAX_CYL; ++c) {
        auto* base = platform_learned_block(kCorrOff[c], sizeof(float));
        corr_[c] = base ? reinterpret_cast<float*>(base) : nullptr;
    }
    on_engine_stop();
}

void Misfire::on_engine_stop() {
    rn_ = rhead_ = 0;
    cut_mask_ = 0;
    cycles_ = 0;
    for (uint8_t c = 0; c < MAX_CYL; ++c) { rough_[c] = 0.0f; events_[c] = 0; }
}

// A cell nothing has taught reads 1.0 — a perfect segment — not whatever the region happened to hold.
// A fresh learned region is zeroes, and dividing a segment by zero correction would be spectacular.
float Misfire::correction(uint8_t cyl) const {
    if (cyl >= MAX_CYL || !corr_[cyl]) return 1.0f;
    const float v = *corr_[cyl];
    return (v > 0.5f && v < 2.0f) ? v : 1.0f;      // sane band; anything else is unlearned or corrupt
}

void Misfire::learn(uint8_t cyl, float ratio) {
    if (cyl >= MAX_CYL || !corr_[cyl] || !cfg_) return;
    if (ratio < 0.5f || ratio > 2.0f) return;      // not geometry — a transient or a bad segment
    float& c = *corr_[cyl];
    if (c <= 0.5f || c >= 2.0f) { c = ratio; return; }   // first evidence: take it whole
    c += (ratio - c) * (static_cast<float>(cfg_->learn_rate) * 0.001f);
}

void Misfire::classify(uint8_t cyl, uint32_t ticks, bool overrun, uint32_t now) {
    if (cyl >= MAX_CYL || ticks == 0) return;
    ++seen_;

    // Correct for the wheel BEFORE comparing. Without this a consistently-narrow tooth reads as a
    // cylinder that misfires on every single cycle.
    const float corrected = static_cast<float>(ticks) / correction(cyl);

    // The neighbourhood mean — taken before this sample joins, so a cylinder is measured against the
    // ones around it and not against a mean it has already pulled up.
    float mean = 0.0f;
    if (rn_ > 0) {
        float sum = 0.0f;
        for (uint8_t i = 0; i < rn_; ++i) sum += static_cast<float>(ring_[i]);
        mean = sum / static_cast<float>(rn_);
    }
    ring_[rhead_] = static_cast<uint32_t>(corrected);
    rhead_ = static_cast<uint8_t>((rhead_ + 1u) % (ncyl_ ? ncyl_ : MAX_CYL));
    if (rn_ < (ncyl_ ? ncyl_ : MAX_CYL)) ++rn_;
    if (mean <= 0.0f) return;                       // nothing to compare against yet

    const float dev = (corrected - mean) / mean;
    rough_[cyl] = dev;

    // OVERRUN IS THE CALIBRATION, not a measurement. Nothing is firing, so every segment should be
    // identical and the spread that remains is geometry. Learn it and judge nothing.
    if (overrun) {
        learning_ = true;
        // The target is the ratio at which this cylinder's CORRECTED segment equals the mean:
        //     new_corr = corr * (corrected / mean) = ticks / mean
        // which is a fixed point — once the corrections are right, every corrected segment equals the
        // mean and the update asks for no change. `mean` is of corrected values, so this converges
        // rather than chasing its own tail.
        if (cfg_->learn_enabled) learn(cyl, static_cast<float>(ticks) / mean);
        return;
    }
    learning_ = false;

    if (dev > static_cast<float>(cfg_->threshold_pct) * 0.001f) {
        if (events_[cyl] < 0xFFFFu) ++events_[cyl];
        if (total_ < 0xFFFFFFFFu) ++total_;
    }

    // ---- Raise, when the tally says it is a fault rather than a bad cycle --------------------
    if (!dtc_) return;
    uint8_t over = 0;
    for (uint8_t c = 0; c < ncyl_ && c < MAX_CYL; ++c)
        if (events_[c] >= cfg_->events_to_dtc) ++over;

    if (over >= cfg_->multi_cyl_for_p0300) {
        // Several cylinders at once is not several faults — it is one, and P0300 is the code that
        // says so. Reporting four per-cylinder codes for a failing coil pack or a fuel-supply problem
        // sends the person holding the spanner to four innocent cylinders.
        // NOT LATCHING, either of these. classify() runs per SEGMENT and re-raises while the tally is
        // over the threshold, so they assert continuously for as long as the engine turns. The latch
        // only ever changed what happens when the engine STOPS — and "currently misfiring" is not true
        // of a stopped engine. The history is what STORED is for, and it survives.
        dtc_->raise(P0300_RANDOM, DtcSource::PROTECTION, DTC_SEV_LEVEL1, now, dtc_ttl());
    } else if (over > 0) {
        for (uint8_t c = 0; c < ncyl_ && c < MAX_CYL; ++c) {
            if (events_[c] < cfg_->events_to_dtc) continue;
            dtc_->raise(static_cast<uint16_t>(P0301_CYL1 + c), DtcSource::PROTECTION, DTC_SEV_LEVEL1,
                        now, dtc_ttl());
            if (cfg_->cut_fuel) cut_mask_ |= static_cast<uint16_t>(1u << c);
        }
    }
}

void Misfire::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) {
    (void)frame;
    if (!cfg_ || !timer_) return;
    const uint32_t now = platform_get_tick_ms();

    if (!cfg_->enabled) {
        SegmentTimer::Segment drop{};
        while (timer_->pop(drop)) {}                // do not let a disabled module bank a backlog
        bus.set(SIG_MISFIRE_CUT_MASK, 0.0f, true, now, ttl());
        // HEAL WHAT WE RAISED, on the enabled->disabled edge. This module never healed a misfire code
        // at all: not when the engine came good, and not when it was switched off — so P0300/P030x
        // stayed current until the next key-off, with nothing in the system able to retire them. The
        // table's ttl cannot do it either, because these codes deliberately LATCH (a tally of events
        // that happened, which no pass re-asserts). A tally nobody is keeping any more is not a fault
        // anybody is still reporting; the history stays STORED, which is where it belongs.
        if (was_enabled_ && dtc_) {
            dtc_->heal(P0300_RANDOM);
            for (uint8_t c = 0; c < MAX_CYL; ++c)
                dtc_->heal(static_cast<uint16_t>(P0301_CYL1 + c));
            for (uint8_t c = 0; c < MAX_CYL; ++c) events_[c] = 0;   // and the tally that would re-raise it
            total_ = 0;
        }
        was_enabled_ = false;
        cut_mask_ = 0;
        return;
    }
    was_enabled_ = true;

    ncyl_ = timer_->segment_count();      // the scheduler's count, via the timer that uses it
    const float rpm = pos.rpm;
    const bool overrun = bus.valid(wk::fuel_cut);

    // Speed window. Below the floor the crank is cranking or stalling and the variation is the
    // starter's; above the ceiling the segments are short enough that timing resolution and torsional
    // resonance swamp the combustion signal. Outside it, drain without judging — a segment measured
    // there is not evidence, and letting it queue would apply it late at a speed where it is.
    const bool in_window = rpm >= static_cast<float>(cfg_->min_rpm)
                        && rpm <= static_cast<float>(cfg_->max_rpm);

    SegmentTimer::Segment s{};
    while (timer_->pop(s)) {
        if (!in_window) { rn_ = 0; continue; }      // and forget the neighbourhood: it spans the gap
        classify(s.cyl, s.ticks, overrun, now);
        if (++cycles_ >= static_cast<uint32_t>(cfg_->window_cycles) * (ncyl_ ? ncyl_ : 1)) {
            // Window elapsed: decay the tallies rather than clearing them, so a fault that is still
            // happening stays raised while one that stopped fades.
            cycles_ = 0;
            for (uint8_t c = 0; c < MAX_CYL; ++c) events_[c] = static_cast<uint16_t>(events_[c] / 2);
        }
    }

    float worst = 0.0f;
    for (uint8_t c = 0; c < ncyl_ && c < MAX_CYL; ++c) worst = std::max(worst, rough_[c]);
    bus.set(SIG_MISFIRE_ROUGH,    worst * 100.0f, true, now, ttl());
    bus.set(SIG_MISFIRE_COUNT,    static_cast<float>(total_), true, now, ttl());
    bus.set(SIG_MISFIRE_CUT_MASK, static_cast<float>(cut_mask_), true, now, ttl());
}
