#include "../platform_hal.h"
#include "../board_hal.h"
#include "../../generated/boards/board.h"
#include "../../generated/learned_layout.h"   // LEARNED_REGION_USED — size of the RAM learned buffer
#include "stm32f7xx_hal.h"
#include "usb_device.h"
#include "FreeRTOS.h"
#include "task.h"

// ---------------------------------------------------------------------------
// Reboot-to-DFU (USB system bootloader). The request flag lives in DTCM, which
// the linker marks NOLOAD and the startup only zero-fills .bss — so it survives
// a soft reset. platform_request_bootloader() stamps the magic and resets;
// platform_enter_bootloader_if_requested() (first line of main(), before any
// clock/USB/RTOS init) sees it and jumps to system memory in a pristine state.
// STM32F767 system memory bootloader base = 0x1FF00000.
// ---------------------------------------------------------------------------
static volatile uint32_t __attribute__((section(".dtcm"))) s_boot_to_dfu;
static constexpr uint32_t kDfuMagic   = 0xB00710ADu;
static constexpr uint32_t kSysMemBase = 0x1FF00000u;

extern "C" void platform_reboot() {
    __DSB();
    NVIC_SystemReset();   // never returns; boots straight back into the app
    while (1) {}
}

// Independent watchdog (IWDG) — register-level (no HAL_IWDG module dependency). LSI ~32 kHz; with
// prescaler /64 each reload count is ~2 ms, so reload = timeout_ms/2 (max 4095 -> ~8.2 s). Once started
// (KR=0xCCCC) it free-runs on the LSI and cannot be stopped; KR=0xAAAA reloads it (the "kick").
extern "C" void platform_watchdog_init(uint32_t timeout_ms) {
    uint32_t reload = timeout_ms / 2u;                 // 2 ms per count (LSI/64)
    if (reload > 4095u) reload = 4095u;
    if (reload < 1u)    reload = 1u;
    // Order matters: START the IWDG first (KR=0xCCCC) — that is what turns the LSI on. Only then will
    // PR/RLR writes propagate and the SR busy flags clear. (Waiting on SR before the start would spin
    // forever, because the LSI isn't running yet.) The brief window before we reconfigure runs at the
    // default ~512 ms timeout, far longer than the µs it takes to reach the reload below.
    IWDG->KR  = 0xCCCCu;                                // start (turns on the LSI; free-running thereafter)
    IWDG->KR  = 0x5555u;                                // enable write access to PR/RLR
    IWDG->PR  = 4u;                                     // prescaler /64
    IWDG->RLR = reload;
    while (IWDG->SR != 0u) { }                          // LSI now running -> wait for PR/RLR to latch
    IWDG->KR  = 0xAAAAu;                                // load RLR (initial refresh)
}

extern "C" void platform_watchdog_refresh() {
    IWDG->KR = 0xAAAAu;
}

extern "C" void platform_request_bootloader() {
    s_boot_to_dfu = kDfuMagic;
    __DSB();
    NVIC_SystemReset();   // never returns
    while (1) {}
}

extern "C" void platform_enter_bootloader_if_requested() {
    if (s_boot_to_dfu != kDfuMagic) return;
    s_boot_to_dfu = 0u;

    // We run before platform_init(): clocks are at reset (HSI), no peripherals or
    // caches are up, and PRIMASK is clear — i.e. the same pristine state a hardware
    // BOOT0 entry sees. Do NOT mask interrupts: the system bootloader's USB DFU
    // relies on the OTG_FS interrupt to enumerate, and (like the real bootloader)
    // expects to start with global interrupts enabled. Just retarget the vector
    // table + stack at system memory and jump.
    __enable_irq();                                 // ensure PRIMASK=0 for the bootloader
    SysTick->CTRL = 0u; SysTick->LOAD = 0u; SysTick->VAL = 0u;
    const uint32_t sp = *reinterpret_cast<volatile uint32_t*>(kSysMemBase);
    const uint32_t pc = *reinterpret_cast<volatile uint32_t*>(kSysMemBase + 4u);
    SCB->VTOR = kSysMemBase;
    __DSB(); __ISB();
    __set_MSP(sp);
    reinterpret_cast<void (*)(void)>(pc)();
    while (1) {}
}

// ---------------------------------------------------------------------------
// STM32F767 System Clock Configuration (216 MHz)
// HSE: 8 MHz crystal on PH0/PH1 (Jaytek V1 board)
// PLL: PLLM=4 → 2 MHz VCO input; PLLN=216 → 432 MHz VCO; PLLP=/2 → 216 MHz
// ---------------------------------------------------------------------------

extern "C" void SystemClock_Config(void) {
    RCC_OscInitTypeDef RCC_OscInitStruct = {};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {};
    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    // THE LSE IS A BOARD CAPABILITY, not a given. PC14/PC15 are the F767's only OSC32 pins,
    // so a board that has no 32.768 kHz crystal is free to use them as GPIO — proteus_f7 puts
    // its two power-good inputs there. Enabling the LSE on such a board would hand those pins
    // to the oscillator and the power-good lines would simply stop reading. So the board schema
    // says whether a crystal exists (BOARD_LSE_HZ) and this honours it.
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
    if constexpr (BOARD_LSE_HZ != 0u) {
        // Try HSE + LSE (RTC crystal), falling back to HSE-only below if the LSE fails to start.
        RCC_OscInitStruct.OscillatorType |= RCC_OSCILLATORTYPE_LSE;
        RCC_OscInitStruct.LSEState        = RCC_LSE_ON;
    }
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM       = 4;
    RCC_OscInitStruct.PLL.PLLN       = 216;
    RCC_OscInitStruct.PLL.PLLP       = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ       = 9;   // 48 MHz for USB OTG FS
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
        RCC_OscInitStruct.LSEState       = RCC_LSE_OFF;
        if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) { while (1); }
    }

    if (HAL_PWREx_EnableOverDrive() != HAL_OK) { while (1); }

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;   // 54 MHz
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;   // 108 MHz
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK) { while (1); }

    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_CLK48;
    PeriphClkInitStruct.Clk48ClockSelection  = RCC_CLK48SOURCE_PLL;
    if constexpr (BOARD_LSE_HZ != 0u) {
        // Only ask for an RTC source on a board that has one. Selecting RCC_RTCCLKSOURCE_LSE
        // where no crystal exists leaves the RTC unclocked anyway, but it also asserts a claim
        // on the backup domain that a board using PC14/PC15 as GPIO should not make.
        PeriphClkInitStruct.PeriphClockSelection |= RCC_PERIPHCLK_RTC;
        PeriphClkInitStruct.RTCClockSelection     = RCC_RTCCLKSOURCE_LSE;
    }
    // Non-fatal — the RTC just won't tick if there is no LSE.
    HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct);
}

// ---------------------------------------------------------------------------
// Platform HAL implementations — delegate I/O to board_hal
// ---------------------------------------------------------------------------

extern "C" bool platform_read_din(uint8_t pin) {
    return board_read_digital(pin);
}

extern "C" bool platform_read_power_good(uint8_t index) {
    return board_read_power_good(index);
}

// NOTE: there is deliberately NO platform_read_ain_raw(). The ECU is ADC-counts-native end to end —
// the counts->mV/V front-end scaling lives CLIENT-SIDE (the studio, from the board params in the meta),
// so the firmware never converts. platform_read_ain_raw() (below) is the only analog read.

extern "C" uint16_t platform_read_ain_raw(uint8_t pin) {
    // Bare ADC counts — no front-end scaling. This is the single source of truth
    // the host display-adjusts (counts -> mV/V). AV and AT share the same 12-bit
    // converter, so the count is directly comparable across both pools.
    // ONE pool index: [0 .. BOARD_ANALOG_T_BASE) voltage, [BOARD_ANALOG_T_BASE ..] temperature.
    // codegen builds the studio's pin dropdowns and the raw telemetry off the same constant, so the
    // two cannot disagree about what index 11 means.
    if (pin < BOARD_ANALOG_T_BASE) {
        return static_cast<uint16_t>(board_read_adc_voltage(pin));
    }
    return static_cast<uint16_t>(
        board_read_adc_temp(static_cast<uint8_t>(pin - BOARD_ANALOG_T_BASE)));
}

// Frequency capture — a per-pin FreqCounter fed by the shared EXTI dispatch
// (Stm32Capture.cpp routes a frequency line straight here via platform_freq_on_edge,
// doing minimal work in the priority-1 ISR — just record the period). The board's
// platform_freq_enable() wires the EXTI line for a frequency sensor's source pin.
// IMPORTANT: on_edge() and hz() MUST share a clock. The capture ISR timestamps with
// TIM5->CNT (the 1 MHz / 1 us capture timebase), so the read side reads TIM5->CNT too
// — never a SysTick-derived microsecond count, which is a DIFFERENT clock and would
// make the staleness check misfire.
#include "../../Sensors/FreqCounter.h"
#include "../../Sensors/PulseCounter.h"

// One counter per frequency-capable input. The freq/pulse/SENT pool IS the DIG
// pool, so the count comes from the board schema rather than a local literal. g_pulse is declared alongside because
// platform_read_freq() prefers it on a both-edge (pulse) pin — see below.
static constexpr uint8_t FREQ_PIN_COUNT  = BOARD_DIGITAL_IN_COUNT;
static constexpr uint8_t PULSE_PIN_COUNT = BOARD_DIGITAL_IN_COUNT;
static FreqCounter  g_freq[FREQ_PIN_COUNT];
static PulseCounter g_pulse[PULSE_PIN_COUNT];

extern "C" void platform_freq_on_edge(uint8_t pin, uint32_t now_us) {
    if (pin < FREQ_PIN_COUNT) g_freq[pin].on_edge(now_us);
}

extern "C" uint32_t platform_read_freq(uint8_t pin) {
    if (pin >= FREQ_PIN_COUNT) return 0;
    // A pin under BOTH-edge pulse capture also yields the frequency from its period.
    // Prefer it, so a Frequency sensor (e.g. flex ethanol) sharing a pin with a
    // Pulse-Width sensor (flex fuel temp) reads the right value off the live counter.
    const uint32_t pulse_hz = g_pulse[pin].hz(TIM5->CNT);
    return pulse_hz ? pulse_hz : g_freq[pin].hz(TIM5->CNT);
}

// SENT (J2716) fast-channel decoders, one per SENT-capable pin (= the DIG pool).
// The capture ISR records a falling edge via platform_sent_on_edge (minimal work);
// platform_read_sent runs the decode in frame context against the same TIM5 (1 µs)
// clock the ISR timestamps with.
#include "../../Sensors/SentDecoder.h"
static constexpr uint8_t SENT_PIN_COUNT = BOARD_DIGITAL_IN_COUNT;
static SentDecoder g_sent[SENT_PIN_COUNT];

extern "C" void platform_sent_on_edge(uint8_t pin, uint32_t now_us) {
    if (pin < SENT_PIN_COUNT) g_sent[pin].on_edge(now_us);
}

extern "C" uint32_t platform_read_sent(uint8_t pin, bool enforce_crc) {
    if (pin >= SENT_PIN_COUNT) return 0;
    uint16_t v = 0;
    return g_sent[pin].value(TIM5->CNT, enforce_crc, v) ? v : 0;
}

// Pulse-width / duty capture (g_pulse declared up top). Both edges feed the counter
// via platform_pulse_on_edge (the ISR reads the pin level to tag the edge);
// read_pulse_us decodes the high-time against the same TIM5 (1 µs) clock.
extern "C" void platform_pulse_on_edge(uint8_t pin, uint32_t now_us, bool rising) {
    if (pin < PULSE_PIN_COUNT) g_pulse[pin].on_edge(now_us, rising);
}

extern "C" uint32_t platform_read_pulse_us(uint8_t pin) {
    return (pin < PULSE_PIN_COUNT) ? g_pulse[pin].high_us(TIM5->CNT) : 0;
}

// Named sensor reads are stubs — actual signal values flow through
// platform_read_ain_raw() + InputMapper + SignalBus at runtime.
extern "C" float platform_read_map_kpa()  { return 101.3f; }
extern "C" float platform_read_tps_pct()  { return 0.0f;   }
extern "C" float platform_read_clt_c()    { return 20.0f;  }
extern "C" float platform_read_iat_c()    { return 25.0f;  }
extern "C" float platform_read_lambda()   { return 1.0f;   }

extern "C" float platform_read_battery_v() {
    const uint16_t counts   = board_read_adc_voltage(BOARD_BATTERY_AIN_INDEX);
    const float    voltage_v = (static_cast<float>(counts) * BOARD_ADC_VREF_V)
                               / static_cast<float>(BOARD_ADC_FULL_SCALE);
    return voltage_v / BOARD_BATTERY_DIVIDER;
}

extern "C" float platform_read_baro_kpa() {
    return board_read_baro_kpa();
}

extern "C" float platform_read_baro_temp_c() {
    return board_read_baro_temp_c();
}

extern "C" bool platform_baro_valid() {
    return board_baro_valid();
}

extern "C" uint32_t platform_cyccnt() {
    return DWT->CYCCNT;
}

extern "C" uint32_t platform_cpu_hz() {
    return SystemCoreClock ? SystemCoreClock : 216000000u;
}

extern "C" uint32_t platform_get_tick_ms() {
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        return xTaskGetTickCount() * portTICK_PERIOD_MS;
    }
    return HAL_GetTick();
}
// The capture timebase itself: TIM5 free-runs at 1 MHz (see Stm32Capture_Init), which is what the
// edge timestamps are in. Reading the counter directly is the cheapest possible microsecond clock and
// needs no state of its own.
extern "C" uint32_t platform_get_tick_us() { return TIM5->CNT; }


// Learned-data region: a plain RAM buffer (zero-initialised → neutral on cold boot). Durability is NOT
// the buffer's job — LearnedStore persists it to rotating SD totem files and reloads it at boot (the exact
// parallel of a config bank), so there is no VBAT/BKPSRAM dependency and no 4 KB cap. The region is laid
// out at build time by codegen (generated/learned_layout.h) — each module maps its block at a FIXED offset
// via platform_learned_block(), not by allocation order — so slices stay put across firmware updates and
// the studio addresses them by the same offsets over the comms learned page.
static uint32_t s_learned_buf[(LEARNED_REGION_USED + 3u) / 4u];   // 4-byte aligned for the f32 cell overlay
extern "C" void* platform_learned_base(uint32_t* cap_out) {
    if (cap_out) *cap_out = LEARNED_REGION_USED;
    return s_learned_buf;
}
// The RAM buffer alone does NOT survive a power cycle — SD (LearnedStore) provides the persistence
// out-of-band. Callers that only want "is there a mappable region" still get a non-null base above.
extern "C" bool platform_learned_persistent() { return false; }

// The pool index for each role comes from the board schema (the STATUS_LED pin's signal NAME), not
// from its position in the list. These were 3/0/1/2 as literals, which is jaytek's declaration order.
extern "C" void platform_led_connected(bool on)    { board_set_led(BOARD_LED_COMMS,   on); }
extern "C" void platform_set_led_running(bool on)  { board_set_led(BOARD_LED_RUNNING, on); }
extern "C" void platform_set_led_warning(bool on)  { board_set_led(BOARD_LED_WARNING, on); }
extern "C" void platform_set_led_error(bool on)    { board_set_led(BOARD_LED_ERROR,   on); }

// ---------------------------------------------------------------------------
// Platform initialization
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Real-time clock (LSE-backed). The RTC clock source/mux is configured in
// SystemClock_Config; here we ungate and initialise the peripheral. Time is
// kept across resets via the backup domain, and across power-off on a board with a
// backup battery on VBAT (jaytek_v1); a default is written only on a cold
// backup-domain reset — every cold boot, on a board without one.
// ---------------------------------------------------------------------------

static RTC_HandleTypeDef s_hrtc;
static bool              s_rtc_ok = false;

static constexpr uint32_t RTC_INIT_MAGIC = 0x32F2u;   // marks "time already set"

static void platform_rtc_init(void) {
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_RTC_ENABLE();
    // The RTC's APB interface clock (APB1ENR.RTCAPBEN). It is on out of reset, so this line looks
    // redundant — but the ROM DFU bootloader clears APB1ENR before it jumps here, and without it every
    // RTC register reads 0: HAL_RTC_Init fails and the ECU reports "no clock" until the next reset.
    __HAL_RCC_RTC_CLK_ENABLE();

    s_hrtc.Instance            = RTC;
    s_hrtc.Init.HourFormat     = RTC_HOURFORMAT_24;
    s_hrtc.Init.AsynchPrediv   = 127;   // 32768 / (127+1) / (255+1) = 1 Hz
    s_hrtc.Init.SynchPrediv    = 255;
    s_hrtc.Init.OutPut         = RTC_OUTPUT_DISABLE;
    s_hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    s_hrtc.Init.OutPutType     = RTC_OUTPUT_TYPE_OPENDRAIN;
    if (HAL_RTC_Init(&s_hrtc) != HAL_OK) { s_rtc_ok = false; return; }
    s_rtc_ok = true;

    if (HAL_RTCEx_BKUPRead(&s_hrtc, RTC_BKP_DR0) != RTC_INIT_MAGIC) {
        // Sane default so reads return in-range values before any set.
        RTC_TimeTypeDef t = {};
        RTC_DateTypeDef d = {};
        t.Hours = 0; t.Minutes = 0; t.Seconds = 0;
        t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
        t.StoreOperation = RTC_STOREOPERATION_RESET;
        d.WeekDay = RTC_WEEKDAY_WEDNESDAY; d.Date = 1; d.Month = 1; d.Year = 25;  // 2025-01-01
        HAL_RTC_SetTime(&s_hrtc, &t, RTC_FORMAT_BIN);
        HAL_RTC_SetDate(&s_hrtc, &d, RTC_FORMAT_BIN);
        HAL_RTCEx_BKUPWrite(&s_hrtc, RTC_BKP_DR0, RTC_INIT_MAGIC);
    }
}

extern "C" bool platform_rtc_get(uint8_t out[8]) {
    if (!s_rtc_ok) return false;
    RTC_TimeTypeDef t; RTC_DateTypeDef d;
    if (HAL_RTC_GetTime(&s_hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) return false;
    // GetDate is mandatory after GetTime — it unlocks the shadow registers.
    if (HAL_RTC_GetDate(&s_hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) return false;
    out[0] = t.Seconds; out[1] = t.Minutes; out[2] = t.Hours;
    out[3] = d.WeekDay; out[4] = d.Date;    out[5] = d.Month; out[6] = d.Year;
    out[7] = 0;
    return true;
}

extern "C" bool platform_rtc_set(const uint8_t in[7]) {
    if (!s_rtc_ok) return false;
    RTC_TimeTypeDef t = {}; RTC_DateTypeDef d = {};
    t.Seconds = in[0]; t.Minutes = in[1]; t.Hours = in[2];
    t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    t.StoreOperation = RTC_STOREOPERATION_RESET;
    d.WeekDay = in[3] ? in[3] : RTC_WEEKDAY_MONDAY;
    d.Date = in[4]; d.Month = in[5]; d.Year = in[6];
    if (HAL_RTC_SetTime(&s_hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) return false;
    if (HAL_RTC_SetDate(&s_hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) return false;
    return true;
}

// Strong override of the weak get_fattime() in diskio.c — gives FatFS real
// timestamps for created/modified files when the RTC is running.
extern "C" uint32_t get_fattime(void) {
    uint8_t r[8];
    if (!platform_rtc_get(r)) return 0;
    uint32_t year = 2000u + r[6];
    if (year < 1980u) year = 1980u;
    return ((year - 1980u) << 25) | ((uint32_t)r[5] << 21) | ((uint32_t)r[4] << 16)
         | ((uint32_t)r[2] << 11) | ((uint32_t)r[1] << 5)  | ((uint32_t)(r[0] >> 1));
}

// (The learned region is now a zero-init RAM buffer, persisted to SD by LearnedStore. The old BKPSRAM
// flash-invalidate guard is gone: staleness is caught by the totem header's layout_hash at load time —
// a totem written under a different firmware layout is simply rejected and the region stays neutral.)

// MPU: mark the linker .dma_nocache region (holds every DMA buffer) as Normal, NON-cacheable + shareable,
// so DMA masters and the CPU see coherent memory once the D-cache is on. Everything else uses the default
// memory map (cacheable) via PRIVDEFENA. This is what lets us enable the D-cache without per-transfer
// clean/invalidate on the ADC rings / knock capture. The region base is 8K-aligned and the size below MUST
// match the linker's `. = _dma_nocache_start + 8K` reservation.
static void mpu_config_dma_nocache() {
    extern uint32_t _dma_nocache_start;
    MPU_Region_InitTypeDef r = {};
    HAL_MPU_Disable();
    r.Enable           = MPU_REGION_ENABLE;
    r.Number           = MPU_REGION_NUMBER0;
    r.BaseAddress      = reinterpret_cast<uint32_t>(&_dma_nocache_start);
    r.Size             = MPU_REGION_SIZE_8KB;
    r.SubRegionDisable = 0x00;
    r.TypeExtField     = MPU_TEX_LEVEL1;                     // TEX=001 + C=0,B=0,S=1 -> Normal, non-cacheable
    r.AccessPermission = MPU_REGION_FULL_ACCESS;
    r.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;     // data only
    r.IsShareable      = MPU_ACCESS_SHAREABLE;
    r.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
    r.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&r);
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);                 // background = default map (cacheable)
}

// WHY THE LAST RESET HAPPENED. Captured before anything else can disturb it, because the flags are
// sticky across resets and are the only record there is — nothing else in this firmware records a
// restart at all, so an ECU that came back up was previously indistinguishable from one that never went
// down, and the cause was pure guesswork. RCC_CSR distinguishes power-on, brown-out, the NRST pin, a
// software reset, and either watchdog. Read once, then cleared so the NEXT reset reports only itself.
static uint32_t s_reset_flags = 0;

// A stack overflow's victim, carried ACROSS the reset. .dtcm is NOLOAD so it is never zeroed at boot —
// the same property that lets s_boot_to_dfu survive — which is exactly what a post-mortem needs: the
// fault kills the board, and the only place to leave a note is memory the next boot will not clear.
// Guarded by a magic so an uninitialised region is not mistaken for a report.
static constexpr uint32_t SO_MAGIC = 0x50565246u;   // 'SOVR'
// NOTE the shape: the section attribute must sit on the VARIABLE, not on an anonymous struct type.
// Written as `static struct {...} __attribute__((section(".dtcm"))) s_stack_overflow;` GCC applies it
// to the TYPE, warns "section attribute does not apply to types", and drops the variable in .bss —
// which IS zeroed at boot, so the record this exists to carry across a reset was being wiped before
// anything could read it. The feature silently never worked.
struct StackOverflowRec { uint32_t magic; char task[16]; };
static StackOverflowRec __attribute__((section(".dtcm"))) s_stack_overflow;

extern "C" const char* platform_stack_overflow_task() {
    return (s_stack_overflow.magic == SO_MAGIC) ? s_stack_overflow.task : nullptr;
}
extern "C" void platform_clear_stack_overflow() { s_stack_overflow.magic = 0; }

// FreeRTOS calls this from the context switch that detected the overrun. Record which task, then reset
// deliberately rather than limping on with a corrupted stack.
extern "C" void vApplicationStackOverflowHook(void* /*xTask*/, char* pcTaskName) {
    s_stack_overflow.magic = SO_MAGIC;
    for (int i = 0; i < 15; i++) {
        s_stack_overflow.task[i] = pcTaskName ? pcTaskName[i] : '\0';
        if (s_stack_overflow.task[i] == '\0') break;
    }
    s_stack_overflow.task[15] = '\0';
    NVIC_SystemReset();
}
extern "C" uint32_t platform_reset_flags() { return s_reset_flags; }

extern "C" void platform_init() {
    s_reset_flags = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;                 // clear, so the next boot reports its own cause only
    SCB->VTOR = 0x08000000;
    HAL_Init();
    SystemClock_Config();

    // Reclaim flash-fetch stalls. The M7 fetches code from flash at FLASH_LATENCY_7 (7 wait states @ 216
    // MHz); with no accelerator every branch/call stalls on the fetch — measured ~13 µs for a table lookup
    // that is ~150 cycles of real work (~18x). Enable the flash ART accelerator (a flash-line I-cache) +
    // prefetch, and BOTH the Cortex-M7 instruction and data caches. Code is not self-modifying (I-cache
    // needs no maintenance); the D-cache is made DMA-safe by an MPU non-cacheable region (below) holding
    // every DMA buffer, so no per-transfer clean/invalidate is needed.
    FLASH->ACR |= FLASH_ACR_PRFTEN | FLASH_ACR_ARTEN;   // prefetch + ART accelerator
    SCB_EnableICache();                                 // CMSIS: invalidates then enables the I-cache
    mpu_config_dma_nocache();                           // DMA buffers -> non-cacheable, BEFORE D-cache on
    SCB_EnableDCache();                                 // CMSIS: invalidates then enables the D-cache

    // THE CYCLE COUNTER platform_cyccnt() reads. It is part of the platform, not a profiler's: the
    // electronic throttle takes its PID timestep from it (a whole-millisecond tick carries up to 100 %
    // timestep error at 1 kHz), and a counter that was never started reads 0 — which the throttle treats
    // as "no counter" and silently falls back to that coarse tick.
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    *reinterpret_cast<volatile uint32_t*>(0xE0001FB0u) = 0xC5ACCE55u;   // DWT->LAR unlock (Cortex-M7)
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    platform_rtc_init();    // LSE-backed RTC (clock source set in SystemClock_Config)
    board_init();           // GPIO, ADC1, ADC3, I2C1, SPI3 pin modes for Jaytek V1
    platform_usb_init();
}

// ---------------------------------------------------------------------------
// SysTick handler — drives both HAL time base (uwTick) and FreeRTOS tick.
// Pre-scheduler, only HAL_IncTick runs so HAL_GetTick / HAL_Delay work.
// Post-scheduler-start, xPortSysTickHandler advances the FreeRTOS tick.
// ---------------------------------------------------------------------------

extern "C" void xPortSysTickHandler(void);

extern "C" void SysTick_Handler(void) {
    HAL_IncTick();
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();
    }
}

// ---------------------------------------------------------------------------
// FreeRTOS static allocation callbacks
// ---------------------------------------------------------------------------

extern "C" void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                               StackType_t  **ppxIdleTaskStackBuffer,
                                               uint32_t      *pulIdleTaskStackSize) {
    static StaticTask_t xIdleTaskTCB;
    static StackType_t  uxIdleTaskStack[configMINIMAL_STACK_SIZE];
    *ppxIdleTaskTCBBuffer   = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *pulIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}
