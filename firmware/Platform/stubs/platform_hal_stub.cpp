#include "../platform_hal.h"

// ---------------------------------------------------------------------------
// Stub HAL — plausible fixed sensor values so the firmware links and runs
// without real hardware.  Replace with ADC/peripheral reads on each board.
// ---------------------------------------------------------------------------

extern "C" bool     platform_read_din(uint8_t /*pin*/)    { return false;   }  // all inputs inactive
extern "C" bool     platform_read_power_good(uint8_t /*index*/) { return true; }  // both 5 V references good
extern "C" uint16_t platform_read_ain_raw(uint8_t /*pin*/) { return 0;      }  // 0 counts
extern "C" uint32_t platform_read_freq(uint8_t /*pin*/)   { return 0;       }  // no signal
extern "C" void     platform_freq_on_edge(uint8_t, uint32_t) {}                // no capture HW
extern "C" bool     platform_freq_enable(uint8_t /*pin*/)  { return false;  }  // no capture HW
extern "C" void     platform_sent_on_edge(uint8_t, uint32_t) {}                // no capture HW
extern "C" bool     platform_sent_enable(uint8_t /*pin*/)  { return false;  }  // no capture HW
extern "C" uint32_t platform_read_sent(uint8_t, bool)      { return 0;       }  // no signal
extern "C" void     platform_pulse_on_edge(uint8_t, uint32_t, bool) {}         // no capture HW
extern "C" bool     platform_pulse_enable(uint8_t /*pin*/) { return false;  }  // no capture HW
extern "C" bool     platform_capture_disable(uint8_t /*pin*/) { return false; } // no capture HW
extern "C" uint32_t platform_read_pulse_us(uint8_t /*pin*/) { return 0;     }  // no signal

extern "C" float platform_read_map_kpa()  { return 101.3f; }  // ~sea level
extern "C" float platform_read_baro_kpa() { return 101.3f; }  // ~sea level
// Ambient from the same on-board part, and whether its cache is fresh. A DIFFERENT value from the
// pressure above on purpose: routing an on-board temperature to the barometric acquire is the exact
// bug these exist to catch, and identical stub values would hide it.
extern "C" float platform_read_baro_temp_c() { return 23.5f; }
extern "C" bool  platform_baro_valid()       { return true; }
extern "C" float platform_read_tps_pct()  { return 0.0f;   }  // closed throttle
extern "C" float platform_read_clt_c()    { return 20.0f;  }  // ambient
extern "C" float platform_read_iat_c()    { return 25.0f;  }  // ambient
extern "C" float platform_read_lambda()   { return 1.0f;   }  // stoich
extern "C" float platform_read_battery_v(){ return 12.6f;  }  // resting battery

// Host-controllable millisecond clock. Defaults to 0 (constant, as before — callers that don't
// touch it are unaffected); a test can advance it (extern "C" uint32_t g_stub_tick_ms) to exercise
// time-dependent logic (e.g. ScriptEngine's setTickRate decimation).
extern "C" { uint32_t g_stub_tick_ms = 0; }   // block form: C linkage without the extern-with-initializer warning
extern "C" uint32_t platform_get_tick_ms() {
    // On a real target this wraps HAL_GetTick() or xTaskGetTickCount().
    return g_stub_tick_ms;
}

// Derived from the same stub tick a test drives, so a test that advances g_stub_tick_ms advances both
// clocks consistently. A test wanting finer control can set g_stub_tick_us directly.
extern "C" { uint32_t g_stub_tick_us = 0; }
// ADVANCE-PER-READ, so a host test can make time pass DURING a call it cannot otherwise interrupt.
// The Lua exec watchdog is the case: its hook samples the clock every 1000 VM instructions from
// inside a running script, and a clock that only moves between test statements can never make it
// fire — so the abort path, and the message it prints, were untestable. 0 (the default) keeps the
// clock perfectly still, which is what every other test wants.
extern "C" { uint32_t g_stub_tick_us_step = 0; }
extern "C" uint32_t platform_get_tick_us() {
    const uint32_t now = g_stub_tick_us ? g_stub_tick_us : g_stub_tick_ms * 1000u;
    g_stub_tick_us = now + g_stub_tick_us_step;
    return now;
}

extern "C" bool platform_rtc_get(uint8_t out[8]) {
    // Fixed plausible time (2025-01-01 00:00:00, Wednesday) for host builds.
    const uint8_t v[8] = {0, 0, 0, 3, 1, 1, 25, 0};
    for (int i = 0; i < 8; ++i) out[i] = v[i];
    return true;
}

extern "C" bool platform_rtc_set(const uint8_t /*in*/[7]) {
    return true;
}

extern "C" void platform_reboot()                        {}  // no reset on host
extern "C" void platform_request_bootloader()            {}  // no DFU on host
extern "C" void platform_enter_bootloader_if_requested() {}  // no DFU on host

extern "C" void platform_led_connected(bool /*on*/)    {}
extern "C" void platform_set_led_running(bool /*on*/)  {}
extern "C" void platform_set_led_warning(bool /*on*/)  {}
extern "C" void platform_set_led_error(bool /*on*/)    {}

extern "C" uint32_t platform_cyccnt() { return 0; }
// The divisor for those cycles. A host build has no DWT, so platform_cyccnt() is a constant 0 and
// any rate derived from this is meaningless — but the modules that read it (ElectronicThrottle's PID
// timestep, EngineTask's frame profiling) COMPILE for the host, so the symbol has to exist. A
// plausible non-zero value keeps the "counter present" branch live; a test wanting the other branch
// defines its own (see tests/test_throttle_control.cpp).
extern "C" uint32_t platform_cpu_hz() { return 216000000u; }
