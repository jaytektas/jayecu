#pragma once

#include <cstdint>
#include <vector>

// IEEE 802.3 CRC32 (zlib-compatible) — matches the firmware's envelope CRC.
namespace crc32_ieee {

inline uint32_t compute(const char *data, int len)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < len; ++i)
        crc = table[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

inline uint32_t compute(const std::vector<uint8_t> &b)
{
    return compute(reinterpret_cast<const char *>(b.data()), static_cast<int>(b.size()));
}

} // namespace crc32_ieee
