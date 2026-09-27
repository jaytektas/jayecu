#pragma once
#include "../../Storage/IFlashBank.h"
#include <cstring>

// RAM-backed IFlashBank for native unit tests.
// Flash semantics: erased = 0xFF, program can only clear bits (not set them),
// but for simplicity this stub allows overwrite — sufficient for testing
// the storage layer logic without needing to model NOR flash restrictions.
template<uint32_t CAPACITY>
class FlashBankStub : public IFlashBank {
public:
    FlashBankStub()  { memset(mem_, 0xFF, CAPACITY); }

    void factory_reset() { memset(mem_, 0xFF, CAPACITY); }

    bool erase() override {
        memset(mem_, 0xFF, CAPACITY);
        return true;
    }

    bool program(uint32_t offset, const uint8_t* data, uint32_t len) override {
        if (offset + len > CAPACITY) return false;
        memcpy(mem_ + offset, data, len);
        if (flip_next_payload_ && offset > 0) { mem_[offset] ^= 0x01; flip_next_payload_ = false; }
        return true;
    }

    bool read(uint32_t offset, uint8_t* out, uint32_t len) const override {
        if (offset + len > CAPACITY) return false;
        memcpy(out, mem_ + offset, len);
        return true;
    }

    const uint8_t* data() const override { return mem_; }

    uint32_t capacity() const override { return CAPACITY; }

    // Direct access for inspection in tests.
    const uint8_t* raw() const { return mem_; }

    // Inject a fault: corrupt a single byte after write (tests CRC detection).
    void corrupt_byte(uint32_t offset, uint8_t mask = 0xFF) {
        if (offset < CAPACITY) mem_[offset] ^= mask;
    }

    // Inject a fault: the next payload program (offset > 0) reports success but lands one bit
    // wrong — a program that "worked" and then fails its read-back verify.
    void flip_next_payload() { flip_next_payload_ = true; }

private:
    bool flip_next_payload_ = false;
    alignas(8) uint8_t mem_[CAPACITY];
};
