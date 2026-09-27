#include "CliRegistry.h"
#include "CliCommands.h"

#include "../../generated/ecu_config.h"     // g_config / EcuConfig
#include "../Platform/platform_hal.h"       // platform_get_tick_ms
#include <FreeRTOS.h>                       // cmd_reset waits for a pending burn to land
#include <task.h>                           // vTaskDelay — yield to the (same-priority) save task
#include "../Platform/board_hal.h"          // board_hbridge_set_enable/direction (bench diag)
#include "../Engine/Modules/KnockDsp.h"      // 'knock' bench command — burst + band level
#include "../Engine/Modules/OutputTest.h"    // 'test' — bench output test (coil / injector / level)
#include "../Diagnostics/CpuStats.h"        // 'cpu' — MCU load + where the time goes
#include "../Diagnostics/ReconfigCost.h"  // …and what the last tune write cost the frame
#include "../Diagnostics/DtcManager.h"       // 'dtc' command — show/clear the DTC table
#include "../Platform/platform_can.h"        // 'canloop' — hardware CAN loopback for bench OBD tests
#include "../Comms/SdProtocol.h"             // 'datalog' — force the SD logger on/off for a bench pass
#include "../Comms/CommsManager.h"           // packed_frame() — the record the logger writes
#include "../Platform/stm32f7xx/SdCardSpi.h"  // 'datalog 0 3' — the SHAPE of the writes FatFS makes
#include <cstring>

// Bench hooks defined in main.cpp (next to the learned store). GLOBAL scope on purpose: declared inside
// this file's anonymous namespace they would be a different bench:: entirely, and fail to link.
namespace bench {
uint32_t learned_read(uint32_t off);
void     learned_poke(uint32_t off, uint32_t val);
void     learned_poke2(uint32_t off, uint32_t val);
uint32_t learned_save();
uint32_t learned_seq();
uint32_t learned_cap();
}


// ---------------------------------------------------------------------------
// Built-in text commands, reachable from the comms link's 'E'/execute (and any raw
// console) via CommsManager. Each handler writes its reply into `out`.
//
// Keep this list decentralised in spirit: a module that owns state should grow
// its own register_*_cli() and have it called alongside this. These are the
// starter set proving the path end to end.
// ---------------------------------------------------------------------------

// Defined in CommsManager.cpp (set by SystemComposer::compose()). Declared at GLOBAL scope so the
// anonymous-namespace cmd_dtc below links to the external symbol, not an internal-linkage shadow.
class DtcManager;
extern DtcManager* g_dtc_table;
namespace Comms { void comms_request_save(); }   // main.cpp shim: flag a burn (same path as wire 'b')
// FILE SCOPE, deliberately: declared inside the anonymous namespace below these become
// (anonymous)::g_config_* and silently fail to link against the real definitions.
extern volatile uint32_t g_config_generation;        // bumped by every config write
extern volatile uint32_t g_config_generation_saved;  // ... and by the save task once it lands

// ElectronicThrottle bench entry points (defined in ElectronicThrottle.cpp; forward to the composed instance).
void throttle_bench_nudge(uint8_t etb, float pct, uint32_t hold_ms) noexcept;
void throttle_bench_autocal(uint8_t etb) noexcept;
void throttle_bench_fillff(uint8_t etb, uint8_t row) noexcept;   // stage-2 FF-map sweep + fill (client-chosen row)
void throttle_bench_autotune(uint8_t etb, uint8_t rule) noexcept;   // relay-feedback PID autotune
void hbridge_bench_nudge(uint8_t half, float demand, uint32_t hold_ms) noexcept;   // drive one half directly (HBridge.cpp)

void app_bench_calibrate() noexcept;                     // pedal calibrate (engine-stopped)

extern bool g_system_active;   // key-on (battery > threshold). Bench cals drive motors / read powered
                               // front-ends, so they MUST NOT run on USB-only power — abort if key-off.

namespace {

// Every bench calibration needs the ECU powered (key-on): a USB-only bench can't drive the actuator or
// read the 5 V-referenced feedback, so a cal would flail. Gate the command on it with a clear message.
static bool require_key_on(const char* what, Cli::Out& o) {
    if (g_system_active) return true;
    o.put(what); o.put(": key-on required (power the ECU; USB-only can't drive/sense)\r\n");
    return false;
}


using namespace Cli;

void cmd_ping(const Argv&, Out& o) {
    o.put("pong\r\n");
}

void cmd_uptime(const Argv&, Out& o) {
    o.print_u32(platform_get_tick_ms() / 1000u);
    o.put(" s\r\n");
}

// Bench diagnostic: enable/direction a generic H-bridge driver channel (the IFX9201). The PWM
// magnitude is driven separately (a SoftPwm output on the channel's PWM pin). Lets the bench
// energize a bridge so its output (e.g. an idle BAC valve) shows on the scope. RAM-only, no burn.
// Drive one half bridge directly, for four seconds, with no controller in the way -- "prove it moves".
//
// This used to poke the DIS/DIR GPIOs and nothing else, which could not work: the HBridge module
// re-asserts every pin every frame (deliberately -- see apply_bridge), so the enable was undone within
// a millisecond, and no duty was ever commanded, so even the millisecond it survived drove nothing. It
// goes through the module now, as a demand it honours in place of the bus.
//
// The argument is a SIGNED DUTY in the producer's units: -100..+100, 0 = off. Sign is direction. It is
// mapped by the half's own dc_map / dc_max_pct / dir_invert, so the buttons test the configuration
// rather than bypassing it -- a body that moves only one way here has its DC map set wrong.
// Bench output test. The ROW is the physical output (row n IS pin n), and what happens to it comes
// from that row's own function: an Ignition row dwells then releases (the release is the spark), an
// Injector row pulses, anything else is held on as a level. So the operator presses a button and
// never types a dwell.
//
// It does what is asked. An omitted on/off time gets a sensible one for the function; a given one is
// used as given, dwell included — see OutputTest.h for why. What the firmware still owns is ending
// the test: engine-stopped only, a start cancels, and it expires on its own because the console
// cannot be relied on to send an Off. `test <row> 0` stops that row, `test 255 0 0 0` stops all.
void cmd_test(const Argv& a, Out& o) {
    if (!g_output_test) { o.put("test: no output test on this build\r\n"); return; }
    const int32_t row = a.i[0], count = a.i[1], on = a.i[2], off = a.i[3];
    if (row == 255) { g_output_test->cancel_all(platform_get_tick_ms()); o.put("test: all stopped\r\n"); return; }
    // 254 = STATUS. A host needs to know whether anything is still firing — to light an Abort button,
    // and because "it stopped on its own" is otherwise not a thing anyone can see.
    if (row == 254) {
        o.put("test: ");
        if (!g_output_test->active()) { o.put("idle\r\n"); return; }
        o.put("active rows");
        for (uint32_t r = 0; r < OUTPUTS_OUTPUT_COUNT; ++r)
            if (g_output_test->active_row(static_cast<uint8_t>(r))) { o.put(" "); o.print_u32(r); }
        o.put("\r\n");
        return;
    }
    if (row < 0 || row >= static_cast<int32_t>(OUTPUTS_OUTPUT_COUNT)) {
        o.put("test: row 0.."); o.print_u32(OUTPUTS_OUTPUT_COUNT - 1u); o.put(" (or 255 = stop all)\r\n"); return;
    }
    if (count < 0 || on < 0 || off < 0) { o.put("test: count/on/off must be >= 0\r\n"); return; }

    const auto mode = g_output_test->mode_of(static_cast<uint8_t>(row));
    const bool ok = g_output_test->start(static_cast<uint8_t>(row), static_cast<uint32_t>(count),
                                         static_cast<uint32_t>(on), static_cast<uint32_t>(off),
                                         platform_get_tick_ms());
    o.put("test "); o.print_u32(static_cast<uint32_t>(row));
    if (count == 0) { o.put(" stopped\r\n"); return; }
    if (!ok) {
        // The pin is spoken for, or there is no pin. Say which rather than looking like a dead output.
        o.put(": REFUSED — the pin is owned by the firing layer or the row is out of range\r\n");
        return;
    }
    uint32_t on_ap = 0, off_ap = 0;
    g_output_test->applied(static_cast<uint8_t>(row), on_ap, off_ap);
    o.put(mode == OutputTest::Mode::Spark  ? " spark x" :
          mode == OutputTest::Mode::Inject ? " inject x" : " on x");
    o.print_u32(static_cast<uint32_t>(count));
    o.put(" on="); o.print_u32(on_ap); o.put("ms");
    if (off_ap) { o.put(" off="); o.print_u32(off_ap); o.put("ms"); }
    o.put(" (engine-stopped; the ECU ends it on its own)\r\n");
}

void cmd_hbridge(const Argv& a, Out& o) {
    const int32_t idx = a.i[0], duty = a.i[1];
    if (idx < 0 || idx >= 2) { o.put("hbridge: idx 0..1\r\n"); return; }
    if (duty < -100 || duty > 100) { o.put("hbridge: duty -100..100\r\n"); return; }
    hbridge_bench_nudge(static_cast<uint8_t>(idx), static_cast<float>(duty), duty == 0 ? 0u : 4000u);
    o.put("hbridge "); o.print_u32(static_cast<uint32_t>(idx));
    if (duty == 0) { o.put(" off\r\n"); return; }
    o.put(" demand "); o.print_i32(duty); o.put("% for 4s (engine-stopped)\r\n");
}

// Bench bring-up for knock: capture an ADC3 burst on a knock channel, band-filter it, and report the
// raw range + RMS level (dB x10). Drive a tone into KNOCK1/KNOCK2 from a wave generator and sweep
// amplitude/frequency to validate the ADC3 burst + DSP path with no engine running. RAM-only.
void cmd_knock(const Argv& a, Out& o) {
    const int32_t ch = a.i[0];
    int32_t freq = a.i[1];
    if (ch < 0 || ch > 1) { o.put("knock: ch 0..1\r\n"); return; }
    if (freq <= 0) freq = 7000;                       // default band center (Hz)
    static uint16_t buf[512];
    const uint16_t n = board_knock_capture(static_cast<uint8_t>(ch), buf, 512);
    if (n == 0u) { o.put("knock: capture failed\r\n"); return; }
    uint16_t mn = 4095u, mx = 0u;
    for (uint16_t i = 0; i < n; ++i) { if (buf[i] < mn) mn = buf[i]; if (buf[i] > mx) mx = buf[i]; }
    KnockDsp dsp;
    dsp.configure(board_knock_sample_rate(), static_cast<float>(freq), 3.0f);
    static KnockProfile prof;
    const int32_t db10 = static_cast<int32_t>(dsp.process(buf, n, prof) * 10.0f);
    o.put("knock"); o.print_u32(static_cast<uint32_t>(ch));
    o.put(" f=");    o.print_u32(static_cast<uint32_t>(freq));
    o.put(" n=");    o.print_u32(n);
    o.put(" raw[");  o.print_u32(mn); o.put(".."); o.print_u32(mx);
    o.put("] p2p="); o.print_u32(static_cast<uint32_t>(mx - mn));
    o.put(" lvl_dBx10="); o.print_i32(db10);
    // The PROFILE, not just the level. A steady wave-gen tone should read flat across every bucket —
    // so a sloped or ragged row here is the bench telling you the burst itself is uneven (a DMA that
    // started mid-transfer, a filter still settling) rather than anything the engine did. It is also
    // the only way to eyeball, on the bench, that phase resolution works at all before trusting it to
    // separate knock from pre-ignition.
    o.put(" buckets="); o.print_u32(prof.count);
    o.put(" dBx10[");
    for (unsigned i = 0; i < prof.count; ++i) {
        if (i) o.put(",");
        o.print_i32(static_cast<int32_t>(prof.db[i] * 10.0f));
    }
    o.put("]\r\n");
}

// Reboot into the STM32 USB DFU bootloader. The board drops off its CDC port and
// re-enumerates as 0483:df11 "STM32 BOOTLOADER" — reflash with dfu-util, no ST-LINK
// needed (replaces a physical BOOT0 + reset on the ST-LINK-less board). Never returns,
// so the 'E' ack/reply is NOT sent: the host should expect the link to drop here.
void cmd_dfu(const Argv&, Out& o) {
    o.put("entering DFU bootloader\r\n");   // best-effort; reset pre-empts the flush
    platform_request_bootloader();          // sets reset-surviving flag + system reset
}

// Plain system reset — reboots back into the application (e.g. to apply a burned
// tune). Like 'dfu', it never returns, so the 'E' reply is not flushed: the host
// should expect the link to drop and re-enumerate as the normal CDC port.
void cmd_reset(const Argv&, Out& o) {
    // A BURN IN FLIGHT MUST LAND BEFORE THE RESET. Burning is asynchronous -- the 'b' command hands the
    // work to the save task and returns immediately -- and it takes the better part of two seconds to
    // reach flash. Resetting inside that window silently discards it: the ECU comes back with the
    // PREVIOUS tune and nothing anywhere says so. That is how a findlimits + autotune, burned and then
    // reset, came back uncalibrated with default gains, looking exactly like a calibration that had
    // never been written.
    //
    // So wait for the save to be acknowledged (config_dirty clearing IS the acknowledgement), bounded
    // so a genuinely stuck save can never make `reset` unusable -- a reset that will not reset is worse
    // than a lost burn.
    const uint32_t t0 = platform_get_tick_ms();
    bool waited = false;
    while (g_config_generation != g_config_generation_saved) {
        if (platform_get_tick_ms() - t0 > 4000u) {       // stuck save -> reset anyway, but SAY so
            o.put("reset: WARNING - a burn is still pending after 4 s; resetting anyway "
                  "(unburned changes will be lost)\r\n");
            break;
        }
        waited = true;
        vTaskDelay(pdMS_TO_TICKS(10));   // YIELD: the save task shares this priority, so a spin would starve it
    }
    if (waited) o.put("reset: pending burn landed\r\n");
    o.put("resetting\r\n");                  // best-effort; reset pre-empts the flush
    platform_reboot();
}

void cmd_dtc(const Argv& a, Out& o) {
    if (!g_dtc_table) { o.put("dtc: unavailable\r\n"); return; }
    if (a.s[0] && !std::strcmp(a.s[0], "clear")) {
        g_dtc_table->clear_all();
        o.put("dtc table cleared\r\n");
        return;
    }
    o.put("dtc active="); o.print_u32(g_dtc_table->active_count());
    o.put(" stored=");    o.print_u32(g_dtc_table->stored_count());
    // How many the TABLE retired because nobody was still asserting them, rather than because their
    // producer said so. Zero means every producer is healing its own and the ttl is pure insurance.
    o.put(" aged=");      o.print_u32(g_dtc_table->aged_count());
    o.put("  ('dtc clear' to wipe)\r\n");
}

// Bench manual ETB nudge: open-loop demand on a throttle for a short auto-expiring window. The ETB
// only moves with engine STOPPED (enforced in ElectronicThrottle); it slews per the configured open/close
// rate and re-asserts DIS on expiry. Routes through ElectronicThrottle -> etb_duty_N -> HBridge.
void cmd_throttle(const Argv& a, Out& o) {
    const int32_t idx = a.i[0];
    int32_t pct = a.i[1];
    if (idx < 0 || idx > 1) { o.put("throttle: idx 0..1\r\n"); return; }
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    throttle_bench_nudge(static_cast<uint8_t>(idx), static_cast<float>(pct), 4000u);  // 4 s auto-expire
    o.put("throttle "); o.print_u32(static_cast<uint32_t>(idx));
    o.put(" demand "); o.print_u32(static_cast<uint32_t>(pct)); o.put("% for 4s (engine-stopped)\r\n");
}

// The three ETB routines all live in ElectronicThrottle and only run for a body that is ENABLED. Say so
// up front rather than reporting "started" for something that cannot start — the module that runs the
// routine is the module that is switched off, which is exactly how pedalcal used to hang.
static bool require_etb_enabled(const char* what, int32_t idx, Out& o) {
    if (g_config.electronic_throttle.etb[idx].enabled) return true;
    o.put(what); o.put(": etb["); o.print_u32(static_cast<uint32_t>(idx));
    o.put("] is disabled - enable it before running this\r\n");
    return false;
}

// Bench autocal: engine-stopped calibration sweep on an ETB (settle -> creep to the open stop ->
// write the feedback sensors' cal + raw DTC thresholds). Backs off the instant the plate stalls.
void cmd_findlimits(const Argv& a, Out& o) {
    const int32_t idx = a.i[0];
    if (idx < 0 || idx > 1) { o.put("findlimits: idx 0..1\r\n"); return; }
    if (!require_key_on("findlimits", o)) return;
    if (!require_etb_enabled("findlimits", idx, o)) return;
    throttle_bench_autocal(static_cast<uint8_t>(idx));
    o.put("findlimits "); o.print_u32(static_cast<uint32_t>(idx));
    o.put(" started (engine-stopped; finds 0%/100% stops + relax%; watch etb_state_N)\r\n");
}

// Bench stage-2 FF fill: engine-stopped quasi-static sweep (down+up) that measures the hold duty at each
// configured ff_table X breakpoint and writes the midpoints into etb[idx].ff_table at the CLIENT-CHOSEN
// Y row (a Y-keyed/CLT row can't be reached by being at that temperature — the throttle won't track there
// until it's filled — so the row is given). Requires a prior `findlimits`. Set the bins client-side first.
void cmd_fillff(const Argv& a, Out& o) {
    const int32_t idx = a.i[0];
    const int32_t row = a.i[1];
    if (idx < 0 || idx > 1) { o.put("fillff: idx 0..1\r\n"); return; }
    if (row < 0)            { o.put("fillff: row >= 0\r\n"); return; }
    if (!require_key_on("fillff", o)) return;
    if (!require_etb_enabled("fillff", idx, o)) return;
    throttle_bench_fillff(static_cast<uint8_t>(idx), static_cast<uint8_t>(row));
    o.put("fillff "); o.print_u32(static_cast<uint32_t>(idx));
    o.put(" row "); o.print_u32(static_cast<uint32_t>(row));
    o.put(" started (engine-stopped; sweeps + fills that ff_table row; needs findlimits first)\r\n");
}

// Bench PID autotune: engine-stopped relay feedback at each ff_table breakpoint -> worst-case (min-Ku)
// gains via the chosen rule. Requires a prior findlimits + fillff. Results print to the console.
void cmd_autotune(const Argv& a, Out& o) {
    const int32_t idx = a.i[0], rule = a.i[1];
    if (idx < 0 || idx > 1)   { o.put("autotune: idx 0..1\r\n"); return; }
    if (rule < 0 || rule > 4) { o.put("autotune: rule 0..4 (0=TyreusLuyben 1=ZN 2=ZNnoOS 3=ZNsomeOS 4=Pessen)\r\n"); return; }
    if (!require_key_on("autotune", o)) return;
    if (!require_etb_enabled("autotune", idx, o)) return;
    throttle_bench_autotune(static_cast<uint8_t>(idx), static_cast<uint8_t>(rule));
    o.put("autotune "); o.print_u32(static_cast<uint32_t>(idx));
    o.put(" started (engine-stopped; relays each ff bin; watch the console for Ku/Tu + gains)\r\n");
}

// Bench pedal calibrate: engine-stopped. Starts a 5 s capture — fully press + release the pedal —
// then writes the APP sensors' cal (released->0%, pressed->100%). Aborts if the engine starts.
// Why the last reset happened, from the sticky RCC_CSR flags captured at boot. Nothing else in this
// firmware records a restart, so without this an ECU that rebooted mid-test looks exactly like one that
// did not, and the cause gets guessed at instead of read.
extern "C" uint32_t platform_reset_flags();
extern "C" const char* platform_stack_overflow_task();
extern "C" void platform_clear_stack_overflow();
void cmd_resetcause(const Argv&, Out& o) {
    const uint32_t f = platform_reset_flags();
    o.put("RCC_CSR=0x"); o.print_u32(f); o.put(" ->");
    if (f & (1u << 31)) o.put(" LOW-POWER");
    if (f & (1u << 30)) o.put(" WINDOW-WATCHDOG");
    if (f & (1u << 29)) o.put(" INDEPENDENT-WATCHDOG");
    if (f & (1u << 28)) o.put(" SOFTWARE");
    if (f & (1u << 27)) o.put(" POWER-ON");
    if (f & (1u << 26)) o.put(" NRST-PIN");
    if (f & (1u << 25)) o.put(" BROWN-OUT");
    if ((f & 0xFE000000u) == 0) o.put(" (none set)");
    // A stack overflow leaves its victim's name in unzeroed DTCM, so the reset it caused can be
    // attributed instead of guessed at. Cleared once reported.
    if (const char* t = platform_stack_overflow_task()) {
        o.put("  STACK OVERFLOW in task '"); o.put(t); o.put("'");
        platform_clear_stack_overflow();
    }
    o.put("\r\n");
}

// Persist the live RAM config to flash — the same request the wire 'b' command makes, exposed as a
// console command so the studio's command dictionary carries it and a button can be bound to it.
// Burn stays DELIBERATE: this flags the request, the save task does the write.
void cmd_burn(const Argv&, Out& o) {
    Comms::comms_request_save();
    o.put("burn requested - the save task will persist g_config to flash\r\n");
}

void cmd_pedalcal(const Argv&, Out& o) {
    // Say why, up front. The routine lives in the App module, so with the module disabled it cannot run
    // at all — and reporting "started" for something that will never start is how you lose ten minutes
    // pressing a pedal at an ECU that stopped listening before you began.
    if (!g_config.app.enabled) {
        o.put("pedalcal: APP module is disabled - enable it (app.enabled = 1) before calibrating\r\n");
        return;
    }
    if (!require_key_on("pedalcal", o)) return;
    app_bench_calibrate();
    o.put("pedalcal started (engine-stopped; press+release the pedal fully over ~5s)\r\n");
}

// canloop <bus> <0|1> — hardware CAN loopback. With it on, everything the ECU transmits on that bus comes
// straight back into its own receive FIFO and no external node has to ACK, so the whole OBD path (request
// parsing, Mode 01/03/09 responses, ISO-TP segmentation + flow control) can be exercised on a bench with
// nothing else on the wire. A lone CAN node cannot otherwise complete a single transmission. While it is
// on the bus is deaf and mute to the outside world, so turn it back off when the test is done.
void cmd_canloop(const Argv& a, Out& o) {
    const int32_t bus = a.i[0], on = a.i[1];
    if (bus < 0 || bus > 1) { o.put("?usage: canloop <bus 0..1> <0=off 1=on>\r\n"); return; }
    if (!platform_can_set_loopback(static_cast<uint8_t>(bus), on != 0)) {
        o.put("canloop: bus down\r\n"); return;
    }
    o.put("canloop bus="); o.print_u32(static_cast<uint32_t>(bus));
    o.put(on != 0 ? " LOOPBACK (deaf to the wire)\r\n" : " normal\r\n");
}

// datalog <0|1> — force the SD datalogger on or off, ignoring the rpm auto-start gate.
//
// The logger only self-starts above 100 rpm, so on a bench with no crank signal the FatFS write path,
// the file close and the readback — the parts that can actually be wrong — are unreachable. This drives
// them directly. It writes the same packed telemetry record the running logger writes, so a file
// produced this way is byte-identical in shape to a real one.
void cmd_datalog(const Argv& a, Out& o) {
    if (a.i[0] != 0) {
        // Up to 1 kHz now, which is the sampler task's tick — a bench pass proving the high-rate
        // path has to be able to ASK for it, and 255 was the old field's ceiling, not the logger's.
        const uint16_t hz = (a.i[1] > 0 && a.i[1] <= 1000) ? static_cast<uint16_t>(a.i[1]) : 10u;
        if (!Comms::sd::start_logging(hz)) { o.put("datalog: start failed (card busy / not ours?)\r\n"); return; }
        Comms::sd::set_forced(true);          // hold off the comms loop's rpm auto-gate
        o.put("datalog START @"); o.print_u32(hz); o.put(" Hz\r\n");
    } else if (a.i[1] == 3) {
        // `datalog 0 3` — the SHAPE of the writes FatFS produced. Whether the card is streamed to
        // (long multi-sector runs) or poked one block at a time is not something to reason about
        // from the FatFS source; it is something to count.
        uint32_t calls = 0, single = 0, multi = 0, sectors = 0, run = 0;
        SdCard_WriteStats(&calls, &single, &multi, &sectors, &run);
        o.put("writes "); o.print_u32(calls);
        o.put(" (single "); o.print_u32(single);
        o.put(", multi ");  o.print_u32(multi);
        o.put("), sectors "); o.print_u32(sectors);
        o.put(", longest run "); o.print_u32(run);
        o.put("\r\n");
    } else if (a.i[1] == 4) {
        SdCard_WriteStatsReset();
        o.put("write stats reset\r\n");
    } else if (a.i[1] == 2) {
        // `datalog 0 2` — did it keep up? The ring's high-water mark says how close the buffer came
        // to full, and dropped says whether it went over. A high-rate log that silently dropped
        // samples looks exactly like one that did not, which is the whole reason these are reported.
        o.put("ring high water "); o.print_u32(Comms::sd::log_high_water());
        o.put(" B, dropped ");     o.print_u32(Comms::sd::log_dropped());
        o.put(" records\r\n");
    } else {
        Comms::sd::set_forced(false);
        Comms::sd::stop_logging_public();
        o.put("datalog STOP\r\n");
    }
}

// datarec — append ONE record to the active datalog file, now. The running logger is paced from the
// comms loop off real telemetry; this lets a bench pass put a known number of records in the file and
// then check exactly that many came back.
void cmd_datarec(const Argv&, Out& o) {
    // Sample one record and push it straight through to the card. Two calls now rather than one,
    // because sampling and writing are two jobs on two tasks — and a bench pass that wants exactly
    // N records in a file has to do both halves itself rather than wait for the sampler's clock.
    const uint32_t now = platform_get_tick_ms();
    Comms::sd::sample_now(Comms::comms_manager(), now);
    Comms::sd::service_logging(now);
    o.put(Comms::sd::is_logging() ? "datarec ok\r\n" : "datarec: not logging\r\n");
}

// learn <op> <arg> — bench access to the LTFT/LTT learned region and its SD totems.
//   learn 0 <off>        read one byte
//   learn 1 <off>        write a marker byte (0xA5) at that offset
//   learn 2 0            force a totem flush now
//   learn 3 0            report the next totem sequence number
//   learn 4 <off>        write a DIFFERENT marker (0x5A) — lets a test tell "restored from the card"
//                        apart from "SRAM simply survived a warm reset", which otherwise look identical
// The region only drifts while the engine learns, so without these the totem mechanism is unreachable on
// a bench — and "it persists" is exactly the claim that has to be tested, not assumed.
void cmd_learn(const Argv& a, Out& o) {
    const int32_t op = a.i[0], arg = a.i[1];
    switch (op) {
        case 0: o.put("learn["); o.print_u32(static_cast<uint32_t>(arg)); o.put("]=");
                o.print_u32(bench::learned_read(static_cast<uint32_t>(arg))); o.put("\r\n"); break;
        case 1: bench::learned_poke(static_cast<uint32_t>(arg), 0xA5u);
                o.put("poked 0xA5 at "); o.print_u32(static_cast<uint32_t>(arg)); o.put("\r\n"); break;
        case 2: { const uint32_t r = bench::learned_save();
                  o.put(r == 0 ? "totem saved\r\n"
                      : r == 1 ? "totem save FAILED: ECU does not own the card\r\n"
                               : "totem save FAILED: store refused the write\r\n"); } break;
        case 3: o.put("next_seq="); o.print_u32(bench::learned_seq());
                o.put(" cap="); o.print_u32(bench::learned_cap()); o.put("\r\n"); break;
        case 4: bench::learned_poke2(static_cast<uint32_t>(arg), 0x5Au);
                o.put("poked 0x5A at "); o.print_u32(static_cast<uint32_t>(arg)); o.put("\r\n"); break;
        default: o.put("?usage: learn <0=read 1=poke-A5 2=save 3=seq 4=poke-5A> <offset>\r\n"); break;
    }
}

// canbaud <bus> <kbit> — re-init a bus at a different bitrate.
//
// A CAN node that hears the wrong bitrate does not politely ignore you, it simply never ACKs — which
// looks EXACTLY like no device being there at all. Sweeping the ECU's own bitrate is the only way to
// tell "nothing is connected" from "connected, configured differently", and that is the difference
// between a wiring problem and a settings problem.
void cmd_canbaud(const Argv& a, Out& o) {
    const int32_t bus = a.i[0], kbit = a.i[1];
    if (bus < 0 || bus > 1 || kbit <= 0) { o.put("?usage: canbaud <bus 0..1> <kbit e.g. 500>\r\n"); return; }
    if (!platform_can_set_bitrate(static_cast<uint8_t>(bus), static_cast<uint32_t>(kbit) * 1000u)) {
        o.put("canbaud: bus down / bad rate\r\n"); return;
    }
    o.put("canbaud bus="); o.print_u32(static_cast<uint32_t>(bus));
    o.put(" -> "); o.print_u32(static_cast<uint32_t>(kbit)); o.put(" kbit\r\n");
}

// WHERE THE TIME GOES. The two percentages answer different questions and both are here on purpose:
// "mcu" is the share of the chip nobody was idle for, "frame" is how much of the 1 kHz engine frame
// the frame body itself used — the one that decides whether spark is ever late. A chip at 40 % can be
// one frame from missing it; a chip at 90 % can be perfectly safe if the 90 is the engine.
//
// The per-task rows are the actionable half: heaviest first, with the deepest each task has ever gone
// into its stack, because "what is eating it" and "what is about to overflow" are the two questions
// you have when something is wrong, and a single percentage answers neither.
extern "C" uint8_t g_engine_frame_load_pct();
extern "C" uint8_t g_engine_frame_load_max_pct();
extern "C" uint32_t g_engine_frame_max_us();
extern "C" uint8_t  g_engine_frame_costs(const char** names, uint32_t* us, uint16_t* period,
                                         uint8_t* per_cycle, uint8_t max);

extern "C" void g_engine_frame_load_reset();

void cmd_cpu(const Argv& a, Out& o) {
    // `cpu reset` clears the frame PEAK. It is sticky by design — the worst frame since boot is what
    // decides whether one was ever late — but boot itself is not the steady state, so there has to be
    // a way to say "from here".
    if (a.s[0] && a.s[0][0] == 'r') { g_engine_frame_load_reset(); o.put("frame peak reset\r\n"); return; }
    o.put("mcu "); o.print_u32(cpustats::load_pct()); o.put("%");
    o.put("  frame "); o.print_u32(g_engine_frame_load_pct()); o.put("%");
    o.put(" (peak "); o.print_u32(g_engine_frame_load_max_pct()); o.put("% = ");
    o.print_u32(g_engine_frame_max_us()); o.put("us)\r\n");
    o.put("  last reconfig:");
    for (uint8_t i = 0; i < reconfig::PART_COUNT; i++) {
        o.put(" "); o.put(reconfig::name(static_cast<reconfig::Part>(i)));
        o.put(" "); o.print_u32(reconfig::us(static_cast<reconfig::Part>(i))); o.put("us");
    }
    o.put("\r\n");
    const uint8_t n = cpustats::task_count();
    if (!n) { o.put("no task window yet (sampled once a second)\r\n"); return; }
    for (uint8_t i = 0; i < n; i++) {
        const cpustats::TaskLoad& t = cpustats::task(i);
        o.put("  "); o.put(t.name);
        o.put("  "); o.print_u32(t.pct); o.put("%  stack free ");
        o.print_u32(t.stack_free); o.put(" words\r\n");
    }
}

// WHERE THE 1 kHz FRAME GOES, module by module. `cpu` says the frame is busy; this says who is
// making it busy — which is the only version of that fact anybody can act on.
void cmd_frame(const Argv&, Out& o) {
    const char* names[16]; uint32_t us[16]; uint16_t period[16]; uint8_t pc[16];
    const uint8_t n = g_engine_frame_costs(names, us, period, pc, 16);
    o.put("frame "); o.print_u32(g_engine_frame_load_pct()); o.put("% of 1000us, by module:\r\n");
    for (uint8_t i = 0; i < n; i++) {
        if (!us[i]) continue;                     // a module costing under a microsecond is not news
        o.put("  "); o.put(names[i] ? names[i] : "?");
        o.put("  "); o.print_u32(us[i]); o.put("us");
        // A per-cycle module is not in this frame at all — it runs on the crank, on its own task.
        if (pc[i])                 o.put(" [per cycle, NOT in the frame]");
        else if (period[i] > 1)  { o.put(" every "); o.print_u32(period[i]); o.put(" frames"); }
        o.put("\r\n");
    }
}

const Command kCmds[] = {
    { "ping",          Args::NONE, cmd_ping,          "ping -> pong" },
    { "dfu",           Args::NONE, cmd_dfu,           "dfu -> reboot into USB DFU bootloader (reflash via dfu-util)" },
    { "reset",         Args::NONE, cmd_reset,         "reset -> reboot into the application (system reset)" },
    { "test",          Args::IIII, cmd_test,          "test <row> <count> <on_ms> <off_ms> -> fire that output as its function says (255 = stop all; engine-stopped)" },
    { "hbridge",       Args::II,   cmd_hbridge,       "hbridge <idx 0..1> <signed duty -100..100, 0=off> -> drive that half for 4s (engine-stopped)" },
    { "throttle",      Args::II,   cmd_throttle,      "throttle <etb 0..1> <demand 0..100%> (4s, engine-stopped)" },
    { "findlimits",    Args::I,    cmd_findlimits,    "findlimits <etb 0..1> -> find the 0%/100% stops + relax% (engine-stopped) @refresh sensors.sensor electronic_throttle.etb[$0]" },
    { "fillff",        Args::II,   cmd_fillff,        "fillff <etb 0..1> <row> -> stage-2 FF sweep, fills that ff_table Y row (engine-stopped) @refresh electronic_throttle.etb[$0]" },
    { "autotune",      Args::II,   cmd_autotune,      "autotune <etb 0..1> <rule 0..4> -> relay-feedback PID tune at each ff bin, worst-case gains (0=TyreusLuyben 1=ZN 2=ZNnoOS 3=ZNsomeOS 4=Pessen; engine-stopped, needs fillff) @refresh electronic_throttle.etb[$0]" },
    { "pedalcal",      Args::NONE,  cmd_pedalcal,      "pedalcal -> calibrate the accelerator pedal (engine-stopped)" },
    { "burn",          Args::NONE,  cmd_burn,          "burn -> persist the live config to flash (the tune is not saved until you do)" },
    { "resetcause",    Args::NONE, cmd_resetcause,    "resetcause -> why the ECU last restarted (power-on / brown-out / pin / software / watchdog)" },
    { "learn",         Args::II,   cmd_learn,         "learn <0=read 1=poke 2=save 3=seq> <offset> -> LTFT/LTT learned-region + SD totem bench access" },
    { "datalog",       Args::II,   cmd_datalog,       "datalog <0|1> [hz] -> force the SD datalogger on/off (bench; ignores the rpm gate)" },
    { "datarec",       Args::NONE, cmd_datarec,       "datarec -> append one telemetry record to the active datalog file" },
    { "canbaud",       Args::II,   cmd_canbaud,       "canbaud <bus 0..1> <kbit> -> re-init that bus at a different CAN bitrate" },
    { "canloop",       Args::II,   cmd_canloop,       "canloop <bus 0..1> <0|1> -> hardware CAN loopback (bench OBD tests; deaf to the wire while on)" },
    { "uptime",        Args::NONE, cmd_uptime,        "uptime -> seconds since boot" },
    { "frame",         Args::NONE, cmd_frame,         "frame -> where the 1 kHz engine frame goes, module by module" },
    { "cpu",           Args::OPT_S, cmd_cpu,          "cpu [reset] -> MCU load, the engine frame's own load and where the time goes per task; reset clears the frame peak" },
    { "knock",         Args::II,   cmd_knock,         "knock <ch 0..1> [freq Hz] -> burst + band level" },
    { "dtc",           Args::OPT_S, cmd_dtc,          "dtc [clear] -> counts, or wipe the table (read it via 'G')" },
};

} // namespace

void Cli::register_default_commands() {
    static bool done = false;
    if (done) return;
    done = true;
    Cli::register_commands(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}
