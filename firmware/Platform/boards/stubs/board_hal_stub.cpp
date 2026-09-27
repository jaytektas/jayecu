#include "../../board_hal.h"

// Stub board HAL — returns safe/plausible values with no hardware dependencies.
// Used by native unit-test builds that link platform_stub.

void     board_init()                                          {}

uint16_t board_read_adc_voltage(uint8_t /*index*/)            { return 0u; }
uint16_t board_read_adc_temp(uint8_t /*index*/)               { return 0u; }
uint16_t board_knock_capture(uint8_t, uint16_t*, uint16_t)   { return 0u; }
float    board_knock_sample_rate(void)                       { return 281250.0f; }

// The non-blocking burst. A host build has no ADC and no DMA completion, so the arm is REFUSED —
// a caller that treats a false return as "no samples this window" behaves correctly, whereas a
// stub that accepted the arm would leave a worker waiting on a callback that can never fire.
bool     board_knock_start_burst(uint8_t, uint16_t*, uint16_t) { return false; }
void     board_knock_end_burst(void)                           {}
void     board_knock_register_complete(void (*)(void))         {}

bool     board_read_digital(uint8_t /*index*/)                { return false; }

bool     board_set_ls(uint8_t /*index*/, bool /*on*/)         { return true; }
bool     board_set_hs(uint8_t /*index*/, bool /*on*/)         { return true; }
bool     board_set_ign(uint8_t /*index*/, bool /*on*/)        { return true; }

bool     board_hbridge_set_enable(uint8_t /*i*/, bool /*en*/)     { return true; }
bool     board_hbridge_set_direction(uint8_t /*i*/, bool /*fwd*/) { return true; }

void     board_set_led(uint8_t /*index*/, bool /*on*/)        {}

bool     board_read_power_good(uint8_t /*index*/)             { return true; }

float    board_read_baro_kpa()                                { return 101.3f; }
float    board_read_baro_temp_c()                             { return 23.5f; }
void     board_baro_service()                                 { }
bool     board_baro_valid()                                   { return true; }

void     board_device_uid(uint8_t uid[12])                   { for (int i = 0; i < 12; ++i) uid[i] = static_cast<uint8_t>(0xA0 + i); }
