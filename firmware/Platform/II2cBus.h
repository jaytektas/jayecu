#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// II2cBus — register read/write on an I2C bus, with no MCU header in sight.
//
// It exists so a PART driver (see Platform/devices/) can be written once against the
// part's datasheet rather than once per silicon. The on-board environmental sensor was
// the case that forced it: jaytek carries an LPS22HB on I2C1 and proteus an LPS25HB on
// I2C2, which is two different parts on two different buses — and the driver for both
// was inline in one board's HAL.
//
// `dev_addr` is the 8-BIT address (7-bit address already shifted left by one), which is
// what the ST HAL takes and what every LPS datasheet quotes as the write address.
// ---------------------------------------------------------------------------
class II2cBus {
public:
    virtual ~II2cBus() = default;

    // Read `len` bytes starting at register `reg`. Returns false on NAK/timeout/bus error.
    // Whether the device auto-increments across a multi-byte read is the DEVICE's business:
    // some parts need a flag bit set in the sub-address, and that belongs in the part driver,
    // not here.
    [[nodiscard]] virtual bool read_reg(uint8_t dev_addr, uint8_t reg,
                                        uint8_t* buf, uint8_t len) noexcept = 0;

    // Write one register. Returns false on NAK/timeout/bus error.
    [[nodiscard]] virtual bool write_reg(uint8_t dev_addr, uint8_t reg, uint8_t value) noexcept = 0;
};
