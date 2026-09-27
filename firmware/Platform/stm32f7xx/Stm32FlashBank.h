#pragma once
#include "../../Storage/IFlashBank.h"
#include <cstdint>

// IFlashBank backed by one STM32F767 internal flash sector.
// Construct with the sector number, base address, and size — see platform_config.h.
class Stm32FlashBank : public IFlashBank {
public:
    Stm32FlashBank(uint32_t sector, uint32_t base_addr, uint32_t size_bytes);

    bool erase() override;
    bool program(uint32_t offset, const uint8_t* data, uint32_t len) override;

    // Flash is memory-mapped — read is a plain memcpy from the mapped address.
    bool read(uint32_t offset, uint8_t* out, uint32_t len) const override;

    const uint8_t* data() const override { return reinterpret_cast<const uint8_t*>(base_); }

    uint32_t capacity() const override { return size_; }

private:
    uint32_t sector_;
    uint32_t base_;
    uint32_t size_;
};
