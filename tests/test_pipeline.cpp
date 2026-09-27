// Host unit test for the polymorphic pipeline engine + stage library.
// Proves: stage composition, the lifted Sensors math (curve / EMA / windows), the
// publish-invalid rule, per-stage state persistence, and the no-data abort.
//   build: tests/CMakeLists.txt -> ctest -R pipeline
#include "test_helpers.h"
#include <initializer_list>
#include "Pipeline/Pipeline.h"
#include "Pipeline/Stages.h"
#include "Platform/AcquireHal.h"
#include "../firmware/Can/CanFrame.h"
#include "../generated/signal_ids.h"

using namespace pipe;

// Build a classic CAN frame from up to 8 bytes.
static CanFrame mk_frame(uint32_t id, std::initializer_list<uint8_t> bytes) {
    CanFrame f{}; f.id = id; f.ext = false; f.dlc = (uint8_t)bytes.size();
    uint8_t i = 0; for (uint8_t b : bytes) { if (i < 8) f.data[i++] = b; }
    return f;
}

// A 2-point curve mapping raw 0..5000 (mV) -> 0..100.0 engineering (val_scale 0.01).
static const uint16_t CURVE_X[2] = {0, 5000};
static const int16_t  CURVE_Y[2] = {0, 10000};

int main() {
    fprintf(stdout, "=== Pipeline ===\n");

    SECTION("analog-style pipeline composes and publishes the curve value");
    {
        ConstAcquireCfg acq{2500, true};      // mid-scale
        CurveCfg   curve{CURVE_X, CURVE_Y, 2, 0.01f};
        PublishCfg pub{300};

        Pipeline p;
        p.signal = SIG_CLT;
        p.stages[0] = {acquire_const, &acq};
        p.stages[1] = {decode_curve,  &curve};
        p.stages[2] = {publish,       &pub};
        p.n = 3;

        SignalBus bus;
        p.run(bus, 1000, 10.0f);
        CHECK(bus.valid(SIG_CLT));
        CHECK_NEAR(bus.get(SIG_CLT), 50.0f, 0.01f);   // 2500/5000 * 100
    }

    // The EMA stage that used to be tested here is GONE — sensors publish what they measure and
    // consumers do their own smoothing. See Stages.h for why.

    SECTION("operating-window: out-of-window value stays VALID + published; fault via diag_tripped");
    {
        ConstAcquireCfg acq{2500, true};       // -> 50.0
        CurveCfg   curve{CURVE_X, CURVE_Y, 2, 0.01f};
        OpWindowCfg op{0.0f, 40.0f, false, true};  // max 40, 50 > 40 -> trips
        PublishCfg pub{300};

        Pipeline p;
        p.signal = SIG_CLT;
        p.stages[0] = {acquire_const,  &acq};
        p.stages[1] = {decode_curve,   &curve};
        p.stages[2] = {cond_op_window, &op};
        p.stages[3] = {publish,        &pub};
        p.n = 4;

        SignalBus bus;
        Ctx c = p.run(bus, 1000, 10.0f);
        CHECK(bus.valid(SIG_CLT));                                 // op-range does NOT invalidate
        CHECK_NEAR(bus.get(SIG_CLT), 50.0f, 0.01f);                // the real (unhealthy) value publishes
        CHECK((c.diag_tripped & (1u << DIAG_SLOT_OP_MAX)) != 0);   // fault reported via diag_tripped, not validity
    }

    SECTION("raw-window invalidates pre-decode but still publishes");
    {
        ConstAcquireCfg acq{2500, true};
        RawWindowCfg rw{0, 1000, false, true}; // raw 2500 > 1000 -> trips (max only)
        CurveCfg   curve{CURVE_X, CURVE_Y, 2, 0.01f};
        PublishCfg pub{300};

        Pipeline p;
        p.signal = SIG_CLT;
        p.stages[0] = {acquire_const,   &acq};
        p.stages[1] = {cond_raw_window, &rw};
        p.stages[2] = {decode_curve,    &curve};
        p.stages[3] = {publish,         &pub};
        p.n = 4;

        SignalBus bus;
        p.run(bus, 1000, 10.0f);
        CHECK(!bus.valid(SIG_CLT));
        CHECK(bus.age_ms(SIG_CLT, 1000) == 0);         // published despite invalid
    }

    SECTION("no-data abort publishes nothing (leaves the slot to expire)");
    {
        ConstAcquireCfg none{0, false};        // source unassigned / no data
        CurveCfg   curve{CURVE_X, CURVE_Y, 2, 0.01f};
        PublishCfg pub{300};

        Pipeline p;
        p.signal = SIG_CLT;
        p.stages[0] = {acquire_const, &none};
        p.stages[1] = {decode_curve,  &curve};
        p.stages[2] = {publish,       &pub};
        p.n = 3;

        SignalBus bus;
        bus.set(SIG_CLT, 99.0f, true, 500, 0); // a prior value at t=500
        p.run(bus, 1000, 10.0f);               // abort -> Publish skipped
        CHECK(bus.valid(SIG_CLT));                     // untouched
        CHECK_NEAR(bus.get(SIG_CLT), 99.0f, 0.01f);
        CHECK(bus.age_ms(SIG_CLT, 1000) == 500);       // NOT refreshed by this run
    }

    SECTION("composition: the same Condition fn is reused across two pipelines");
    {
        // A CAN-style pipeline (linear decode) and an analog one (curve decode) share
        // the exact same cond_op_window stage function — the reuse the design promises.
        ConstAcquireCfg a1{80, true};          // raw 80
        LinearCfg  lin{1.0f, 0.0f};            // -> 80.0
        OpWindowCfg op{0.0f, 120.0f, true, true};  // both in range
        PublishCfg pub{300};

        Pipeline can_p;
        can_p.signal = SIG_MAP;
        can_p.stages[0] = {acquire_const,  &a1};
        can_p.stages[1] = {decode_linear,  &lin};
        can_p.stages[2] = {cond_op_window, &op};      // <-- shared fn ptr
        can_p.stages[3] = {publish,        &pub};
        can_p.n = 4;

        ConstAcquireCfg a2{2500, true};
        CurveCfg curve{CURVE_X, CURVE_Y, 2, 0.01f};
        Pipeline an_p;
        an_p.signal = SIG_CLT;
        an_p.stages[0] = {acquire_const,  &a2};
        an_p.stages[1] = {decode_curve,   &curve};
        an_p.stages[2] = {cond_op_window, &op};       // <-- same fn ptr
        an_p.stages[3] = {publish,        &pub};
        an_p.n = 4;

        CHECK(can_p.stages[2].fn == an_p.stages[2].fn);   // literally the same primitive

        SignalBus bus;
        can_p.run(bus, 1000, 10.0f);
        an_p.run(bus, 1000, 10.0f);
        CHECK_NEAR(bus.get(SIG_MAP), 80.0f, 0.01f);
        CHECK_NEAR(bus.get(SIG_CLT),   50.0f, 0.01f);
        CHECK(bus.valid(SIG_MAP) && bus.valid(SIG_CLT));
    }

    SECTION("CAN acquire: a decoded field becomes the sensor's reading");
    {
        // The bit surgery is no longer here — one decode does it for both directions and the sensor
        // acquires the RESULT, so what this has to prove is the hand-off, not the unpacking.
        GenericCanFieldValue fv{ 1.0f, 1000 };
        CanFieldAcquireCfg acq{ &fv, 300 };
        LinearCfg  ident{1.0f, 0.0f};                          // the field already did the scaling
        PublishCfg pub{300};
        Pipeline p; p.signal = SIG_LAMBDA_1;
        p.stages[0] = {acquire_can_field, &acq};
        p.stages[1] = {decode_linear,     &ident};
        p.stages[2] = {publish,           &pub};
        p.n = 3;
        SignalBus bus;
        p.run(bus, 1100, 10.0f);                               // 100 ms old < 300 ttl
        CHECK(bus.valid(SIG_LAMBDA_1));
        CHECK_NEAR(bus.get(SIG_LAMBDA_1), 1.0f, 0.0001f);
    }

    SECTION("CAN acquire: it hands on a FLOAT, already through its scale");
    {
        // RAW_F32, not RAW_U32: the field's own multiplier and offset have run, and a second decode
        // stage downstream would scale the value twice.
        GenericCanFieldValue fv{ -40.0f, 500 };
        CanFieldAcquireCfg acq{ &fv, 0 };
        Ctx c{}; State st{}; c.now_ms = 600;
        acquire_can_field(c, &acq, st);
        CHECK(!c.abort && c.valid);
        CHECK(c.raw_kind == RAW_F32);
        CHECK_NEAR(raw_as_float(c), -40.0f, 0.01f);
    }

    SECTION("CAN acquire: never arrived / stale -> abort");
    {
        GenericCanFieldValue fv{};                             // at_ms 0 = nothing has arrived
        CanFieldAcquireCfg acq{ &fv, 300 };
        { Ctx c{}; State st{}; c.now_ms = 1000; acquire_can_field(c, &acq, st);
          CHECK(c.abort); }                                    // never received -> abort
        fv.v = 1.0f; fv.at_ms = 1000;
        { Ctx c{}; State st{}; c.now_ms = 1400; acquire_can_field(c, &acq, st);
          CHECK(c.abort); }                                    // 400 ms old > 300 -> abort
        { Ctx c{}; State st{}; c.now_ms = 1200; acquire_can_field(c, &acq, st);
          CHECK(!c.abort && c.valid); }                        // 200 ms old < 300 -> ok
        // An abort publishes NOTHING, which is what lets the slot expire rather than be written
        // invalid over whatever else might be driving the channel.
        CanFieldAcquireCfg dead{ &fv, 300 };
        LinearCfg  ident{1.0f, 0.0f};
        PublishCfg pub{300};
        Pipeline p; p.signal = SIG_LAMBDA_1;
        p.stages[0] = {acquire_can_field, &dead};
        p.stages[1] = {decode_linear, &ident};
        p.stages[2] = {publish, &pub};
        p.n = 3;
        SignalBus bus;
        p.run(bus, 9000, 10.0f);                               // far past the ttl
        CHECK(!bus.valid(SIG_LAMBDA_1));
    }


    SECTION("analog acquire: unassigned source aborts; assigned source reads SIG_HW off the bus");
    {
        PinAcquireCfg none{SOURCE_NONE, false, SIG_NONE};
        CurveCfg   curve{CURVE_X, CURVE_Y, 2, 0.01f};
        PublishCfg pub{300};
        Pipeline p; p.signal = SIG_CLT;
        p.stages[0] = {acquire_analog, &none};
        p.stages[1] = {decode_curve,   &curve};
        p.stages[2] = {publish,        &pub};
        p.n = 3;
        SignalBus bus;
        bus.set(SIG_CLT, 42.0f, true, 500, 0);
        p.run(bus, 1000, 10.0f);                               // abort -> nothing published
        CHECK(bus.age_ms(SIG_CLT, 1000) == 500);               // untouched

        // Assigned source: Acquire reads the raw counts HardwareInput publishes as SIG_HW_AV1.
        PinAcquireCfg pin0{0, false, SIG_HW_AV1};
        bus.set_u32(SIG_HW_AV1, 0u, true, 1000, 5);            // 0 counts on the bus
        p.stages[0] = {acquire_analog, &pin0};
        p.run(bus, 1000, 10.0f);
        CHECK(bus.valid(SIG_CLT));
        CHECK_NEAR(bus.get(SIG_CLT), 0.0f, 0.01f);             // 0 counts -> 0.0 on the curve

        // No raw published for the bound channel -> Acquire yields invalid (input has no data).
        PinAcquireCfg pin_np{5, false, SIG_HW_AV6};            // nothing set for SIG_HW_AV6
        p.signal = SIG_IAT;
        p.stages[0] = {acquire_analog, &pin_np};
        p.run(bus, 1000, 10.0f);
        CHECK(bus.age_ms(SIG_IAT, 1000) == 0);                // published this run (not aborted)
        CHECK(!bus.valid(SIG_IAT));                           // ...but invalid: the input had no data
    }

    return test_summary();
}
