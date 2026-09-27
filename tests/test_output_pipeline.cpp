// Host unit test for the OUTPUT side of the polymorphic pipeline — proves the bus->world
// mirror runs on the same pipe::Pipeline engine: arbitrate (fan-in) -> encode -> emit, with
// the fail-safe-on-invalid mirror of the publish-invalid rule.
//   build: tests/CMakeLists.txt -> ctest -R output_pipeline
#include "test_helpers.h"
#include <vector>
#include <cstdint>
#include "Pipeline/Pipeline.h"
#include "Pipeline/OutputStages.h"
#include "Integration/EmitSinks.h"
#include "../generated/signal_ids.h"

using namespace pipe;

// A capturing sink standing in for a real SoftPwm / digital-pin driver.
static float g_cmd = -1.0f;
static bool  g_cmd_valid = false;
static int   g_cmd_calls = 0;
static void capture(void*, float cmd, bool valid) { g_cmd = cmd; g_cmd_valid = valid; ++g_cmd_calls; }

// --- fakes for the SoftPwm end-to-end test (same shape as test_soft_pwm) ---
class FakeAlarm final : public IAlarmTimer {
public:
    uint32_t clock = 0;
    void arm(uint32_t t) noexcept override { deadline_ = t; armed_ = true; }
    void disarm() noexcept override { armed_ = false; }
    uint32_t now() const noexcept override { return clock; }
    void register_callback(MatchCallback cb, void* ud) noexcept override { cb_ = cb; ud_ = ud; }
    void advance(uint32_t dt) {
        const uint32_t target = clock + dt; int guard = 0;
        while (armed_ && (int32_t)(deadline_ - target) <= 0) {
            if ((int32_t)(deadline_ - clock) > 0) clock = deadline_;
            armed_ = false;
            if (cb_) cb_(ud_);
            if (++guard > 1000000) { CHECK(!"runaway"); return; }
        }
        clock = target;
    }
private:
    MatchCallback cb_ = nullptr; void* ud_ = nullptr; uint32_t deadline_ = 0; bool armed_ = false;
};
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
        if (!driven_) return;
        if (a == OutputAction::DRIVE_HIGH) edges.push_back({clk_->now(), true});
        else if (a == OutputAction::DRIVE_LOW) edges.push_back({clk_->now(), false});
    }
private:
    const FakeAlarm* clk_; bool driven_ = false;
};
static std::vector<FakePin::Edge> transitions(const std::vector<FakePin::Edge>& e) {
    std::vector<FakePin::Edge> out;
    for (const auto& x : e) if (out.empty() || out.back().high != x.high) out.push_back(x);
    return out;
}

int main() {
    fprintf(stdout, "=== Output pipeline ===\n");

    SECTION("single source: arbitrate -> encode -> emit drives the command");
    {
        ArbitrateCfg arb{{SIG_MAP}, 1, ARB_PRIORITY, 0.0f};
        EncodeCfg    enc{2.0f, 0.0f, 0.0f, 100.0f};        // command = value*2, clamped 0..100
        EmitCfg      em{capture, nullptr, 0.0f};

        Pipeline p;
        p.stages[0] = {arbitrate,     &arb};
        p.stages[1] = {encode_linear, &enc};
        p.stages[2] = {emit_sink,     &em};
        p.n = 3;

        SignalBus bus; bus.set(SIG_MAP, 30.0f);
        g_cmd = -1.0f; p.run(bus, 1000, 10.0f);
        CHECK(g_cmd_valid);
        CHECK_NEAR(g_cmd, 60.0f, 0.01f);                   // 30*2
    }

    SECTION("encode clamps to the actuator range");
    {
        ArbitrateCfg arb{{SIG_MAP}, 1, ARB_PRIORITY, 0.0f};
        EncodeCfg    enc{2.0f, 0.0f, 0.0f, 100.0f};
        EmitCfg      em{capture, nullptr, 0.0f};
        Pipeline p; p.stages[0]={arbitrate,&arb}; p.stages[1]={encode_linear,&enc}; p.stages[2]={emit_sink,&em}; p.n=3;
        SignalBus bus; bus.set(SIG_MAP, 80.0f);          // 80*2=160 -> clamps to 100
        p.run(bus, 1000, 10.0f);
        CHECK_NEAR(g_cmd, 100.0f, 0.01f);
    }

    SECTION("arbitration: a safety limiter clamps the controller via MIN");
    {
        // candidate 0 = the controller's request, candidate 1 = a safety limit; MIN picks the
        // lower (the limiter wins when it's below the request).
        ArbitrateCfg arb{{SIG_BOOST_EST, SIG_MAP}, 2, ARB_MIN, 0.0f};
        EncodeCfg    enc{1.0f, 0.0f, 0.0f, 1000.0f};
        EmitCfg      em{capture, nullptr, 0.0f};
        Pipeline p; p.stages[0]={arbitrate,&arb}; p.stages[1]={encode_linear,&enc}; p.stages[2]={emit_sink,&em}; p.n=3;
        SignalBus bus;
        bus.set(SIG_BOOST_EST, 200.0f);                    // controller wants 200
        bus.set(SIG_MAP,      150.0f);                   // limiter says max 150
        p.run(bus, 1000, 10.0f);
        CHECK_NEAR(g_cmd, 150.0f, 0.01f);                  // MIN -> limiter wins
    }

    SECTION("arbitration: PRIORITY takes the first VALID candidate (override falls through)");
    {
        ArbitrateCfg arb{{SIG_BOOST_EST, SIG_MAP}, 2, ARB_PRIORITY, 0.0f};
        EncodeCfg    enc{1.0f, 0.0f, 0.0f, 1000.0f};
        EmitCfg      em{capture, nullptr, 0.0f};
        Pipeline p; p.stages[0]={arbitrate,&arb}; p.stages[1]={encode_linear,&enc}; p.stages[2]={emit_sink,&em}; p.n=3;
        SignalBus bus;
        bus.set(SIG_MAP, 77.0f);                         // only the fallback candidate is valid
        // SIG_BOOST_EST left invalid -> priority falls through to candidate 1
        p.run(bus, 1000, 10.0f);
        CHECK_NEAR(g_cmd, 77.0f, 0.01f);
    }

    SECTION("publish-invalid mirror: no valid source -> emit the FAIL-SAFE");
    {
        ArbitrateCfg arb{{SIG_BOOST_EST}, 1, ARB_PRIORITY, 0.0f};
        EncodeCfg    enc{1.0f, 0.0f, 0.0f, 1000.0f};
        EmitCfg      em{capture, nullptr, /*failsafe*/ 5.0f};
        Pipeline p; p.stages[0]={arbitrate,&arb}; p.stages[1]={encode_linear,&enc}; p.stages[2]={emit_sink,&em}; p.n=3;
        SignalBus bus;                                     // SIG_BOOST_EST never set -> invalid
        g_cmd_calls = 0;
        p.run(bus, 1000, 10.0f);
        CHECK(g_cmd_calls == 1);                           // emit STILL runs (fail-safe, not silence)
        CHECK(!g_cmd_valid);
        CHECK_NEAR(g_cmd, 5.0f, 0.01f);                    // drove the fail-safe command
    }

    SECTION("end-to-end: a bus signal drives a real SoftPwm waveform via the output pipeline");
    {
        FakeAlarm clk; FakePin pin(&clk); SoftPwm pwm;
        pwm.bind(&clk);
        pwm.set_pin(0, &pin, /*active_high*/ true);
        PwmEmitCtx pctx{&pwm, 0, /*period_ticks*/ 1000};   // 1000-tick period

        ArbitrateCfg arb{{SIG_MAP}, 1, ARB_PRIORITY, 0.0f};
        EncodeCfg    enc{1.0f, 0.0f, 0.0f, 100.0f};         // value IS the duty %
        EmitCfg      em{pwm_emit_sink, &pctx, 0.0f};
        Pipeline p;
        p.stages[0]={arbitrate,&arb}; p.stages[1]={encode_linear,&enc}; p.stages[2]={emit_sink,&em};
        p.n=3;

        SignalBus bus; bus.set(SIG_MAP, 30.0f);           // request 30% duty
        p.run(bus, 0, 10.0f);                                // -> set_waveform(0, 1000, 300)
        clk.advance(3000);
        auto t = transitions(pin.edges);
        CHECK(t.size() >= 4);
        CHECK(t[2].t - t[0].t == 1000u);                    // period
        CHECK(t[1].t - t[0].t == 300u);                     // 30% duty high-time

        bus.set(SIG_MAP, 70.0f);                           // re-request 70% live
        p.run(bus, 0, 10.0f);
        clk.advance(2500);                                   // let the channel adopt the new duty
        pin.edges.clear(); clk.advance(3000);
        t = transitions(pin.edges);
        uint32_t high = 0;                                   // measure a settled high-phase
        for (size_t i = 0; i + 1 < t.size(); i++) if (t[i].high) { high = t[i+1].t - t[i].t; break; }
        CHECK(high == 700u);                                 // duty tracked the bus
    }

    SECTION("end-to-end: digital emit drives a pin from a bus switch signal");
    {
        FakeAlarm clk; FakePin pin(&clk);
        pin.enable_output(OutputAction::DRIVE_LOW);          // bind: driven, idle low
        DigitalEmitCtx dctx{&pin, /*active_high*/ true, /*threshold*/ 0.5f};
        ArbitrateCfg arb{{SIG_CLUTCH_SW}, 1, ARB_PRIORITY, 0.0f};
        EncodeCfg    enc{1.0f, 0.0f, 0.0f, 1.0f};
        EmitCfg      em{digital_emit_sink, &dctx, 0.0f};
        Pipeline p;
        p.stages[0]={arbitrate,&arb}; p.stages[1]={encode_linear,&enc}; p.stages[2]={emit_sink,&em};
        p.n=3;
        SignalBus bus;
        bus.set_bool(SIG_CLUTCH_SW, true);
        p.run(bus, 0, 10.0f);
        CHECK(!pin.edges.empty() && pin.edges.back().high);  // driven HIGH
        bus.set_bool(SIG_CLUTCH_SW, false);
        p.run(bus, 0, 10.0f);
        CHECK(!pin.edges.back().high);                       // driven LOW
    }

    SECTION("roled arbitration: primary -> limit clamp -> override veto");
    {
        const RoledCandidate cand[3] = {
            {SIG_BOOST_EST, ROLE_PRIMARY},
            {SIG_MAP,     ROLE_LIMIT},
            {SIG_CLT,       ROLE_OVERRIDE},
        };
        ArbitrateRoledCfg cfg{cand, 3, PRIM_FIRST, 7.0f};

        SignalBus bus;
        bus.set(SIG_BOOST_EST, 80.0f);                 // primary only
        { State st{}; Ctx c{}; c.bus = &bus; arbitrate_roled(c, &cfg, st);
          CHECK(c.valid); CHECK_NEAR(c.value, 80.0f, 0.01f); }

        bus.set(SIG_MAP, 60.0f);                     // a limit below the primary clamps it
        { State st{}; Ctx c{}; c.bus = &bus; arbitrate_roled(c, &cfg, st);
          CHECK_NEAR(c.value, 60.0f, 0.01f); }

        bus.set(SIG_CLT, 10.0f);                        // an override vetoes everything
        { State st{}; Ctx c{}; c.bus = &bus; arbitrate_roled(c, &cfg, st);
          CHECK_NEAR(c.value, 10.0f, 0.01f); }
    }

    SECTION("roled arbitration: ALL candidates invalid -> fail-safe (still valid for emit)");
    {
        const RoledCandidate cand[1] = {{SIG_BOOST_EST, ROLE_PRIMARY}};
        ArbitrateRoledCfg cfg{cand, 1, PRIM_FIRST, 5.0f};
        SignalBus empty;                               // nothing published
        State st{}; Ctx c{}; c.bus = &empty;
        arbitrate_roled(c, &cfg, st);
        CHECK(c.valid);                                // failsafe IS a valid command
        CHECK_NEAR(c.value, 5.0f, 0.01f);              // forced to the safe state
    }

    SECTION("roled arbitration: dual primary sub-policy (MAX wins, e.g. dual throttle)");
    {
        const RoledCandidate cand[2] = {{SIG_TPS, ROLE_PRIMARY}, {SIG_AUX_1, ROLE_PRIMARY}};
        ArbitrateRoledCfg cfg{cand, 2, PRIM_MAX, 0.0f};
        SignalBus bus; bus.set(SIG_TPS, 30.0f); bus.set(SIG_AUX_1, 45.0f);
        State st{}; Ctx c{}; c.bus = &bus;
        arbitrate_roled(c, &cfg, st);
        CHECK_NEAR(c.value, 45.0f, 0.01f);             // highest of the two primaries
    }

    return test_summary();
}
