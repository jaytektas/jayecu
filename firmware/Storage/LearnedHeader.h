#pragma once
#include <cstdint>
#include "Crc32.h"

// On-SD header for a learned-region "totem" file: [LearnedHeader][payload]. The exact parallel of
// ConfigHeader (a config bank), but for the RAM-backed learned region (LTFT/LTT). Totems rotate across
// a few files; the newest with a valid header AND crc wins on load, so a torn write falls back to the
// previous good totem. `layout_hash` rejects a totem written under a different firmware layout — the
// role the old BKPSRAM flash-guard played, now carried in the file header.
//
// crc32 is the LAST field so it covers EVERY preceding header byte (magic..timestamp) + the payload.

static constexpr uint32_t LEARNED_MAGIC = 0x4A4C524Eu;   // 'JLRN' (JayECU learned)

#pragma pack(push, 1)
struct LearnedHeader {
    uint32_t magic;        // LEARNED_MAGIC
    uint32_t sequence;     // monotonic — highest valid wins on load
    uint32_t data_len;     // payload bytes (== LEARNED_REGION_USED at write time)
    uint32_t layout_hash;  // JAYECU_LAYOUT_HASH — a totem from another layout is rejected
    uint32_t timestamp;    // best-effort seconds (0 = unknown); informational only
    uint32_t crc32;        // CRC32 over [magic..timestamp] (this struct minus crc32) + payload
};
#pragma pack(pop)
static_assert(sizeof(LearnedHeader) == 24, "LearnedHeader must be 24 bytes");

// Bytes of the header that the CRC covers (everything before the crc32 field).
static constexpr uint32_t LEARNED_HDR_PRE_CRC = 20u;

// CRC32 over the header's pre-crc bytes + the payload. Pure — no FatFs, host-testable.
inline uint32_t learned_compute_crc(const LearnedHeader& h, const uint8_t* payload) {
    uint32_t c = crc32_init();
    c = crc32_update(c, reinterpret_cast<const uint8_t*>(&h), LEARNED_HDR_PRE_CRC);
    c = crc32_update(c, payload, h.data_len);
    return crc32_final(c);
}

// Header is structurally acceptable for THIS firmware: right magic, right size, right layout.
inline bool learned_header_ok(const LearnedHeader& h, uint32_t expect_len, uint32_t expect_layout) {
    return h.magic == LEARNED_MAGIC && h.data_len == expect_len && h.layout_hash == expect_layout;
}

// Full validity: structurally acceptable AND the crc matches the payload.
inline bool learned_totem_valid(const LearnedHeader& h, const uint8_t* payload,
                                uint32_t expect_len, uint32_t expect_layout) {
    return learned_header_ok(h, expect_len, expect_layout) && h.crc32 == learned_compute_crc(h, payload);
}

// Stamp a header for a fresh write (computes the crc over header + payload).
inline void learned_fill_header(LearnedHeader& h, uint32_t sequence, uint32_t data_len,
                                uint32_t layout_hash, uint32_t timestamp, const uint8_t* payload) {
    h.magic       = LEARNED_MAGIC;
    h.sequence    = sequence;
    h.data_len    = data_len;
    h.layout_hash = layout_hash;
    h.timestamp   = timestamp;
    h.crc32       = 0;
    h.crc32       = learned_compute_crc(h, payload);
}

// Write a totem from a region THAT IS STILL BEING LEARNED INTO. The engine keeps updating LTFT/LTT while
// a flush is on the card, so a CRC taken over the region first and the bytes written after it do not
// match, and the totem is rejected at boot — learning was lost on exactly the drives that learned most.
// Instead each chunk is copied once into `buf`, CRC'd, and written from that copy: the CRC is over the
// bytes that actually reached the card. The payload goes first and the header LAST, so a write torn part
// way leaves the slot's old header (whose CRC no longer matches) and load() skips it.
//
// write(offset, data, n) -> bool puts n bytes at file offset `offset`. Pure — host-testable.
template <class Write>
inline bool learned_write_stable(LearnedHeader& h, uint32_t sequence, uint32_t data_len,
                                 uint32_t layout_hash, uint32_t timestamp, const uint8_t* region,
                                 uint8_t* buf, uint32_t buf_len, Write&& write) {
    if (!buf || buf_len == 0) return false;
    h.magic       = LEARNED_MAGIC;
    h.sequence    = sequence;
    h.data_len    = data_len;
    h.layout_hash = layout_hash;
    h.timestamp   = timestamp;
    h.crc32       = 0;
    uint32_t c = crc32_init();
    c = crc32_update(c, reinterpret_cast<const uint8_t*>(&h), LEARNED_HDR_PRE_CRC);
    for (uint32_t off = 0; off < data_len; off += buf_len) {
        const uint32_t n = (data_len - off < buf_len) ? (data_len - off) : buf_len;
        for (uint32_t i = 0; i < n; ++i) buf[i] = region[off + i];
        c = crc32_update(c, buf, n);
        if (!write(static_cast<uint32_t>(sizeof(LearnedHeader)) + off, buf, n)) return false;
    }
    h.crc32 = crc32_final(c);
    return write(0u, reinterpret_cast<const uint8_t*>(&h), static_cast<uint32_t>(sizeof(h)));
}
