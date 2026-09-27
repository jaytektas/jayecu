// WHEN an output is on: the latch, the deadband and the timers behind a fuel pump, a thermo fan and
// a starter motor.
//
// The expression VM answers "is this true"; nothing in it remembers. Everything that remembers is
// OutputGate, and the rules it applies are exactly the ones that make these loads safe to drive:
// a fan that does not chatter at its threshold, a pump that stops when the engine stops, a starter
// that cannot be cranked forever by a held button.
//
//   cmake --build build --target test_output_gate && ./build/test_output_gate

#include "test_helpers.h"
#include "../firmware/Integration/OutputGate.h"
#include "../firmware/Pipeline/OutputStages.h"
#include "../firmware/Signal/SignalBus.h"
#include "../generated/signal_ids.h"

int main() {
    fprintf(stdout, "=== OutputGate ===\n");

    SECTION("one condition: off is simply not-on");
    {
        OutputGate g; g.on = 0;
        const OutputGateTimings t{};
        g.step(/*ask_on=*/false, /*ask_off=*/true, t, 1000);   CHECK(g.on == 0);
        g.step(true,  false, t, 1010);                          CHECK(g.on == 1);
        g.step(false, true,  t, 1020);                          CHECK(g.on == 0);
    }

    SECTION("two conditions make a DEADBAND — the whole of hysteresis");
    {
        // A fan: on above 95, off below 90. Between them neither condition is true, and the state
        // holds. With a single threshold the output chatters on every wobble of the reading.
        OutputGate g; g.on = 0;
        const OutputGateTimings t{};
        auto at = [&](float clt, uint32_t ms) { g.step(clt > 95.f, clt < 90.f, t, ms); return g.on != 0; };
        CHECK(!at(80.f, 0));
        CHECK(!at(92.f, 100));      // inside the band, coming up: still off
        CHECK( at(96.f, 200));      // over the top: on
        CHECK( at(92.f, 300));      // inside the band, coming down: STAYS on
        CHECK( at(90.5f, 400));
        CHECK(!at(89.f, 500));      // under the bottom: off
        CHECK(!at(92.f, 600));      // and back in the band: stays off
    }

    SECTION("minimum on and off times: a relay that chatters is a relay that welds");
    {
        OutputGate g; g.on = 0;
        const OutputGateTimings t{ /*min_on*/ 500, /*min_off*/ 300, 0, 0 };
        g.step(true, false, t, 1000);   CHECK(g.on == 1);        // it was never on, so nothing holds it off
        g.step(false, true, t, 1200);   CHECK(g.on == 1);        // asked to stop after 200ms — too soon
        g.step(false, true, t, 1499);   CHECK(g.on == 1);
        g.step(false, true, t, 1500);   CHECK(g.on == 0);        // 500ms served
        g.step(true, false, t, 1600);   CHECK(g.on == 0);        // asked to start after 100ms — too soon
        g.step(true, false, t, 1800);   CHECK(g.on == 1);        // 300ms served
    }

    SECTION("maximum on time + re-arm: a starter cannot be cranked forever");
    {
        // The button is held down for the whole of this section — ask_on never stops being true.
        OutputGate g; g.on = 0;
        const OutputGateTimings t{ 0, 0, /*max_on*/ 10000, /*rearm*/ 30000 };
        g.step(true, false, t, 0);            CHECK(g.on == 1);
        g.step(true, false, t, 9999);         CHECK(g.on == 1);
        g.step(true, false, t, 10000);        CHECK(g.on == 0);   // released, button or no button
        g.step(true, false, t, 20000);        CHECK(g.on == 0);   // …and locked out while it cools
        g.step(true, false, t, 39999);        CHECK(g.on == 0);
        g.step(true, false, t, 40000);        CHECK(g.on == 1);   // re-arm served: it may crank again
        fprintf(stdout, "    cranked 10s, locked out 30s, re-armed\n");
    }

    SECTION("a lockout with no re-arm delay still releases before it re-arms");
    {
        // rearm_ms = 0 must not mean "no lockout at all" — the output has to drop for at least one
        // frame, or a max-on trip with a held button would be invisible.
        OutputGate g; g.on = 0;
        const OutputGateTimings t{ 0, 0, /*max_on*/ 100, /*rearm*/ 0 };
        g.step(true, false, t, 0);     CHECK(g.on == 1);
        g.step(true, false, t, 100);   CHECK(g.on == 0);
        g.step(true, false, t, 101);   CHECK(g.on == 1);
    }

    SECTION("an unanswerable condition means what the SLOT says it means");
    {
        // A fan fails ON (the engine keeps cooling), a starter and a pump fail OFF, and a load that
        // is merely inconvenient holds its last state. Nothing about the sensor's failure decides it.
        CHECK(gate_answer(/*answered*/true,  1.0f, GATE_INVALID_OFF,  false) == true);
        CHECK(gate_answer(true,  0.0f, GATE_INVALID_ON,   true)  == false);   // answered wins outright
        CHECK(gate_answer(false, 0.0f, GATE_INVALID_OFF,  true)  == false);   // starter / fuel pump
        CHECK(gate_answer(false, 0.0f, GATE_INVALID_ON,   false) == true);    // cooling fan
        CHECK(gate_answer(false, 0.0f, GATE_INVALID_HOLD, true)  == true);    // hold: last state stands
        CHECK(gate_answer(false, 0.0f, GATE_INVALID_HOLD, false) == false);
    }

    SECTION("a fuel pump: prime at key-on, then run while the wheel turns");
    {
        // The convention, written as the two conditions a template would install:
        //   on : uptime_s < prime_s  or  age(trigger_teeth) < 1500
        //   off: neither of those
        // The point is the SECOND term: teeth, not sync. The pump is running before the decoder has
        // locked and long before anything is fired, which is what keeps the rail up for the start.
        OutputGate g; g.on = 0;
        const OutputGateTimings t{};
        auto frame = [&](float uptime_s, uint32_t tooth_age_ms, uint32_t ms) {
            const bool on  = (uptime_s < 3.0f) || (tooth_age_ms < 1500);
            g.step(on, !on, t, ms);
            return g.on != 0;
        };
        CHECK( frame(0.1f, 0xFFFFFFFF, 100));      // key on, nothing turning: priming
        CHECK( frame(2.9f, 0xFFFFFFFF, 2900));
        CHECK(!frame(3.1f, 0xFFFFFFFF, 3100));     // prime over, engine never turned: pump stops
        CHECK( frame(4.0f, 20,         4000));     // first teeth — before sync, before any spark
        CHECK( frame(30.f, 5,          30000));    // running
        CHECK( frame(30.f, 1400,       31000));    // one slow revolution: still on
        CHECK(!frame(31.f, 1600,       32000));    // teeth stopped: pump off (this is the safety half)
        fprintf(stdout, "    primed 3s, ran from the first tooth, stopped 1.5s after the last\n");
    }

    SECTION("the gate says WHEN, the value says HOW MUCH — and they do not know about each other");
    {
        // A variable-speed fan is a duty MAP and a condition, not one or the other: the condition
        // decides it runs at all, the map decides how hard. Proved through the stages the manager
        // wires, because that composition is the claim.
        uint8_t gate = 0;
        const float off_value = 0.0f;

        // A duty map: 4 breakpoints of coolant, duty rising with temperature. Cells are % x10, the
        // scale the schema declares.
        float    x_axis[4] = { 80.f, 90.f, 100.f, 110.f };
        uint8_t  x_n = 4;
        int16_t  x_src = static_cast<int16_t>(SIG_CLT);
        uint16_t cells[4] = { 300, 500, 800, 1000 };     // 30 / 50 / 80 / 100 %
        tbl::TableDesc duty{ cells, tbl::CELL_U16, 0.1f,
                             { x_axis, tbl::CELL_F32, &x_n, 0, &x_src, nullptr, 1.0f }, {}, {} };

        pipe::SourceCfg src{ &duty, 0.0f, &gate, off_value };
        pipe::Ctx c{};
        SignalBus bus{};
        bus.set(SIG_CLT, 90.0f);
        c.bus = &bus;
        pipe::State st{};

        pipe::value_from_table(c, &src, st);
        fprintf(stdout, "    gate closed, clt 90: value = %.1f (expect 0)\n", (double)c.value);
        CHECK_NEAR(c.value, 0.0, 0.01);                  // closed: the map is not even read

        gate = 1;
        pipe::value_from_table(c, &src, st);
        fprintf(stdout, "    gate open,   clt 90: value = %.1f%% (expect 50)\n", (double)c.value);
        CHECK_NEAR(c.value, 50.0, 0.5);                  // open: interpolated from the map

        bus.set(SIG_CLT, 105.0f);
        pipe::value_from_table(c, &src, st);
        fprintf(stdout, "    gate open,   clt 105: value = %.1f%% (expect 90)\n", (double)c.value);
        CHECK_NEAR(c.value, 90.0, 1.0);                  // …and it MOVES with the channel

        // A FIXED source answers the same gate, which is what most relays and many pumps want.
        pipe::SourceCfg fixed{ nullptr, 70.0f, &gate, off_value };
        pipe::value_fixed(c, &fixed, st);
        CHECK_NEAR(c.value, 70.0, 0.01);
        gate = 0;
        pipe::value_fixed(c, &fixed, st);
        CHECK_NEAR(c.value, 0.0, 0.01);
    }

    return test_summary();
}
