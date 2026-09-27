#include "VirtualTrigger.h"
#include "../Platform/platform_hal.h"   // platform_irq_save / restore

// Velocity smoothing: ticks_per_vtooth EMA ≈ 2^N samples. The decoder's real
// teeth are the samples; this is the PLL's frequency estimate.
static constexpr uint32_t VT_VEL_EMA_SHIFT = 3u;   // ~8-tooth average

// Phase-correction slew clamp: a single real tooth may move the pending virtual tooth by at most
// (ticks_per_vtooth >> N). Half a tooth. It was an eighth, which is too timid for a cranking engine:
// the speed swings +/-40 % through every compression, the clock could not follow, and it ran up to
// 15 deg AHEAD of the crank — early spark, kickback (tests/test_angle_clock_ripple.cpp, 1.3 deg now).
// Real teeth reach here already judged by the decoder's match window, so they are not noise to reject.
static constexpr uint32_t VT_SLEW_SHIFT    = 1u;   // ≤ 1/2 of a vtooth per correction

VirtualTrigger::VirtualTrigger(IAlarmTimer& dco,
                               AngleDeg10     cycle_angle,
                               uint16_t       teeth_per_cycle) noexcept
    : dco_(dco), cb_(nullptr), cb_data_(nullptr),
      cycle_angle_(cycle_angle), teeth_per_cycle_(teeth_per_cycle),
      vt_angle_step_(static_cast<AngleDeg10>(cycle_angle / (teeth_per_cycle ? teeth_per_cycle : 1))),
      ticks_per_vtooth_(0), next_vt_angle_(0), next_vt_tick_(0),
      locked_(false), vt_emitted_(0), last_err_(0)
{}

void VirtualTrigger::register_callback(VirtualToothCallback cb, void* user_data) noexcept {
    cb_ = cb; cb_data_ = user_data;
}

// MASKED, because this runs from a task and on_real_tooth runs in the capture ISR. A tooth landing
// between reset() and the new geometry re-anchored and LOCKED on the old cycle and step, and nothing
// afterwards corrected the label — the grid could sit a revolution out at PHASE.
void VirtualTrigger::reconfigure(AngleDeg10 cycle_angle, uint16_t teeth_per_cycle) noexcept {
    const uint32_t pm = platform_irq_save();
    reset();
    cycle_angle_     = cycle_angle;
    teeth_per_cycle_ = teeth_per_cycle;
    vt_angle_step_   = static_cast<AngleDeg10>(
        cycle_angle / (teeth_per_cycle ? teeth_per_cycle : 1));
    vt_emitted_ = 0;
    platform_irq_restore(pm);
}

AngleDeg10 VirtualTrigger::wrap(int32_t a) const noexcept {
    while (a < 0)             a += cycle_angle_;
    while (a >= cycle_angle_) a -= cycle_angle_;
    return static_cast<AngleDeg10>(a);
}

void VirtualTrigger::arm_dco(uint32_t tick) noexcept {
    // The output-compare layer fires a callback at this tick; if it's already in
    // the past it fires immediately (the "no skip" guarantee) rather than waiting
    // a 32-bit wrap.
    dco_.arm(tick);
}

void VirtualTrigger::reset() noexcept {
    locked_ = false;
    ticks_per_vtooth_ = 0;     // re-prime the velocity EMA on next acquisition
    last_inst_ = 0;
    dco_.disarm();
}

// ---------------------------------------------------------------------------
// Fed by the decoder per real tooth. Runs in the capture ISR (higher priority
// than the DCO match), so it can preempt on_dco_match but never vice-versa.
// ---------------------------------------------------------------------------
void VirtualTrigger::on_real_tooth(AngleDeg10 angle, uint32_t tick,
                                   uint32_t real_tooth_ticks, AngleDeg10 real_tooth_angle,
                                   bool last_was_gap) noexcept
{
    if (real_tooth_angle <= 0 || real_tooth_ticks == 0 || vt_angle_step_ <= 0) return;

    // --- Frequency: convert this real-tooth period into ticks-per-virtual-tooth
    //     and EMA it. This is the live velocity every consumer reads. (Bench note:
    //     including the gap tooth's sample tracks BETTER than excluding it — the
    //     decoder feeds a consistent period/angle ratio across the gap.)
    // 32-bit hardware UDIV (NOT 64-bit __aeabi_uldivmod): at the 1 MHz timebase the
    // product fits uint32 for any running engine (overflow needs a >~42 s tooth).
    const uint32_t inst =
        (real_tooth_ticks * static_cast<uint32_t>(vt_angle_step_)) / static_cast<uint32_t>(real_tooth_angle);
    if (inst == 0) return;
    last_inst_ = inst;
    ticks_per_vtooth_ = (ticks_per_vtooth_ == 0u)
        ? inst
        : static_cast<uint32_t>((int32_t)ticks_per_vtooth_ +
              (((int32_t)inst - (int32_t)ticks_per_vtooth_) >> VT_VEL_EMA_SHIFT));

    // --- Re-anchor the grid to the decoder's ground-truth angle on ACQUISITION
    //     or across a GAP (a position discontinuity — slewing across it would
    //     just stall the DCO). Aim the next virtual tooth at the grid boundary
    //     strictly ahead of the current angle.
    if (!locked_ || last_was_gap) {
        const uint16_t idx = static_cast<uint16_t>(angle / vt_angle_step_ + 1);
        next_vt_angle_ = wrap(static_cast<int32_t>(idx) * vt_angle_step_);
        const AngleDeg10 dist = wrap(static_cast<int32_t>(next_vt_angle_) - angle);
        next_vt_tick_ = tick + static_cast<uint32_t>(
            (static_cast<uint32_t>(dist) * ticks_per_vtooth_) / static_cast<uint32_t>(vt_angle_step_));
        locked_ = true;
        last_err_ = 0;
        arm_dco(next_vt_tick_);
        return;
    }

    // --- Phase: re-aim the PENDING virtual tooth from this fresh anchor, slewed.
    //     Real tooth says the engine is HERE now; the next virtual tooth is
    //     `dist` ahead, so it *should* land at ideal_tick. Move toward it,
    //     clamped, never into the past.
    //
    //     dist is a SIGNED angular distance folded to (-cycle/2, +cycle/2]. If the
    //     real angle has just OVERTAKEN the pending tooth (DCO emitted a hair slow),
    //     the raw difference is a small NEGATIVE — the tooth is OVERDUE. It must read
    //     as due-now (dist = 0), NOT wrap to ~+one revolution. The old wrap()-to-
    //     [0,cycle) form turned "a hair late" into "a whole rev early", spiking the
    //     phase error to ~one rev and stalling the DCO (grid under-emitted at CRANK
    //     high-RPM, where the per-rev gap re-anchor perturbs phase most). "Fire a
    //     hair late, never skip."
    int32_t rel = static_cast<int32_t>(next_vt_angle_) - angle;
    while (rel >   cycle_angle_ / 2) rel -= cycle_angle_;
    while (rel <= -cycle_angle_ / 2) rel += cycle_angle_;
    const AngleDeg10 dist = (rel > 0) ? static_cast<AngleDeg10>(rel) : 0;  // overdue → due now
    // PROJECT WITH THIS TOOTH'S SPEED, not the smoothed one. The pending grid tooth is at most a tooth
    // or so ahead, and over that distance the speed the engine had a moment ago is the best estimate
    // there is. The ~8-tooth average is right for the grid's cadence between real teeth, but used here
    // it projects the crank at the speed of the last half-revolution — through every compression
    // slow-down of a cranking engine, so the grid ran ahead of the crank (early spark) by up to 15 deg
    // at +/-40 % ripple.
    const uint32_t ideal_tick = tick + static_cast<uint32_t>(
        (static_cast<uint32_t>(dist) * inst) / static_cast<uint32_t>(vt_angle_step_));
    int32_t err = static_cast<int32_t>(ideal_tick - next_vt_tick_);
    last_err_ = err;   // telemetry: lock quality (pre-slew)
    const int32_t clamp = static_cast<int32_t>(ticks_per_vtooth_ >> VT_SLEW_SHIFT);
    if (err >  clamp) err =  clamp;
    if (err < -clamp) err = -clamp;
    next_vt_tick_ += static_cast<uint32_t>(err);
    arm_dco(next_vt_tick_);
}

// ---------------------------------------------------------------------------
// DCO match — one virtual tooth. Runs in the output-compare ISR. We advance the
// grid pointer and re-arm BEFORE invoking the (heavier) scheduler callback, so a
// real tooth preempting the callback re-times the *next* tooth correctly.
// ---------------------------------------------------------------------------
void VirtualTrigger::on_dco_match() noexcept
{
    if (!locked_) return;
    const uint32_t now = dco_.now();

    // snapshot the tooth that just came due
    VirtualToothData vt;
    vt.angle            = next_vt_angle_;
    vt.index            = static_cast<uint16_t>(next_vt_angle_ / vt_angle_step_);
    vt.tick             = now;
    vt.ticks_per_vtooth = ticks_per_vtooth_;
    vt.cycle_start      = (vt.index == 0);

    // advance + re-arm the DCO for the next grid tooth. Advance on the IDEAL grid
    // (next_vt_tick_ += period), NOT from `now`: `now` is the ISR-entry time, which
    // lags the scheduled compare by the interrupt latency, so `now + period` would
    // bake that latency into every emit — a steady phase bias the loop then fights
    // forever (~30-tick residual). Advancing the scheduled tick keeps the grid on
    // its true cadence; real teeth nudge it. If the ISR ever overran a whole tooth
    // (next tick already in the past), resync to `now` rather than spiral/burst.
    next_vt_angle_ = wrap(static_cast<int32_t>(next_vt_angle_) + vt_angle_step_);
    // The cadence to the next grid tooth is the LAST real tooth's speed, for the same reason the
    // projection above uses it: over one tooth, the most recent speed is the best predictor. The
    // smoothed speed stays the reported velocity (rpm, dwell).
    const uint32_t pace = last_inst_ ? last_inst_ : ticks_per_vtooth_;
    next_vt_tick_ += pace;
    if (static_cast<int32_t>(next_vt_tick_ - now) <= 0)
        next_vt_tick_ = now + pace;
    arm_dco(next_vt_tick_);

    ++vt_emitted_;
    if (cb_) cb_(vt, cb_data_);
}
