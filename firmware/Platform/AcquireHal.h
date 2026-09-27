#pragma once
//
// HAL-bound Acquire stages — the source addressing for physical inputs.  These live in the
// PLATFORM tier (not firmware/Pipeline/, which stays pure) because they depend on the platform
// HAL.  Each is a thin wrapper that turns a pin read into a normalised raw scalar for the pure
// pipeline core; the switch debounce logic is lifted from firmware/Sensors/Sensors.cpp.
//
// source == SOURCE_NONE -> abort (nothing published; the slot expires on TTL).
//
#include "../Pipeline/Pipeline.h"
#include "../Pipeline/Stages.h"         // CurveCfg + curve_eval (fusion subs reuse the cal curve)
#include "platform_hal.h"
#include "../Scheduler/EngineSyncSampler.h"   // engine_sync_read_mv: angle-windowed for engine-sync pins

namespace pipe {

constexpr uint8_t SOURCE_NONE = 255;

// analog: `hw_sig` is the raw-count channel HardwareInput publishes for this pin (SIG_HW_BY_POOL[source]);
// a normal pin reads it from the bus. `engine_sync` pins (MAP) instead take the crank-angle-windowed
// average from the grid ISR shadow — a derived value that is NOT the plain pin read, so it can't come
// off the bus. source is retained for the engine-sync read + the SOURCE_NONE abort.
struct PinAcquireCfg    { uint8_t source; bool engine_sync; SignalId hw_sig; };
// Digital acquires: `source` is the DIG pin (for the capture ENABLE + the SOURCE_NONE abort); `hw_sig`
// is the raw channel HardwareInput publishes for that (pin, mode) — the VALUE is read off the bus, so
// the pin is sampled once (by HardwareInput). SENT CRC is always enforced at the HardwareInput read
// (all J2716/GM variants tried; see gen_hw_input_publish + SentDecoder) — there is no per-sensor knob.
struct FreqAcquireCfg   { uint8_t source; SignalId hw_sig; };                       // frequency (Hz)
struct PulseAcquireCfg  { uint8_t source; SignalId hw_sig; };                       // pulse high-time (µs)
struct SentAcquireCfg   { uint8_t source; SignalId hw_sig; };                       // SENT fast-channel
struct SwitchAcquireCfg { uint8_t source; bool invert; uint8_t debounce; SignalId hw_sig; };  // debounce 0 -> 3

// --- analog: raw ADC counts -> raw u32 (Decode applies the cal curve, which is in counts) ---
// A normal pin reads its counts FROM THE BUS (HardwareInput published SIG_HW_* this frame, ahead of
// Sensors) — so the pin is touched once, at the HAL, by HardwareInput. An engine-sync pin (MAP) takes
// the crank-angle-windowed average from the grid ISR shadow (engine_sync_read_raw), a derived value
// that isn't the plain pin read and so has no bus channel.
inline void acquire_analog(Ctx& c, const void* cfg, State&) {
    const auto* a = static_cast<const PinAcquireCfg*>(cfg);
    if (a->source == SOURCE_NONE) { c.abort = true; return; }
    if (a->engine_sync) {
        c.raw.u = engine_sync_read_raw(a->source);          // ISR angle-windowed average (MAP)
        c.valid = true;
    } else {
        c.raw.u = c.bus->get_u32(a->hw_sig, 0);             // raw counts off the bus (HardwareInput)
        c.valid = c.bus->valid(a->hw_sig);                  // no fresh raw published -> input has no data
    }
    c.raw_kind = RAW_U32;
}

// --- frequency: enable capture (config-driven) + read Hz OFF THE BUS -> raw u32 ---
inline void acquire_freq(Ctx& c, const void* cfg, State&) {
    const auto* a = static_cast<const FreqAcquireCfg*>(cfg);
    if (a->source == SOURCE_NONE) { c.abort = true; return; }
    platform_freq_enable(a->source);              // enable this pin's capture (idempotent); HardwareInput reads it
    c.raw.u = c.bus->get_u32(a->hw_sig, 0);
    c.raw_kind = RAW_U32;
    c.valid = c.bus->valid(a->hw_sig);
}

// --- pulse width / duty: high-time µs OFF THE BUS -> raw u32 ---
inline void acquire_pulse(Ctx& c, const void* cfg, State&) {
    const auto* a = static_cast<const PulseAcquireCfg*>(cfg);
    if (a->source == SOURCE_NONE) { c.abort = true; return; }
    platform_pulse_enable(a->source);
    c.raw.u = c.bus->get_u32(a->hw_sig, 0);
    c.raw_kind = RAW_U32;
    c.valid = c.bus->valid(a->hw_sig);
}

// --- SENT (J2716) fast channel: 12-bit value OFF THE BUS -> raw u32 ---
inline void acquire_sent(Ctx& c, const void* cfg, State&) {
    const auto* a = static_cast<const SentAcquireCfg*>(cfg);
    if (a->source == SOURCE_NONE) { c.abort = true; return; }
    platform_sent_enable(a->source);
    c.raw.u = c.bus->get_u32(a->hw_sig, 0);       // CRC already enforced at the HardwareInput read
    c.raw_kind = RAW_U32;
    c.valid = c.bus->valid(a->hw_sig);
}

// --- digital switch: read raw level OFF THE BUS, invert, integrating debounce -> raw 0/1 AND value 0/1 ---
// (A switch needs no Decode — the debounced state IS the engineering value. Debounce state stays here.)
inline void acquire_switch(Ctx& c, const void* cfg, State& st) {
    const auto* a = static_cast<const SwitchAcquireCfg*>(cfg);
    if (a->source == SOURCE_NONE) { c.abort = true; return; }
    bool raw = c.bus->get_u32(a->hw_sig, 0) != 0u;
    if (a->invert) raw = !raw;
    const uint8_t th = a->debounce ? a->debounce : 3;
    const bool was = st.sw.state;
    if (raw) { if (st.sw.cnt < th) st.sw.cnt++; if (st.sw.cnt >= th) st.sw.state = true; }
    else     { if (st.sw.cnt > 0)  st.sw.cnt--; if (st.sw.cnt == 0)  st.sw.state = false; }
    if (st.sw.state != was) st.sw.change_ms = c.now_ms;   // transition -> reset stuck timer
    c.raw.u    = st.sw.state ? 1u : 0u;
    c.raw_kind = RAW_U32;
    c.value    = st.sw.state ? 1.0f : 0.0f;
    c.valid    = true;
}

// --- on-board voltage (battery): board divider returns volts directly -> value ---
// raw is exposed as millivolts (u32) so a raw-window Condition can range-check it.
inline void acquire_onboard_voltage(Ctx& c, const void*, State&) {
    c.value = platform_read_battery_v();
    c.raw.u = static_cast<uint32_t>(c.value * 1000.0f); c.raw_kind = RAW_U32;
    c.valid = true;
}

// --- on-board barometric pressure: LPS22HB returns kPa directly -> value ---
inline void acquire_onboard_baro(Ctx& c, const void*, State&) {
    c.value = platform_read_baro_kpa();
    c.raw.f = c.value; c.raw_kind = RAW_F32;
    // Validity comes from the SAMPLER, not from the fact that a number exists. The cache always has
    // a value; what matters is whether anyone has managed to read the part recently.
    c.valid = platform_baro_valid();
}

// On-board ambient temperature — the same LPS22HB, a different register. This existed on the board
// HAL and nothing called it, so every on-board TEMPERATURE sensor fell through to the pressure
// acquire above and published kPa through a temperature curve (measured: ecu_temp = 94.60).
inline void acquire_onboard_temp(Ctx& c, const void*, State&) {
    c.value = platform_read_baro_temp_c();
    c.raw.f = c.value; c.raw_kind = RAW_F32;
    c.valid = platform_baro_valid();
}

} // namespace pipe
