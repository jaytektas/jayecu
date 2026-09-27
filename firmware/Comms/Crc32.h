#pragma once

#include <cstdint>
#include <cstddef>

namespace Comms {

// Standard reflected CRC32 (Ethernet/IEEE 802.3): poly 0x04C11DB7, init 0xFFFFFFFF, final XOR 0xFFFFFFFF.
class Crc32 {
public:
    // Incremental API — lets a CRC span several non-contiguous chunks
    // (e.g. a framing flag byte followed by a payload that lives elsewhere)
    // without copying them into one buffer first.
    static uint32_t init() { return 0xFFFFFFFFu; }

    static uint32_t update(uint32_t crc, const uint8_t* data, size_t length) {
        for (size_t i = 0; i < length; i++) {
            crc ^= data[i];
            for (int j = 0; j < 8; j++) {
                uint32_t mask = -(int32_t)(crc & 1);
                crc = (crc >> 1) ^ (0xEDB88320u & mask);
            }
        }
        return crc;
    }

    static uint32_t final(uint32_t crc) { return ~crc; }

    // One-shot convenience wrapper.
    static uint32_t compute(const uint8_t* data, size_t length) {
        return final(update(init(), data, length));
    }
};

} // namespace Comms
