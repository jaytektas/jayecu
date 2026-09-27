#pragma once
#include "IFlashBank.h"
#include "ConfigHeader.h"

// Wraps one IFlashBank with header-based config storage.
// Layout: [ConfigHeader 24 bytes][payload data_len bytes]
class ConfigBank {
public:
    explicit ConfigBank(IFlashBank& flash);

    // Read and validate the header magic + CRC.
    // Fills header_out if non-null. Returns false on blank/corrupt bank.
    bool is_valid(ConfigHeader* header_out = nullptr) const;

    // Read header only (no CRC check of payload).
    bool read_header(ConfigHeader& out) const;

    // Load payload into buf (up to max_len bytes).
    // Returns bytes read, or 0 on failure.
    // Fills header_out if non-null.
    uint32_t load(uint8_t* buf, uint32_t max_len,
                  ConfigHeader* header_out = nullptr) const;

    // Pointer to the payload IN PLACE. STM32 flash is memory-mapped (Stm32FlashBank::read is a
    // memcpy from a mapped address), so a validated bank can be read where it lies — no staging
    // copy. Valid only after is_valid() has passed; null otherwise. This is what lets boot load a
    // tune with ONE copy straight into g_config.
    const uint8_t* payload() const;

    // Erase bank, write header + payload, compute CRC in header.
    // sequence and source are recorded in the header.
    bool save(const uint8_t* data, uint32_t data_len,
              uint32_t sequence, ConfigSource source,
              uint32_t timestamp = 0);

    // Read back payload and compare byte-for-byte.
    // Use after save() to confirm flash programming succeeded.
    bool verify(const uint8_t* expected, uint32_t expected_len) const;

    // Monotonic sequence of the stored config, or 0 if invalid.
    uint32_t sequence() const;

private:
    IFlashBank& flash_;

    uint32_t compute_crc(const ConfigHeader& hdr, const uint8_t* payload) const;
};
