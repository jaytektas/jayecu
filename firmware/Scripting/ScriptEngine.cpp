#include "ScriptEngine.h"
#include "well_known_signals.h"   // wk:: roles — rename-safe signal bindings (wk::rpm)
#include "../Signal/SignalBus.h"
#include "../Platform/platform_hal.h"
#include "../Can/CanBroker.h"
#include "../Can/CanFrame.h"
#include "../../generated/script_lookups.h"   // getCalibration / evalTable name lookups
#include "../../generated/signal_ids.h"        // SIG_LUA_STATE / _ERROR_COUNT / _ERROR_LINE
#include "../Diagnostics/TextLog.h"             // g_text_log — error text + ecu_print to the host console
#include "../Diagnostics/DtcManager.h"
#include "../Storage/Crc32.h"                 // crc32_buf — reload only when the Lua settings changed

namespace {
// Lua errors read "<chunk>:<line>: <message>"; a string chunk renders as [string "..."]:LINE:.
// Pull the line out (anchored on "]:" for string chunks, else the first ":<digit>"). 0 = unknown.
uint16_t parse_lua_error_line(const char* err) {
    if (!err) return 0;
    const char* p = nullptr;
    for (const char* s = err; *s; ++s)
        if (s[0] == ']' && s[1] == ':') { p = s + 2; break; }
    if (!p)
        for (const char* s = err; *s; ++s)
            if (s[0] == ':' && s[1] >= '0' && s[1] <= '9') { p = s + 1; break; }
    if (!p) return 0;
    unsigned long line = 0;
    bool any = false;
    for (; *p >= '0' && *p <= '9'; ++p) { line = line * 10 + (*p - '0'); any = true; }
    return (any && line <= 65535) ? static_cast<uint16_t>(line) : 0;
}
}  // namespace

// Pull in the full Lua API only in this TU.
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#include <cstring>
#include <cstdio>
#include <algorithm>

// On the STM32 target the Lua heap MUST be the bounded FreeRTOS pool (heap_4) — newlib's malloc has no
// _sbrk here (syscalls.c omits it deliberately), so its weak default grows unbounded and a boot-time Lua
// table resize walks off the top of RAM into an imprecise BusFault. These headers declare pvPortMalloc /
// vPortFree for the target allocator below. Guarded like SignalLock.h so host/test builds (no FreeRTOS)
// keep the plain realloc path.
#if defined(JAYECU_FIRMWARE)
#include "FreeRTOS.h"
#include "portable.h"
#endif

// ---------------------------------------------------------------------------
// Embedded allocator
//
// On the STM32 target, newlib's malloc is backed by FreeRTOS heap_4 or a
// linker-defined heap region.  realloc() is synthesised here using
// malloc/memcpy/free so we don't need a platform realloc.
//
// On native/test builds the system realloc is used.
// ---------------------------------------------------------------------------

#if defined(JAYECU_FIRMWARE)   // was #ifdef FREERTOS_H — DEAD guard: FreeRTOS.h's real macro is
                               // INC_FREERTOS_H, so Lua silently used newlib realloc + an unbounded _sbrk
                               // and overflowed RAM on a boot-time table resize. Bind to the target flag.
static void* lua_alloc(void* /*ud*/, void* ptr, size_t osize, size_t nsize) {
    if (nsize == 0) {
        vPortFree(ptr);
        return nullptr;
    }
    void* np = pvPortMalloc(nsize);
    if (np && ptr && osize > 0) {
        memcpy(np, ptr, osize < nsize ? osize : nsize);
        vPortFree(ptr);
    }
    return np;
}
#else
static void* lua_alloc(void* /*ud*/, void* ptr, size_t /*osize*/, size_t nsize) {
    if (nsize == 0) { free(ptr); return nullptr; }
    return realloc(ptr, nsize);
}
#endif

// ---------------------------------------------------------------------------
// Lua API trampolines — registered as Lua globals.
// Each function retrieves the ScriptEngine* from the Lua registry.
// ---------------------------------------------------------------------------

extern DtcManager* g_dtc_table;   // defined in CommsManager.cpp; shared by CLI, OBD, and Lua DTC API

namespace {

ScriptEngine* engine_from(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "__engine");
    auto* eng = static_cast<ScriptEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return eng;
}

// --- Sensor reads ---

static int l_rpm(lua_State* L) {
    auto* eng = engine_from(L);
    const SignalBus* bus = eng->current_bus();   // rpm is a bus signal like any other now
    lua_pushnumber(L, bus ? bus->get(wk::rpm, 0.0f) : 0.0);
    return 1;
}

#define BUS_SENSOR_FN(fname, sig_id, fallback)               \
static int l_##fname(lua_State* L) {                         \
    auto* eng = engine_from(L);                              \
    const SignalBus* bus = eng->current_bus();               \
    lua_pushnumber(L, bus ? bus->get(sig_id, fallback) : (fallback)); \
    return 1;                                                \
}

BUS_SENSOR_FN(map,     wk::map,    0.0f)
BUS_SENSOR_FN(tps,     wk::tps,   0.0f)
BUS_SENSOR_FN(clt,     wk::clt,    20.0f)
BUS_SENSOR_FN(iat,     wk::iat,    20.0f)
BUS_SENSOR_FN(lambda,  wk::lambda, 1.0f)
BUS_SENSOR_FN(battery, wk::battery, 12.0f)

#undef BUS_SENSOR_FN

// --- Signal bus API ---

// signalRead("name")  → number | nil
static int l_signalRead(lua_State* L) {
    auto* eng = engine_from(L);
    const SignalBus* bus = eng->current_bus();
    if (!bus) { lua_pushnil(L); return 1; }

    const char* name = luaL_checkstring(L, 1);
    const SignalId sid = SignalBus::id_by_name(name);
    if (sid == SIG_NONE) { lua_pushnil(L); return 1; }
    if (!bus->valid(sid)) { lua_pushnil(L); return 1; }

    // The channel's CELL TYPE decides the accessor: reading .f from an integer cell reinterprets its bits
    // (a mask, a counter, a state word — all garbage), which is what a script saw for the 61 u32-typed
    // channels. Integer channels are pushed as Lua INTEGERS (5.4 has a 64-bit integer subtype), so a
    // 32-bit mask or counter survives exactly, which a float could not do past 2^24.
    switch (SIGNAL_TYPES[sid]) {
        case SIG_T_U32: lua_pushinteger(L, static_cast<lua_Integer>(bus->get_u32(sid))); break;
        case SIG_T_I32: lua_pushinteger(L, static_cast<lua_Integer>(bus->get_i32(sid))); break;
        default:        lua_pushnumber(L, bus->get(sid, 0.0f));                          break;
    }
    return 1;
}

// signalWrite("name", value)  → bool
// ScriptEngine runs last in the pipeline; writes take effect next tick.
// const_cast is intentional — bus is non-const in reality, const here by convention.
static int l_signalWrite(lua_State* L) {
    auto* eng = engine_from(L);
    const SignalBus* bus = eng->current_bus();
    if (!bus) { lua_pushboolean(L, 0); return 1; }

    const char* name  = luaL_checkstring(L, 1);
    // Read as a Lua number and let the bus place it in the right cell (set_by_name -> set_typed): storing
    // a float where consumers read .u corrupts the value just as badly in this direction.
    const float value = static_cast<float>(luaL_checknumber(L, 2));
    // Optional 3rd arg: freshness TTL in ms (0/absent = never expire). A signal a script refreshes
    // periodically (e.g. a CAN-decoded value in onCanRx) should pass a TTL so it goes stale if the
    // source stops — consumers then see bus.valid()==false and fall back.
    const uint32_t ttl_ms = static_cast<uint32_t>(luaL_optinteger(L, 3, 0));

    SignalBus* mbus = const_cast<SignalBus*>(bus);
    const uint32_t now_ms = platform_get_tick_ms();
    // PRIO_LUA: a script write OUT-VOTES the base producer for as long as it stays fresh, which is
    // what "last word by precedence" means now that Lua no longer runs last in the engine task.
    // ttl == 0 latches (a safety the base can't release — dead-script-safe); ttl > 0 reverts to the
    // base after it ages out (the script stops refreshing).
    const bool ok = mbus->set_by_name(name, value, now_ms, ttl_ms, PRIO_LUA);
    // AN UNKNOWN CHANNEL NAME IS A TYPO, NOT A RESULT. set_by_name returns false and the return value
    // is almost never checked — a script that writes a channel this firmware does not have simply had
    // no effect, for ever, with nothing anywhere saying so. A rename is enough to cause it: bench_fuel
    // wrote "tps_1" long after the catalogue called it "tps", so its tip-in step never moved the
    // throttle and the transient suite reported the enrichment broken. Say it once per name — the
    // caller is on a 200 Hz tick and a log line per write would drown the console.
    if (!ok) {
        static const char* s_last_bad = nullptr;
        if (s_last_bad != name) {          // pointer compare: the literal is stable per call site
            s_last_bad = name;
            g_text_log.printf("[lua] signalWrite: no channel named '%s'\n", name);
        }
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// --- Config / table read API (generated name lookups) ---

// getCalibration("namespaced_name") -> tune scalar value (scale applied) | nil if unknown.
static int l_getCalibration(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    float v;
    if (script_get_calibration(name, v)) lua_pushnumber(L, v);
    else                                 lua_pushnil(L);
    return 1;
}

// evalTable("namespaced_name") -> the configurable table interpolated at the current bus axes | nil.
// A curve is just a 1-D table, so evalCurve maps to the same call.
static int l_evalTable(lua_State* L) {
    auto* eng = engine_from(L);
    const SignalBus* bus = eng->current_bus();
    const char* name = luaL_checkstring(L, 1);
    float v;
    if (bus && script_eval_table(name, *const_cast<SignalBus*>(bus), v)) lua_pushnumber(L, v);
    else                                                                 lua_pushnil(L);
    return 1;
}

// Lua has NO fuel/ign/correction functions and reaches into NO other module's state. To affect
// fuel, ignition, a cut, a cylinder trim, anything — a script writes the relevant signal on the bus
// (signalWrite, at override priority). The engine task reads bus signals and does its work; that's
// none of Lua's business, and what Lua writes is none of the engine task's.

// --- Utilities ---

static int l_tickMs(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(platform_get_tick_ms()));
    return 1;
}

// setTickRate(hz) — decimate onTick to ~hz (0/absent = every cycle, 1 kHz). Call in top-level setup.
static int l_setTickRate(lua_State* L) {
    auto* eng = engine_from(L);
    eng->set_tick_rate_hz(static_cast<float>(luaL_optnumber(L, 1, 0.0)));
    return 0;
}

// setWatchDog(us) — the script's own exec budget, overriding the tune's max_exec_us. 0 = off.
// Call it in the top-level setup, next to setTickRate.
static int l_setWatchDog(lua_State* L) {
    auto* eng = engine_from(L);
    const lua_Number us = luaL_optnumber(L, 1, 0.0);
    eng->set_watchdog_us(us > 0.0 ? static_cast<uint32_t>(us) : 0u);
    return 0;
}

static int l_ecu_print(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    g_text_log.printf("%s\n", msg);   // -> host ECU console (verbatim, one line)
    fprintf(stderr, "[script] %s\n", msg);
    return 0;
}

// interp(x, xs_table, ys_table)  → number
// xs and ys are 1-indexed Lua arrays of equal length.
static int l_interp(lua_State* L) {
    const float x = static_cast<float>(luaL_checknumber(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);

    int n = static_cast<int>(lua_rawlen(L, 2));
    if (n < 2) { lua_pushnumber(L, 0); return 1; }

    n = std::min(n, 32);
    float xs[32], ys[32];
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        xs[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        lua_rawgeti(L, 3, i + 1);
        ys[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    if (x <= xs[0])     { lua_pushnumber(L, ys[0]);     return 1; }
    if (x >= xs[n - 1]) { lua_pushnumber(L, ys[n - 1]); return 1; }

    int i = 0;
    while (i < n - 2 && xs[i + 1] < x) i++;
    float t = (xs[i + 1] > xs[i]) ? (x - xs[i]) / (xs[i + 1] - xs[i]) : 0.0f;
    lua_pushnumber(L, ys[i] * (1.0f - t) + ys[i + 1] * t);
    return 1;
}

// --- CAN API ---

// canSend(bus, id, {byte0, byte1, ...})  → bool
static int l_canSend(lua_State* L) {
    auto* eng = engine_from(L);
    if (!eng->can_broker()) { lua_pushboolean(L, 0); return 1; }

    const int bus = static_cast<int>(luaL_checkinteger(L, 1));
    const uint32_t id = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    luaL_checktype(L, 3, LUA_TTABLE);

    CanFrame frame{};
    frame.id  = id;
    frame.ext = (id > 0x7FFu);
    frame.rtr = false;

    int n = static_cast<int>(lua_rawlen(L, 3));
    if (n < 1) { lua_pushboolean(L, 0); return 1; }
    if (n > 8) n = 8;
    frame.dlc = static_cast<uint8_t>(n);
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 3, i + 1);
        frame.data[i] = static_cast<uint8_t>(lua_tointeger(L, -1) & 0xFF);
        lua_pop(L, 1);
    }

    const bool ok = eng->can_broker()->send_frame(static_cast<uint8_t>(bus), frame);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// canSubscribe(id [, mask [, bus]]) — deliver matching RX frames to onCanRx(bus, id, data).
// Call in top-level setup. mask default = exact id; bus default = any. data is a 1-indexed byte table.
static int l_canSubscribe(lua_State* L) {
    auto* eng = engine_from(L);
    const uint32_t id   = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    const uint32_t mask = static_cast<uint32_t>(luaL_optinteger(L, 2, 0xFFFFFFFF));
    const int      bus  = static_cast<int>(luaL_optinteger(L, 3, 0xFF));   // 0xFF = any bus
    eng->can_subscribe(static_cast<uint8_t>(bus), id, mask);
    return 0;
}

// ---------------------------------------------------------------------------
// DTC API
// ---------------------------------------------------------------------------

// setDtc(code, sev=1) — raise/keep-active a script-sourced DTC.
// Wraps in SigLockGuard because Lua runs at prio-1 and can be preempted by the engine task
// mid-raise(); the guard blocks the scheduler for the duration of the call (a handful of
// field stores on 64 slots).
static int l_setDtc(lua_State* L) {
    if (!g_dtc_table) return 0;
    const auto code = static_cast<uint16_t>(luaL_checkinteger(L, 1));
    const auto sev  = static_cast<uint8_t>(luaL_optinteger(L, 2, DTC_SEV_LEVEL1));
    {
        SigLockGuard g(true);
        // LATCHING: a script says setDtc once and expects it to stand until it says clearDtc. Ageing
        // it would quietly retire a fault the script is still reporting.
        g_dtc_table->raise(code, DtcSource::LUA, sev, platform_get_tick_ms(), DTC_TTL_LATCH);
    }
    return 0;
}

// clearDtc(code) — heal a script-raised DTC.
static int l_clearDtc(lua_State* L) {
    if (!g_dtc_table) return 0;
    const auto code = static_cast<uint16_t>(luaL_checkinteger(L, 1));
    {
        SigLockGuard g(true);
        g_dtc_table->heal(code);
    }
    return 0;
}

// dtcActive(code) → bool — is this code currently active?
// Read-only; individual uint8_t status reads are atomic on Cortex-M7. No critsec needed.
static int l_dtcActive(lua_State* L) {
    if (!g_dtc_table) { lua_pushboolean(L, 0); return 1; }
    const auto code = static_cast<uint16_t>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, g_dtc_table->code_severity(code) > 0 ? 1 : 0);
    return 1;
}

// dtcCount() → number — count of currently active DTCs.
static int l_dtcCount(lua_State* L) {
    lua_pushinteger(L, g_dtc_table ? static_cast<lua_Integer>(g_dtc_table->active_count()) : 0);
    return 1;
}

// dtcList() → table of {code=N, sev=N} for each active DTC.
static int l_dtcList(lua_State* L) {
    lua_newtable(L);
    if (!g_dtc_table) return 1;
    uint16_t codes[DtcManager::SLOTS];
    const uint8_t n = g_dtc_table->list_active(codes, DtcManager::SLOTS);
    for (uint8_t i = 0; i < n; ++i) {
        lua_newtable(L);
        lua_pushinteger(L, codes[i]);       lua_setfield(L, -2, "code");
        lua_pushinteger(L, g_dtc_table->code_severity(codes[i])); lua_setfield(L, -2, "sev");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Library registration table
// ---------------------------------------------------------------------------

static const luaL_Reg ECU_API[] = {
    { "rpm",          l_rpm          },
    { "map",          l_map          },
    { "tps",          l_tps          },
    { "clt",          l_clt          },
    { "iat",          l_iat          },
    { "lambda",       l_lambda       },
    { "battery",      l_battery      },
    { "signalRead",   l_signalRead   },
    { "signalWrite",  l_signalWrite  },
    { "getCalibration", l_getCalibration },
    { "evalTable",    l_evalTable    },
    { "evalCurve",    l_evalTable    },   // a curve is a 1-D table — same lookup
    { "interp",       l_interp       },
    { "tickMs",       l_tickMs       },
    { "setTickRate",  l_setTickRate  },
    { "setWatchDog",  l_setWatchDog  },
    { "ecu_print",    l_ecu_print    },
    { "canSend",      l_canSend      },
    { "canSubscribe", l_canSubscribe },
    { "setDtc",       l_setDtc       },
    { "clearDtc",     l_clearDtc     },
    { "dtcActive",    l_dtcActive    },
    { "dtcCount",     l_dtcCount     },
    { "dtcList",      l_dtcList      },
    { nullptr,        nullptr        }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// ScriptEngine — lifecycle
// ---------------------------------------------------------------------------

void ScriptEngine::close() {
    if (L_) { lua_close(L_); L_ = nullptr; }
    has_on_tick_ = false;
}

// Check the exec watchdog every this-many Lua VM instructions: small enough to trip a tight infinite
// loop within ~1 ms, large enough that the per-call overhead is negligible on a normal script.
static constexpr int kWatchdogInstrStep = 1000;

// LUA_MASKCOUNT hook enforcing cfg_->max_exec_us. The timebase is platform_get_tick_ms(), so enforcement
// lands at ~1 ms granularity — a sub-ms budget bounds at the next whole millisecond. That matches
// last_exec_us (also ms-quantized) and is what matters here: a runaway loop is stopped in ~1 ms instead
// of wedging the low-priority engine thread. max_exec_us == 0 disables the watchdog.
void ScriptEngine::watchdog_hook(lua_State* L, lua_Debug*) {
    ScriptEngine* eng = engine_from(L);
    if (!eng) return;
    const uint32_t budget_us = eng->watchdog_us();   // setWatchDog(us) if the script said, else the tune
    if (budget_us == 0) return;                      // 0 = no watchdog, which is the default
    // MICROSECONDS, because the budget is in microseconds. This measured the elapsed time in whole
    // MILLISECONDS and compared it against a microsecond budget, so the difference read 1 for anything
    // from a nanosecond to two milliseconds — and the default budget is 500 us. A script that started
    // just before the tick rolled was therefore aborted almost immediately, having used a fraction of
    // what it was allowed. Seen on the bench as a Lua error on a five-line script that cannot take
    // 500 us. Unsigned subtraction wraps correctly, as everywhere else on this timebase.
    const uint32_t elapsed_us = platform_get_tick_us() - eng->wd_start_us_;
    if (elapsed_us >= budget_us)
        // %d, NOT %u. luaL_error formats through lua_pushvfstring, which accepts only
        // %s %f %I %p %d %c %U %% — anything else makes Lua raise "invalid option" about the MESSAGE
        // and throw that instead of what happened. So the one line that was supposed to explain the
        // abort reported a Lua internal error against whichever line was executing, and the real
        // reason never appeared anywhere.
        luaL_error(L, "script exceeded its exec budget (%d us)", (int)budget_us);
}

bool ScriptEngine::load(const char* source) {
    close();

    L_ = lua_newstate(lua_alloc, nullptr);
    if (!L_) { state = 2; return false; }

    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, LUA_REGISTRYINDEX, "__engine");

    // Arm the exec watchdog for every Lua call on this state (setup chunk + onTick + onCanRx).
    lua_sethook(L_, watchdog_hook, LUA_MASKCOUNT, kWatchdogInstrStep);

    luaL_requiref(L_, "_G",      luaopen_base,   1); lua_pop(L_, 1);
    luaL_requiref(L_, "math",    luaopen_math,   1); lua_pop(L_, 1);
    luaL_requiref(L_, "string",  luaopen_string, 1); lua_pop(L_, 1);
    luaL_requiref(L_, "table",   luaopen_table,  1); lua_pop(L_, 1);

    for (const luaL_Reg* fn = ECU_API; fn->name; fn++) {
        lua_pushcfunction(L_, fn->func);
        lua_setglobal(L_, fn->name);
    }

    static const char* const REMOVE[] = {
        "dofile", "loadfile", "require", "collectgarbage",
        "rawequal", "rawget", "rawset", "rawlen",
        nullptr
    };
    for (int i = 0; REMOVE[i]; i++) {
        lua_pushnil(L_);
        lua_setglobal(L_, REMOVE[i]);
    }

    // Reset the tick rate to the default (every cycle) before running the chunk — the script's
    // top-level setTickRate(hz), if any, then sets it during this dostring.
    tick_min_ms_ = 0; last_tick_ms_ = 0; first_tick_pending_ = true;
    // A NEW SCRIPT INHERITS NOTHING. setWatchDog belongs to the script that called it, exactly as
    // setTickRate above does — carrying the previous script's budget into this one is how a script
    // gets aborted for a limit it never asked for.
    wd_override_us_ = 0; wd_override_set_ = false;
    wd_start_us_ = platform_get_tick_us();   // arm the exec watchdog for the top-level chunk below

    // Drop any CAN subscriptions from a previous load + flush the RX queue; the new script's
    // top-level canSubscribe() calls re-register during the dostring below.
    if (can_broker_) can_broker_->clear_rx(this);
    rx_head_ = rx_tail_ = 0;

    if (luaL_dostring(L_, source) != LUA_OK) {
        const char* err = lua_tostring(L_, -1);
        error_line = parse_lua_error_line(err);
        g_text_log.printf("[lua] compile error L%u: %s\n", error_line, err ? err : "?");
        fprintf(stderr, "[ScriptEngine] load error: %s\n", err ? err : "?");
        lua_pop(L_, 1);
        state = 2;
        close();
        return false;
    }

    auto has_fn = [&](const char* name) {
        lua_getglobal(L_, name);
        bool ok = lua_isfunction(L_, -1);
        lua_pop(L_, 1);
        return ok;
    };
    has_on_tick_   = has_fn("onTick");
    has_on_can_rx_ = has_fn("onCanRx");

    state = 0;
    error_line = 0;       // a clean compile clears the last error position
    return true;
}

void ScriptEngine::call_hook(const char* fn) {
    if (!L_) return;
    lua_getglobal(L_, fn);
    if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); return; }

    wd_start_us_ = platform_get_tick_us();   // arm the exec watchdog for this hook call
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L_, -1);
        error_line = parse_lua_error_line(err);
        g_text_log.printf("[lua] runtime error L%u: %s\n", error_line, err ? err : "?");
        fprintf(stderr, "[ScriptEngine] %s error: %s\n", fn, err ? err : "?");
        lua_pop(L_, 1);
        if (error_count < 255) error_count++;
        state = 3;
    }
}

void ScriptEngine::can_subscribe(uint8_t bus, uint32_t id, uint32_t mask) {
    if (can_broker_) {
        can_broker_->add_rx(bus, id, (id > 0x7FFu), &ScriptEngine::on_can_frame, this, mask);
    }
}

// can_task context: ENQUEUE only — never touch the Lua state here (it's owned by the engine task).
void ScriptEngine::on_can_frame(void* ctx, uint8_t bus, const CanFrame& frame, uint32_t /*tick_ms*/) {
    auto* eng = static_cast<ScriptEngine*>(ctx);
    const uint8_t head = eng->rx_head_;
    const uint8_t next = static_cast<uint8_t>((head + 1u) % RX_RING);
    if (next == eng->rx_tail_) return;                 // ring full -> drop this frame
    eng->rx_ring_[head].bus   = bus;
    eng->rx_ring_[head].frame = frame;
    eng->rx_head_ = next;                              // publish (single-producer)
}

// script-thread context: dispatch queued frames to onCanRx(bus, id, data). bus_ is set, so
// signalWrite() inside onCanRx works. Runs every tick (not gated by setTickRate).
void ScriptEngine::drain_can_rx() {
    while (rx_tail_ != rx_head_) {
        const RxItem& it = rx_ring_[rx_tail_];
        if (has_on_can_rx_) {
            lua_getglobal(L_, "onCanRx");
            lua_pushinteger(L_, it.bus);
            lua_pushinteger(L_, static_cast<lua_Integer>(it.frame.id));
            lua_createtable(L_, it.frame.dlc, 0);
            for (uint8_t i = 0; i < it.frame.dlc; i++) {
                lua_pushinteger(L_, it.frame.data[i]);
                lua_rawseti(L_, -2, i + 1);            // 1-indexed byte table
            }
            wd_start_us_ = platform_get_tick_us();   // arm the exec watchdog for this onCanRx call
            if (lua_pcall(L_, 3, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(L_, -1);
                error_line = parse_lua_error_line(err);
                g_text_log.printf("[lua] runtime error L%u: %s\n", error_line, err ? err : "?");
                fprintf(stderr, "[ScriptEngine] onCanRx error: %s\n", err ? err : "?");
                lua_pop(L_, 1);
                if (error_count < 255) error_count++;
                state = 3;
            }
        }
        rx_tail_ = static_cast<uint8_t>((rx_tail_ + 1u) % RX_RING);
    }
}

// ---------------------------------------------------------------------------
// ScriptEngine — EngineModule interface
// ---------------------------------------------------------------------------

void ScriptEngine::apply_config() {
    if (cfg_) loaded_crc_ = crc32_buf(reinterpret_cast<const uint8_t*>(cfg_), sizeof(*cfg_));
    if (cfg_ && cfg_->enabled && cfg_->source[0] != '\0') {
        load(cfg_->source);          // builds a fresh lua_State from the config source
    } else {
        close();
        state = 1;
    }
}

void ScriptEngine::init(const LuaConfig& cfg) {
    cfg_ = &cfg;
    extern volatile uint32_t g_config_generation;
    cfg_gen_seen_ = g_config_generation;
    apply_config();
}

void ScriptEngine::on_config_change(const LuaConfig& cfg) {
    cfg_ = &cfg;
    apply_config();
}

void ScriptEngine::tick(SignalBus& bus) {
    // on_config_change() is not wired in this firmware (modules self-watch g_config_generation, like
    // Sensors): reload the script live when a TS/bench push bumps the generation. This
    // runs on the script thread, so the lua_close()+reload never races the running interpreter.
    //
    // ONLY WHEN THE LUA SETTINGS CHANGED. The generation moves on every write to the tune — a VE cell,
    // a spark trim — and this reloaded on all of them, so a script's state (a counter, a latch, a timer
    // it was running) was wiped by an unrelated edit. The settings' own bytes are compared instead.
    extern volatile uint32_t g_config_generation;
    if (g_config_generation != cfg_gen_seen_) {
        cfg_gen_seen_ = g_config_generation;
        if (!cfg_ || crc32_buf(reinterpret_cast<const uint8_t*>(cfg_), sizeof(*cfg_)) != loaded_crc_) {
            // THE OLD SCRIPT'S WRITES GO WITH IT. A write with no ttl latches at PRIO_LUA, and nothing
            // else is allowed to replace it — so switching a script off, or loading a new one, left
            // whatever the old one last wrote (a cut, a trim) in force until the ECU restarted.
            bus.release_prio(PRIO_LUA);
            apply_config();
        }
    }
    // Inactive (no compiled script / disabled): publish status and bail — onTick won't run, so `state`
    // is already final and the host's "OK / error on line N" indicator stays live regardless.
    if (!L_ || !cfg_ || !cfg_->enabled) {
        publish_status(bus);
        return;
    }

    bus_ = &bus;

    drain_can_rx();   // dispatch queued CAN RX -> onCanRx (every tick, before onTick)

    // onTick is rate-limited by setTickRate(hz). There are no lifecycle hooks: a script that wants
    // to react to engine start/stop reads the engine_state (or rpm) signal off the bus and
    // edge-triggers its own logic in onTick — Lua is a pure bus participant.
    const uint32_t now = platform_get_tick_ms();
    const bool do_tick = has_on_tick_ && (first_tick_pending_ || (tick_min_ms_ == 0)
                      || (now - last_tick_ms_ >= tick_min_ms_));

    if (do_tick) {
        first_tick_pending_ = false;
        last_tick_ms_ = now;
        // MEASURED IN MICROSECONDS, like the budget it is compared against. This was
        // (tick_ms - now) * 1000, so it could only ever report 0, 1000 or 2000 — a script is asked to
        // fit inside 500 us and the one instrument that says how long it took could not resolve
        // anything below a millisecond. Sizing max_exec_us from it was impossible; every healthy
        // script read 0.
        const uint32_t t0_us = platform_get_tick_us();
        call_hook("onTick");
        const uint32_t dt_us = platform_get_tick_us() - t0_us;
        last_exec_us = static_cast<uint16_t>(dt_us > 65535u ? 65535u : dt_us);
    }

    bus_ = nullptr;

    publish_status(bus);   // after onTick, so a same-cycle compile/runtime error reflects immediately
}

void ScriptEngine::publish_status(SignalBus& bus) const {
    // Timed, like every other publish: an untimed write is read by SignalBus as "the time is 0" and
    // ages whatever is in the slot against it, which is how a live override gets thrown away.
    const uint32_t now = platform_get_tick_ms();
    bus.set(SIG_LUA_STATE, static_cast<float>(state), true, now);
    bus.set(SIG_LUA_ERROR_COUNT, static_cast<float>(error_count), true, now);
    bus.set(SIG_LUA_ERROR_LINE, static_cast<float>(error_line), true, now);
    // HOW LONG onTick ACTUALLY TOOK. The script is held to max_exec_us and had no way to show its own
    // cost — last_exec_us was computed and read by nothing, so the budget could only be guessed at,
    // and the guess that shipped (500 us) aborts real scripts. Published so the limit can be sized
    // from a measurement.
    bus.set(SIG_LUA_EXEC_US, static_cast<float>(last_exec_us), true, now);
}
