#include "KnockDetector.h"

#include "../../Signal/SignalBus.h"
#include "../../../generated/signal_ids.h"
#include "../../Platform/platform_hal.h"   // platform_get_tick_ms — every publish carries its tick

void KnockDetector::configure(float sampleRate, float knockFreqHz, float Q) {
    sample_rate_ = sampleRate;
    dsp_.configure(sampleRate, knockFreqHz, Q);
}

float KnockDetector::on_burst(unsigned sensor, const uint16_t* samples, unsigned count,
                              SignalBus& bus, KnockProfile& out, const Window& win, float vref) {
    const float db = dsp_.process(samples, count, out, vref);

    // Stamp the angle domain. A bucket spans count/out.count samples, each sample is 1/sample_rate
    // seconds, and the crank turns deg_per_sec — so the bucket's angular width is just that product.
    // Left unstamped (step 0) when the caller could not say how fast the engine was turning, because a
    // guessed speed here would put energy at a confident wrong angle, which is worse than none.
    if (win.deg_per_sec > 0.0f && sample_rate_ > 0.0f && out.count > 0) {
        const float samples_per_bucket = static_cast<float>(count) / static_cast<float>(out.count);
        out.start_deg = win.start_deg;
        out.step_deg  = samples_per_bucket / sample_rate_ * win.deg_per_sec;
    }

    // WITH A LIFETIME. Published with none (ttl 0) the channel stayed valid for ever after the first
    // burst, so Knock's "is this sensor reporting" check could never trip. 1 s spans the slowest firing
    // interval a running engine has on one sensor; bursts that stop coming now let it expire.
    bus.set(sensor == 0 ? SIG_KNOCK_1 : SIG_KNOCK_2, db, true, platform_get_tick_ms(), 1000u);
    return db;
}
