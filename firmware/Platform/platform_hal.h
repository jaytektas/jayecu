#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Platform HAL — sensor reads and system services.
//
// Implemented in firmware/Platform/<target>/platform_hal.cpp for each board.
// The stub implementation in Platform/stubs/ returns plausible fixed values
// so the firmware links and runs without real hardware.
// ---------------------------------------------------------------------------

// Platform initialization — call once at the start of main().
extern "C" void platform_init();

// Plain system reset — reboots straight back into the application (re-runs main,
// re-loads the burned tune). Never returns.
extern "C" void platform_reboot();

// Reboot into the chip's USB DFU bootloader. Sets a reset-surviving flag and
// triggers a system reset; the early-boot check below then jumps to system
// memory. Never returns. Lets the ST-LINK-less board be reflashed over USB DFU
// (dfu-util) without a physical BOOT0 — see platform_enter_bootloader_if_requested.
extern "C" void platform_request_bootloader();

// MUST be the very first statement in main(), before platform_init(): if the
// DFU flag is set (from a prior platform_request_bootloader()), clear it and
// jump to the system bootloader while the clocks/USB/RTOS are still uninitialised.
extern "C" void platform_enter_bootloader_if_requested();

// Independent watchdog (IWDG). init() starts a free-running, can't-be-stopped countdown on the LSI
// (survives a main-clock failure); refresh() must be called within timeout_ms or the chip resets.
// A hung firmware stops refreshing -> hardware reset -> reboot (DIS defaults HIGH -> ETB spring-closes).
// Refresh is gated on a liveness heartbeat (see the watchdog task), so a hung control frame trips it,
// not just a total lockup. Refresh immediately before any CPU-stalling op (flash erase) that outlasts
// the normal refresh cadence.
extern "C" void platform_watchdog_init(uint32_t timeout_ms);
extern "C" void platform_watchdog_refresh();

// Sensor reads — return NaN or out-of-range value on hardware fault.
extern "C" float platform_read_map_kpa();
extern "C" float platform_read_tps_pct();
extern "C" float platform_read_clt_c();
extern "C" float platform_read_iat_c();
extern "C" float platform_read_lambda();
extern "C" float platform_read_battery_v();

// Digital inputs — returns true when pin is active (before any invert logic).
// pin: 0 .. DIGITAL_INPUT_COUNT-1
extern "C" bool platform_read_din(uint8_t pin);

// The 5 V sensor references' power-good lines (board POWER_GOOD pins). true = the reference is good.
// index: 0 = 5V Sensor 1, 1 = 5V Sensor 2 (board_read_power_good's numbering). Meaningful only with the
// key on — with it off, USB back-feeds the followers through a diode.
extern "C" bool platform_read_power_good(uint8_t index);

// Analog inputs — raw ADC counts [0..ADC full scale, 4095 on a 12-bit]. The ECU is counts-native;
// the counts -> mV/V front-end conversion is the CLIENT's job (studio, from the meta board params).
// pin: 0 .. ANALOG_INPUT_COUNT-1. (There is no _mv variant — firmware never converts.)
extern "C" uint16_t platform_read_ain_raw(uint8_t pin);

// Frequency inputs — measured input frequency in Hz on a digital capture pin
// (0 if no signal / pin lacks capture). Used by frequency-type sensors (vehicle/
// turbo/trans speed, flow). pin: 0 .. (capture pin count)-1.
extern "C" uint32_t platform_read_freq(uint8_t pin);

// Frequency capture edge hook — the board's capture interrupt calls this on each
// rising edge of a frequency pin, with a free-running microsecond timestamp. The
// platform feeds a per-pin FreqCounter that platform_read_freq() then reads.
extern "C" void platform_freq_on_edge(uint8_t pin, uint32_t now_us);

// Enable EXTI edge capture on a frequency-pool pin (0 .. capture-pin-count-1) so
// platform_read_freq(pin) returns a live measurement. Idempotent; a frequency-type
// sensor calls it for its source pin on first use. Returns false if the pin can't
// be a frequency input on this board (no capture HW / out of range).
extern "C" bool platform_freq_enable(uint8_t pin);

// SENT (SAE J2716) fast-channel sensors. Same minimal-ISR shape as frequency: the
// capture interrupt only timestamps each FALLING edge via platform_sent_on_edge();
// the decode (sync calibration, nibbles, CRC) runs at read time in platform_read_sent.
extern "C" void     platform_sent_on_edge(uint8_t pin, uint32_t now_us);  // ISR: record edge
extern "C" bool     platform_sent_enable(uint8_t pin);                    // wire falling-edge EXTI
extern "C" uint32_t platform_read_sent(uint8_t pin, bool enforce_crc);    // latest 12-bit value, 0 if none

// Pulse-width / duty capture — for inputs whose HIGH-TIME carries information
// (e.g. a flex-fuel sensor: frequency = ethanol %, pulse width = fuel temp). Both
// edges are captured; `rising` is the pin level after the edge. read_pulse_us
// returns the most recent high-time in µs (frequency-independent), 0 if stopped.
extern "C" void     platform_pulse_on_edge(uint8_t pin, uint32_t now_us, bool rising);
extern "C" bool     platform_pulse_enable(uint8_t pin);                   // wire both-edge EXTI
extern "C" uint32_t platform_read_pulse_us(uint8_t pin);                  // high-time µs, 0 if none

// Tear down a DIG pin's freq/pulse/SENT capture — the symmetric half of the *_enable calls, invoked
// when the pin is no longer assigned to any capture sensor, so a leaked EXTI can't keep firing its
// handler until the next reset. Idempotent; a no-op (returns false) for a pin this layer never enabled
// (a trigger input on the same DIG pool, or an unused pin), so it can never disturb a trigger's capture.
extern "C" bool     platform_capture_disable(uint8_t pin);

// System tick in milliseconds (rolls over ~49 days on 32-bit).
extern "C" uint32_t platform_get_tick_ms();

// Mask every interrupt for a few instructions, from TASK context, and put it back as it was. For state
// an ISR also writes where a torn update is the bug (EnginePositionHal::set_firing_gate). Nests.
// Host builds have no interrupts to mask.
inline uint32_t platform_irq_save() {
#if defined(__arm__)
    uint32_t pm;
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(pm) :: "memory");
    return pm;
#else
    return 0;
#endif
}
inline void platform_irq_restore(uint32_t pm) {
#if defined(__arm__)
    __asm volatile("msr primask, %0" :: "r"(pm) : "memory");
#else
    (void)pm;
#endif
}

// FREE-RUNNING MICROSECOND COUNTER, wrapping. Same clock the capture path stamps edges with (TIM5 at
// 1 MHz on STM32), so a duration measured here is measured against the timebase everything else uses.
//
// It exists because a millisecond tick cannot bound a sub-millisecond budget. Subtracting two ms
// readings gives 1 for anything from a nanosecond to two milliseconds — so the Lua watchdog, whose
// budget defaults to 500 us, aborted a script the instant the tick happened to roll, however little
// of its budget had actually been used. Wrap-safe by unsigned subtraction, like every other user of
// this timebase.
extern "C" uint32_t platform_get_tick_us();

// Free-running CPU cycle counter for fine (sub-µs) profiling — DWT CYCCNT on Cortex-M, 0 on host.
// Wraps at 2^32 cycles (~20 s @ 216 MHz); take deltas. HAL-agnostic so portable code can profile.
extern "C" uint32_t platform_cyccnt();
// CPU frequency in Hz — what platform_cyccnt()'s cycles have to be divided by to become seconds. An
// accessor rather than the CMSIS SystemCoreClock global directly, because the modules that need it are
// compiled for the host test build too, where CMSIS does not exist.
extern "C" uint32_t platform_cpu_hz();

// The learned-data region (LTFT/LTT): a live RAM buffer that modules map their trim tables onto.
// platform_learned_base returns the whole region; *cap_out = total bytes. Durability is out-of-band —
// LearnedStore persists this buffer to rotating SD totem files and reloads it at boot (the parallel of
// a config bank), so there is no battery/BKPSRAM dependency. platform_learned_persistent() reports
// whether the buffer ITSELF survives a power cycle (false here — SD provides persistence). The studio
// reaches the SAME live bytes over the comms "learned page" (config r/w block protocol).
extern "C" void* platform_learned_base(uint32_t* cap_out);
extern "C" bool  platform_learned_persistent();   // true iff the base buffer alone survives a power cycle

// Pointer to a FIXED-offset block within the learned region (float cells overlay directly at the offset),
// or null if the block would not fit. Offsets are codegen-assigned — generated/learned_layout.h
// LEARNED_<ID>_OFFSET — so the layout is a build-time decision (the parallel of EcuConfig), NOT an
// accident of module init order.
inline uint32_t* platform_learned_block(uint32_t offset, uint32_t bytes) {
    uint32_t cap = 0;
    void* base = platform_learned_base(&cap);
    if (!base || static_cast<uint64_t>(offset) + bytes > cap) return nullptr;
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(base) + offset);
}

// Real-time clock. Time fields are plain calendar values (not BCD):
//   [0]=second 0-59  [1]=minute 0-59  [2]=hour 0-23  [3]=weekday 1-7
//   [4]=date 1-31    [5]=month 1-12   [6]=year (2000-based, i.e. 25 == 2025)
// platform_rtc_get fills 8 bytes (trailing byte reserved, set to 0).
// platform_rtc_set consumes the first 7 bytes. Returns false if the RTC is
// not running (e.g. LSE absent). Safe to call from task context.
extern "C" bool platform_rtc_get(uint8_t out[8]);
extern "C" bool platform_rtc_set(const uint8_t in[7]);

// Barometric (ambient) pressure in kPa — from onboard LPS22HBTR sensor.
extern "C" float platform_read_baro_kpa();
// Ambient temperature from the same on-board part. Cache read — see board_baro_service().
extern "C" float platform_read_baro_temp_c();
// True when that cache is fresh; false means the sampler has not had a good read recently.
extern "C" bool  platform_baro_valid();

// Status LEDs — engine modules use these rather than board_hal.h directly.
extern "C" void platform_led_connected(bool on);   // COMMS    LED (blue)
extern "C" void platform_set_led_running(bool on); // RUNNING  LED (green)
extern "C" void platform_set_led_warning(bool on); // WARNING  LED (orange)
extern "C" void platform_set_led_error(bool on);   // ERROR    LED (red)
