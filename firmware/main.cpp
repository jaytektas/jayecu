#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <cstring>

#include "Scheduler/EnginePositionHal.h"
#include "Scheduler/VirtualTrigger.h"
#include "Scheduler/SegmentTimer.h"   // misfire segment timing (real-tooth tap)
#include "Platform/AssignmentResolver.h"
#include "Platform/board_profile.h"
#include "Stm32Capture.h"
#include "Stm32OutputCompare.h"
#include "Platform/stm32f7xx/Stm32SoftTimer.h"
#include "Scheduler/SoftPwm.h"
#include "Engine/EngineTask.h"
#include "Diagnostics/CpuStats.h"
#include "Engine/SystemComposer.h"
#include "Engine/Modules/KnockDetector.h"   // knock burst DSP (worker task)
#include "Platform/board_hal.h"             // board_knock_capture / board_knock_sample_rate
#include "Comms/CommsManager.h"
#include "Comms/UsbTransport.h"
#include "Cli/CliRegistry.h"
#include "Platform/platform_hal.h"
#include "Platform/platform_can.h"
#include "Can/CanBroker.h"
#include "Led/LedTask.h"
#include "Led/LedStatus.h"
#include "Storage/SdDtcStore.h"
#include "Storage/SdArbitrator.h"
#include "Storage/StorageManager.h"
#include "Storage/ConfigBank.h"
#include "Platform/stm32f7xx/SdConfigStore.h"
#include "Platform/stm32f7xx/LearnedStore.h"
#include "Storage/Crc32.h"
#include "Platform/stm32f7xx/Stm32FlashBank.h"
#include "Platform/stm32f7xx/SdCardSpi.h"
#include "Platform/stm32f7xx/platform_config.h"
#include "Platform/stm32f7xx/usbd_cdc_if.h"
#include "Platform/stm32f7xx/usb_msc.h"
#include "Scheduler/Log.h"
#include "../generated/ecu_config.h"
// CAN TX broadcast is pending rearchitecture (config-selected decoder/broadcaster modules +
// broker-as-router). The old schema-driven generic-frame generator + serializers were removed
// (they targeted the pre-rework per-module telemetry struct). OBD-II RX below stays live.
#include "../generated/schema_meta.h"
#include "../generated/shadow_meta.h"   // JayecuShadowModule bits for the safe-boundary apply

// ---------------------------------------------------------------------------
// Board configuration — edit for each hardware variant.
// ---------------------------------------------------------------------------

static constexpr uint8_t BOARD_CYLINDER_COUNT  = 12;  // bench worst-case (12-cyl); displacement is a tune field now

// ---------------------------------------------------------------------------
// Global: engine running flag — read by usbd_msc_storage.cpp to block writes
// ---------------------------------------------------------------------------

bool g_engine_running = false;

// System-active gate = debounced key-on (battery > threshold, owned by EngineTask). False on
// USB/bench power: only comms/SD/LED + the bootstrap battery sensor run; firing and per-cycle
// fuel/spark commit are suppressed so a bench trigger (ardustim) can't drive injectors/coils.
// The battery sensor SOURCES key_on, so it runs regardless of this flag (see Sensors::update).
bool g_system_active = false;

// THE FREERTOS HEAP, PLACED IN DTCM (configAPPLICATION_ALLOCATED_HEAP — see FreeRTOSConfig.h). Its
// only customer is the Lua interpreter: nothing in this firmware creates a task, queue or semaphore
// dynamically. DTCM cannot be reached by DMA and Lua never asks it to be, so the 25 KB it took out of
// the 384 K region — which was down to 1.2 KB of headroom — is better spent on the tune image.
// Block form, deliberately: `extern "C" uint8_t ucHeap[…];` with no initialiser is a DECLARATION in
// C++, not a definition — the symbol never gets emitted and the link fails on the heap's own use of it.
extern "C" { __attribute__((section(".dtcm"))) uint8_t ucHeap[configTOTAL_HEAP_SIZE]; }
// NO VALID TUNE. Set at boot when no flash bank (or SD record) passes the CRC / size / layout gate.
// The hardware cannot be configured without a tune — every pin assignment, cylinder count and
// channel mapping lives in it — so the ECU comes up DISABLED: the scheduler is never started and no
// module claims a pin, which leaves the whole output pool Hi-Z (the arbiter's default). Comms DOES
// run, so the studio can connect, push a tune and burn it; a reset then boots normally.
//
// Zeros are not "off": an unassigned channel is 255 in a real tune, so a zeroed config would claim
// coil 0 and injector 0. That is why this skips the bring-up rather than gating it afterwards.
bool g_no_tune = false;

// Set by the 'reconfig' text command (CLI/'E'); the config-save task applies a forced
// EnginePositionHal reconfigure on the next tick, bypassing the engine-stopped gate. Lets a
// host (e.g. the bench harness) apply a freshly-written tune LIVE without a flash burn + MCU
// reset — the decoder/scheduler/angle-clock fully re-init from g_config. A plain bool write
// from the comms task; the actual reconfigure runs in the save task's safe context.
volatile bool g_force_reconfig = false;

// Bench override for the SD-ownership key (the 'key' text command). 0 = auto (battery
// voltage, the normal path); +1 = force key ON (SD stays ECU-owned); -1 = force key OFF
// (SD hands to USB MSC so the host can mount the card). Lets a host trigger the SD→USB
// handoff on demand to inspect dtc.bin / fault_log.bin without touching the bench PSU.
volatile int g_key_override = 0;

// Bench override for the two 5 V sensor references' power-good lines (the 'pg' text command), per
// reference: 0 = the real pin, +1 = force good, -1 = force FAULT. Simulates a follower failing — every
// pin sensor invalid, P0641/P0651 after 100 ms — without overloading one. Read in Sensors::update_supplies.
volatile int g_pg_override[2] = {0, 0};

static void reresolve_capture_assignment();   // defined below; used by config_save_task

// ---------------------------------------------------------------------------
// Globals — static storage, no heap.
// ---------------------------------------------------------------------------

// SD card arbitrator — must be named g_sd_arb for SdArbitratorShim.cpp
SdArbitrator g_sd_arb;

// The bank base MUST be the start of the sector erase() clears, and must match
// the FLASH_CFG_* regions reserved in STM32F767XX_FLASH.ld. A mismatch erases
// one region while programming/reading another (verify fails, neighbouring
// bank corrupted). Single-bank STM32F767: sector 10 = 0x08180000, 11 = 0x081C0000.
static_assert(FLASH_BANK_A_BASE == 0x08180000UL, "Bank A base must be flash sector 10 start");
static_assert(FLASH_BANK_B_BASE == 0x081C0000UL, "Bank B base must be flash sector 11 start");
static_assert(FLASH_BANK_A_BASE + FLASH_BANK_A_SIZE == FLASH_BANK_B_BASE,
              "Config banks A and B must be contiguous");

static Stm32FlashBank  g_flash_a_raw(FLASH_BANK_A_SECTOR, FLASH_BANK_A_BASE, FLASH_BANK_A_SIZE);
static Stm32FlashBank  g_flash_b_raw(FLASH_BANK_B_SECTOR, FLASH_BANK_B_BASE, FLASH_BANK_B_SIZE);
static ConfigBank      g_bank_a(g_flash_a_raw);
static ConfigBank      g_bank_b(g_flash_b_raw);
static SdConfigStore   g_sd_config(g_sd_arb);
static StorageManager  g_storage_manager(g_bank_a, g_bank_b, &g_sd_config, g_sd_arb);
static SdDtcStore      g_sd_dtc_store(g_sd_arb);
static LearnedStore    g_learned_store(g_sd_arb);   // LTFT/LTT RAM region <-> rotating SD totems

// Learned-region persistence bookkeeping (config_save_task): CRC of the region as last written to SD, and
// the tick of the last flush. A change in the CRC over ~30 s rolls a new totem (see config_save_task).
static uint32_t s_learned_crc      = 0;
static uint32_t s_learned_flush_ms = 0;

// Bench hooks for the 'learn' CLI. The learned region only drifts when the engine is actually learning
// (closed-loop lambda, engine running), so on a bench the TOTEM MECHANISM — write, rotate, header+CRC
// validate, reload on boot — cannot be reached at all. These poke a known pattern in, force a flush, and
// read it back, so the persistence can be proven without an engine. They touch the region, never the
// learning.
namespace bench {
uint32_t learned_read(uint32_t off) {
    uint32_t cap = 0; auto* p = static_cast<uint8_t*>(platform_learned_base(&cap));
    return (off < cap) ? p[off] : 0xFFFFFFFFu;
}
void learned_poke2(uint32_t off, uint32_t val) {
    uint32_t cap = 0; auto* p = static_cast<uint8_t*>(platform_learned_base(&cap));
    if (off < cap) p[off] = static_cast<uint8_t>(val);
}
void learned_poke(uint32_t off, uint32_t val) {
    uint32_t cap = 0; auto* p = static_cast<uint8_t*>(platform_learned_base(&cap));
    if (off < cap) p[off] = static_cast<uint8_t>(val);
}
// Returns 0 on success, else a reason: 1 = the ECU does not own the card, 2 = the store refused the
// write. "save failed" on its own sends you looking at the totem writer when the real answer is usually
// that a host MSC session still holds the card.
uint32_t learned_save() {
    uint32_t cap = 0; auto* p = static_cast<uint8_t*>(platform_learned_base(&cap));
    g_sd_arb.writer_busy(SD_WRITER_LEARNED);
    const bool owned = g_sd_arb.ecu_has_card() && g_sd_arb.writes_allowed();
    const bool ok = owned && g_learned_store.save(p, cap);
    g_sd_arb.writer_idle(SD_WRITER_LEARNED);
    if (ok) s_learned_crc = crc32_buf(p, cap);   // keep the dirty baseline honest
    return ok ? 0u : (owned ? 2u : 1u);
}
uint32_t learned_cap() { uint32_t cap = 0; (void)platform_learned_base(&cap); return cap; }
uint32_t learned_seq() { return g_learned_store.next_sequence(); }
}  // namespace bench

static EcuHardwareAssignment g_assignment {};
static PinArbiter            g_pins;   // output-pin ownership, one entry per output row

// Software-PWM engine + its dedicated TIM4 tick source (NVIC prio 3, below the
// scheduler/decoder). Drives every PWM output row.
static constexpr uint32_t    SOFTPWM_TPS = 100000u;   // 100 kHz / 10 us ticks
static Stm32SoftTimer        g_soft_timer;
static SoftPwm               g_softpwm;

// Engine-position scheduler. Reads its config DIRECTLY from the RAM tune — one
// source, no copy: g_config.trigger (wheel geometry + cylinder count) and
// g_config.cylinders (firing modes / polarity / per-cylinder map). Boot loads the
// factory flash tune (the schema defaults) into g_config; a TS write re-binds live
// via reconfigure(). The bench programs its own config straight into g_config RAM
// over the serial 'w' command — the firmware carries no bench-specific defaults.
static EnginePositionHal g_epos_hal(g_assignment, g_config.trigger, g_config.engine, g_config.outputs);

// ---- VirtualTrigger — the "software eTPU" angle clock (the firing time base) ----
// The decoder feeds real teeth into the PLL; the DCO emits a uniform virtual-tooth
// grid (CRANK 360°/36, widened to 720°/72 at PHASE) that the scheduler mounts on
// and fires off. This IS the firing clock, not a parallel observer.
static VirtualTrigger g_vtrig(board_dco_channel(), ANGLE_360, 36);

static void vtrig_dco_match(void* /*ud*/) noexcept { g_vtrig.on_dco_match(); }
// Each virtual tooth drives the scheduler on the uniform grid (the mount).
static void vtrig_vtooth(const VirtualToothData& vt, void* /*ud*/) noexcept {
    g_epos_hal.on_grid_tooth(vt.index, vt.angle, vt.tick, vt.ticks_per_vtooth, g_vtrig.vtooth_angle());
    // Wake the spark/fuel compute (the "per-cycle" task). NVIC_SetPendingIRQ is a bare register write,
    // legal from this ISR even though it's above the FreeRTOS syscall ceiling; SPDIF_RX_IRQHandler
    // does the notify.
    //
    // EVERY FIRING INTERVAL, NOT EVERY CYCLE. It woke only when the grid index wrapped to 0 — once an
    // engine cycle — so spark advance, dwell and pulse width were a cycle old by the time the last
    // cylinder used them: 150 ms at 800 rpm, half a second while cranking, which is where the step
    // from cranking to running values lives. Now it wakes at each cylinder's interval (cycle/ncyl),
    // but never more often than every WAKE_MIN_US, so at high rpm it falls back towards once a cycle
    // and the heavy fuel maths stays off the CPU. Index 0 always wakes, as before.
    {
        constexpr uint32_t WAKE_MIN_US = 8000u;   // <= 125 wakes a second
        static uint32_t s_last_wake = 0;
        const uint8_t    ncyl  = g_config.engine.cylinder_count ? g_config.engine.cylinder_count : 1;
        const AngleDeg10 step  = g_vtrig.vtooth_angle();
        const int32_t    cyc   = engine_cycle_angle(g_config.engine.cycle_type);
        const uint32_t   stride = (step > 0) ? std::max<uint32_t>(1u, static_cast<uint32_t>(cyc / ncyl / step)) : 0u;
        const uint32_t   hz    = g_epos_hal.timebase_hz();
        const uint32_t   min_ticks = hz ? static_cast<uint32_t>((uint64_t)hz * WAKE_MIN_US / 1000000u) : 0u;
        const bool on_interval = stride && (vt.index % stride) == 0;
        if (vt.index == 0 || (on_interval && (vt.tick - s_last_wake) >= min_ticks)) {
            s_last_wake = vt.tick;
            NVIC_SetPendingIRQ(SPDIF_RX_IRQn);
        }
    }
}

// weak hook called from EnginePositionHal::tooth_cb_trampoline — feed the PLL.
void vtrig_feed(AngleDeg10 angle, uint32_t ticks, uint32_t T0,
                AngleDeg10 tooth_angle, bool last_was_gap) noexcept {
    g_vtrig.on_real_tooth(angle, ticks, T0, tooth_angle, last_was_gap);
}

// weak hook called from EnginePositionHal::handle_sync_level_change — widen the
// grid to 720°/72 at PHASE (so it emits a full-cycle index and the scheduler can
// tell the two revolutions apart) and narrow back to 360°/36 at CRANK/NONE.
void vtrig_reconfigure(AngleDeg10 cycle_angle, uint16_t teeth) noexcept {
    g_vtrig.reconfigure(cycle_angle, teeth);
}

static Comms::CommsManager g_comms;
// Bench hook: the CLI's 'datarec' needs the very record the datalogger writes, without knowing where
// the manager lives (see CommsManager.h).
namespace Comms { const EcuTelemetry& comms_packed_frame() { return g_comms.packed_frame(); }
                  CommsManager&        comms_manager()      { return g_comms; } }
// Bench hook: the CLI's 'burn' flags the same persist request the wire 'b' command does.
namespace Comms { void comms_request_save() { g_comms.request_save(); } }
static Comms::UsbTransport g_usb_transport;
static CanBroker           g_can_broker;

static EngineTask    g_engine_task(g_epos_hal, g_can_broker);
// The engine frame's own load, for the CLI — which must not pull EngineTask's header (and FreeRTOS,
// and the whole engine) into its translation unit to ask one number.
extern "C" uint8_t g_engine_frame_load_pct()     { return g_engine_task.frame_load_pct(); }
extern "C" uint8_t g_engine_frame_load_max_pct() { return g_engine_task.frame_load_max_pct(); }
extern "C" void    g_engine_frame_load_reset()    { g_engine_task.frame_load_reset(); }
extern "C" uint32_t g_engine_frame_max_us()      { return g_engine_task.frame_max_us(); }
// The per-module frame costs, flattened for the CLI (which must not see EngineTask's header).
extern "C" uint8_t g_engine_frame_costs(const char** names, uint32_t* us, uint16_t* period,
                                        uint8_t* per_cycle, uint8_t max) {
    EngineTask::FrameCost rows[24];
    const uint8_t n = g_engine_task.frame_costs(rows, max < 24 ? max : 24);
    for (uint8_t i = 0; i < n; i++) {
        names[i] = rows[i].name; us[i] = rows[i].us;
        period[i] = rows[i].period; per_cycle[i] = rows[i].per_cycle ? 1u : 0u;
    }
    return n;
}
static SystemComposer g_composer;   // owns + wires the concrete managers into the scheduler

// Per-cycle wake relay. The DCO/vtooth ISR (above the syscall ceiling) pends SPDIF_RX — a spare
// vector, unused on an ECU — at each cycle boundary; this handler runs BELOW the ceiling, so it can
// do the FromISR notify that wakes the per-cycle compute task. Crank-clocked, replacing the old
// 1 kHz frame poll. Priority is set FromISR-legal (>= configMAX_SYSCALL) in main().
extern "C" void SPDIF_RX_IRQHandler(void) {
    BaseType_t woken = pdFALSE;
    TaskHandle_t h = g_engine_task.cycle_task_handle();
    if (h) vTaskNotifyGiveFromISR(h, &woken);
    portYIELD_FROM_ISR(woken);
}

// ---------------------------------------------------------------------------
// Knock burst worker (A1 stage C). The per-cylinder knock window (compute hook, alarm ISR) deposits
// the fired cylinder into g_epos_hal's mailbox and calls knock_worker_pend(), which pends the SAI1
// relay (a spare vector); SAI1_IRQHandler (priority >= configMAX_SYSCALL) does the FromISR notify.
// This worker then, per fired cylinder, captures an RPM-scaled ADC burst on the cylinder's banked
// knock sensor and runs the (bench-validated) DSP -> knock_1/2 dB -> Knock::post_measurement(cyl, db):
// the ring the engine-module task drains, never the classifier itself (see the POST note below).
//
// NB: arming BORROWS ADC3 from the AV13-16 scan, and this worker is what gives it back — the
// board_knock_end_burst() below runs the moment the samples are ours (the DMA has finished writing
// them) and before the DSP, which costs the four analog inputs the length of a burst rather than the
// whole time knock is enabled. Every exit path from the wait has to make that call. The burst itself
// is non-blocking (board_knock_start_burst + a DMA-complete ISR the worker sleeps on); the mailbox is
// bounded and drops when full, so at high RPM the worker samples a subset of windows rather than
// falling behind.
//
// BUFFER SIZE IS A WINDOW LIMIT, not just an allocation. The sample count is window_duration_deg
// scaled to the current RPM, so a fixed buffer caps the window in TIME and therefore truncates it in
// ANGLE at low RPM — the opposite end from where you would expect to run out. At the 281.25 kHz
// sample rate, 3072 samples is 10.9 ms: the default 40 deg window down to ~610 rpm, and a window opened
// early for pre-ignition (10 deg before a 30 deg spark, closing 50 deg ATDC: 90 deg) down to ~1370 rpm.
// It was 2048 until the look-ahead made windows longer — 7.28 ms, 90 deg only above ~2060 rpm, and
// low-speed pre-ignition lives below that. (512 once reached only ~3660 rpm, so a 40 deg window at idle
// was silently sampling ~11 deg.) s_knock_truncs counts every clamp so the ceiling is visible on the
// bench instead of being mistaken for a quiet engine.
static KnockDetector g_knock_detector;
static uint16_t __attribute__((section(".dma_nocache"))) s_knock_buf[3072];
static uint16_t      s_knock_cfg_freq = 0;      // last DSP band-center configured (worker-owned)
static volatile uint32_t s_knock_captures = 0;  // worker capture count (health diagnostic, 'fire' CLI)
static volatile uint32_t s_knock_truncs   = 0;  // windows clamped to the buffer (RPM below the floor)
// The latest burst's phase-resolved profile. Worker-owned and reused per burst rather than built on
// the stack: KnockProfile is ~150 bytes and this task's stack is 512 words.
static KnockProfile  s_knock_profile;
static StackType_t   s_knock_stack[512];
static StaticTask_t  s_knock_tcb;
static TaskHandle_t  s_knock_task = nullptr;
static StaticSemaphore_t s_knock_sem_buf;
static SemaphoreHandle_t s_knock_burst_sem = nullptr;   // given by the DMA-complete ISR; the worker sleeps on it

// Weak hook target (declared in EnginePositionHal.cpp): pend the worker from the alarm ISR. A bare
// NVIC register write, legal above the FreeRTOS syscall ceiling (as with vtrig_vtooth -> SPDIF_RX).
void knock_worker_pend() noexcept { NVIC_SetPendingIRQ(SAI1_IRQn); }

// Knock burst-complete callback (runs in the DMA2_Stream1 ISR, NVIC prio 8 -> FromISR legal): wake the
// worker, which then runs the DSP. This is what replaces the old blocking poll-spin — the worker sleeps.
static void knock_burst_complete(void) {
    BaseType_t woken = pdFALSE;
    if (s_knock_burst_sem) xSemaphoreGiveFromISR(s_knock_burst_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

extern "C" void SAI1_IRQHandler(void) {
    BaseType_t woken = pdFALSE;
    if (s_knock_task) vTaskNotifyGiveFromISR(s_knock_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void knock_task(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint8_t cyl;
        while (g_epos_hal.pop_knock_cyl(cyl)) {
            const KnockConfig& kc = g_config.knock;
            if (!kc.enabled || kc.source != 0) continue;                   // onboard only
            // Which physical input hears this cylinder. AUTO (the default) derives it from the bank
            // the cylinder already declares under Engine, so the common case needs no second
            // declaration; an explicit value covers the engines where sensors and banks do not line
            // up (an inline six with two sensors, a V with one).
            uint8_t sensor = (cyl < KNOCK_CYL_SENSOR_COUNT) ? kc.cyl_sensor[cyl].input : 0;
            if (sensor > 1) {
                const uint8_t bank = (cyl < MAX_CYLINDERS) ? g_config.engine.cyl[cyl].bank : 1;
                sensor = static_cast<uint8_t>(bank >= 2 ? 1 : 0);
            }

            // (Re)configure the DSP band on a frequency change — worker-owned, no race with process().
            // 0 = FROM THE BORE, as the setting says. This was a fixed 7 kHz whatever the engine, with no bore
            // setting anywhere — so a large bore (~5 kHz) or small one (~9 kHz) was partly deaf to knock while
            // looking configured. Draper: f(kHz) = 900 / (pi * radius_mm). No bore entered: the generic 7 kHz.
            const float bore_mm = g_config.engine.bore_mm * 0.1f;
            const uint16_t freq = kc.knock_frequency ? kc.knock_frequency
                                : (bore_mm > 10.0f ? static_cast<uint16_t>(900000.0f / (3.14159265f * bore_mm * 0.5f))
                                                   : 7000);
            if (freq != s_knock_cfg_freq) {
                g_knock_detector.configure(board_knock_sample_rate(), static_cast<float>(freq));
                s_knock_cfg_freq = freq;
            }

            // THE WINDOW RUNS FROM WHERE IT OPENED TO WHERE IT CLOSES. It opens at Window Start, or earlier
            // by the pre-ignition look-ahead before this cylinder's spark (the scheduler says which), and
            // it always closes at Window Start - Window Duration: the knock part is the same either way,
            // and the look-ahead only adds the stretch before it.
            const float open_btdc  = static_cast<float>(g_epos_hal.knock_window_open_btdc(cyl)) * 0.1f;
            const float close_btdc = static_cast<float>(kc.window_start_btdc) - static_cast<float>(kc.window_duration_deg);
            const float span_deg   = open_btdc - close_btdc;
            // Sample count = that span's worth of time at the current RPM,
            // clamped to the buffer. oneDegreeUs = 60e6 / (360*rpm) = 1.6667e6 / rpm_x10.
            const uint32_t rpm_x10 = g_epos_hal.get_rpm_x10();
            const uint16_t cap = static_cast<uint16_t>(sizeof(s_knock_buf) / sizeof(s_knock_buf[0]));
            uint16_t count = cap;
            if (rpm_x10 > 0 && span_deg > 0.0f) {
                const float one_deg_us = 1666666.7f / static_cast<float>(rpm_x10);
                float c = span_deg * one_deg_us
                          * board_knock_sample_rate() / 1000000.0f;
                if (c < 100.0f) c = 100.0f;
                // Clamped = the window we sampled is SHORTER than the one configured. Counted, because
                // nothing else distinguishes "40 deg of quiet" from "11 deg of it, and we stopped".
                if (c > static_cast<float>(cap)) { c = static_cast<float>(cap); s_knock_truncs++; }
                count = static_cast<uint16_t>(c);
            }

            // Non-blocking arm: the DMA fills s_knock_buf autonomously and the worker SLEEPS on the
            // burst semaphore (given by knock_burst_complete from the DMA ISR) — no CPU poll-spin.
            if (!board_knock_start_burst(sensor, s_knock_buf, count)) continue;
            s_knock_captures++;
            // Timeout guard — it must OUTLAST the burst it is guarding. A full 3072-sample burst is
            // 10.9 ms at 281.25 kHz (the old 10 ms guard was already too tight at 2048: a timeout here
            // silently drops the measurement, which reads as a quiet cylinder). 25 ms still catches a
            // DMA that never completes, just later.
            const bool burst_ok = xSemaphoreTake(s_knock_burst_sem, pdMS_TO_TICKS(25)) == pdTRUE;
            // Give ADC3 back FIRST, on both paths. The samples are already in s_knock_buf, so the DSP
            // below does not need the ADC held — and on the timeout path holding it would strand
            // AV13-16 on a burst that is never coming.
            board_knock_end_burst();
            if (!burst_ok) continue;
            const uint16_t n = count;   // the burst filled the whole buffer
            // The window's geometry, which only this loop knows: where it opened (the same angle the
            // scheduler armed; the profile carries it ATDC-positive, so it is negated) and how fast the
            // crank was turning when it did. rpm_x10 of 0 leaves deg_per_sec at 0, and the profile goes
            // out unstamped rather than carrying a confident wrong angle.
            KnockDetector::Window win;
            win.start_deg   = -open_btdc;
            win.deg_per_sec = (rpm_x10 > 0) ? static_cast<float>(rpm_x10) * 0.6f : 0.0f;  // rpm*360/60
            const float db = g_knock_detector.on_burst(sensor, s_knock_buf, n, g_engine_task.bus(),
                                                       s_knock_profile, win);
            // POST, do not classify. This is the knock worker task; the verdict, the retard and the
            // learned noise map all belong to the engine-module task, which drains this ring in
            // Knock::update(). Calling on_knock_sense() from here — as this did — mutated controller
            // state from two tasks at once.
            g_composer.knock().post_measurement(cyl, db, s_knock_profile);
        }
    }
}

// ---------------------------------------------------------------------------
// CAN task
// ---------------------------------------------------------------------------

static StaticTask_t s_can_tcb;
static StackType_t  s_can_stack[256];

// The tune's per-bus CAN settings -> the platform edge. main is the only thing that reads g_config
// (platform_can and CanBroker never do), so the translation lives here, next to the other composition.
// The bitrate enum is an INDEX, not a rate: 0=125k 1=250k 2=500k 3=1M.
static void apply_can_bus_config() {
    static const uint32_t kRates[] = { 125000u, 250000u, 500000u, 1000000u };
    for (uint8_t i = 0; i < 2; ++i) {
        const auto& b = g_config.can.bus[i];
        const uint8_t sel = (b.bitrate < 4) ? b.bitrate : 2;      // out of range -> 500k (OBD default)
        platform_can_apply(i, b.enabled != 0, kRates[sel], b.listen_only != 0);
        // The broker needs the rate to turn a bit count into a LOAD PERCENTAGE. It never reads
        // g_config itself, so the translation stays here with the rest of the composition.
        g_can_broker.set_bus_bitrate(i, kRates[sel]);
        g_can_broker.set_bus_listen_only(i, b.listen_only != 0);
    }
}

static void can_task(void* /*pv*/) {
    extern volatile uint32_t g_config_generation;
    uint32_t cfg_seen = g_config_generation;
    // ONLY WHEN THE CAN SECTION CHANGED. The generation moves on EVERY config write — a VE cell, a
    // lambda gain — and each one rebuilt the bus: transmit deadlines reset, the held "hold last value"
    // bytes dropped, a repacked field briefly reading another field's value as fresh. A CRC of the CAN
    // block says whether this write was about CAN at all.
    uint32_t can_crc_seen = crc32_buf(reinterpret_cast<const uint8_t*>(&g_config.can), sizeof(g_config.can));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1));
        // A tune edit that moves OBD to the other bus (or switches it off) takes effect NOW. The
        // responder's RX subscriptions are registered per bus, so without this the change sat in the
        // config doing nothing until the next reset — the tune said one thing and the ECU did another.
        // Self-watching the config generation is how the other live-reconfigurable modules do it.
        if (g_config_generation != cfg_seen) {
            cfg_seen = g_config_generation;
            const uint32_t can_crc = crc32_buf(reinterpret_cast<const uint8_t*>(&g_config.can), sizeof(g_config.can));
            if (can_crc == can_crc_seen) { g_can_broker.update(platform_get_tick_ms()); continue; }
            can_crc_seen = can_crc;
            apply_can_bus_config();          // bitrate / listen-only / enable, per bus
            g_can_broker.reconfigure_obd(g_config.can.obd_enabled != 0, g_config.can.obd_bus);
            // Same contract for the tune's own frames: adding a frame, changing a rate or switching
            // generic CAN on for a bus takes effect now, not at the next reset.
            g_can_broker.reconfigure_generic(g_config.can, platform_get_tick_ms());
        }
        g_can_broker.update(platform_get_tick_ms());
    }
}

// ---------------------------------------------------------------------------
// Config-save + fault-flush task
// Runs every 500ms: persists config burns and flushes new fault log entries to SD.
// ---------------------------------------------------------------------------

static StaticTask_t s_save_tcb;
static StackType_t  s_save_stack[512];

// ---------------------------------------------------------------------------
// Board service — the ONLY owner of the on-board I2C bus.
//
// It exists so the engine frame never blocks on a bus. The barometric/ambient part was being read
// synchronously from inside the 1 kHz frame behind a 100 ms cache, which made 99 calls free and the
// 100th cost ~505 us of a 1000 us budget. Sampling belongs somewhere with no timing obligations.
//
// Not folded into the Save task: that ticks at 500 ms, which would both starve a 10 Hz sensor and
// couple its cadence to config-burn timing. Not the LED task either — 1 KB of stack and the job of
// holding the error light is no place for a blocking HAL call. One task, one bus, findable by name.
// ---------------------------------------------------------------------------
static StaticTask_t s_board_tcb;
static StackType_t  s_board_stack[384];   // room for the HAL I2C call chain, with margin

static void board_service_task(void* /*pv*/) {
    uint16_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(50));
        board_baro_service();             // self-rate-limits to its own 100 ms period
        // ONCE A SECOND, take the CPU window. Here rather than in the engine frame because the
        // snapshot suspends the scheduler while it walks the task lists, which is the last thing the
        // 1 kHz frame should be doing — and because a second is the window the number describes.
        if (++tick >= 20) { tick = 0; cpustats::sample(); }
    }
}

static void config_save_task(void* /*pv*/) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));

        // Persist config if the host requested a burn. Engine stopped (USB /
        // bench) → straight to flash, glitch-free. Engine running → SD if
        // present (glitch-free), else flash now and accept the stall; there is
        // no safe deferral since the engine stopping means the key is off and
        // ECU power is about to drop.
        if (g_comms.save_requested()) {
            // Persist the live config. Integrity is the storage ConfigHeader CRC
            // computed by the burn over the whole record — the config has no
            // internal CRC field to maintain.
            // Mark the card busy so a key-off release waits for the burn to
            // finish (the SD path inside burn runs only when the ECU owns it).
            g_sd_arb.writer_busy(SD_WRITER_CONFIG);
            // Snapshot the generation BEFORE the burn: burn() takes the g_config bytes as they are now,
            // so that is the generation being persisted. A 'w' landing mid-burn moves the counter past
            // this and correctly leaves the tune dirty afterwards.
            extern volatile uint32_t g_config_generation;
            extern volatile uint32_t g_config_generation_saved;
            const uint32_t burning_gen = g_config_generation;
            const BurnResult br = g_storage_manager.burn(
                reinterpret_cast<const uint8_t*>(&g_config),
                sizeof(g_config),
                g_engine_running,
                platform_get_tick_ms());
            g_sd_arb.writer_idle(SD_WRITER_CONFIG);
            g_comms.clear_save_request();
            if (br != BurnResult::ERR) g_config_generation_saved = burning_gen;   // only a WRITE clears dirty
            EFI_LOG_DEBUG("burn", "save_task: burn returned result=%d", (int)br);
        }

        // Apply pending STRUCTURAL reconfig at a safe boundary. A 'w' to a Trigger or
        // Engine (count/firing) field set that module's bit; the new bytes are live in
        // g_config RAM but the decoder/scheduler read shadows. They can only rebuild
        // engine-stopped — changing wheel teeth / cylinder count on a spinning engine
        // is catastrophic — so gate on !g_engine_running (rpm <= 50). reconfigure()
        // re-pulls every module's shadow; clear only the bits we serviced.
        {
            const uint32_t pos_bits = (1u << JAYECU_SHADOW_TRIGGER) |
                                      (1u << JAYECU_SHADOW_ENGINE);
            const bool forced = g_force_reconfig;
            // Auto: a structural 'w' applies only when engine-stopped (safe). Forced: the
            // 'reconfig' command bypasses the gate — used to apply a live tune to a decoder
            // that's still limping on the previous wheel (it never drops below the rpm gate).
            // A changed OUTPUT MAP is the same kind of change: a coil or injector row edited — function,
            // cylinder, stage, plug, polarity — binds only at a stopped reconfigure. The Outputs shadow
            // bit cannot say so (OutputManager consumes it for the generic rows, and most Outputs writes
            // touch no firing row at all), so the answer is asked of the rows themselves.
            const bool map_changed = g_epos_hal.output_map_pending();
            if (forced || (((g_comms.shadow_pending_mask() & pos_bits) || map_changed) && !g_engine_running)) {
                reresolve_capture_assignment();   // re-bind crank/cam pins + edges from the live tune
                g_epos_hal.reconfigure();
                g_comms.clear_shadow(pos_bits);
                g_force_reconfig = false;
                EFI_LOG_DEBUG("reconfig", forced ? "applied FORCED reconfigure"
                                         : map_changed ? "applied output-map reconfigure (engine stopped)"
                                                       : "applied trigger/engine reconfigure (engine stopped)");
            }
        }

        // Knock sampling window: track the live Knock config each pass (cheap). Enabled only for the
        // onboard source. Window Start is degrees BTDC, + advanced / - retarded like spark advance, and
        // SIGNED: the default -10 opens 10 deg after TDC. With pre-ignition detection on, the look-ahead
        // opens each window that far before its own spark instead (EnginePositionHal::knock_open_btdc).
        g_epos_hal.set_knock_window(
            g_config.knock.enabled != 0 && g_config.knock.source == 0,
            static_cast<AngleDeg10>(g_config.knock.window_start_btdc * 10),
            static_cast<AngleDeg10>(g_config.knock.preign_enabled ? g_config.knock.preign_lookahead_deg * 10 : 0));

        // Persist the DTC table to SD when it changed (raise/heal/clear/clear_all +
        // freeze-frame writes set the dirty flag; the save takes it). The whole
        // table — codes, counts, timestamps AND per-code freeze-frames — lives in this
        // one image; there is no separate fault-log file. Busy-bracketed so a key-off
        // release waits for an in-flight write to finish and close.
        {
            g_sd_arb.writer_busy(SD_WRITER_FAULTLOG);
            g_sd_dtc_store.save_if_dirty(g_engine_task.dtc());
            g_sd_arb.writer_idle(SD_WRITER_FAULTLOG);
        }

        // Persist the learned region (LTFT/LTT) to a new SD totem when it has drifted. The region is a live
        // RAM buffer the engine learns into; on a slow cadence (or immediately when the engine has just
        // stopped) we CRC it and, if it changed since the last write, roll a fresh totem — LearnedStore keeps
        // a few and the newest valid wins on boot. SD is over SPI, so this is glitch-free while running; the
        // worst-case loss on an abrupt power-cut is one flush interval of learning. No battery/BKPSRAM.
        {
            constexpr uint32_t LEARN_FLUSH_MS = 30000u;   // ~30 s dirty-detect cadence
            static bool s_prev_running = false;
            const uint32_t now = platform_get_tick_ms();
            const bool just_stopped = s_prev_running && !g_engine_running;   // grab a final snapshot at key-off
            s_prev_running = g_engine_running;

            if (just_stopped || (now - s_learned_flush_ms) >= LEARN_FLUSH_MS) {
                s_learned_flush_ms = now;
                uint32_t cap = 0;
                auto* lregion = static_cast<uint8_t*>(platform_learned_base(&cap));
                const uint32_t crc = crc32_buf(lregion, cap);
                if (crc != s_learned_crc) {
                    g_sd_arb.writer_busy(SD_WRITER_LEARNED);
                    const bool ok = g_learned_store.save(lregion, cap);
                    g_sd_arb.writer_idle(SD_WRITER_LEARNED);
                    if (ok) s_learned_crc = crc;   // only advance the baseline on a fully-written totem
                    EFI_LOG_DEBUG("learn", "totem flush crc=%08lx ok=%d%s",
                                  (unsigned long)crc, (int)ok, just_stopped ? " (key-off)" : "");
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Comms poll task
// ---------------------------------------------------------------------------

static StaticTask_t s_comms_tcb;
static StackType_t  s_comms_stack[512];

static void comms_task(void* /*pv*/) {
    uint32_t last_tick = xTaskGetTickCount();
    while (true) {
        // No fixed poll delay: update() blocks inside the RX read (up to ~5 ms) when the line is idle,
        // yielding the CPU, and returns the instant the RX ISR delivers a packet — event-driven, not
        // a 10 ms poll. A burst is drained back-to-back; the preemptive scheduler still lets the engine
        // tasks (higher priority) run.
        const uint32_t now   = xTaskGetTickCount();
        const uint32_t delta = (now - last_tick) * portTICK_PERIOD_MS;
        last_tick = now;
        g_led_status.usb_connected = UsbCdc_IsConnected();
        g_comms.update(delta);
    }
}

// ---------------------------------------------------------------------------
// Datalog sampler — THE CLOCK the SD log is sampled on.
//
// It used to be the comms task, which is not a clock: it blocks up to ~5 ms in the RX read and
// returns the instant a packet lands, so the sample interval was a description of what the laptop
// was doing (~200 Hz, jittery, faster while the studio streamed). This task does one thing on a
// fixed period — pack a frame, gather a record, push it into the ring — and touches neither the
// filesystem nor SPI, so nothing it does can block. The comms task drains the ring to the card
// whenever it next runs, and a card stall lands in the buffer instead of in the timebase.
//
// PRIORITY 2: below the 1 kHz engine frame (3), so it can never delay spark or fuel, and above the
// comms task (1) that consumes from it. It also sits below the SD mutex's caller ceiling, which is
// enforced — and it never takes that mutex, which is the design saying so.
//
// vTaskDelayUntil, not vTaskDelay: the period is measured from the last WAKE, so the sample interval
// does not drift by however long the work took. The tick is 1 kHz, so 1 ms is the floor.
// ---------------------------------------------------------------------------

#include "Comms/SdProtocol.h"     // the sampler's half of the datalogger (sample_now / period)

// In DTCM with the ring it feeds: RAM is the scarce region on this part and this task exists to
// keep a buffer full, not to be reached by DMA (which cannot address DTCM at all).
__attribute__((section(".dtcm"))) static StaticTask_t s_logsmp_tcb;
__attribute__((section(".dtcm"))) static StackType_t  s_logsmp_stack[512];

static void log_sample_task(void* /*pv*/) {
    TickType_t last = xTaskGetTickCount();
    while (true) {
        const uint32_t period = Comms::sd::sample_period_ms();
        if (period == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));      // not logging: cost nothing, wake to notice
            last = xTaskGetTickCount();
            continue;
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(period));
        Comms::sd::sample_now(Comms::comms_manager(), platform_get_tick_ms());
    }
}

// ---------------------------------------------------------------------------
// LED task — 10ms tick, drives all four status LEDs
// ---------------------------------------------------------------------------

static StaticTask_t s_led_tcb;
static StackType_t  s_led_stack[256];

// THE BATTERY WITH NO TUNE. The battery input is board hardware — a fixed divider on a fixed pin, not a
// user sensor — yet its scaling lives in the tune, so an ECU that rejected its tune read 0 V at any supply
// and sat at KEY OFF with the real problem hidden behind it. With no tune the engine task never starts, so
// nothing else publishes; this reads the pin with the board's own divider (the same line codegen derives
// the default calibration from: full scale = vref / divider) and decides the key the way Sensors does.
// Enough for the studio to show the supply and the key while it says why nothing else is running.
static void no_tune_battery_tick() {
    static bool key = false;
    SignalBus& bus = g_engine_task.bus();
    const uint32_t now = platform_get_tick_ms();
    const float vbat = static_cast<float>(platform_read_ain_raw(BOARD_BATTERY_AIN_INDEX))
                     * (BOARD_ADC_VREF_V / BOARD_BATTERY_DIVIDER) / static_cast<float>(BOARD_ADC_FULL_SCALE);
    key = key ? (vbat > Sensors::KEY_OFF_V) : (vbat > Sensors::KEY_ON_V);
    bus.set(wk::battery, vbat, true, now, 500u);
    bus.set(SIG_KEY_ON, key ? 1.0f : 0.0f, true, now, 500u);
}

static void led_task(void* /*pv*/) {
    LedTask led;
    extern bool g_no_tune;
    uint32_t tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));
        // No tune: hold the red ERROR LED solid rather than flashing DTC codes. There is no engine
        // running to have faults, and a steady light says "this board is disabled" unambiguously —
        // a flashing one invites reading a P-code off it.
        if (g_no_tune) {
            platform_set_led_error(true);
            if (++tick % 10u == 0u) no_tune_battery_tick();   // 10 Hz: the supply and the key, nothing else
            continue;
        }
        led.tick(g_led_status);
    }
}

// ---------------------------------------------------------------------------
// Lua script task — runs user scripts OFF the engine task, at the lowest
// non-idle priority (below every base producer). This is the crash-isolation
// boundary: a runaway or panicking script can starve only this thread, never
// the engine. It keeps its "last word" on the bus by PRIORITY (signalWrite at
// PRIO_LUA), not by running last. Wakes every 1 ms to drain CAN RX promptly and
// run onTick (itself decimated by the script's setTickRate). Lua needs a larger
// stack than the housekeeping tasks (pcall + C trampolines).
// ---------------------------------------------------------------------------

static StaticTask_t s_script_tcb;
// …and the Lua task's stack, for the same reason: a task stack is CPU-only by definition.
static StackType_t __attribute__((section(".dtcm"))) s_script_stack[2048];

static void script_task(void* /*pv*/) {
    ScriptEngine& eng = g_composer.script_engine();
    SignalBus&    bus = g_engine_task.bus();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1));
        eng.tick(bus);
    }
}

// ---------------------------------------------------------------------------
// Watchdog task — refreshes the IWDG, GATED on the 1 kHz control frame's heartbeat.
// Runs at prio 2: above the housekeeping tasks (1) so a flash-burn stall in the Save
// task can't starve it, below the control frame (3). It pets the IWDG only while
// frame_wakes() keeps advancing — so a hung OR spinning control frame (ElectronicThrottle
// lives there) stops the pets and the chip hard-resets in <= WDG_TIMEOUT_MS. On reboot
// the H-bridge DIS defaults HIGH, so the ETB spring-returns to limp. This is the lockup
// backstop; live faults are handled faster by the closed-loop A/B + runaway latch + DIS.
// ---------------------------------------------------------------------------

static constexpr uint32_t WDG_TIMEOUT_MS = 4000;   // must exceed the worst-case 256 KB flash erase

static StaticTask_t s_wdg_tcb;
static StackType_t  s_wdg_stack[128];

static void watchdog_task(void* /*pv*/) {
    extern bool g_no_tune;
    uint32_t last_frame = g_engine_task.frame_wakes();
    platform_watchdog_refresh();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        // DISABLED (no valid tune): the engine task was never started, so there is no control frame
        // to watch and its counter can never advance. Watching it anyway withholds the pet forever
        // and the IWDG reboots the board in a loop — which also kills the USB link, so the one
        // recovery route (connect, push a tune, burn) is gone too. The watchdog exists to catch a
        // STALLED frame, not a deliberately absent one; here this task's own liveness is the thing
        // being proven, so pet unconditionally.
        if (g_no_tune) { platform_watchdog_refresh(); continue; }
        const uint32_t f = g_engine_task.frame_wakes();
        if (f != last_frame) {                  // control frame is alive -> pet
            last_frame = f;
            platform_watchdog_refresh();
        }
        // else: frame stalled -> WITHHOLD the pet -> IWDG resets the chip.
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

// Apply an EXTI edge to a (possibly null) capture channel. The composition root knows the
// concrete Stm32 capture type. Used at boot and on every live trigger reconfigure.
static void apply_capture_edge(ICaptureChannel* ch, CaptureEdge e) {
    if (ch) static_cast<Stm32CaptureChannel*>(ch)->configure(e);
}

// Re-resolve the capture (crank/cam) pin + edge bindings from the LIVE trigger config and
// re-apply the EXTI edges, preserving the fields the composition root sets AFTER the boot
// resolve (pin arbiter + domain alarms; the resolver memsets everything else, which is
// unused on this board). Must run before g_epos_hal.reconfigure() so a live tune change
// actually re-binds the inputs — otherwise the decoder rebuilds against the boot pins/edges
// (the crank only "worked" by always landing on the same pool index; the cam did not).
static void reresolve_capture_assignment() {
    PinArbiter*  arb = g_assignment.pin_arbiter;
    IAlarmTimer* aa  = g_assignment.angle_alarm;
    IAlarmTimer* ta  = g_assignment.time_alarm;
    IAlarmTimer* tt  = g_assignment.tooth_alarm;
    descriptor_to_assignment(board_profile(), g_config.trigger, g_assignment);
    g_assignment.pin_arbiter = arb;
    g_assignment.angle_alarm = aa;
    g_assignment.time_alarm  = ta;
    g_assignment.tooth_alarm = tt;   // the resolver memsets it; losing it would silently disable
                                     // liveness detection on the first live tune change
    apply_capture_edge(g_assignment.crank_primary,   g_assignment.crank_primary_edge);
    apply_capture_edge(g_assignment.crank_secondary, g_assignment.crank_secondary_edge);
    for (int i = 0; i < MAX_CAM_CHANNELS; ++i)
        apply_capture_edge(g_assignment.cam[i], g_assignment.cam_edge[i]);
}

// 'reconfig' text command (reachable via the comms link's 'E'/execute): queue a forced
// EnginePositionHal reconfigure so a freshly-written tune applies LIVE — no burn, no reset.
static void cmd_reconfig(const Cli::Argv&, Cli::Out& o) {
    g_force_reconfig = true;
    o.put("reconfig queued\r\n");
}

// 'key on|off|auto' text command (via the comms link's 'E'/execute): override the SD
// ownership key so a host can force the SD→USB handoff and mount dtc.bin/fault_log.bin.
static void cmd_key(const Cli::Argv& a, Cli::Out& o) {
    const char* s = a.s[0];
    if      (s && !strcmp(s, "on"))   { g_key_override =  1; o.put("key forced ON (SD = ECU)\r\n"); }
    else if (s && !strcmp(s, "off"))  { g_key_override = -1; o.put("key forced OFF (SD -> USB MSC)\r\n"); }
    else if (s && !strcmp(s, "auto")) { g_key_override =  0; o.put("key = auto (battery)\r\n"); }
    else                                o.put("usage: key on|off|auto\r\n");
}
// 'pg 1|2 ok|fault|auto' text command: force a 5 V sensor reference's power-good line (see g_pg_override).
static void cmd_pg(const Cli::Argv& a, Cli::Out& o) {
    const char* n = a.s[0]; const char* s = a.s[1];
    const int k = (n && !strcmp(n, "1")) ? 0 : (n && !strcmp(n, "2")) ? 1 : -1;
    int v = 2;
    if (s && !strcmp(s, "ok"))    v =  1;
    if (s && !strcmp(s, "fault")) v = -1;
    if (s && !strcmp(s, "auto"))  v =  0;
    if (k < 0 || v == 2) { o.put("usage: pg 1|2 ok|fault|auto\r\n"); return; }
    g_pg_override[k] = v;
    o.put("5V sensor "); o.put(k ? "2" : "1");
    o.put(v > 0 ? " power-good forced OK\r\n" : v < 0 ? " power-good forced FAULT\r\n" : " power-good = auto (pin)\r\n");
}
// 'faults' diagnostic: DTC table active/stored counts + worst severity/code.
static void cmd_faults(const Cli::Argv&, Cli::Out& o) {
    o.put("active=");    o.print_u32(g_engine_task.dtc().active_count());
    o.put(" stored=");   o.print_u32(g_engine_task.dtc().stored_count());
    o.put(" worst_sev="); o.print_u32(g_engine_task.dtc().worst_severity());
    o.put(" ecu_card="); o.put(g_sd_arb.ecu_has_card() ? "1" : "0");
    o.put("\r\n");
}
// 'fire' diagnostic: firing-gate / sync-epoch arm interlock state.
static void cmd_fire(const Cli::Argv&, Cli::Out& o) {
    o.put("firing_enabled="); o.print_u32(g_epos_hal.firing_enabled() ? 1 : 0);
    o.put(" gate=");          o.print_u32(g_epos_hal.firing_gate() ? 1 : 0);
    o.put(" sync_level=");    o.print_u32(static_cast<uint32_t>(g_epos_hal.get_sync_level()));
    o.put(" sync_epoch=");    o.print_u32(g_epos_hal.firing_epoch());
    o.put(" armed_epoch=");   o.print_u32(g_epos_hal.armed_epoch());
    const bool epoch_match = g_epos_hal.firing_epoch() == g_epos_hal.armed_epoch();
    o.put(epoch_match ? " epoch:match" : " epoch:STALE");
    o.put(g_epos_hal.firing_enabled() ? " -> FIRING" : " -> blocked");
    o.put(" knock_windows="); o.print_u32(g_epos_hal.knock_window_fires());
    o.put(" knock_cap=");     o.print_u32(s_knock_captures);
    o.put(" knock_trunc=");   o.print_u32(s_knock_truncs);   // >0 = window clamped by the buffer (low RPM)
    o.put("\r\n");
}
// 'wdgtest' diagnostic: deliberately hang the CPU with interrupts masked (no task, no ISR runs) to
// prove the IWDG recovers a total lockup. The chip should hard-reset + re-enumerate within
// WDG_TIMEOUT_MS. Bench-only; never returns (the reply won't flush — expect the USB link to drop).
static void cmd_wdgtest(const Cli::Argv&, Cli::Out& o) {
    o.put("hanging CPU (IRQs off) — IWDG should reset in ~4s\r\n");
    __disable_irq();
    for (;;) { }   // nothing pets the IWDG now -> hardware reset
}
// 'kinj' — inject a synthetic knock measurement at a CHOSEN CRANK ANGLE, bypassing the ADC entirely.
//
// The analog front end is already bench-proven (an 8 kHz tone into KNOCK1 peaks the band exactly).
// What has never run on hardware is everything ABOVE it: the worker mailbox, the threshold/max-retard
// table lookups, the learned noise region, the pre-ignition conjunction, the DTC raise, and an
// injector channel actually going dark. None of that is reachable from a signal generator whose burst
// is manual-triggered, because placing energy at a chosen angle is exactly what it cannot do.
//
// So this hands the classifier a profile it could not otherwise be given. It proves the PATH, not the
// calibration: the dB levels here are ones you typed, so nothing about a real sensor's response is
// tested. Use it to establish that a cylinder cuts when it should, then trust a real signal to say
// what the numbers ought to be.
//
//   kinj <cyl> <dB x10> <angle deg ATDC>
//     e.g.  kinj 0 0 30     — 0.0 dB at 30 deg ATDC  -> after the spark  -> knock, retard
//           kinj 0 0 -30    — 0.0 dB at 30 deg BTDC  -> before the spark -> pre-ignition, cut
static void cmd_kinj(const Cli::Argv& a, Cli::Out& o) {
    const int32_t cyl = a.i[0];
    const float   db  = static_cast<float>(a.i[1]) * 0.1f;
    const float   ang = static_cast<float>(a.i[2]);
    if (cyl < 0 || cyl >= static_cast<int32_t>(KNOCK_CYL_SENSOR_COUNT)) { o.put("kinj: cyl out of range\r\n"); return; }

    // A narrow spike of energy at `ang`, everything else at the silent floor. Deliberately narrow: the
    // point is an unambiguous phase, not a realistic combustion signature.
    KnockProfile p{};
    p.count      = 16;
    p.start_deg  = -60.0f;
    p.step_deg   = 10.0f;             // 16 buckets spanning -60 .. +100 deg
    p.overall_db = db;
    for (unsigned i = 0; i < p.count; ++i) {
        const float c = p.angleOf(i);
        p.db[i] = (ang >= c - p.step_deg * 0.5f && ang < c + p.step_deg * 0.5f) ? db : KnockProfile::SILENT_DB;
    }
    const bool ok = g_composer.knock().post_measurement(static_cast<uint8_t>(cyl), db, p);
    o.put(ok ? "kinj queued" : "kinj DROPPED (ring full)");
    o.put(" cyl=");   o.print_u32(static_cast<uint32_t>(cyl));
    o.put(" ang=");   o.print_i32(static_cast<int32_t>(ang));
    o.put(" floor="); o.print_i32(static_cast<int32_t>(g_composer.knock().noise_floor(static_cast<uint8_t>(cyl)) * 10.0f));
    o.put(" n=");     o.print_i32(static_cast<int32_t>(g_composer.knock().noise_samples(static_cast<uint8_t>(cyl))));
    o.put("\r\n");
}

// 'knk' — the classifier's live state, which is what you watch while injecting or sweeping a tone.
static void cmd_knk(const Cli::Argv&, Cli::Out& o) {
    const Knock& k = g_composer.knock();
    o.put("retard_x10="); o.print_i32(static_cast<int32_t>(k.current_retard() * 10.0f));
    o.put(" knocks=");    o.print_u32(k.knock_count());
    o.put(" intensity_x10="); o.print_i32(static_cast<int32_t>(k.last_intensity() * 10.0f));
    o.put(" pre_frac_pct=");  o.print_i32(static_cast<int32_t>(k.last_pre_frac() * 100.0f));
    o.put(" preign_cuts=");   o.print_u32(k.preign_cuts());
    o.put(" dropped=");       o.print_u32(k.dropped());
    o.put("\r\n  floors_x10:");
    for (uint8_t c = 0; c < g_config.engine.cylinder_count && c < Knock::MAX_CYL; ++c) {
        o.put(" ");  o.print_i32(static_cast<int32_t>(k.noise_floor(c) * 10.0f));
        o.put("/");  o.print_i32(static_cast<int32_t>(k.noise_samples(c)));
        o.put("/");  o.print_u32(k.preign_events(c));
    }
    o.put("   (dB x10 / samples / preign events)\r\n");

    // THE LAST SHOT — the profile with everything needed to read it. A bucket row alone is
    // unreadable: it only means something against the floor it was measured from, the spark it was
    // placed relative to, and the threshold it had to beat.
    const Knock::Shot& sh = k.last_shot();
    if (sh.seq == 0) { o.put("  last: (nothing classified yet)\r\n"); return; }
    static const char* kVerdict[] = { "clean", "bootstrap", "KNOCK", "PRE-IGNITION" };
    o.put("  last#");     o.print_u32(sh.seq);
    o.put(" cyl=");       o.print_u32(sh.cyl);
    o.put(" ");           o.put(kVerdict[sh.verdict & 3]);
    o.put(" db_x10=");    o.print_i32(static_cast<int32_t>(sh.db * 10.0f));
    o.put(" floor_x10="); o.print_i32(static_cast<int32_t>(sh.floor_db * 10.0f));
    o.put(" over_x10=");  o.print_i32(static_cast<int32_t>(sh.intensity * 10.0f));
    o.put(" thr_x10=");   o.print_i32(static_cast<int32_t>(sh.threshold * 10.0f));
    o.put(" spark=");     o.print_i32(static_cast<int32_t>(sh.spark_deg));
    o.put(" pre_pct=");   o.print_i32(static_cast<int32_t>(sh.pre_frac * 100.0f));
    o.put("\r\n  profile ");
    if (!sh.profile.hasPhase()) {
        o.put("(no phase stamp)");
    } else {
        // BTDC, + advanced / - retarded, like spark= above (the profile itself carries ATDC-positive).
        o.put("btdc["); o.print_i32(static_cast<int32_t>(-sh.profile.start_deg));
        o.put("..");    o.print_i32(static_cast<int32_t>(-(sh.profile.start_deg +
                                      sh.profile.step_deg * static_cast<float>(sh.profile.count))));
        o.put("] step_x10="); o.print_i32(static_cast<int32_t>(sh.profile.step_deg * 10.0f));
    }
    o.put(" dBx10:");
    for (unsigned i = 0; i < sh.profile.count; ++i) {
        o.put(" "); o.print_i32(static_cast<int32_t>(sh.profile.db[i] * 10.0f));
    }
    o.put("\r\n");

    // THE WINDOW HAS TO REACH THE SPARK, or pre-ignition detection is switched on and blind. With the
    // look-ahead it opens before every spark by itself; this still catches a look-ahead of 0, and a
    // spark so advanced that the window hit its earliest-possible limit (85 deg BTDC).
    if (g_config.knock.preign_enabled && sh.profile.hasPhase() &&
        sh.profile.start_deg > -sh.spark_deg) {
        o.put("  WARNING: pre-ignition is ENABLED but the window opens at ");
        o.print_i32(static_cast<int32_t>(-sh.profile.start_deg));
        o.put(" deg BTDC, AFTER the spark at ");
        o.print_i32(static_cast<int32_t>(sh.spark_deg));
        o.put(" deg BTDC. Nothing before the spark is sampled -> it can never fire.\r\n"
              "           Set knock.preign_lookahead_deg above 0 (e.g. 10).\r\n");
    }
}

static const Cli::Command kBenchCmds[] = {
    { "kinj", Cli::Args::III, cmd_kinj,
      "kinj <cyl> <dBx10> <angleATDC> -> inject a knock measurement at a chosen crank angle (no ADC)" },
    { "knk", Cli::Args::NONE, cmd_knk,
      "knk -> knock/pre-ignition state: retard, intensity, pre-spark fraction, cuts, learned floors" },
    { "wdgtest", Cli::Args::NONE, cmd_wdgtest,
      "wdgtest -> hang the CPU (IRQs off); IWDG must hard-reset the chip (~4s)" },
    { "fire", Cli::Args::NONE, cmd_fire,
      "fire -> firing-enable + sync-epoch arm interlock state" },
    { "reconfig", Cli::Args::NONE, cmd_reconfig,
      "reconfig -> apply g_config to the position subsystem now (forced; bypasses the engine-stopped gate)" },
    { "key", Cli::Args::S, cmd_key,
      "key on|off|auto -> override the SD-ownership key (bench USB-MSC handoff); auto = battery voltage" },
    { "pg", Cli::Args::SS, cmd_pg,
      "pg 1|2 ok|fault|auto -> force a 5 V sensor reference's power-good (simulate a failed follower); auto = the pin" },
    { "faults", Cli::Args::NONE, cmd_faults,
      "faults -> DTC table active/stored counts + RAM fault-log entry count" },
};

int main() {
    // If a prior 'dfu' command requested it, jump to the USB DFU bootloader now —
    // BEFORE clocks/USB/RTOS come up, while the chip is in a clean post-reset state.
    platform_enter_bootloader_if_requested();

    // Initialise clocks, board GPIO/ADC/I2C, USB peripheral.
    platform_init();

    // SD card init — attempt to bring up the SD card over SPI3.
    // If unavailable, ECU runs from flash config.
    const bool sd_ok = SdCard_Init();
    // Boot: the SdArbitrator defaults to ECU ownership so config sync can read
    // the card here, pre-kernel. Once the engine task runs, service() takes over
    // and ownership becomes purely key-driven (battery > threshold).
    if (sd_ok && g_sd_arb.ecu_has_card()) {
        g_sd_config.mount();
    }

    // Boot arbitration: load config from highest-sequence source.
    {
        // No staging buffer. Flash is memory-mapped, so boot_arbitrate CRCs the winning bank where
        // it lies and hands back a pointer into it; we check size + layout_hash through that pointer
        // and then do ONE copy into g_config. The old code reserved a whole EcuConfig in DTCM to
        // stage a candidate it had already validated.
        // Arbitration gates on layout_hash AND size, then picks the highest sequence among what
        // qualifies — so a stale-layout tune with a higher sequence loses to a valid one instead of
        // winning and being thrown out here.
        const uint8_t* payload = nullptr;
        uint32_t len = 0;
        // g_config IS the scratch. An SD record cannot be staged anywhere else — it is 141 KB and
        // there is no spare RAM — so the streaming path reads it straight into its destination and
        // hands the same pointer back. The flash paths are unchanged: they still return a pointer
        // into memory-mapped flash and the memcpy below is the one copy.
        // DISABLE THE ENGINE BEFORE A BURN THAT WILL STALL THE CPU. Programming internal flash on this
        // single-bank part blocks all instruction fetch for ~1-2 s, and every coil and injector edge is
        // delivered from an ISR — so a pin freezes wherever it was. Begin mid-dwell and the coil stays
        // energised for a thousand times its design dwell; begin with an injector open and it stays
        // open. Parking first costs the engine, which is going to stop through a 1-2 s blackout
        // regardless: a stopped engine is the safe state, a frozen half-scheduled one is not.
        //
        // The gate closes first so nothing arms another event (it also drops sync, so the decoder
        // re-acquires cleanly afterwards), then the scheduler forces every claimed output to its
        // polarity-correct idle. Neither needs undoing: EngineTask re-drives the firing gate from
        // g_system_active every 1 kHz frame, and firing returns once sync does.
        g_storage_manager.set_prestall_hook([](void*) {
            g_epos_hal.set_firing_gate(false);
            g_epos_hal.scheduler().quiesce_volatiles();
        }, nullptr);

        const BootResult boot = g_storage_manager.boot_arbitrate(
            JAYECU_LAYOUT_HASH, sizeof(g_config), &payload, &len,
            reinterpret_cast<uint8_t*>(&g_config), sizeof(g_config));
        // Accept a stored tune ONLY if it matches this firmware's layout exactly:
        // right size AND right layout_hash. The size check alone is not enough — a
        // schema change that reorders/repurposes fields without changing the total
        // byte count produces a same-size but misaligned tune, which would memcpy in
        // verbatim and silently corrupt whatever moved (observed: a same-size layout
        // change left 6 sensors with garbage enabled/interface bytes). layout_hash is
        // field 0 of EcuConfig — a content hash of the layout, so it changes whenever
        // any byte moves and can never be forgotten on a schema change. Honour it here.
        const uint32_t stored_hash =
            (payload && len >= sizeof(uint32_t))
                ? *reinterpret_cast<const uint32_t*>(payload) : 0u;
        if (boot != BootResult::OK_DEFAULT && payload && len == sizeof(g_config) &&
            stored_hash == JAYECU_LAYOUT_HASH) {
            // Already there when the SD path streamed into g_config itself; memcpy onto itself is
            // undefined, so skip it rather than rely on it being harmless.
            if (payload != reinterpret_cast<const uint8_t*>(&g_config))
                memcpy(&g_config, payload, sizeof(g_config));   // the ONLY copy
        } else {
            // NO VALID TUNE — come up DISABLED rather than dead. g_config is .bss, so there are no
            // compiled defaults to run on, and a zeroed config is not a safe one: an unassigned
            // channel is 255 in a real tune, so zeros would claim coil 0 and injector 0.
            //
            // Not a hard halt, because a halted board needs an ST-LINK to recover. Comms still
            // comes up, so the studio can connect, push a tune and burn it — then a reset boots
            // normally. Everything that touches the engine is skipped below.
            g_no_tune = true;
        }
    }

    // ---- Learned region: restore the newest valid SD totem into the RAM learned buffer ----
    // The learned tables (LTFT/LTT) live in a zero-init RAM buffer; LearnedStore reloads the last good
    // totem here (SD already mounted above), before the engine task maps/uses them. A wrong-layout, torn,
    // or crc-failed totem is rejected → the buffer stays neutral (re-learn). Seed s_learned_crc from the
    // restored (or neutral) region so the first flush only writes once something actually changes.
    {
        uint32_t cap = 0;
        auto* lregion = static_cast<uint8_t*>(platform_learned_base(&cap));
        const bool loaded = g_learned_store.load(lregion, cap);
        s_learned_crc = crc32_buf(lregion, cap);
        EFI_LOG_DEBUG("learn", "boot totem load=%d seq_next=%lu crc=%08lx",
                      (int)loaded, (unsigned long)g_learned_store.next_sequence(), (unsigned long)s_learned_crc);
    }

    // ---- Trigger / capture layer -------------------------------------------
    // Start the TIM5 timebase + EXTI core, resolve the trigger-input mapping
    // onto the board's capture pool, and configure the bound pins' edges. The
    // engine task's epos_hal.start() then arms the channels. A later tune change
    // re-runs the resolver + epos_hal.reconfigure() to re-bind live, no reset.
    if (!g_no_tune) {
        // The decoder/scheduler already hold live references into g_config.trigger
        // and g_config.cylinders (bound at construction). The resolver reads the
        // crank/cam input mapping straight from g_config.trigger too — no copy.
        const BoardProfile& prof = board_profile();
        Stm32Capture_Init(prof.ticks_per_second);          // TIM5 counter (timebase)
        Stm32OutputCompare_Init(prof.ticks_per_second);    // TIM5 CCR1 compare → IGN/LS firing
        g_soft_timer.init(SOFTPWM_TPS);                    // TIM4 tick source (prio 3) for soft-PWM
        g_softpwm.bind(&g_soft_timer);                     // the PWM output rows run on this engine
        descriptor_to_assignment(prof, g_config.trigger, g_assignment);

        // The output pool is owned by the arbiter, one entry per output row (outputs.output[i] IS
        // pin i): pins stay Hi-Z until a module claims them. The scheduler claims the rows that are
        // coils/injectors of firing cylinders; OutputManager claims the Generic rows.
        board_build_output_pool();
        uint8_t out_pool_n = 0;
        ITimerChannel** out_pool = board_output_pool(out_pool_n);
        g_pins.bind(out_pool, out_pool_n);
        g_assignment.pin_arbiter = &g_pins;

        // Firing-layer domain alarms (CCR2 = angle / Timer 1, CCR3 = time / Timer 2).
        // The scheduler drives these instead of per-channel deadlines. Set AFTER the
        // resolver (it fills the rest of g_assignment); bound in epos start()→wire.
        static Stm32Alarm s_angle_alarm(2);
        static Stm32Alarm s_time_alarm(3);
        g_assignment.angle_alarm = &s_angle_alarm;
        g_assignment.time_alarm  = &s_time_alarm;
        // CCR4 = the DECODER's deadline: "a tooth I predicted has not arrived". Its own compare
        // channel on purpose. The detection used to ride inside the DCO's match ISR (CCR1), which
        // every sync transition resets — so the ECU was blind to a stopped engine in exactly the
        // window a stop is most likely. Same TIM5 counter as the capture timestamps, so the two ends
        // of every comparison are read from one clock.
        static Stm32Alarm s_tooth_alarm(4);
        g_assignment.tooth_alarm = &s_tooth_alarm;

        // Engine-cycle capture. Wired inside the !g_no_tune block on purpose: with no tune
        // there is no scheduler running and nothing to record, and the comms handler answers
        // a capture request with an Idle header rather than pretending a buffer exists.
        // IN DTCM. A capture buffer is written by the capture ISR and read by comms — never by DMA, which
        // cannot reach DTCM at all on this part — so it belongs in the fast core-coupled RAM rather than
        // in the 384K the tune image also has to fit in. (The tune grew past that budget the day the
        // outputs gained conditions and duty maps; this is where the room came from.)
        static CycleRecorder __attribute__((section(".dtcm"))) s_cycle_recorder;
        g_epos_hal.assign_cycle_recorder(&s_cycle_recorder);   // wires the scheduler's half too
        extern CycleRecorder* g_cycle_recorder;                // the 0x26 arm/read command
        g_cycle_recorder = &s_cycle_recorder;

        // The RAW TRIGGER LOG (0x27) — time-domain edges, for a wheel the decoder cannot sync to.
        // Fed from the capture ISR before the decoder sees the edge, so it works with no sync at all,
        // which is the only condition in which an unknown wheel can be examined.
        // SEGMENT TIMING — the misfire signal, taken from the real teeth rather than the PLL's
        // residual. Configured by start() from the live cylinder layout; availability is decided from
        // the measured tooth pitch, so a wheel too coarse to time a segment simply reports nothing.
        static SegmentTimer s_segment_timer;
        g_epos_hal.assign_segment_timer(&s_segment_timer);
        g_composer.misfire().set_timer(&s_segment_timer);   // the consumer half of the same timer
        extern Knock* g_knock_scope;                        // 0x28 reads the last classified shot
        g_knock_scope = &g_composer.knock();

        // IN DTCM. 24 KB of capture buffer, appended to from the trigger ISR on every edge — which
        // is the one place in this firmware where a wait state is paid per edge rather than per loop.
        // DTCM is zero-wait and is not on the AXI bus the DMA engines contend for, so the capture is
        // both faster and less perturbed there; it also keeps 24 KB of main RAM for the tune, which
        // is where the generic CAN pool went. Nothing DMAs into this buffer, so .dma_nocache does not
        // apply and the normal cache rules are irrelevant to it.
        static TriggerLogger s_trigger_logger __attribute__((section(".dtcm")));
        // The SAME timebase the edges are stamped in — taken from the resolved assignment rather
        // than reached for independently, so the log's clock and the edge clock cannot be two things.
        s_trigger_logger.set_clock(g_assignment.timebase);
        g_epos_hal.assign_trigger_logger(&s_trigger_logger);
        extern TriggerLogger* g_trigger_logger;                // the 0x27 arm/read command
        g_trigger_logger = &s_trigger_logger;
        extern EnginePositionHal* g_position_hal;              // 0x27 arms by DISABLING the decoder
        g_position_hal = &g_epos_hal;

        // VirtualTrigger DCO: fire on_dco_match at each predicted virtual tooth.
        board_dco_channel().register_callback(vtrig_dco_match, nullptr);
        g_vtrig.register_callback(vtrig_vtooth, nullptr);

        // Per-cycle wake relay vector (SPDIF_RX, repurposed). Priority 6 is >= configMAX_SYSCALL (5),
        // so its handler may call vTaskNotifyGiveFromISR. Below the capture(1)/fire(2)/softpwm(3)
        // ISRs — it's just a wake, not time-critical. Pended by vtrig_vtooth at the cycle boundary.
        HAL_NVIC_SetPriority(SPDIF_RX_IRQn, 6, 0);
        HAL_NVIC_EnableIRQ(SPDIF_RX_IRQn);
        // Knock worker wake relay (SAI1, another repurposed spare vector). Same FromISR-legal priority
        // 6; pended by knock_worker_pend() from the alarm ISR when a knock window fires.
        HAL_NVIC_SetPriority(SAI1_IRQn, 6, 0);
        HAL_NVIC_EnableIRQ(SAI1_IRQn);
        // Knock burst-complete handoff: the worker sleeps on this semaphore, given by the DMA-complete
        // ISR (board_knock_register_complete) — replaces the old blocking poll-spin in the capture.
        s_knock_burst_sem = xSemaphoreCreateBinaryStatic(&s_knock_sem_buf);
        board_knock_register_complete(knock_burst_complete);
        // Composition root knows the concrete capture type — apply EXTI edges.
        apply_capture_edge(g_assignment.crank_primary,   g_assignment.crank_primary_edge);
        apply_capture_edge(g_assignment.crank_secondary, g_assignment.crank_secondary_edge);
        for (int i = 0; i < MAX_CAM_CHANNELS; ++i)
            apply_capture_edge(g_assignment.cam[i], g_assignment.cam_edge[i]);
    }

    // Wire CAN buses — main composes against ICanChannel only; the bxCAN bring-up lives in
    // platform_can (firmware/Platform/stm32f7xx/PlatformCan.cpp).
    platform_can_init();
    apply_can_bus_config();                  // the tune's bitrate / listen-only / enable, over the defaults
    g_can_broker.add_bus(0, &platform_can_bus(0));
    g_can_broker.add_bus(1, &platform_can_bus(1));
    if (g_config.can.obd_enabled) {
        g_can_broker.enable_obd(g_config.can.obd_bus);
    }
    // The tune's own CAN frames, both directions. Built here rather than waiting for the first config
    // write, for the same reason the OBD responder is: an ECU booting on a stored tune must do what the
    // tune says without somebody opening the studio first.
    g_can_broker.reconfigure_generic(g_config.can, platform_get_tick_ms());

    // Register USB as primary transport.
    g_comms.add_transport(&g_usb_transport);
    g_comms.set_signal_bus(&g_engine_task.bus());   // comms packs telemetry from the bus on demand

    // Register the bench/diagnostic text commands. The 'E' handler lazily registers the
    // default set too (register_commands appends, so both coexist).
    Cli::register_commands(kBenchCmds, sizeof(kBenchCmds) / sizeof(kBenchCmds[0]));

    // Start engine control pipeline.
    // Composition: SystemComposer creates/inits/cross-wires the concrete managers and
    // registers them with the scheduler in phase order; then the scheduler task starts.
    // The board's two H-bridges as IHBridge objects (DMA-PWM on PD6/PD3 + DIR/DIS HAL), for the
    // generic HBridge actuator-driver. Built before compose; start disabled (DIS high, DMA idle).
    IHBridge* hbridge_a = nullptr;
    IHBridge* hbridge_b = nullptr;
    board_hbridges(hbridge_a, hbridge_b);
    // With no tune there is nothing to compose FROM, and composing would claim pins out of a zeroed
    // config. Skipping it leaves every output Hi-Z and the scheduler unstarted — the disabled state.
    if (!g_no_tune) {
        g_composer.compose(g_engine_task, g_can_broker, g_softpwm,
                           SOFTPWM_TPS, g_pins, g_comms, hbridge_a, hbridge_b);
        g_engine_task.start(BOARD_CYLINDER_COUNT);
    } else {
        platform_set_led_error(true);      // solid red: no tune, engine disabled
        platform_set_led_running(false);
        platform_set_led_warning(false);
    }

    // Restore the persisted DTC table and bump the power-cycle counter. start()
    // inits the table empty (boot_id 0); this reloads the stored codes (STORED but
    // not active — each re-arms only if its condition is still true) and assigns
    // this session a fresh boot_id (prev + 1). The card is ECU-owned at boot.
    // The DTC table is shared by several tasks (see DtcManager::set_lock). Scheduler suspend: a few
    // microseconds at most, no interrupt is held off, and it nests.
    DtcManager::set_lock(
        [] { if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) vTaskSuspendAll(); },
        [] { if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) (void)xTaskResumeAll(); });
    g_sd_dtc_store.boot_restore(g_engine_task.dtc());

    // Housekeeping tasks.
    UsbCdc_StartTxTask();
    UsbMsc_StartTask();   // MSC BOT thread — also performs the one-shot USBD_Start (device connect)
    xTaskCreateStatic(can_task,        "CAN",   256, nullptr, 2, s_can_stack,   &s_can_tcb);
    xTaskCreateStatic(comms_task,      "Comms", 512, nullptr, 1, s_comms_stack, &s_comms_tcb);
    xTaskCreateStatic(config_save_task,"Save",  512, nullptr, 1, s_save_stack,  &s_save_tcb);
    xTaskCreateStatic(board_service_task,"Board",384, nullptr, 1, s_board_stack, &s_board_tcb);
    s_knock_task = xTaskCreateStatic(knock_task, "Knock", 512, nullptr, 2, s_knock_stack, &s_knock_tcb);
    xTaskCreateStatic(log_sample_task, "LogSmp", 512, nullptr, 2, s_logsmp_stack, &s_logsmp_tcb);
    xTaskCreateStatic(led_task,        "LED",   256,  nullptr, 1, s_led_stack,    &s_led_tcb);
    xTaskCreateStatic(script_task,     "Lua",   2048, nullptr, 1, s_script_stack, &s_script_tcb);
    xTaskCreateStatic(watchdog_task,   "Wdg",   128,  nullptr, 2, s_wdg_stack,    &s_wdg_tcb);

    // Start the IWDG last, after all the slow boot init (SD mount, flash config load) — those run
    // unwatched; from here on, a hung control frame -> no pet -> hardware reset. The watchdog task's
    // first pet lands within ~ms of the scheduler starting, far inside the timeout.
    platform_watchdog_init(WDG_TIMEOUT_MS);

    vTaskStartScheduler();
    while (true) {}
}
