// Host unit test for SoftPwm — the portable software pulse/PWM engine.
//
// Drives the engine with a fake 32-bit tick source (FakeAlarm) and a fake GPIO
// sink (FakePin) that timestamps every edge, exactly as the TIM4 backend would on
// target. We then assert the emitted waveform: period, duty, multi-channel
// interleaving on the single list, live waveform changes, off/park, and 0%/100%
// holds. Pure integer math -> exact, no tolerances.
//
//   build: part of tests/CMakeLists.txt  ->  ctest -R soft_pwm
#include <vector>
#include <cstdint>
#include "Scheduler/SoftPwm.h"
#include "test_helpers.h"

// --- fake tick source: a manual 32-bit clock + a one-shot armed deadline -------
class FakeAlarm final : public IAlarmTimer {
public:
    uint32_t clock = 0;
    void arm(uint32_t abs_ticks) noexcept override { deadline_ = abs_ticks; armed_ = true; }
    void disarm() noexcept override { armed_ = false; }
    uint32_t now() const noexcept override { return clock; }
    void register_callback(MatchCallback cb, void* ud) noexcept override { cb_ = cb; ud_ = ud; }

    // Advance the clock by dt, firing the (re-arming) callback at each deadline it
    // crosses — this stands in for the TIM4 compare ISR.
    void advance(uint32_t dt) {
        const uint32_t target = clock + dt;
        int guard = 0;
        while (armed_ && (int32_t)(deadline_ - target) <= 0) {
            if ((int32_t)(deadline_ - clock) > 0) clock = deadline_;   // jump fwd to the edge
            armed_ = false;                                            // one-shot; cb may re-arm
            if (cb_) cb_(ud_);
            if (++guard > 1000000) { CHECK(!"runaway alarm loop"); return; }
        }
        clock = target;
    }
private:
    MatchCallback cb_ = nullptr;
    void*         ud_ = nullptr;
    uint32_t      deadline_ = 0;
    bool          armed_ = false;
};

// --- fake GPIO sink: records (timestamp, level) of every driven edge -----------
class FakePin final : public ITimerChannel {
public:
    explicit FakePin(const FakeAlarm* clk) : clk_(clk) {}
    struct Edge { uint32_t t; bool high; };
    std::vector<Edge> edges;

    uint32_t get_current_ticks()    const noexcept override { return clk_->now(); }
    uint32_t get_ticks_per_second() const noexcept override { return 100000; }
    void enable_output(OutputAction) noexcept override { driven_ = true; }
    void disable_output() noexcept override { driven_ = false; }
    void force_output_now(OutputAction a) noexcept override {
        if (!driven_) return;            // models reset Hi-Z: an input pin doesn't drive
        if (a == OutputAction::DRIVE_HIGH) edges.push_back({clk_->now(), true});
        else if (a == OutputAction::DRIVE_LOW) edges.push_back({clk_->now(), false});
    }
private:
    const FakeAlarm* clk_;
    bool driven_ = false;
};

// Collapse repeated same-level drives (the hold states re-drive every period) into
// transitions only, so interval checks see the actual square wave.
static std::vector<FakePin::Edge> transitions(const std::vector<FakePin::Edge>& e) {
    std::vector<FakePin::Edge> out;
    for (const auto& x : e)
        if (out.empty() || out.back().high != x.high) out.push_back(x);
    return out;
}

int main() {
    SECTION("50% square wave: exact period + duty");
    {
        FakeAlarm clk; FakePin pin(&clk); SoftPwm pwm;
        pwm.bind(&clk);
        pwm.set_pin(0, &pin, /*active_high=*/true);
        pwm.set_waveform(0, /*period=*/1000, /*high=*/500);
        clk.advance(10000);                          // 10 full periods
        auto t = transitions(pin.edges);
        CHECK(t.size() >= 20);
        CHECK(t.front().high == true);               // first edge drives the ON phase
        // rising-edge interval == period; high-phase duration == high
        int checked = 0;
        for (size_t i = 0; i + 2 < t.size(); i += 2) {
            CHECK(t[i].high == true && t[i + 1].high == false);
            CHECK(t[i + 1].t - t[i].t == 500u);      // ON phase
            CHECK(t[i + 2].t - t[i].t == 1000u);     // full period
            checked++;
        }
        CHECK(checked >= 4);
    }

    SECTION("asymmetric duty: 200/1000 (20%)");
    {
        FakeAlarm clk; FakePin pin(&clk); SoftPwm pwm;
        pwm.bind(&clk);
        pwm.set_pin(0, &pin, true);
        pwm.set_waveform(0, 1000, 200);
        clk.advance(5000);
        auto t = transitions(pin.edges);
        CHECK(t.size() >= 8);
        CHECK(t[1].t - t[0].t == 200u);              // ON phase = high
        CHECK(t[2].t - t[1].t == 800u);              // OFF phase = period - high
        CHECK(t[2].t - t[0].t == 1000u);             // period
    }

    SECTION("active-low polarity inverts the phases");
    {
        FakeAlarm clk; FakePin pin(&clk); SoftPwm pwm;
        pwm.bind(&clk);
        pwm.set_pin(0, &pin, /*active_high=*/false);  // ON phase drives LOW
        pwm.set_waveform(0, 1000, 300);
        clk.advance(3000);
        auto t = transitions(pin.edges);
        CHECK(t.front().high == false);               // ON phase = LOW for active-low
        CHECK(t[1].t - t[0].t == 300u);               // ON (low) phase length = high
    }

    SECTION("two channels share one list, each exact + independent");
    {
        FakeAlarm clk; FakePin a(&clk), b(&clk); SoftPwm pwm;
        pwm.bind(&clk);
        pwm.set_pin(0, &a, true);
        pwm.set_pin(1, &b, true);
        pwm.set_waveform(0, 1000, 500);               // 100 Hz-ish
        pwm.set_waveform(1, 600, 300);                // faster, coprime-ish
        clk.advance(6000);
        auto ta = transitions(a.edges), tb = transitions(b.edges);
        CHECK(ta.size() >= 10 && tb.size() >= 16);
        CHECK(ta[2].t - ta[0].t == 1000u);            // ch0 period intact
        CHECK(tb[2].t - tb[0].t == 600u);             // ch1 period intact
        CHECK(ta[1].t - ta[0].t == 500u);             // ch0 duty
        CHECK(tb[1].t - tb[0].t == 300u);             // ch1 duty
    }

    SECTION("live waveform change is adopted within one period");
    {
        FakeAlarm clk; FakePin pin(&clk); SoftPwm pwm;
        pwm.bind(&clk);
        pwm.set_pin(0, &pin, true);
        pwm.set_waveform(0, 1000, 500);
        clk.advance(3000);                            // run a few periods at 1000
        pwm.set_waveform(0, 400, 200);                // change on the fly
        clk.advance(2000);
        auto t = transitions(pin.edges);
        // the LAST few transitions must reflect the new 400/200 wave
        size_t n = t.size();
        CHECK(n >= 6);
        CHECK(t[n - 1].t - t[n - 3].t == 400u);       // new period
        CHECK(t[n - 2].t - t[n - 3].t == 200u);       // new duty
    }

    SECTION("period=0 parks the pin at idle and stops pulsing");
    {
        FakeAlarm clk; FakePin pin(&clk); SoftPwm pwm;
        pwm.bind(&clk);
        pwm.set_pin(0, &pin, true);
        pwm.set_waveform(0, 1000, 500);
        clk.advance(2500);
        pwm.set_waveform(0, 0, 0);                    // OFF
        clk.advance(50);                              // let the reconcile run
        const size_t after_off = pin.edges.size();
        CHECK(pin.edges.back().high == false);        // parked at idle (LOW)
        clk.advance(10000);                           // no further activity
        CHECK(pin.edges.size() == after_off);
    }

    SECTION("100% duty holds active, 0% duty holds idle");
    {
        FakeAlarm clk; FakePin pin(&clk); SoftPwm pwm;
        pwm.bind(&clk);
        pwm.set_pin(0, &pin, true);
        pwm.set_waveform(0, 1000, 1000);              // 100% -> hold HIGH
        clk.advance(3000);
        CHECK(pin.edges.back().high == true);
        CHECK(transitions(pin.edges).size() == 1);    // never toggles low
        pwm.set_waveform(0, 1000, 0);                 // 0% -> hold LOW
        clk.advance(3000);
        CHECK(pin.edges.back().high == false);
    }

    SECTION("frequency accuracy: no drift over many periods");
    {
        FakeAlarm clk; FakePin pin(&clk); SoftPwm pwm;
        pwm.bind(&clk);
        pwm.set_pin(0, &pin, true);
        pwm.set_waveform(0, 333, 166);                // odd period, watch for accumulation
        clk.advance(333 * 200);
        auto t = transitions(pin.edges);
        // first and last rising edges must be an exact multiple of the period apart
        CHECK(t.front().high == true);
        size_t last_rise = 0;
        for (size_t i = 0; i < t.size(); ++i) if (t[i].high) last_rise = i;
        const uint32_t span = t[last_rise].t - t[0].t;
        CHECK(span % 333u == 0u);                     // zero accumulated error
    }

    return test_summary();
}
