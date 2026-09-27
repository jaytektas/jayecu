// C-callable shim so diskio.c (plain C) can ask whether the ECU owns the SD card.
// diskio.c includes this declaration: extern bool SdArbitrator_EcuHasCard(void);
//
// The SdArbitrator singleton is owned by the platform init code. Declare it
// extern here and define it wherever platform startup creates the instance.
#include "../../Storage/SdArbitrator.h"
#include "../platform_hal.h"   // platform_get_tick_ms()

extern SdArbitrator g_sd_arb;   // defined in platform startup / main

extern "C" bool SdArbitrator_EcuHasCard(void) {
    return g_sd_arb.ecu_has_card();
}

extern "C" bool SdArbitrator_UsbHasCard(void) {
    return g_sd_arb.usb_has_card();
}

// Beacon from the MSC write callback so the arbitrator can wait for the host to
// go block-write quiet before withdrawing the card on key-on.
extern "C" void SdArbitrator_UsbNoteWrite(void) {
    g_sd_arb.usb_note_write(platform_get_tick_ms());
}

// Ownership is purely key-driven now — USB enumeration / bus resets must NOT
// change it (that flipping was the source of mid-session host read errors).
extern "C" void SdArbitrator_OnUsbConfigured(void) { /* no-op (key-driven) */ }
extern "C" void SdArbitrator_OnUsbReset(void)      { /* no-op (key-driven) */ }

// ECU writer cleanup protocol — exposed to comms (SdProtocol.cpp, plain calls).
extern "C" bool SdArbitrator_WritesAllowed(void)   { return g_sd_arb.writes_allowed(); }
extern "C" void SdArbitrator_WriterBusy(uint8_t w) { g_sd_arb.writer_busy((SdWriter)w); }
extern "C" void SdArbitrator_WriterIdle(uint8_t w) { g_sd_arb.writer_idle((SdWriter)w); }
