#pragma once

#include "../../generated/modules/lua_config.h"   // LuaConfig (enabled/max_exec_us/source)
#include "../Can/CanFrame.h"                       // CanFrame (RX ring stores frames by value)
#include <cstdint>

struct lua_State;
struct lua_Debug;   // for the exec-watchdog debug hook (lua_Hook signature)
class CanBroker;
class SignalBus;

// ---------------------------------------------------------------------------
// ScriptEngine — runs user Lua scripts on a dedicated low-priority thread.
//
// It is NOT an engine module and does not run in the engine task: a driver thread calls tick(bus)
// periodically (see the script task in main.cpp). It runs below every base producer, so a runaway
// script can starve only itself — never the engine.
//
// A script is a pure SignalBus participant: it reads signals and writes signals (at override
// priority, so a write wins over the base producer until it expires or is released). It does not
// touch any other module's state or any engine frame. To "trim fuel" a script writes straight over
// the relevant bus signal; what the fuel module then does with that signal is none of Lua's business.
//
// Lua API globals registered per session:
//
//   Sensor reads (from SignalBus):
//     rpm() map() tps() clt() iat() lambda() battery()
//
//   Signal bus (read/write any slot by name):
//     signalRead("name")           → number | nil
//     signalWrite("name", value)   → bool  (fuel/ign/cut/trim/anything: write the signal, override prio)
//
//   CAN direct frame TX:
//     canSend(bus, id, {byte0, ...})  → bool
//
//   Utilities:
//     interp(x, xs, ys)   — 1-D interpolation
//     tickMs()             — system milliseconds
//     ecu_print(msg)       — debug log
//
// Script must define (at minimum):
//   function onTick() end
//
// There are no engine-lifecycle callbacks: a script that wants to act on engine start/stop reads
// the engine_state (or rpm) signal off the bus and edge-triggers in onTick. Lua only reads and
// writes bus signals — it never reaches into another module, and nothing calls into it beyond the
// periodic onTick (plus onCanRx for subscribed frames, which are events, not state).
// ---------------------------------------------------------------------------

class ScriptEngine {
public:
    // Telemetry fields — public by value, collected for the telemetry frame (benign torn reads).
    uint16_t last_exec_us = 0;
    uint8_t  error_count  = 0;
    uint16_t error_line   = 0;  // line of the last compile/runtime error (0 = none); parsed from lua_tostring
    uint8_t  state        = 0;  // 0=ok, 1=disabled, 2=load-error, 3=runtime-error

    void init(const LuaConfig& cfg);
    void on_config_change(const LuaConfig& cfg);

    // One iteration of the script thread: live-reload if the config bumped, drain CAN RX into
    // onCanRx, run onTick (decimated by setTickRate), publish status. Called periodically by the
    // script task; `bus` is the global SignalBus. All Lua state is touched only here, on one thread.
    void tick(SignalBus& bus);

    void set_can_broker(CanBroker* broker) { can_broker_ = broker; }

    // canSubscribe(id[,mask[,bus]]) trampoline target: register a CAN RX subscription so matching
    // frames are delivered to the script's onCanRx(bus,id,data) hook (drained on the script thread).
    void can_subscribe(uint8_t bus, uint32_t id, uint32_t mask);

    // setTickRate(hz) trampoline target: 0/negative -> run onTick every tick. Otherwise onTick is
    // decimated to ~hz (the thread still wakes at its base cadence to drain CAN promptly).
    void set_tick_rate_hz(float hz) {
        tick_min_ms_ = (hz > 0.0f) ? static_cast<uint32_t>(1000.0f / hz + 0.5f) : 0u;
    }

    // setWatchDog(us) trampoline target: the script's own abort budget, overriding the tune's
    // max_exec_us for as long as this script is loaded. 0 disables it.
    //
    // THE SCRIPT IS WHAT KNOWS HOW LONG IT MEANS TO TAKE. The budget lived only in the tune, so a
    // heavier script arrived on an ECU still holding the previous script's limit and was aborted for
    // it. Setting it beside setTickRate — in the top-level setup, where the cadence is already
    // declared — keeps the two halves of "how often, and for how long" in one place, and travels with
    // the script rather than with the tune it happens to be loaded onto.
    //
    // MEASURED IN WALL CLOCK, so it must allow for preemption: the Lua task is the lowest non-idle
    // priority, and time spent suspended by the engine frame counts against the budget. On the bench
    // 3 ms of work measured up to 6 ms wall at 1203 rpm, and more at higher rpm.
    void set_watchdog_us(uint32_t us) { wd_override_us_ = us; wd_override_set_ = true; }
    [[nodiscard]] uint32_t watchdog_us() const {
        return wd_override_set_ ? wd_override_us_ : (cfg_ ? cfg_->max_exec_us : 0u);
    }

    // Accessible to Lua API trampolines during tick().
    const SignalBus*      current_bus()      const { return bus_; }
    CanBroker*            can_broker()               { return can_broker_; }

private:
    bool load(const char* source);
    void apply_config();            // (re)load or disable per cfg_ — shared by init + the live re-derive
    void call_hook(const char* fn);
    void drain_can_rx();            // pop queued RX frames -> onCanRx (script-thread context)
    // Lua debug hook (LUA_MASKCOUNT) enforcing cfg_->max_exec_us: aborts a script that overruns its
    // budget so a runaway onTick/onCanRx/setup can't wedge the engine thread. wd_start_us_ is stamped at
    // the start of each Lua call; the hook trips when elapsed time passes the budget (see note in .cpp).
    static void watchdog_hook(lua_State* L, lua_Debug* ar);
    // CanBroker RxCallback: runs on the can_task — only ENQUEUES (never touches Lua), drained later.
    static void on_can_frame(void* ctx, uint8_t bus, const CanFrame& frame, uint32_t tick_ms);
    void publish_status(SignalBus& bus) const;   // push lua_state/error_count/error_line to telemetry
    void close();

    lua_State*            L_          = nullptr;
    const LuaConfig* cfg_       = nullptr;
    const SignalBus*      bus_        = nullptr;
    CanBroker*            can_broker_ = nullptr;

    bool has_on_tick_  = false;
    uint32_t cfg_gen_seen_ = 0;     // last g_config_generation acted on (live script re-load on push)
    // The Lua settings the running interpreter was loaded from (CRC-32 of LuaConfig). The generation
    // moves on ANY write; the script is reloaded only when ITS settings did, because a reload is a fresh
    // lua_State — every variable, timer and state machine in the script starts over.
    uint32_t loaded_crc_ = 0;
    uint32_t tick_min_ms_  = 0;     // onTick min period (0 = every cycle); set by setTickRate(hz)
    uint32_t wd_override_us_ = 0;   // setWatchDog(us) — the script's own budget, 0 = disabled
    bool     wd_override_set_ = false;  // ...whether the script said anything at all
    uint32_t last_tick_ms_ = 0;     // when onTick last ran (for the decimation)
    uint32_t wd_start_us_  = 0;     // start of the current Lua call, in the 1 MHz capture timebase
    bool     first_tick_pending_ = true;  // first onTick after a (re)load always runs, then decimates

    // CAN RX: a single-producer (can_task) / single-consumer (script thread) ring of matched frames.
    bool has_on_can_rx_ = false;
    struct RxItem { uint8_t bus; CanFrame frame; };
    static constexpr unsigned RX_RING = 16;          // power of 2 not required; simple modulo
    RxItem            rx_ring_[RX_RING];
    volatile uint8_t  rx_head_ = 0;                  // producer (can_task) writes
    volatile uint8_t  rx_tail_ = 0;                  // consumer (script thread) reads
};
