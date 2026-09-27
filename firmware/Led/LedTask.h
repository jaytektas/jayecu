#pragma once

#include "LedStatus.h"

// ---------------------------------------------------------------------------
// LedTask — 10ms-tick state machines for four status LEDs.
//
// Call tick() from a dedicated FreeRTOS task every 10ms.
// Reads from the LedStatus reference passed to tick() (typically g_led_status).
//
// LED assignments (Jaytek V1, active LOW):
//   RUNNING  PA8   green   — engine sync rate indicator
//   WARNING  PC7   orange  — OBD-I flash-out of the worst active Level 1-2 DTC P-code
//   ERROR    PC8   red     — OBD-I flash-out of the worst active Level 3 DTC P-code
//   COMMS    PC9   blue    — USB comms heartbeat / connection solid
//
// OBD-I flash-out: each P-code digit is flashed GM-style — a digit 1-9 is that many
// short pulses, a digit 0 is one LONG pulse, with a gap between digits and a longer
// gap before the code repeats. The four nibbles of the code are the four digits
// (P0117 = 0x0117). Read the number off the dash, look it up in a P-code list.
// ---------------------------------------------------------------------------

class LedTask {
public:
    void tick(const LedStatus& s);

private:
    // ---- Per-LED P-code OBD-I flasher state machine -----------------------
    struct CodeFlasher {
        void    (*set_fn)(bool on) = nullptr;  // platform LED set function
        uint16_t code  = 0;   // code currently being flashed (0 = idle/off)
        uint8_t  index = 0;   // which code in the band list we're cycling on
        uint8_t  digit = 0;   // which digit (0..3, most-significant first)
        uint8_t  pulse = 0;   // short pulses emitted so far in this digit
        uint8_t  phase = 0;   // 0=pulse-on 1=inter-pulse-off 2=digit-gap 3=code-gap
        uint16_t timer = 0;
    };

    // Cycle through `codes` (contiguous active codes, 0 = end), flashing each in turn.
    void tick_code(CodeFlasher& f, const uint16_t* codes, uint8_t max);

    // ---- RUNNING LED state ------------------------------------------------
    uint16_t run_timer_  = 0;
    uint8_t  run_burst_  = 0;

    // ---- COMMS LED state --------------------------------------------------
    uint16_t comms_timer_ = 0;

    // ---- Fault flashers — function pointers set at construction -----------
    CodeFlasher warn_led_;   // orange: Level 1-2 code
    CodeFlasher err_led_;    // red:    Level 3 code

public:
    LedTask();
};
