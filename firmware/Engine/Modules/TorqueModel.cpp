#include "TorqueModel.h"
#include "well_known_signals.h"
#include "../TableEval.h"
#include "../../../generated/table_descs.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../../Platform/platform_hal.h"   // platform_get_tick_ms
#include "../EngineFrame.h"
#include "../../../generated/signal_ids.h"
#include <algorithm>

void TorqueModel::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now = platform_get_tick_ms();
    if (!cfg_ || !cfg_->enabled) {
        torque_nm_ = 0.0f; power_kw_ = 0.0f;
        bus.set(SIG_ENGINE_TORQUE_NM, 0.0f, true, now, ttl());
        bus.set(SIG_ENGINE_POWER_KW, 0.0f, true, now, ttl());
        bus.set(SIG_LAMBDA_TORQUE_RATIO, 1.0f, true, now, ttl());
        return;
    }
    // Reference brake torque at the current (rpm, load) operating point, from the calibration table
    // (calibrated at the base-map spark + best-torque lambda).
    const float base_nm = tbl::table_eval(torque_table_desc(cfg_), bus);

    // Lambda trim: index the curve by lambda/best_torque_lambda so the peak (ratio 1.0) is fuel-set
    // by the best_torque_lambda scalar, not baked into the curve. Absent lambda -> ratio 1.0 (no trim).
    const float best_lambda = static_cast<float>(cfg_->best_torque_lambda) * 0.001f;
    const float lambda = bus.get(wk::lambda, best_lambda);
    const float ratio  = (best_lambda > 0.01f) ? lambda / best_lambda : 1.0f;
    bus.set(SIG_LAMBDA_TORQUE_RATIO, ratio, true, now, ttl());  // publish BEFORE the trim eval reads it

    // Spark trim: indexed (by default) on spark_below_map — how far the commanded advance sits below the
    // main map, whatever put it there; Ignition publishes it. Both trims are % efficiency (0..100) -> fraction; default rows are 100% at the
    // reference (zero retard / ratio 1.0), so an untrimmed tune leaves the base estimate unchanged.
    const float spark_eff  = tbl::table_eval(torque_spark_trim_desc(cfg_),  bus) * 0.01f;
    const float lambda_eff = tbl::table_eval(torque_lambda_trim_desc(cfg_), bus) * 0.01f;
    torque_nm_ = base_nm * spark_eff * lambda_eff;

    // Power from torque and angular speed: P[W] = T[Nm] * omega[rad/s], omega = rpm * 2*pi/60.
    // Only meaningful while turning; below a floor RPM report 0 (avoids a divide-free but noisy tail).
    const float rpm   = pos.rpm;
    const float omega = rpm * 0.104719755f;                 // 2*pi/60
    power_kw_ = (rpm > 100.0f) ? std::max(0.0f, torque_nm_ * omega * 0.001f) : 0.0f;

    bus.set(SIG_ENGINE_TORQUE_NM, torque_nm_, true, now, ttl());
    bus.set(SIG_ENGINE_POWER_KW,  power_kw_, true, now, ttl());
}
