// WHEN THE CARD LOGS — the two conditions, the deadband, and the fail direction.
//
// The gate is OutputGate (already tested for outputs); what is tested here is what the DATALOGGER
// asks of it, because the answers differ from an output's in ways that matter:
//   - the deadband stops a chattering condition opening a FILE per transition, not just a relay cycle
//   - an unanswerable condition fails ON, where a starter must fail off
//   - a maximum-on with a zero re-arm is file rolling rather than a stop
//
//   cmake --build build --target test_datalog_gate && ./build/test_datalog_gate
#include "Integration/OutputGate.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

int main() {
    std::printf("=== SD datalog gate ===\n");

    // A PULL LOGGER: start above 4000, stop below 3000. The band between them is the whole point —
    // an engine crossing 4000 repeatedly must not produce a file per crossing.
    {
        OutputGate g; g.on = 0; g.since_ms = 0;
        const OutputGateTimings t{};
        auto at = [&](bool above4000, bool below3000, uint32_t ms) { g.step(above4000, below3000, t, ms); return g.on != 0; };
        ck(!at(false, true, 10),  "below the start point: not logging");
        ck( at(true,  false, 20), "crossing the start point: logging");
        ck( at(false, false, 30), "inside the band: still logging");        // 3000..4000
        ck( at(false, false, 40), "…and again, because the band HOLDS");
        ck(!at(false, true, 50),  "below the stop point: logging ends");
        ck(!at(false, false, 60), "back inside the band: still stopped");
        // That last one is the file-per-crossing bug: with one threshold this would restart here.
    }

    // A LOGGER FAILS ON. gate_answer is the shared rule; the datalogger's default policy is ON, so a
    // dead channel starts a recording rather than suppressing one.
    {
        ck(gate_answer(false, 0.0f, GATE_INVALID_ON,  false) == true,
           "an unanswerable condition logs, by default");
        ck(gate_answer(false, 0.0f, GATE_INVALID_OFF, false) == false,
           "…unless the tune says otherwise");
        ck(gate_answer(false, 0.0f, GATE_INVALID_HOLD, true) == true,
           "…or asks it to hold what it had");
        ck(gate_answer(true, 1.0f, GATE_INVALID_OFF, false) == true,
           "an ANSWERED condition ignores the policy entirely");
    }

    // MINIMUM ON: a condition that flickers must not produce a file per flicker.
    {
        OutputGate g; g.on = 0; g.since_ms = 0;
        const OutputGateTimings t{ /*min_on*/ 500, /*min_off*/ 0, 0, 0 };
        g.step(true, false, t, 1000);
        ck(g.on, "logging starts");
        g.step(false, true, t, 1100);
        ck(g.on, "…and a 100 ms flicker does not end it");
        g.step(false, true, t, 1600);
        ck(!g.on, "…but a real stop, past the minimum, does");
    }

    // MAXIMUM ON with a ZERO re-arm is file ROLLING: it closes and reopens on the next tick.
    {
        OutputGate g; g.on = 1; g.since_ms = 0; g.on_since_ms = 0;
        const OutputGateTimings t{ 0, 0, /*max_on*/ 10000, /*rearm*/ 0 };
        g.step(true, false, t, 9000);
        ck(g.on, "before the limit: still one file");
        g.step(true, false, t, 10000);
        ck(!g.on, "at the limit: the file closes");
        g.step(true, false, t, 10001);
        ck(g.on, "…and the next one opens immediately (rolling)");
    }

    // …whereas a re-arm delay is a real lockout, which is what a rate limit needs.
    {
        OutputGate g; g.on = 1; g.since_ms = 0; g.on_since_ms = 0;
        const OutputGateTimings t{ 0, 0, 10000, /*rearm*/ 60000 };
        g.step(true, false, t, 10000);
        ck(!g.on, "the limit trips");
        g.step(true, false, t, 30000);
        ck(!g.on, "…and the condition cannot restart it during the delay");
        g.step(true, false, t, 71000);
        ck(g.on, "…until the delay has passed");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All datalog gate tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
