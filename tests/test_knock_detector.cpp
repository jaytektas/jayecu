#include "test_helpers.h"
#include "../firmware/Engine/Modules/KnockDetector.h"
#include "../firmware/Signal/SignalBus.h"
#include "../generated/signal_ids.h"

#include <cmath>
#include <vector>

static std::vector<uint16_t> sine(float freq, float fs, unsigned n, float amp = 1000.0f) {
    std::vector<uint16_t> s(n);
    for (unsigned i = 0; i < n; ++i) {
        const float v = 2048.0f + amp * std::sin(2.0f * 3.14159265f * freq * i / fs);
        s[i] = static_cast<uint16_t>(v < 0 ? 0 : (v > 4095 ? 4095 : v));
    }
    return s;
}

int main() {
    fprintf(stdout, "=== KnockDetector ===\n");
    const float fs = 280000.0f, fknock = 7000.0f;
    KnockDetector det;
    det.configure(fs, fknock, 3.0f);
    SignalBus bus{};

    KnockProfile p;

    SECTION("a loud knock burst publishes a high level to its sensor channel (knock_1)");
    {
        const auto loud = sine(fknock, fs, 512, 1500.0f);
        const float db = det.on_burst(0, loud.data(), loud.size(), bus, p, KnockDetector::Window{});
        CHECK(db > -20.0f);   // a strong in-band tone reads well above the silence floor (~-50 dB)
        CHECK(bus.get(SIG_KNOCK_1, -100.0f) == db);   // published to sensor 1's channel
        CHECK(bus.get(SIG_KNOCK_2, 5.0f) == 5.0f);    // sensor 2 untouched
    }

    SECTION("a quiet burst on sensor 2 publishes the floor to knock_2");
    {
        const std::vector<uint16_t> quiet(512, 2048);
        const float db = det.on_burst(1, quiet.data(), quiet.size(), bus, p, KnockDetector::Window{});
        CHECK(db < -40.0f);
        CHECK(bus.get(SIG_KNOCK_2, 0.0f) == db);
    }

    // ---- The angle stamp: where the burst STOPS being samples and becomes crank degrees ----------

    SECTION("a window with a known speed stamps the profile into the angle domain");
    {
        // 2048 samples at 280 kHz is 7.314 ms; at 6000 rpm (36000 deg/s) that is 263.3 deg of crank.
        // Opening 10 deg BTDC, the profile therefore starts at -10 and each bucket is that span / count.
        const auto s = sine(fknock, fs, 2048);
        KnockDetector::Window win;
        win.start_deg   = -10.0f;
        win.deg_per_sec = 36000.0f;
        det.on_burst(0, s.data(), s.size(), bus, p, win);

        CHECK(p.hasPhase());
        CHECK(p.start_deg == -10.0f);
        const float span = 2048.0f / fs * win.deg_per_sec;         // total crank degrees in the burst
        const float expect_step = span / static_cast<float>(p.count);
        fprintf(stdout, "  buckets=%u span=%.1fdeg step=%.2fdeg\n", p.count, span, p.step_deg);
        CHECK(std::fabs(p.step_deg - expect_step) < 0.01f);

        // Bucket 0 straddles the window opening, and the last one ends at the far edge.
        CHECK(p.angleOf(0) > -10.0f);
        CHECK(std::fabs((p.start_deg + p.step_deg * static_cast<float>(p.count)) - (-10.0f + span)) < 0.1f);
    }

    SECTION("a window that opens BTDC really does report negative angles");
    {
        // The reason window_start_deg had to become signed: pre-ignition evidence sits before the
        // spark, and a window pinned to 0..180 ATDC cannot address that region at all.
        const auto s = sine(fknock, fs, 1024);
        KnockDetector::Window win;
        win.start_deg   = -30.0f;
        win.deg_per_sec = 12000.0f;    // 2000 rpm
        det.on_burst(0, s.data(), s.size(), bus, p, win);
        CHECK(p.hasPhase());
        CHECK(p.angleOf(0) < 0.0f);    // the first bucket is genuinely BTDC
    }

    SECTION("an unknown crank speed leaves the profile unstamped rather than guessing");
    {
        const auto s = sine(fknock, fs, 1024);
        p.start_deg = 0.0f; p.step_deg = 0.0f;
        det.on_burst(0, s.data(), s.size(), bus, p, KnockDetector::Window{});   // deg_per_sec = 0
        CHECK(!p.hasPhase());   // energy is still measured; the angle is honestly absent
        CHECK(p.count > 0);
    }

    return test_summary();
}
