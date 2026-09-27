#pragma once

// Biquad — one second-order IIR section in transposed direct form II, the form with the best
// numerical behaviour in single precision. Coefficients follow the usual convention: b0..b2 on the
// input (numerator), a1..a2 on the output (denominator), normalised so a0 = 1.
// Used by the knock DSP as a band-pass around the knock frequency; generic enough to reuse.
class Biquad {
public:
    // INLINE, deliberately: it is called once per sample, and the knock DSP feeds it ~190,000 samples
    // a second on a twelve-cylinder engine. Out of line it was a call, a spill and a return for five
    // multiply-adds the FPU does in as many cycles.
    float filter(float x) {
        const float y = b0_ * x + s1_;
        s1_ = b1_ * x - a1_ * y + s2_;
        s2_ = b2_ * x - a2_ * y;
        return y;
    }
    void  reset() { s1_ = s2_ = 0.0f; }
    // Put the state where a constant input `x` would have left it, so a filter started mid-signal
    // does not ring from an artificial step at zero.
    void  primeSteadyState(float x);
    // Constant-0 dB-peak band-pass centred on `centerFreq` (RBJ Audio EQ Cookbook form).
    void  configureBandpass(float sampleRate, float centerFreq, float Q);

private:
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;   // coefficients (a0 = 1)
    float s1_ = 0.0f, s2_ = 0.0f;                                       // state
};
