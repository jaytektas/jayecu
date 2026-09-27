#pragma once
#include <cstdint>

// Four-byte magic: 'JECU' (JayECU config unit)
static constexpr uint32_t CONFIG_MAGIC = 0x4A454355u;

enum class ConfigSource : uint8_t {
    FLASH_BANK_A = 0,
    FLASH_BANK_B = 1,
    SD_CARD      = 2,
    DEFAULT      = 3,
};

// Stored at offset 0 in every flash bank.
// CRC32 covers this header (excluding the crc32 field itself) + the payload bytes.
#pragma pack(push, 1)
struct ConfigHeader {
    uint32_t magic;       // CONFIG_MAGIC
    uint32_t sequence;    // Monotonic counter — higher is newer
    uint32_t data_len;    // Payload bytes that follow this header
    uint32_t crc32;       // IEEE 802.3 CRC32 over [magic..data_len] + payload
    uint32_t timestamp;   // Seconds since epoch (best-effort, 0 = unknown)
    uint8_t  source;      // ConfigSource cast to uint8_t
    uint8_t  reserved[3]; // Must be zero
};
#pragma pack(pop)
static_assert(sizeof(ConfigHeader) == 24, "ConfigHeader must be 24 bytes");
