#include "ConfigBank.h"
#include "Crc32.h"
#include "../Scheduler/Log.h"
#include <cstring>

ConfigBank::ConfigBank(IFlashBank& flash) : flash_(flash) {}

// CRC covers the header (everything before the crc32 field) + payload.
uint32_t ConfigBank::compute_crc(const ConfigHeader& hdr, const uint8_t* payload) const {
    // Bytes in header before the crc32 field: magic(4) + sequence(4) + data_len(4) = 12
    static constexpr uint32_t PRE_CRC_BYTES = 12;

    uint32_t crc = crc32_init();
    crc = crc32_update(crc, reinterpret_cast<const uint8_t*>(&hdr), PRE_CRC_BYTES);
    crc = crc32_update(crc, payload, hdr.data_len);
    return crc32_final(crc);
}

bool ConfigBank::read_header(ConfigHeader& out) const {
    return flash_.read(0, reinterpret_cast<uint8_t*>(&out), sizeof(ConfigHeader));
}

bool ConfigBank::is_valid(ConfigHeader* header_out) const {
    ConfigHeader hdr{};
    if (!read_header(hdr))           return false;
    if (hdr.magic != CONFIG_MAGIC)   return false;
    if (hdr.data_len == 0 ||
        hdr.data_len > flash_.capacity() - sizeof(ConfigHeader)) return false;

    uint32_t crc = crc32_init();
    crc = crc32_update(crc, reinterpret_cast<const uint8_t*>(&hdr), 12);
    crc = crc32_update(crc, flash_.data() + sizeof(ConfigHeader), hdr.data_len);
    if (hdr.crc32 != crc32_final(crc)) return false;

    if (header_out) *header_out = hdr;
    return true;
}

const uint8_t* ConfigBank::payload() const {
    ConfigHeader hdr{};
    if (!is_valid(&hdr)) return nullptr;
    return flash_.data() + sizeof(ConfigHeader);
}

uint32_t ConfigBank::load(uint8_t* buf, uint32_t max_len,
                          ConfigHeader* header_out) const {
    ConfigHeader hdr{};
    if (!is_valid(&hdr))             return 0;
    if (hdr.data_len > max_len)      return 0;

    if (!flash_.read(sizeof(ConfigHeader), buf, hdr.data_len)) return 0;
    if (header_out) *header_out = hdr;
    return hdr.data_len;
}

bool ConfigBank::save(const uint8_t* data, uint32_t data_len,
                      uint32_t sequence, ConfigSource source,
                      uint32_t timestamp) {
    if (!data || data_len == 0)      return false;
    if (sizeof(ConfigHeader) + data_len > flash_.capacity()) return false;

    EFI_LOG_DEBUG("burn", "ConfigBank::save seq=%lu len=%lu: erase",
                  (unsigned long)sequence, (unsigned long)data_len);
    if (!flash_.erase())             { EFI_LOG_WARN("burn", "%s", "erase FAILED"); return false; }

    ConfigHeader hdr{};
    hdr.magic      = CONFIG_MAGIC;
    hdr.sequence   = sequence;
    hdr.data_len   = data_len;
    hdr.timestamp  = timestamp;
    hdr.source     = static_cast<uint8_t>(source);
    hdr.crc32      = compute_crc(hdr, data);

    EFI_LOG_DEBUG("burn", "%s", "save: program header");
    if (!flash_.program(0, reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr))) {
        EFI_LOG_WARN("burn", "%s", "program header FAILED");
        return false;
    }
    EFI_LOG_DEBUG("burn", "%s", "save: program payload");
    if (!flash_.program(sizeof(hdr), data, data_len)) {
        EFI_LOG_WARN("burn", "%s", "program payload FAILED");
        return false;
    }

    EFI_LOG_DEBUG("burn", "%s", "save: done");
    return true;
}

bool ConfigBank::verify(const uint8_t* expected, uint32_t expected_len) const {
    ConfigHeader hdr{};
    if (!is_valid(&hdr))               return false;
    if (hdr.data_len != expected_len)  return false;
    return memcmp(flash_.data() + sizeof(ConfigHeader), expected, expected_len) == 0;
}

uint32_t ConfigBank::sequence() const {
    ConfigHeader hdr{};
    if (!is_valid(&hdr)) return 0;
    return hdr.sequence;
}
