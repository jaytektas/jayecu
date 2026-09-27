#pragma once
#include <cstdint>

// Pure virtual flash bank interface — MCU-agnostic.
// One bank = one erase unit (e.g. a single flash sector).
// All offsets are relative to the bank's base address.
class IFlashBank {
public:
    virtual ~IFlashBank() = default;

    // Erase the entire bank (fills with 0xFF).
    virtual bool erase() = 0;

    // Program len bytes from data into the bank starting at offset.
    // The region must have been erased first (caller's responsibility).
    virtual bool program(uint32_t offset, const uint8_t* data, uint32_t len) = 0;

    // Read len bytes from offset into out.
    virtual bool read(uint32_t offset, uint8_t* out, uint32_t len) const = 0;

    // Direct pointer to the bank's mapped contents, valid for capacity() bytes.
    virtual const uint8_t* data() const = 0;

    // Total usable bytes in this bank.
    virtual uint32_t capacity() const = 0;
};
