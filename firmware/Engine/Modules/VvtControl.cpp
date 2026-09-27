#include "VvtControl.h"
#include "../TableEval.h"                        // tbl::table_eval
#include "../../../generated/table_descs.h"      // intake/exhaust_target_table_desc
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"         // platform_get_tick_ms, platform_learned_block
#include "../../../generated/signal_ids.h"
#include "../../../generated/learned_layout.h"    // LEARNED_VVT_LTT_* (fixed offset + magic + dims)
#include <algorithm>
#include <cmath>

static_assert(LEARNED_VVT_LTT_CELLS == VvtControl::LTT_CELLS, "learned_layout VVT LTT dims != module");

namespace {
constexpr float LEARN_WINDOW = 3.0f;        // learn only within ±3° of target (settled)
}

void VvtControl::init(const VvtControlConfig& cfg) {
    cfg_ = &cfg;
    reset();
    // Map the 4 per-cam LTT grids onto their FIXED slice of the learned region (offset from
    // generated/learned_layout.h). The region is a live RAM buffer, restored from the SD totem before init,
    // so just map it: 0 cells = neutral (re-learn), non-zero = restored trim. Validity/staleness is the
    // totem header's job (crc32 + layout_hash), not a per-block magic.
    const uint32_t need = LTT_CELLS * sizeof(float);
    auto* base = platform_learned_block(LEARNED_VVT_LTT_OFFSET, need);
    if (base) {
        ltt_ = reinterpret_cast<float*>(base);
    } else {
        static float fallback[LTT_CELLS] = {};   // region wouldn't fit → RAM, re-learn each boot
        ltt_ = fallback;
    }
}

// The learned cell for (cam, coolant). The LTT store IS a table — 4 cam rows x CLT columns — so its
// column comes from its own declared axis through the one bin lookup, not from arithmetic here.
//
// It used to be `static_cast<int>(clt / CLT_SPAN * CLT_BINS)` against a private CLT_SPAN of 140, which
// was wrong at the end of the range that matters most: a negative coolant reading truncates toward zero,
// so -40 °C and +17 °C both landed in bin 0 and shared one learned value, while bins were spent on
// 120-140 °C where the engine rarely sits. The axis is a tunable config array now and can start below
// zero, and this stopped having an opinion about it.
int VvtControl::ltt_cell(int loop, float clt) const {
    if (!cfg_) return loop * CLT_BINS;
    const tbl::TableDesc d = vvt_ltt_desc(cfg_);
    const int cols = tbl::axis_live_n(d.x);
    return loop * (cols > 0 ? cols : 1) + tbl::nearest_bin(d.x, clt);
}

void VvtControl::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now = platform_get_tick_ms();
    const float    dt  = (last_ms_ != 0) ? static_cast<float>(now - last_ms_) * 0.001f : 0.0f;
    last_ms_ = now;

    if (!cfg_ || !cfg_->enabled) {
        reset();
        bus.set(SIG_VVT_DUTY_1, 0.0f, true, now, ttl()); bus.set(SIG_VVT_DUTY_2, 0.0f, true, now, ttl());
        bus.set(SIG_VVT_DUTY_3, 0.0f, true, now, ttl()); bus.set(SIG_VVT_DUTY_4, 0.0f, true, now, ttl());
        bus.set(SIG_VVT_LTT_1, 0.0f, true, now, ttl());  bus.set(SIG_VVT_LTT_2, 0.0f, true, now, ttl());
        bus.set(SIG_VVT_LTT_3, 0.0f, true, now, ttl());  bus.set(SIG_VVT_LTT_4, 0.0f, true, now, ttl());
        return;
    }

    bus.set(wk::rpm, pos.rpm, true, platform_get_tick_ms());   // freshen the target-table rpm axis; decoder owns rpm freshness (no ttl)                   // freshen the target table's rpm axis

    const int   mode      = cfg_->mode;                           // 0 Intake · 1 Exhaust · 2 both
    const bool  intakeOn  = (mode == 0 || mode == 2);
    const bool  exhaustOn = (mode == 1 || mode == 2);
    const bool  bank2     = cfg_->num_banks >= 2;
    const float maxDelta  = static_cast<float>(cfg_->max_delta_rate) * 0.1f;   // deg/s (0 = no limit)
    const float clt       = bus.get(wk::clt, 20.0f);                          // LTT + gain-table axis

    struct L { bool active; bool intake; int bank; SignalId angle; SignalId duty; SignalId ltt; };
    const L defs[4] = {
        { intakeOn,           true,  0, SIG_VVT_ANGLE_1, SIG_VVT_DUTY_1, SIG_VVT_LTT_1 },   // Intake  Bank 1
        { exhaustOn,          false, 0, SIG_VVT_ANGLE_2, SIG_VVT_DUTY_2, SIG_VVT_LTT_2 },   // Exhaust Bank 1
        { intakeOn  && bank2, true,  1, SIG_VVT_ANGLE_3, SIG_VVT_DUTY_3, SIG_VVT_LTT_3 },   // Intake  Bank 2
        { exhaustOn && bank2, false, 1, SIG_VVT_ANGLE_4, SIG_VVT_DUTY_4, SIG_VVT_LTT_4 },   // Exhaust Bank 2
    };

    for (int i = 0; i < 4; ++i) {
        const L&  d  = defs[i];
        Loop&     lp = loops_[i];
        if (!d.active) {                                          // idle loop → hold at 0, no windup
            lp.pi.reset(); lp.ramped = 0.0f; lp.last_err = 0.0f; lp.duty = 0.0f;
            bus.set(d.duty, 0.0f, true, now, ttl());
            bus.set(d.ltt, 0.0f, true, now, ttl());
            continue;
        }

        // desired advance = target table + fixed overall correction (°), slew-limited to max_delta_rate
        const float overall = static_cast<float>(d.intake ? cfg_->intake_overall_corr : cfg_->exhaust_overall_corr) * 0.1f;
        const float target = tbl::table_eval(
            d.intake ? intake_target_table_desc(cfg_) : exhaust_target_table_desc(cfg_), bus) + overall;
        if (maxDelta <= 0.0f) {
            lp.ramped = target;                                  // no limit → command the target directly
        } else if (dt > 0.0f) {
            const float step = maxDelta * dt;
            lp.ramped += std::clamp(target - lp.ramped, -step, step);
        }                                                        // else: dt<=0 with a limit → hold ramped

        // per-type output shaping
        const float dutyMin  = static_cast<float>(d.intake ? cfg_->intake_duty_min  : cfg_->exhaust_duty_min)  * 0.1f;
        const float dutyMax  = static_cast<float>(d.intake ? cfg_->intake_duty_max  : cfg_->exhaust_duty_max)  * 0.1f;
        const float deadBand = static_cast<float>(d.intake ? cfg_->intake_dead_band : cfg_->exhaust_dead_band) * 0.1f;
        const int   dir      = d.intake ? cfg_->intake_direction : cfg_->exhaust_direction;   // 0 Advance · 1 Retard

        // NO CAM READING, NO CLOSED LOOP. Read as 0° it was a full error the loop drove hard against —
        // with no RPM or sync gate, from key-on onwards. Without a measured angle the cam runs on its base
        // duty (+ learned trim), the integrator held, until the angle is there again.
        const bool  have_cam = bus.valid(d.angle);
        const float measured = bus.get(d.angle, 0.0f);
        const float rawErr   = lp.ramped - measured;             // + = behind target (cam needs more advance)
        float err = (std::fabs(rawErr) <= deadBand) ? 0.0f : rawErr;   // dead band → hold (anti-hunt)
        if (dir != 0) err = -err;                                // Retard solenoid moves the cam the other way

        // gains + base duty from the per-type / per-bank Coolant-temp tables (table_eval applies scale)
        const float kp = tbl::table_eval(d.intake ? intake_p_gain_desc(cfg_) : exhaust_p_gain_desc(cfg_), bus);
        const float ki = tbl::table_eval(d.intake ? intake_i_gain_desc(cfg_) : exhaust_i_gain_desc(cfg_), bus);
        const float kd = tbl::table_eval(d.intake ? intake_d_gain_desc(cfg_) : exhaust_d_gain_desc(cfg_), bus);
        const tbl::TableDesc baseDesc = d.intake
            ? (d.bank == 0 ? intake_base_duty_1_desc(cfg_)  : intake_base_duty_2_desc(cfg_))
            : (d.bank == 0 ? exhaust_base_duty_1_desc(cfg_) : exhaust_base_duty_2_desc(cfg_));
        const float baseDuty = tbl::table_eval(baseDesc, bus);

        lp.pi.kp = kp; lp.pi.ki = ki;
        lp.pi.out_min = -dutyMax; lp.pi.out_max = dutyMax;       // PID trims ± around the base + LTT
        // THE WARM-UP SCALAR CUTS THE LOOP'S AUTHORITY ON PURPOSE, so the integrator must not learn to
        // fight it. It used to scale the output AFTER the PI: the loop never saw its authority cut, wound
        // to ±dutyMax while cold, and overshot the moment the scalar reached 100 % — the opposite of the
        // scalar's own help ("rather than letting the loop fight the oil and wind up"). While scaled, P and
        // D still act; only the integrator is held.
        const float scalar  = tbl::table_eval(target_scalar_desc(cfg_), bus) * 0.01f;
        const bool  loop_ok = have_cam && dt > 0.0f && scalar >= 0.999f;
        const float dterm   = (have_cam && dt > 0.0f && kd != 0.0f) ? kd * (err - lp.last_err) / dt : 0.0f;
        float trim;
        if (loop_ok)       trim = lp.pi.step(err, dt, dterm);    // D counts toward saturation
        else if (have_cam) trim = std::clamp(kp * err + lp.pi.integ + dterm, -dutyMax, dutyMax);
        else               trim = lp.pi.integ;                    // no angle: hold, correct nothing new
        if (have_cam) lp.last_err = err;

        // Long Term Trim: apply the learned per-cam duty (when enabled) and slowly migrate the PI hold-
        // trim into it while the cam is settled at target — so the feed-forward learns the hold duty and
        // the PI integral recenters (Lambda's STFT→LTFT idea). Persisted in battery-backed RAM.
        float ltt = 0.0f;
        if (ltt_ && cfg_->enable_ltt) {
            const int cell = ltt_cell(i, clt);
            ltt = ltt_[cell];
            if (loop_ok && std::fabs(rawErr) < LEARN_WINDOW) {        // learn only from a real, unscaled loop
                const float gain = static_cast<float>(d.intake ? cfg_->intake_ltt_gain : cfg_->exhaust_ltt_gain) * 0.01f;
                const float auth = static_cast<float>(cfg_->ltt_authority_pct) * 0.1f;
                const float neu    = std::clamp(ltt + gain * lp.pi.integ * dt, -auth, auth);
                const float actual = neu - ltt;
                ltt_[cell] = neu; ltt = neu;
                lp.pi.bleed(actual);                             // recenter PI (total correction preserved)
                trim -= actual;
            }
        }

        // final duty = base + LTT + PID trim, clamped, then scaled by the shared warm-up ramp
        float duty = std::clamp(baseDuty + ltt + trim, dutyMin, dutyMax);
        duty *= scalar;
        lp.duty = duty;
        bus.set(d.duty, lp.duty, true, now, ttl());
        bus.set(d.ltt, ltt, true, now, ttl());
    }
}
