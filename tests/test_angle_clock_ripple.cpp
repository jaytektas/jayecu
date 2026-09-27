// THE ANGLE CLOCK UNDER CRANKING SPEED RIPPLE (audit T4).
//
// A cranking engine does not turn at a steady speed: it slows into every compression and surges out
// of it. The angle clock (VirtualTrigger) predicts the grid teeth from a smoothed speed and nudges them
// toward each real tooth. If the nudge is too timid the clock runs ahead of the crank on every
// deceleration — and an early grid tooth is an early spark, which is kickback while cranking.
//
// This simulates a 36-tooth wheel with a sinusoidal speed ripple (two slow-downs per revolution, a
// four-cylinder) and measures, at every emitted grid tooth, how far the clock's angle is from the
// crank's true angle.
#include "test_helpers.h"
#include "Scheduler/VirtualTrigger.h"
#include <cmath>
#include <cstdio>

namespace {

uint64_t g_t = 0;
struct Dco final : IAlarmTimer {
    bool armed = false; uint32_t at = 0;
    void arm(uint32_t t) noexcept override { armed = true; at = t; }
    void disarm() noexcept override { armed = false; }
    [[nodiscard]] uint32_t now() const noexcept override { return static_cast<uint32_t>(g_t); }
    void register_callback(MatchCallback, void*) noexcept override {}
};

double g_theta = 0.0;          // true crank angle, degrees (unwrapped)
double g_max_err = 0.0;
int    g_emits = 0;

void on_vt(const VirtualToothData& vt, void*) noexcept {
    const double truth = std::fmod(g_theta, 360.0);
    double err = vt.angle / 10.0 - truth;             // + = clock AHEAD of the crank
    while (err > 180.0) err -= 360.0;
    while (err < -180.0) err += 360.0;
    if (g_emits > 72 && std::fabs(err) > g_max_err) g_max_err = std::fabs(err);   // after settling
    ++g_emits;
}

// Mean rpm, ripple fraction (0.4 = +/-40 %), revolutions. Returns the worst angle error in degrees.
// gap: a 36-1 wheel (tooth 35 missing, the next tooth reports a 20 deg pitch). jitter_us: +/- edge
// timestamp noise, uniformly distributed (a VR sensor's zero-crossing wobble).
double run(double rpm, double ripple, int revs, bool gap = false, int jitter_us = 0) {
    Dco dco;
    VirtualTrigger vt(dco, 3600, 36);
    vt.register_callback(on_vt, nullptr);
    g_t = 0; g_theta = 0.0; g_max_err = 0.0; g_emits = 0;
    const double w0 = rpm * 360.0 / 60.0 / 1e6;       // deg per tick (1 MHz)
    int next_tooth = 1;
    uint64_t last_tooth_t = 0;
    uint32_t seed = 12345u;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 8) & 0xFFFF; };
    bool skipped = false;
    while (g_theta < revs * 360.0) {
        ++g_t;
        g_theta += w0 * (1.0 + ripple * std::sin(2.0 * g_theta * M_PI / 180.0));
        if (g_theta >= next_tooth * 10.0) {
            if (gap && (next_tooth % 36) == 35) { skipped = true; ++next_tooth; continue; }
            const int jit = jitter_us ? static_cast<int>(rnd() % (2 * jitter_us + 1)) - jitter_us : 0;
            const uint64_t stamp = g_t + jit;
            const uint32_t period = static_cast<uint32_t>(stamp - last_tooth_t);
            last_tooth_t = stamp;
            const AngleDeg10 a = static_cast<AngleDeg10>((next_tooth % 36) * 100);
            if (next_tooth > 2) vt.on_real_tooth(a, static_cast<uint32_t>(stamp), period, skipped ? 200 : 100, false);
            skipped = false;
            ++next_tooth;
        }
        if (dco.armed && static_cast<int32_t>(dco.at - static_cast<uint32_t>(g_t)) <= 0) {
            dco.armed = false;
            vt.on_dco_match();
        }
    }
    return g_max_err;
}

} // namespace

int main() {
    std::puts("=== Angle clock under cranking ripple ===");
    struct Case { double rpm, ripple; bool gap; int jitter; double limit; } cases[] = {
        {  800.0, 0.10, false, 0, 1.5 },    // idle: must stay tight
        {  220.0, 0.20, false, 0, 3.0 },
        {  220.0, 0.40, false, 0, 5.0 },    // the audit modelled 15 deg here
        {  220.0, 0.50, false, 0, 6.0 },    // ...and 22 deg here
        {  220.0, 0.40, true,  0, 5.0 },    // the same on a 36-1 wheel
        {  220.0, 0.40, true, 20, 5.0 },    // plus +/-20 us edge jitter
        { 6000.0, 0.02, true,  5, 1.5 },    // at speed: +/-5 us is 2 % of a 278 us tooth
        { 3000.0, 0.05, true,  5, 1.5 },
    };
    for (const auto& c : cases) {
        const double e = run(c.rpm, c.ripple, 8, c.gap, c.jitter);
        std::printf("  %4.0f rpm, +/-%2.0f%% ripple, %s, jitter %2d us: worst error %5.2f deg (limit %.1f)\n",
                    c.rpm, c.ripple * 100.0, c.gap ? "36-1" : "36  ", c.jitter, e, c.limit);
        CHECK(e <= c.limit);
    }
    return test_summary();
}
