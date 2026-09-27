#include "EngineSyncSampler.h"
#include "../Platform/platform_hal.h"

EngineSyncSampler g_engine_sync;

void EngineSyncSampler::configure(const uint8_t* pins, uint8_t n, uint16_t window_deg10) noexcept {
    if (n > MAX_PINS) n = MAX_PINS;
    n_ = 0;                                   // idle the ISR feed() while we rewrite the pin set
    for (uint8_t i = 0; i < n; i++) {
        pins_[i] = pins[i];
        sum_[i] = 0; cnt_[i] = 0; win_valid_[i] = false;
    }
    if (window_deg10) window_deg10_ = window_deg10;
    acc_deg10_ = 0;
    n_ = n;                                   // publish the new set last
}

void EngineSyncSampler::feed(uint16_t step_deg10) noexcept {
    const uint8_t n = n_;
    if (n == 0) return;
    for (uint8_t i = 0; i < n; i++) { sum_[i] += platform_read_ain_raw(pins_[i]); cnt_[i]++; }
    acc_deg10_ += step_deg10;
    if (acc_deg10_ >= window_deg10_) {
        for (uint8_t i = 0; i < n; i++) {
            if (cnt_[i]) { win_raw_[i] = static_cast<uint16_t>(sum_[i] / cnt_[i]); win_valid_[i] = true; }
            sum_[i] = 0; cnt_[i] = 0;
        }
        acc_deg10_ = 0;
    }
}

uint16_t EngineSyncSampler::read_raw(uint8_t pin) const noexcept {
    const uint8_t n = n_;
    for (uint8_t i = 0; i < n; i++)
        if (pins_[i] == pin && win_valid_[i]) return win_raw_[i];
    return platform_read_ain_raw(pin);
}

uint16_t engine_sync_read_raw(uint8_t pin) { return g_engine_sync.read_raw(pin); }
