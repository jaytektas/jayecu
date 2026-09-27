#include "test_helpers.h"
#include "../firmware/Engine/EngineStateMachine.h"

// Defaults mirror the schema Engine group: cranking 400. STOPPED is rpm == 0 (no config threshold).
static EngineStateMachine::Params kDefaults() {
    return EngineStateMachine::Params{400.0f};
}

int main() {
    fprintf(stdout, "=== EngineStateMachine ===\n");

    SECTION("STOPPED -> CRANKING -> RUNNING climb, started edge fires once on RUNNING");
    {
        EngineStateMachine m; m.configure(kDefaults());
        bool started, stopped;
        CHECK(m.update(0.0f,   started, stopped) == EngineRunState::STOPPED);  CHECK(!started);
        CHECK(m.update(100.0f, started, stopped) == EngineRunState::CRANKING); CHECK(!started);
        CHECK(m.update(350.0f, started, stopped) == EngineRunState::CRANKING); CHECK(!started);
        CHECK(m.update(450.0f, started, stopped) == EngineRunState::RUNNING);  CHECK(started);  CHECK(!stopped);
        CHECK(m.update(800.0f, started, stopped) == EngineRunState::RUNNING);  CHECK(!started); // no re-fire
    }

    SECTION("RUNNING full hysteresis: holds RUNNING through any RPM dip (while still turning) until a real stop");
    {
        EngineStateMachine m; m.configure(kDefaults());
        bool s, x;
        m.update(0.0f, s, x); m.update(500.0f, s, x);            // -> RUNNING (started)
        CHECK(m.update(300.0f, s, x) == EngineRunState::RUNNING); CHECK(!s);  // dip below cranking: holds
        CHECK(m.update(120.0f, s, x) == EngineRunState::RUNNING); CHECK(!s);  // low lug, still turning: holds
        CHECK(m.update(15.0f,  s, x) == EngineRunState::RUNNING); CHECK(!s);  // crawling but position live: holds
        CHECK(m.update(700.0f, s, x) == EngineRunState::RUNNING); CHECK(!s);  // recovery: NO spurious re-anchor
    }

    SECTION("Real stop: rpm 0 (lost position) stops immediately (the watchdog already debounced it)");
    {
        EngineStateMachine m; m.configure(kDefaults());
        bool s, x;
        m.update(0.0f, s, x); m.update(800.0f, s, x);            // -> RUNNING
        CHECK(m.update(0.0f, s, x) == EngineRunState::STOPPED); CHECK(x);    // lost sync -> STOPPED + edge
        CHECK(m.update(0.0f, s, x) == EngineRunState::STOPPED); CHECK(!x);   // no re-fire
    }

    SECTION("Direct STOPPED -> RUNNING jump still fires the started edge");
    {
        EngineStateMachine m; m.configure(kDefaults());
        bool s, x;
        CHECK(m.update(600.0f, s, x) == EngineRunState::RUNNING); CHECK(s);
    }

    SECTION("Restart re-anchors: a stall and recovery fires started again");
    {
        EngineStateMachine m; m.configure(kDefaults());
        bool s, x;
        m.update(600.0f, s, x);                                  // RUNNING (started)
        m.update(0.0f, s, x);                                    // STOPPED (rpm 0)
        CHECK(m.state() == EngineRunState::STOPPED);
        CHECK(m.update(600.0f, s, x) == EngineRunState::RUNNING); CHECK(s);  // re-anchor
    }

    SECTION("CRANKING that never catches stops as soon as rpm falls to stopped (failed start)");
    {
        EngineStateMachine m; m.configure(kDefaults());
        bool s, x;
        CHECK(m.update(150.0f, s, x) == EngineRunState::CRANKING);            // turning, below cranking
        CHECK(m.update(200.0f, s, x) == EngineRunState::CRANKING);            // starter still spinning it
        CHECK(m.update(0.0f,   s, x) == EngineRunState::STOPPED); CHECK(x);   // rpm gone -> STOPPED
    }

    return test_summary();
}
