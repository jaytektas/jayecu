#include "Biquad.h"

#include <cmath>

namespace {
constexpr float kPi = 3.14159265358979323846f;
// The bilinear transform squeezes the whole analog axis into 0..fs/2, so a centre close to Nyquist
// lands somewhere quite different from where it was asked for. Below 40 % of the sample rate the
// warp is small enough to trust; above it, pass the signal through untouched rather than filter
// around the wrong frequency.
constexpr float kMaxCentreFraction = 0.4f;
}

void Biquad::configureBandpass(float sampleRate, float centerFreq, float Q) {
    if (centerFreq <= 0.0f || Q <= 0.0f || centerFreq > kMaxCentreFraction * sampleRate) {
        b0_ = 1.0f; b1_ = b2_ = a1_ = a2_ = 0.0f;
        reset();
        return;
    }
    const float w0    = 2.0f * kPi * centerFreq / sampleRate;
    const float alpha = std::sin(w0) / (2.0f * Q);
    const float a0    = 1.0f + alpha;
    b0_ =  alpha / a0;
    b1_ =  0.0f;
    b2_ = -alpha / a0;
    a1_ = -2.0f * std::cos(w0) / a0;
    a2_ = (1.0f - alpha) / a0;
    reset();
}

void Biquad::primeSteadyState(float x) {
    // At steady state every sample is x and every output is y = x * H(1), the DC gain. Solving the two
    // state equations of filter() with those values gives the state to start from.
    const float y = x * (b0_ + b1_ + b2_) / (1.0f + a1_ + a2_);
    s2_ = b2_ * x - a2_ * y;
    s1_ = b1_ * x - a1_ * y + s2_;
}
