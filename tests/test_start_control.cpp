// Host test for StartControl — the start sequence: a latched press, the rail brought up before the crank,
// and a request that follows the button once the starter has had its chance. The throttle half of the wait
// is tested against a real ElectronicThrottle in test_throttle_control; here there is none (nullptr).
#include "test_helpers.h"
#include "../firmware/Engine/Modules/StartControl.h"
#include "../firmware/Engine/Modules/ElectronicThrottle.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../generated/signal_ids.h"
#include "../firmware/Diagnostics/DtcManager.h"
#include "../firmware/Diagnostics/TextLog.h"
#include <string>

extern "C" uint32_t g_stub_tick_ms;
bool g_system_active = true;
// StartControl only calls this through a pointer this test never sets; the definition is for the linker.
bool ElectronicThrottle::start_proven() const noexcept { return true; }

static std::string console() { size_t n = 0; const char* t = g_text_log.swap_drain(&n); return std::string(t, n); }

static StartControlConfig make_cfg() {
    StartControlConfig c{};
    c.enabled = 1; c.prime_enable = 1; c.press_src = SIG_FUEL_PRESSURE; c.min_press_kpa = 2500;   // 250 kPa
    c.max_wait_ms = 3000; c.prime_ms = 1000; c.give_up_ms = 8000;
    return c;
}

struct Rig {
    StartControlConfig cfg = make_cfg();
    StartControl sc;
    SignalBus bus;
    EnginePosition pos{};
    EngineFrame frame{};
    float sw = 0.0f, state = 0.0f, press = -1.0f;   // press < 0 = no rail reading
    bool  sw_live = true;                           // false = start_sw not published (key off: the sensor is skipped)
    Rig() { sc.init(cfg); }
    void step(int ms = 10) {
        g_stub_tick_ms += ms;
        if (sw_live) bus.set(SIG_START_SW, sw, true, g_stub_tick_ms);
        else         bus.set(SIG_START_SW, 0.0f, false, g_stub_tick_ms);
        bus.set(SIG_ENGINE_STATE, state, true, g_stub_tick_ms);
        if (press >= 0.0f) bus.set(SIG_FUEL_PRESSURE, press, true, g_stub_tick_ms);
        else               bus.set(SIG_FUEL_PRESSURE, 0.0f, false, g_stub_tick_ms);
        sc.update(pos, bus, frame);
    }
    void run(int ms) { for (int t = 0; t < ms; t += 10) step(); }
    bool crank() const { return bus.get_bool(SIG_CRANK_REQ); }
    bool pump()  const { return bus.get_bool(SIG_START_PUMP); }
    int  st()    const { return static_cast<int>(bus.get(SIG_START_STATE)); }
    // ms from now until crank_req, or -1 within `limit`
    int until_crank(int limit) { for (int t = 0; t <= limit; t += 10) { if (crank()) return t; step(); } return -1; }
};

int main() {
    fprintf(stdout, "=== StartControl ===\n");

    SECTION("switched off (the default): crank_req is the button, nothing waits and nothing asks for the pump");
    { Rig r; r.cfg.enabled = 0; r.press = 20.0f; r.step();
      CHECK(!r.crank());
      r.sw = 1; r.step();
      CHECK(r.crank()); CHECK(!r.pump()); CHECK(r.st() == StartControl::IDLE);
      r.sw = 0; r.step();
      CHECK(!r.crank()); }                                     // a tap is a tap: no latch when off

    SECTION("switching it off heals the code it raised");
    { Rig r; DtcManager dtc; r.sc.set_dtc(&dtc); r.press = 30.0f; r.step();
      r.sw = 1; r.step(); r.sw = 0;
      CHECK(r.until_crank(5000) >= 0);
      CHECK(dtc.code_severity(0x0087) == 1);
      r.cfg.enabled = 0; r.step();
      CHECK(dtc.code_severity(0x0087) == 0);
      console(); }

    SECTION("prime off: a press is a crank request at once, and nothing asks for the pump");
    { Rig r; r.cfg.prime_enable = 0; r.step();
      r.sw = 1; r.step();
      CHECK(r.crank()); CHECK(!r.pump()); CHECK(r.st() == StartControl::CRANK); }

    SECTION("no rail reading: the pump runs from the press and the starter waits Prime Time");
    { Rig r; r.step();
      r.sw = 1; r.step();
      CHECK(r.pump()); CHECK(!r.crank()); CHECK(r.st() == StartControl::PRIMING);
      const int t = r.until_crank(3000);
      fprintf(stdout, "    granted after %d ms\n", t);
      CHECK(t >= 980 && t <= 1010);
      CHECK(r.pump()); }

    SECTION("a reading rail: the starter waits for pressure, not the clock");
    { Rig r; r.press = 20.0f; r.step();
      r.sw = 1; r.step();
      r.run(2000);
      CHECK(!r.crank()); CHECK(r.pump());                      // 2 s, still 20 kPa: waiting (not Prime Time)
      r.press = 260.0f; r.step();
      CHECK(r.crank()); }

    SECTION("a warm rail still up: no wait at all");
    { Rig r; r.press = 290.0f; r.step();
      r.sw = 1; r.step();
      CHECK(r.crank()); }

    SECTION("a rail that never comes up cranks anyway after Longest Pressure Wait");
    { Rig r; r.press = 30.0f; r.step();
      r.sw = 1; r.step();
      const int t = r.until_crank(5000);
      fprintf(stdout, "    granted after %d ms\n", t);
      CHECK(t >= 2980 && t <= 3010); }

    SECTION("a rail cranked on low raises P0087 and says what it reached; a good start heals it");
    { Rig r; DtcManager dtc; r.sc.set_dtc(&dtc); r.press = 30.0f; r.step(); console();
      r.sw = 1; r.step(); r.sw = 0;
      CHECK(r.until_crank(5000) >= 0);
      CHECK(dtc.code_severity(0x0087) == 1);
      const std::string c = console();
      fprintf(stdout, "    console: %s", c.c_str());
      CHECK(c.find("rail 30 kPa") != std::string::npos);
      CHECK(c.find("cranking anyway") != std::string::npos);
      r.state = 2.0f; r.step(); r.state = 0.0f; r.step();     // ran, stopped
      r.press = 290.0f; r.sw = 1; r.step();                     // next start reaches pressure
      CHECK(r.crank());
      CHECK(dtc.code_severity(0x0087) == 0);
      CHECK(console().empty()); }

    SECTION("no reading is not a low rail: Prime Time runs out without a code");
    { Rig r; DtcManager dtc; r.sc.set_dtc(&dtc); r.step(); console();
      r.sw = 1; r.step();
      CHECK(r.until_crank(2000) >= 0);
      CHECK(dtc.code_severity(0x0087) == 0);
      CHECK(console().empty()); }

    SECTION("the press is latched: a tap is enough");
    { Rig r; r.step();
      r.sw = 1; r.step(); r.sw = 0; r.step();
      CHECK(!r.crank()); CHECK(r.pump());
      CHECK(r.until_crank(2000) >= 0); }

    SECTION("once the engine turns the request follows the button: held = held, released = over");
    { Rig r; r.cfg.prime_enable = 0; r.step();
      r.sw = 1; r.step(); CHECK(r.crank());
      r.state = 1.0f; r.run(500);                              // cranking
      CHECK(r.crank());                                        // still held
      r.sw = 0; r.step();
      CHECK(!r.crank()); CHECK(r.st() == StartControl::IDLE);
      r.run(5000); CHECK(!r.crank()); }                        // and it stays over (no crank on its own)

    SECTION("a tap that the starter's own interlock refuses is not left standing");
    { Rig r; r.cfg.prime_enable = 0; r.step();
      r.sw = 1; r.step(); r.sw = 0; r.step();
      CHECK(r.crank());                                        // granted: the tap is latched...
      r.run(1600);                                             // ...but the engine never turned (not in neutral)
      CHECK(!r.crank()); CHECK(r.st() == StartControl::IDLE);  // so it is not still asking when the clutch goes down
    }

    SECTION("running ends it; a held button on a running engine asks for nothing");
    { Rig r; r.cfg.prime_enable = 0; r.step();
      r.sw = 1; r.step(); r.state = 1.0f; r.run(300);
      r.state = 2.0f; r.step();
      CHECK(!r.crank()); CHECK(!r.pump()); CHECK(r.st() == StartControl::IDLE);
      r.sw = 0; r.step(); r.sw = 1; r.step();                  // pressed again while running
      CHECK(!r.crank()); }

    SECTION("key-off ends a pending start, and a button held through key-on is not a press");
    { Rig r; r.step();
      r.sw = 1; r.step(); CHECK(r.pump());
      g_system_active = false; r.step();
      CHECK(!r.pump()); CHECK(r.st() == StartControl::IDLE);
      g_system_active = true; r.run(3000);
      CHECK(!r.crank()); CHECK(!r.pump()); }                   // still held from before: no edge, no start

    // KELLY'S UTE, 25 Oct. The starter's draw pulled the battery terminal off mid-crank; USB held the ECU up.
    // Key off (the start switch stops being read, so start_sw goes unpublished), terminal fixed, key on with
    // nobody near the key: the switch's frozen debounce published "pressed" for its first samples, then
    // released. That was taken for a press and the engine cranked. Replayed here, in both modes.
    SECTION("a stale 'pressed' at key-on (power lost mid-crank) never cranks");
    for (int en = 0; en <= 1; ++en) {
      Rig r; r.cfg.enabled = static_cast<uint8_t>(en); r.press = 30.0f; r.step();
      r.sw = 1; r.step();                                    // the real press: cranking
      CHECK(r.crank() || en);                                // (on: the request may still be waiting)
      g_system_active = false; r.sw_live = false; r.run(2000);   // battery gone: key off, switch unread
      CHECK(!r.crank());
      g_system_active = true; r.sw_live = true;
      r.sw = 1; r.step(); r.step(); r.step();                // the frozen debounce: three samples of "pressed"
      bool cranked = r.crank();
      r.sw = 0;                                              // ...then the pin's real reading
      for (int t = 0; t < 10000; t += 10) { r.step(); cranked = cranked || r.crank(); }
      CHECK(!cranked);
      r.sw = 1; r.step();                                    // and a real press after it still works
      CHECK(r.until_crank(5000) >= 0);
      r.sw = 0; r.run(2000);
    }

    SECTION("a button already pressed at key-on is not a press until it is let go and pressed again");
    for (int en = 0; en <= 1; ++en) {
      Rig r; r.cfg.enabled = static_cast<uint8_t>(en); r.press = 30.0f;
      g_system_active = false; r.sw_live = false; r.run(500);
      g_system_active = true; r.sw_live = true; r.sw = 1;   // held (or stuck) through key-on
      CHECK(r.until_crank(3000) < 0);
      r.sw = 0; r.step(); r.sw = 1; r.step();
      CHECK(r.until_crank(5000) >= 0);
      r.sw = 0; r.run(2000);
    }

    SECTION("power_on() is a fresh module: nothing of the last key cycle survives");
    { Rig r; r.press = 30.0f; r.step();
      r.sw = 1; r.step(); CHECK(r.st() != StartControl::IDLE);
      EngineModule::power_on(r.sc); r.sc.init(r.cfg);
      r.sw = 1; r.step();                                    // still held: not armed in a fresh module
      CHECK(r.st() == StartControl::IDLE); CHECK(!r.crank()); }

    SECTION("a wait longer than Give Up After ends the request");
    { Rig r; r.cfg.prime_ms = 5000; r.cfg.give_up_ms = 2000; r.step();
      r.sw = 1; r.step(); r.sw = 0;
      console();
      CHECK(r.until_crank(6000) < 0);
      CHECK(r.st() == StartControl::IDLE); CHECK(!r.pump());
      const std::string c = console();
      fprintf(stdout, "    console: %s", c.c_str());
      CHECK(c.find("prime time not finished") != std::string::npos);
      CHECK(c.find("press dropped") != std::string::npos); }

    return test_summary();
}
