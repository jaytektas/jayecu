#pragma once
#include <cstdint>

// IEEE 802.3 reflected (little-endian) CRC32.
// Usage:
//   uint32_t crc = crc32_init();
//   crc = crc32_update(crc, data, len);
//   uint32_t final = crc32_final(crc);

inline uint32_t crc32_init() { return 0xFFFF'FFFFu; }
inline uint32_t crc32_final(uint32_t crc) { return crc ^ 0xFFFF'FFFFu; }

inline uint32_t crc32_update(uint32_t crc, const uint8_t* data, uint32_t len) {
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB8'8320u & ~((crc & 1u) - 1u));
    }
    return crc;
}

inline uint32_t crc32_buf(const uint8_t* data, uint32_t len) {
    return crc32_final(crc32_update(crc32_init(), data, len));
}
