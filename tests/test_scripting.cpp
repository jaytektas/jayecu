#include "test_helpers.h"
#include "../firmware/Scripting/ScriptEngine.h"
#include "../firmware/Diagnostics/TextLog.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Can/CanBroker.h"
#include "../firmware/Can/ICanChannel.h"
#include "../generated/signal_ids.h"
#include <vector>

volatile uint32_t g_config_generation = 0;
// The written byte range, which on target the comms layer records (see CommsManager). A host test
// writes g_config directly, so it records none — and Sensors then falls back to the full sweep, which
// is exactly the behaviour an unknown range is supposed to get.
volatile uint32_t g_config_dirty_lo = 0xFFFFFFFFu;
volatile uint32_t g_config_dirty_hi = 0;   // ScriptEngine watches this (defined in CommsManager on target)
class DtcManager; DtcManager* g_dtc_table = nullptr;   // ScriptEngine reads DTCs via this (CommsManager on target)
extern "C" uint32_t g_stub_tick_ms;          // host-controllable ms clock (defined in platform_hal_stub)
extern "C" uint32_t g_stub_tick_us;          // ...and the us clock the exec watchdog measures against
extern "C" uint32_t g_stub_tick_us_step;     // us added per read: makes time pass DURING a Lua call


// Drive one script-thread iteration. rpm() reads the bus now (wk::rpm = SIG_RPM), so seed it.
static void run_tick(ScriptEngine& eng, SignalBus& bus, float rpm = 3000.0f) {
    bus.set(SIG_RPM, rpm);
    eng.tick(bus);
}

static SignalBus make_bus(float map_val  = 101.3f,
                           float tps_val  = 20.0f,
                           float clt_val  = 80.0f,
                           float iat_val  = 25.0f,
                           float batt_val = 12.6f) {
    SignalBus bus{};
    bus.set(SIG_MAP,    map_val);
    bus.set(SIG_TPS,    tps_val);
    bus.set(SIG_CLT,      clt_val);
    bus.set(SIG_IAT,      iat_val);
    bus.set(SIG_LAMBDA_1, 1.0f);
    bus.set(SIG_BATTERY,  batt_val);
    return bus;
}

static LuaConfig make_cfg(const char* source, bool enabled = true) {
    LuaConfig cfg{};
    cfg.enabled     = enabled ? 1 : 0;
    cfg.max_exec_us = 500;
    strncpy(cfg.source, source, sizeof(cfg.source) - 1);
    return cfg;
}

int main() {
    fprintf(stdout, "=== ScriptEngine ===\n");

    SECTION("disabled -- script does not run");
    {
        auto cfg = make_cfg("function onTick() signalWrite('lua_gauge_1', 1) end", false);
        ScriptEngine eng;
        eng.init(cfg);
        SignalBus bus = make_bus();
        run_tick(eng, bus);
        CHECK(eng.state == 1);                              // disabled
        CHECK(bus.get_by_name("lua_gauge_1") == 0.0f);      // onTick never ran
    }

    SECTION("empty script -- no error");
    {
        auto cfg = make_cfg("function onTick() end");
        ScriptEngine eng;
        eng.init(cfg);
        { SignalBus _b = make_bus(); run_tick(eng, _b); }
        CHECK(eng.state == 0);
        CHECK(eng.error_count == 0);
    }

    SECTION("a script affects the system ONLY by writing a bus signal");
    {
        // No setFuelAdd/setIgnRetard/setFuelCut/setIgnCut — Lua reaches into no module. To do anything
        // it writes the relevant signal; the engine task reads signals and does its own work.
        auto cfg = make_cfg("function onTick() signalWrite('lua_gauge_1', 42) end");
        ScriptEngine eng;
        eng.init(cfg);
        SignalBus bus = make_bus();
        run_tick(eng, bus);
        CHECK_NEAR(bus.get_by_name("lua_gauge_1"), 42.0f, 1e-6);
        CHECK(eng.error_count == 0);
    }

    SECTION("sensor reads -- values match bus and position");
    {
        auto cfg = make_cfg(
            "local ok = true\n"
            "function onTick()\n"
            "    ok = (math.abs(rpm()      - 3000.0) < 0.1)\n"
            "      and (math.abs(map()     - 101.3)  < 0.1)\n"
            "      and (math.abs(tps()     - 20.0)   < 0.1)\n"
            "      and (math.abs(clt()     - 80.0)   < 0.1)\n"
            "      and (math.abs(battery() - 12.6)   < 0.01)\n"
            "    signalWrite('lua_gauge_1', ok and 1 or 0)\n"
            "end\n"
        );
        ScriptEngine eng;
        eng.init(cfg);
        SignalBus bus = make_bus(101.3f, 20.0f, 80.0f, 25.0f, 12.6f);
        run_tick(eng, bus);
        CHECK(bus.get_by_name("lua_gauge_1") >= 0.5f);   // all reads matched
        CHECK(eng.error_count == 0);
    }

    SECTION("signalRead -- returns bus value by name");
    {
        auto cfg = make_cfg(
            "function onTick()\n"
            "    local v = signalRead('map')\n"
            "    local ok = (v ~= nil) and (math.abs(v - 101.3) < 0.1)\n"
            "    signalWrite('lua_gauge_1', ok and 1 or 0)\n"
            "end\n"
        );
        ScriptEngine eng;
        eng.init(cfg);
        SignalBus bus = make_bus(101.3f);
        run_tick(eng, bus);
        CHECK(bus.get_by_name("lua_gauge_1") >= 0.5f);
        CHECK(eng.error_count == 0);
    }

    SECTION("signalRead -- returns nil for unknown name");
    {
        auto cfg = make_cfg(
            "function onTick()\n"
            "    local v = signalRead('does_not_exist')\n"
            "    signalWrite('lua_gauge_1', (v == nil) and 1 or 0)\n"
            "end\n"
        );
        ScriptEngine eng;
        eng.init(cfg);
        SignalBus bus = make_bus();
        run_tick(eng, bus);
        CHECK(bus.get_by_name("lua_gauge_1") >= 0.5f);   // unknown name -> nil
        CHECK(eng.error_count == 0);
    }

    SECTION("signalWrite -- writes value to bus");
    {
        auto cfg = make_cfg(
            "function onTick()\n"
            "    signalWrite('boost_est', 150.0)\n"
            "end\n"
        );
        ScriptEngine eng;
        eng.init(cfg);
        SignalBus bus = make_bus();
        run_tick(eng, bus);
        CHECK(eng.error_count == 0);
        // The bus write takes effect in the bus itself
        CHECK(bus.valid(SIG_BOOST_EST));
        CHECK_NEAR(bus.get(SIG_BOOST_EST), 150.0f, 0.01f);
    }

    SECTION("signalWrite -- overrides a base producer (PRIO_LUA) and a ttl=0 latch holds");
    {
        // This is the whole point of the priority bus: a script writes straight over a signal a
        // base producer owns, and wins for as long as it stays fresh. ttl 0 latches (the base can't
        // release it — dead-script-safe).
        auto cfg = make_cfg("function onTick() signalWrite('boost_est', 200.0, 0) end");
        ScriptEngine eng;
        eng.init(cfg);
        SignalBus bus = make_bus();
        bus.set_by_name("boost_est", 50.0f, /*now*/0, /*ttl*/0, PRIO_BASE);  // base owns it
        run_tick(eng, bus);
        CHECK_NEAR(bus.get_by_name("boost_est"), 200.0f, 0.01f);             // Lua out-voted the base
        bus.set_by_name("boost_est", 50.0f, /*now*/1, 0, PRIO_BASE);         // base can't reclaim a fresh latch
        CHECK_NEAR(bus.get_by_name("boost_est"), 200.0f, 0.01f);
    }

    SECTION("switching the script off releases its latched writes; the base takes the channel back");
    {
        // A ttl-0 write latches against the base producer — and used to outlive the script that wrote it:
        // turn the script off and its last cut or trim stayed in force until the ECU restarted.
        auto cfg = make_cfg("function onTick() signalWrite('boost_est', 200.0, 0) end");
        ScriptEngine eng; eng.init(cfg);
        SignalBus bus = make_bus();
        bus.set_by_name("boost_est", 50.0f, 0, 0, PRIO_BASE);
        run_tick(eng, bus);
        CHECK_NEAR(bus.get_by_name("boost_est"), 200.0f, 0.01f);           // the script holds it
        cfg.enabled = 0;
        g_config_generation++;
        run_tick(eng, bus);                                                 // reload: script gone
        CHECK(!bus.valid(SignalBus::id_by_name("boost_est")));              // its value withdrawn…
        bus.set_by_name("boost_est", 50.0f, 1, 0, PRIO_BASE);
        CHECK_NEAR(bus.get_by_name("boost_est"), 50.0f, 0.01f);            // …and the base is back
    }

    SECTION("the manual's example scripts (chapter 35) load and run without error");
    {
        // Copied from manual/docs/part3/35-lua.md. A script printed as something to type has to run.
        static const char* kManual[] = {
            "setTickRate(20)\n"
            "function onTick()\n"
            "  local kpa = signalRead(\"map\")\n"
            "  if kpa then signalWrite(\"lua_gauge_1\", (kpa - 101.3) * 0.145, 200) end\n"
            "end\n",
            "setTickRate(10)\n"
            "local fanOn = false\n"
            "function onTick()\n"
            "  local t = signalRead(\"clt\")\n"
            "  if t == nil then fanOn = true        -- no coolant reading: run the fan\n"
            "  elseif t > 95 then fanOn = true\n"
            "  elseif t < 90 then fanOn = false end\n"
            "  signalWrite(\"lua_gauge_2\", fanOn and 1 or 0, 500)\n"
            "end\n",
            "canSubscribe(0x640)\n"
            "function onCanRx(bus, id, data)\n"
            "  if #data >= 2 then\n"
            "    signalWrite(\"lua_gauge_3\", (data[1] * 256 + data[2]) / 10, 500)\n"
            "  end\n"
            "end\n",
            "setTickRate(10)\n"
            "function onTick()\n"
            "  local r = math.floor(rpm())\n"
            "  canSend(0, 0x700, { r // 256, r % 256 })\n"
            "end\n",
            "setTickRate(5)\n"
            "local low = false\n"
            "function onTick()\n"
            "  local p = signalRead(\"oil_pressure\")\n"
            "  local now = p ~= nil and p < 100 and rpm() > 2000\n"
            "  if now and not low then setDtc(0x1F01, 2) end\n"
            "  if low and not now then clearDtc(0x1F01) end\n"
            "  low = now\n"
            "end\n",
        };
        for (const char* src : kManual) {
            auto cfg = make_cfg(src);
            cfg.max_exec_us = 0;                            // no watchdog: this is about the API, not time
            ScriptEngine eng; eng.init(cfg);
            SignalBus bus = make_bus(200.0f);               // 200 kPa MAP, 80 C coolant
            run_tick(eng, bus); run_tick(eng, bus);
            CHECK(eng.state == 0);
            CHECK(eng.error_count == 0);
        }
        // …and the two gauge examples publish what the manual says they do.
        {
            auto cfg = make_cfg(kManual[0]); cfg.max_exec_us = 0;
            ScriptEngine eng; eng.init(cfg);
            SignalBus bus = make_bus(200.0f);
            run_tick(eng, bus);
            CHECK_NEAR(bus.get_by_name("lua_gauge_1"), (200.0f - 101.3f) * 0.145f, 0.01f);
        }
        {
            auto cfg = make_cfg(kManual[1]); cfg.max_exec_us = 0;
            ScriptEngine eng; eng.init(cfg);
            SignalBus bus = make_bus(101.3f, 20.0f, 97.0f); // hot: fan on
            run_tick(eng, bus);
            CHECK_NEAR(bus.get_by_name("lua_gauge_2"), 1.0f, 1e-6);
        }
    }

    SECTION("interp -- linear table lookup");
    {
        auto cfg = make_cfg(
            "function onTick()\n"
            "    local xs = {0, 100, 200}\n"
            "    local ys = {0, 10,  20}\n"
            "    signalWrite('lua_gauge_1', interp(150, xs, ys))\n"
            "end\n"
        );
        ScriptEngine eng;
        eng.init(cfg);
        SignalBus bus = make_bus();
        run_tick(eng, bus);
        CHECK_NEAR(bus.get_by_name("lua_gauge_1"), 15.0f, 1e-4);   // interp(150) over (0,100,200)->(0,10,20)
        CHECK(eng.error_count == 0);
    }

    SECTION("syntax error -- state set to load-error, runs without crashing");
    {
        auto cfg = make_cfg("function onTick( INVALID SYNTAX !!!");
        ScriptEngine eng;
        eng.init(cfg);
        CHECK(eng.state == 2);
        // A load error must leave the engine inert, not crash the tick — and write nothing.
        SignalBus bus = make_bus();
        run_tick(eng, bus);
        CHECK(bus.get_by_name("lua_gauge_1") == 0.0f);
    }

    SECTION("runtime error -- error_count increments, no bus write from the failed tick");
    {
        auto cfg = make_cfg(
            "function onTick()\n"
            "    signalWrite('lua_gauge_1', 1)\n"
            "    error('deliberate test error')\n"   // after the write, but the count must still log
            "end\n"
        );
        ScriptEngine eng;
        eng.init(cfg);
        SignalBus bus = make_bus();
        run_tick(eng, bus);
        CHECK(eng.error_count == 1);
        CHECK(eng.state == 3);
    }

    SECTION("status + error line published to the bus");
    {
        // Runtime error on line 2 -> state 3, error_line 2, all published to the bus (after onTick).
        auto rcfg = make_cfg(
            "function onTick()\n"
            "    return (nil).x\n"   // index a nil value -> runtime error on line 2
            "end\n"
        );
        ScriptEngine eng;
        eng.init(rcfg);
        SignalBus bus = make_bus();
        run_tick(eng, bus);
        CHECK(eng.state == 3);
        CHECK(eng.error_line == 2);
        CHECK((int)(bus.get(SIG_LUA_STATE, -1.0f)) == 3);
        CHECK((int)(bus.get(SIG_LUA_ERROR_LINE, 0.0f)) == 2);
        CHECK((int)(bus.get(SIG_LUA_ERROR_COUNT, 0.0f)) >= 1);

        // A clean reload reports OK and clears the error line.
        auto okcfg = make_cfg("function onTick() end\n");
        ScriptEngine eng2;
        eng2.init(okcfg);
        SignalBus bus2 = make_bus();
        run_tick(eng2, bus2);
        CHECK(eng2.state == 0);
        CHECK(eng2.error_line == 0);
        CHECK((int)(bus2.get(SIG_LUA_STATE, -1.0f)) == 0);

        // Compile error on line 2 -> load-error state 2, error_line 2 (captured during init's load).
        auto badcfg = make_cfg(
            "function onTick()\n"
            "    x = = 1\n"   // syntax error on line 2
            "end\n"
        );
        ScriptEngine eng3;
        eng3.init(badcfg);
        SignalBus bus3 = make_bus();
        run_tick(eng3, bus3);
        CHECK(eng3.state == 2);
        CHECK(eng3.error_line == 2);
        CHECK((int)(bus3.get(SIG_LUA_ERROR_LINE, 0.0f)) == 2);

        // The full error text also reached the text console (for the 'D' channel): a tagged compile
        // error line carrying the line number.
        size_t dn = 0;
        const char* txt = g_text_log.swap_drain(&dn);
        CHECK(dn > 0);
        CHECK(strstr(txt, "[lua] compile error L2:") != nullptr);
    }

    SECTION("engine-stop -- detected by the script reading the bus, not a firmware hook");
    {
        // The replacement for the old onStop hook: a script watches rpm (a bus value) across ticks
        // and fires its own logic on the falling edge to 0. The engine task never calls into Lua.
        auto cfg = make_cfg(
            "prev = 0\n"
            "function onTick()\n"
            "    local r = rpm()\n"
            "    if prev > 0 and r == 0 then signalWrite('lua_gauge_1', 1) end\n"
            "    prev = r\n"
            "end\n"
        );
        ScriptEngine eng;
        eng.init(cfg);
        SignalBus bus = make_bus();
        run_tick(eng, bus);   // running — no edge yet
        CHECK_NEAR(bus.get_by_name("lua_gauge_1"), 0.0f, 1e-6);
        run_tick(eng, bus, 0.0f);      // rpm fell to 0 -> script's stop logic fires
        CHECK_NEAR(bus.get_by_name("lua_gauge_1"), 1.0f, 1e-6);
        CHECK(eng.error_count == 0);
    }

    SECTION("on_config_change -- reloads script");
    {
        auto cfg1 = make_cfg("function onTick() signalWrite('lua_gauge_1', 1) end");
        ScriptEngine eng;
        eng.init(cfg1);
        SignalBus bus = make_bus();
        run_tick(eng, bus);
        CHECK_NEAR(bus.get_by_name("lua_gauge_1"), 1.0f, 1e-6);

        auto cfg2 = make_cfg("function onTick() signalWrite('lua_gauge_1', 2) end");
        eng.on_config_change(cfg2);
        run_tick(eng, bus);
        CHECK_NEAR(bus.get_by_name("lua_gauge_1"), 2.0f, 1e-6);
    }

    SECTION("a write elsewhere in the tune keeps the script's state; a change to the script reloads it");
    {
        // The generation moves on EVERY tune write, and this used to reload the script on all of them —
        // a fresh lua_State, so `n` below went back to 0 because somebody edited a VE cell.
        auto cfg = make_cfg("local n = 0\nfunction onTick() n = n + 1; signalWrite('lua_gauge_1', n) end\n");
        ScriptEngine eng; eng.init(cfg);
        SignalBus bus = make_bus();
        run_tick(eng, bus); run_tick(eng, bus); run_tick(eng, bus);
        CHECK_NEAR(bus.get_by_name("lua_gauge_1"), 3.0f, 1e-6);
        g_config_generation++;                               // a write to some other part of the tune
        run_tick(eng, bus);
        CHECK_NEAR(bus.get_by_name("lua_gauge_1"), 4.0f, 1e-6);   // counted on: not reloaded
        strncpy(cfg.source, "local n = 100\nfunction onTick() n = n + 1; signalWrite('lua_gauge_1', n) end\n",
                sizeof(cfg.source) - 1);                     // the script itself edited in place
        g_config_generation++;
        run_tick(eng, bus);
        CHECK_NEAR(bus.get_by_name("lua_gauge_1"), 101.0f, 1e-6); // reloaded with the new source
    }

    SECTION("setTickRate -- decimates onTick to ~hz; default runs every cycle");
    {
        // 100 Hz => 10 ms period. onTick bumps a counter published to lua_gauge_1.
        auto cfg = make_cfg(
            "setTickRate(100)\n"
            "local n = 0\n"
            "function onTick() n = n + 1; signalWrite('lua_gauge_1', n) end\n");
        ScriptEngine eng; eng.init(cfg);
        SignalBus bus = make_bus();
        g_stub_tick_ms = 1000;
        run_tick(eng, bus);                       // first update always ticks
        CHECK_NEAR(bus.get(SIG_LUA_GAUGE_1), 1.0f, 0.01f);
        // run 1 update/ms for 100 ms — at 10 ms period that's ~10 more ticks, not 100
        for (int ms = 1001; ms <= 1100; ms++) { g_stub_tick_ms = ms; run_tick(eng, bus); }
        const float ticks = bus.get(SIG_LUA_GAUGE_1);
        CHECK(ticks >= 10.0f && ticks <= 12.0f);                  // ~11 (t=1000,1010,...,1100), not 101
        g_stub_tick_ms = 0;

        // default (no setTickRate) runs onTick every cycle
        auto cfg2 = make_cfg("local n=0\nfunction onTick() n=n+1; signalWrite('lua_gauge_2', n) end\n");
        ScriptEngine e2; e2.init(cfg2);
        SignalBus b2 = make_bus();
        for (int i = 0; i < 5; i++) run_tick(e2, b2);
        CHECK_NEAR(b2.get(SIG_LUA_GAUGE_2), 5.0f, 0.01f);         // every cycle
    }

    SECTION("canSubscribe + onCanRx -- matched CAN frame dispatched to the script");
    {
        struct MockCan : ICanChannel {
            std::vector<CanFrame> q;
            bool send(const CanFrame&) override { return true; }
            bool receive(CanFrame& f) override {
                if (q.empty()) return false;
                f = q.front(); q.erase(q.begin()); return true;
            }
            bool is_up() const override { return true; }
        };
        // exact-id subscription: 0x360 -> onCanRx decodes 2 bytes onto a bus signal
        MockCan mock; CanBroker broker; broker.add_bus(0, &mock);
        auto cfg = make_cfg(
            "canSubscribe(0x360)\n"
            "function onCanRx(bus, id, data)\n"
            "  if id == 0x360 then signalWrite('lua_gauge_1', data[1] * 256 + data[2]) end\n"
            "end\n");
        ScriptEngine eng; eng.set_can_broker(&broker); eng.init(cfg);
        CanFrame f{}; f.id = 0x360; f.ext = false; f.dlc = 2; f.data[0] = 1; f.data[1] = 44;  // 300
        mock.q.push_back(f);
        broker.update(0);                                   // can_task: process_rx -> enqueue
        SignalBus bus = make_bus();
        run_tick(eng, bus);                 // engine task: drain -> onCanRx -> signalWrite
        CHECK_NEAR(bus.get(SIG_LUA_GAUGE_1), 300.0f, 0.01f);

        // mask subscription: 0x100/0x700 matches 0x1xx (e.g. 0x123)
        MockCan mock2; CanBroker broker2; broker2.add_bus(0, &mock2);
        auto cfg2 = make_cfg(
            "canSubscribe(0x100, 0x700)\n"
            "function onCanRx(bus, id, data) signalWrite('lua_gauge_2', id) end\n");
        ScriptEngine e2; e2.set_can_broker(&broker2); e2.init(cfg2);
        CanFrame g{}; g.id = 0x123; g.ext = false; g.dlc = 1; g.data[0] = 7;
        mock2.q.push_back(g);
        broker2.update(0);
        SignalBus b2 = make_bus();
        run_tick(e2, b2);
        CHECK_NEAR(b2.get(SIG_LUA_GAUGE_2), float(0x123), 0.01f);
    }

    SECTION("getCalibration / evalTable -- read a tune scalar + interpolate a table by name");
    {
        auto cfg = make_cfg(
            "function onTick()\n"
            "  signalWrite('lua_gauge_1', getCalibration('idle_max_duty_pct') or -1)\n"
            "  signalWrite('lua_gauge_2', evalTable('idle_base_duty_table') or -1)\n"
            "  signalWrite('lua_gauge_3', getCalibration('does_not_exist') == nil and 1 or 0)\n"
            "end\n");
        ScriptEngine eng; eng.init(cfg);
        SignalBus bus = make_bus(101.3f, 20.0f, 0.0f);   // clt = 0
        run_tick(eng, bus);
        CHECK_NEAR(bus.get(SIG_LUA_GAUGE_1), 90.0f, 0.01f);   // idle max-duty default scalar
        CHECK_NEAR(bus.get(SIG_LUA_GAUGE_2), 64.85f, 0.1f);   // base-duty (default 8-pt table) interp @ clt 0: 72.0@-10, 57.7@+10 -> midpoint 64.85
        CHECK_NEAR(bus.get(SIG_LUA_GAUGE_3), 1.0f, 0.01f);    // unknown name -> nil
    }

    SECTION("signalWrite ttl -- a Lua-published signal expires if not refreshed");
    {
        auto cfg = make_cfg("function onTick() signalWrite('lua_gauge_4', 7, 100) end");  // ttl 100ms
        ScriptEngine eng; eng.init(cfg);
        SignalBus bus = make_bus();
        g_stub_tick_ms = 1000;
        run_tick(eng, bus);
        CHECK(bus.valid(SIG_LUA_GAUGE_4));
        CHECK_NEAR(bus.get(SIG_LUA_GAUGE_4), 7.0f, 0.01f);
        bus.expire_stale(1050); CHECK(bus.valid(SIG_LUA_GAUGE_4));   // 50ms < ttl
        bus.expire_stale(1200); CHECK(!bus.valid(SIG_LUA_GAUGE_4));  // 200ms > ttl -> stale
    }

    // THE EXEC WATCHDOG. Two things were wrong at once and each hid the other: the elapsed time was
    // measured in whole MILLISECONDS against a budget in MICROSECONDS (default 500), so a script was
    // aborted the instant the ms tick rolled however little it had used; and the message explaining
    // the abort used %u, which luaL_error cannot format — Lua raised "invalid option '%u' to
    // lua_pushfstring" about the message itself and threw that instead, naming whichever line
    // happened to be executing. On the bench that surfaced as a Lua error on a five-line script.
    SECTION("exec watchdog -- a script well inside its budget is not aborted");
    {
        // A busy loop long enough to run the instruction hook many times, and a clock advancing 1 us
        // per sample — nowhere near the 500 us budget.
        LuaConfig cfg = make_cfg("function onTick()\n"
                                 "  local s = 0\n"
                                 "  for i = 1, 20000 do s = s + i end\n"
                                 "  signalWrite('lua_gauge_1', s)\n"
                                 "end\n");
        ScriptEngine eng; eng.init(cfg);
        SignalBus bus = make_bus();
        g_stub_tick_ms = 5; g_stub_tick_us = 5000; g_stub_tick_us_step = 1;
        run_tick(eng, bus);
        g_stub_tick_us_step = 0;
        CHECK(eng.error_count == 0);
        CHECK(bus.valid(SIG_LUA_GAUGE_1));      // it ran to completion
    }

    SECTION("exec watchdog -- setWatchDog(us) overrides the tune, and 0 disables it");
    {
        // The tune says abort at 500 us; the script says it needs more and sets its own budget.
        LuaConfig cfg = make_cfg("setWatchDog(50000)\n"
                                 "function onTick()\n"
                                 "  local s = 0\n"
                                 "  for i = 1, 20000 do s = s + i end\n"
                                 "  signalWrite('lua_gauge_1', s)\n"
                                 "end\n");
        cfg.max_exec_us = 500;
        ScriptEngine eng; eng.init(cfg);
        SignalBus bus = make_bus();
        g_stub_tick_ms = 5; g_stub_tick_us = 5000; g_stub_tick_us_step = 200;   // ~ms of wall clock
        run_tick(eng, bus);
        g_stub_tick_us_step = 0;
        CHECK(eng.error_count == 0);            // the script's 50 ms budget, not the tune's 500 us
        CHECK(bus.valid(SIG_LUA_GAUGE_1));
    }

    SECTION("exec watchdog -- a genuine overrun aborts, and SAYS SO");
    {
        LuaConfig cfg = make_cfg("function onTick()\n"
                                 "  local s = 0\n"
                                 "  for i = 1, 2000000 do s = s + i end\n"
                                 "end\n");
        ScriptEngine eng; eng.init(cfg);
        SignalBus bus = make_bus();
        // 200 us per hook sample: the budget is crossed after a handful of them.
        g_stub_tick_ms = 5; g_stub_tick_us = 5000; g_stub_tick_us_step = 200;
        run_tick(eng, bus);
        g_stub_tick_us_step = 0;
        CHECK(eng.error_count > 0);
        // The message must be the ONE we wrote. With %u Lua throws about the format string instead,
        // and the reason for the abort never reaches the log at all.
        size_t n = 0;
        const char* err = g_text_log.swap_drain(&n);
        if (!err) err = "";
        CHECK(strstr(err, "exec budget") != nullptr);
        CHECK(strstr(err, "invalid option") == nullptr);
    }

    return test_summary();
}
