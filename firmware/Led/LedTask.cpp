#include "LedTask.h"
#include "../Platform/platform_hal.h"

// ---------------------------------------------------------------------------
// Global LED status — written by EngineTask + usbd_cdc_if, read here.
// ---------------------------------------------------------------------------
LedStatus g_led_status;

LedTask::LedTask() {
    warn_led_.set_fn = platform_set_led_warning;
    err_led_.set_fn  = platform_set_led_error;
}

// Timing constants (all in 10ms ticks)
static constexpr uint16_t COMMS_BEAT_ON     = 10;   // 100ms heartbeat on
static constexpr uint16_t COMMS_BEAT_PERIOD = 100;  // 1000ms total period

static constexpr uint16_t RUN_CRANK_PERIOD  = 50;   // 500ms total (2 Hz)
static constexpr uint16_t RUN_CRANK_ON      = 25;   // 250ms on

static constexpr uint16_t RUN_HALF_ON       = 10;   // 100ms burst flash
static constexpr uint16_t RUN_HALF_OFF      = 10;
static constexpr uint16_t RUN_HALF_PAUSE    = 60;   // 600ms after triple burst

// ---------------------------------------------------------------------------
// OBD-I P-code flasher. Flashes `code`'s four HEX nibbles (most-significant first):
// a digit 1-F = that many short pulses (so A-F flash 10-15 pulses); a digit 0 = one
// LONG pulse. The full hex space is supported — P-codes are NOT restricted to decimal
// digits; any standard/mfr code (e.g. P164F, P06A0) flashes verbatim. A scan tool reads
// the exact code over OBD anyway; the LED is the rough at-the-car readout. Gaps:
// inter-pulse, digit gap, then a code gap before the whole code repeats. code = 0
// → LED off. A new code restarts the sequence from the first digit.
//
// Phases: 0 = pulse-on, 1 = inter-pulse-off, 2 = digit gap, 3 = code gap.
// ---------------------------------------------------------------------------
static constexpr uint16_t PULSE_SHORT_ON = 25;   // 250ms short pulse (digit 1-F = that many)
static constexpr uint16_t PULSE_LONG_ON  = 70;   // 700ms long pulse (digit 0)
static constexpr uint16_t PULSE_OFF      = 25;   // 250ms inter-pulse gap
static constexpr uint16_t DIGIT_GAP      = 100;  // 1000ms between digits
static constexpr uint16_t CODE_GAP       = 250;  // 2500ms before the code repeats

void LedTask::tick_code(CodeFlasher& f, const uint16_t* codes, uint8_t max) {
    uint8_t count = 0;                             // contiguous active codes (0 = end of list)
    while (count < max && codes[count] != 0) count++;
    if (count == 0) { f.set_fn(false); f.code = 0; f.index = 0; return; }
    if (f.index >= count) f.index = 0;             // list shrank → wrap

    const uint16_t code = codes[f.index];
    if (code != f.code) {                          // a different code now at this slot → (re)start
        f.code = code; f.digit = 0; f.pulse = 0; f.phase = 0; f.timer = 0;
    }

    const uint8_t nib     = static_cast<uint8_t>((f.code >> (4u * (3u - f.digit))) & 0xFu);
    const bool    is_zero = (nib == 0);
    f.timer++;

    switch (f.phase) {
    case 0: // PULSE ON (long for 0, short for 1-9)
        f.set_fn(true);
        if (f.timer >= (is_zero ? PULSE_LONG_ON : PULSE_SHORT_ON)) {
            f.timer = 0;
            f.phase = is_zero ? 2 : 1;             // 0 = one long pulse → straight to digit gap
        }
        break;
    case 1: // INTER-PULSE OFF
        f.set_fn(false);
        if (f.timer >= PULSE_OFF) {
            f.timer = 0;
            f.phase = (++f.pulse >= nib) ? 2 : 0;  // emitted all pulses for this digit?
        }
        break;
    case 2: // DIGIT GAP
        f.set_fn(false);
        if (f.timer >= DIGIT_GAP) {
            f.timer = 0; f.pulse = 0;
            if (++f.digit >= 4) { f.digit = 0; f.phase = 3; }   // all four digits done
            else                { f.phase = 0; }
        }
        break;
    case 3: // CODE GAP — pause, then advance to the NEXT active code in the band (cycle)
    default:
        f.set_fn(false);
        if (f.timer >= CODE_GAP) {
            f.timer = 0; f.digit = 0; f.pulse = 0; f.phase = 0;
            f.index = static_cast<uint8_t>((f.index + 1u) % count);  // cycle (wraps to itself if 1)
            f.code  = 0;   // force re-pick of codes[index] on the next tick
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Main tick — call every 10ms from a FreeRTOS task
// ---------------------------------------------------------------------------
void LedTask::tick(const LedStatus& s) {

    // ---- RUNNING (green, PA8) — engine sync / crank state ---------------
    {
        // Turning at all: teeth arriving, or a speed. rpm alone is 0 until sync, so on its own it made
        // the cranking-without-sync blink below unreachable.
        const bool any_rpm = s.turning || (s.rpm > 50.0f);

        if (!any_rpm && s.sync_level == SyncLevel::NONE) {
            // Engine stopped — LED off
            platform_set_led_running(false);
            run_timer_ = 0; run_burst_ = 0;

        } else if (s.sync_level == SyncLevel::NONE) {
            // Cranking, no sync — 2 Hz blink (250ms on / 250ms off)
            if (++run_timer_ >= RUN_CRANK_PERIOD) run_timer_ = 0;
            platform_set_led_running(run_timer_ < RUN_CRANK_ON);

        } else if (s.sync_level == SyncLevel::CRANK) {
            // Half sync — triple flash burst then 600ms pause
            const uint16_t burst_cycle = 3u * (RUN_HALF_ON + RUN_HALF_OFF) + RUN_HALF_PAUSE;
            if (++run_timer_ >= burst_cycle) run_timer_ = 0;
            bool on = false;
            for (uint8_t i = 0; i < 3; i++) {
                const uint16_t s0 = static_cast<uint16_t>(i * (RUN_HALF_ON + RUN_HALF_OFF));
                if (run_timer_ >= s0 && run_timer_ < s0 + RUN_HALF_ON) { on = true; break; }
            }
            platform_set_led_running(on);

        } else {
            // Full sync (PHASE) — solid on
            platform_set_led_running(true);
            run_timer_ = 0;
        }
    }

    // ---- COMMS (blue, PC9) — USB heartbeat / connection ------------------
    {
        if (++comms_timer_ >= COMMS_BEAT_PERIOD) comms_timer_ = 0;

        if (s.usb_connected) {
            if (s.usb_rx_pulse) {
                // Brief 50ms off-pulse when data flowing (every other 5-tick window)
                platform_led_connected((comms_timer_ % 5u) != 0u);
            } else {
                platform_led_connected(true);
            }
        } else {
            // Slow heartbeat: 100ms on / 900ms off
            platform_led_connected(comms_timer_ < COMMS_BEAT_ON);
        }
    }

    // ---- WARNING (orange, PC7) — cycle every active Level 1-2 DTC, OBD-I flash-out ----
    tick_code(warn_led_, s.dtc_warn_codes, LedStatus::DTC_LED_MAX);

    // ---- ERROR (red, PC8) — cycle every active Level 3 DTC, OBD-I flash-out ----
    tick_code(err_led_, s.dtc_fault_codes, LedStatus::DTC_LED_MAX);
}
