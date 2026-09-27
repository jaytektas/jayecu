#include "LearnedStore.h"
#include "SdCardSpi.h"
#include "SdVolume.h"
#include "ff.h"
#include "../../../generated/schema_meta.h"   // JAYECU_LAYOUT_HASH — rejects a totem from another layout

extern "C" uint32_t get_fattime(void);   // packed FAT datetime from the RTC (0 if no RTC) — informational stamp

// 8.3 names only (FF_USE_LFN=0): "learnN.jlt" → stem 6 chars + ext 3 = valid.
static const char* totem_path(uint8_t slot) {
    switch (slot) {
        case 0:  return "0:/learn0.jlt";
        case 1:  return "0:/learn1.jlt";
        default: return "0:/learn2.jlt";
    }
}

// The volume mount is shared global state: see SdVolume.h.
bool LearnedStore::sd_ready() const {
    return sd_volume_ensure(arb_);
}

bool LearnedStore::read_header(uint8_t slot, LearnedHeader& h) const {
    FIL f;
    if (f_open(&f, totem_path(slot), FA_READ) != FR_OK) return false;
    UINT br = 0;
    const FRESULT r = f_read(&f, &h, sizeof(h), &br);
    f_close(&f);
    return r == FR_OK && br == sizeof(h);
}

bool LearnedStore::read_totem(uint8_t slot, LearnedHeader& h, uint8_t* region, uint32_t len) const {
    FIL f;
    if (f_open(&f, totem_path(slot), FA_READ) != FR_OK) return false;
    UINT bh = 0, bp = 0;
    const FRESULT rh = f_read(&f, &h, sizeof(h), &bh);
    const FRESULT rp = (rh == FR_OK && bh == sizeof(h)) ? f_read(&f, region, len, &bp) : FR_DISK_ERR;
    f_close(&f);
    return rh == FR_OK && bh == sizeof(h) && rp == FR_OK && bp == len;
}

bool LearnedStore::write_totem(uint8_t slot, uint32_t sequence,
                               const uint8_t* region, uint32_t len) const {
    FIL f;
    // OPEN_ALWAYS + rewrite in place: reuse the totem file's existing clusters instead of CREATE_ALWAYS's
    // truncate + reallocation, so a flush doesn't churn the FAT/directory every 30 s (less card wear,
    // shorter mutex hold). The totem is a fixed size for a given firmware, so the file length is stable.
    if (f_open(&f, totem_path(slot), FA_OPEN_ALWAYS | FA_WRITE) != FR_OK) return false;

    // Small chunks through one static buffer. FatFs takes its per-volume mutex for EACH f_write and
    // releases it between calls, so a concurrent datalogger slips its records in between our chunks.
    // The buffer is also what makes the CRC honest: see learned_write_stable().
    constexpr uint32_t CHUNK = 2048u;
    static uint8_t s_chunk[CHUNK];
    LearnedHeader h;
    bool ok = learned_write_stable(h, sequence, len, JAYECU_LAYOUT_HASH, get_fattime(), region,
                                   s_chunk, CHUNK,
        [&f](uint32_t off, const uint8_t* d, uint32_t n) {
            UINT bw = 0;
            return f_lseek(&f, off) == FR_OK && f_write(&f, d, n, &bw) == FR_OK && bw == n;
        });
    ok = (f_sync(&f) == FR_OK) && ok;   // one FAT/dir flush makes the totem durable + visible
    f_close(&f);
    return ok;
}

bool LearnedStore::load(uint8_t* region, uint32_t len) {
    if (!sd_ready()) return false;

    // Pass 1: read just the headers; collect the header-valid slots.
    struct Cand { uint32_t seq; uint8_t slot; } cand[NUM_TOTEMS];
    uint8_t n = 0;
    for (uint8_t s = 0; s < NUM_TOTEMS; ++s) {
        LearnedHeader h;
        if (read_header(s, h) && learned_header_ok(h, len, JAYECU_LAYOUT_HASH))
            cand[n++] = { h.sequence, s };
    }
    // Order by sequence, newest first (n ≤ 3 → insertion sort).
    for (uint8_t i = 1; i < n; ++i) {
        Cand k = cand[i]; int j = i - 1;
        while (j >= 0 && cand[j].seq < k.seq) { cand[j + 1] = cand[j]; --j; }
        cand[j + 1] = k;
    }
    // Pass 2: read the full payload of each candidate newest-first; the first whose crc validates wins. A
    // torn/corrupt newest totem is skipped and the previous good one is used.
    for (uint8_t i = 0; i < n; ++i) {
        LearnedHeader h;
        if (read_totem(cand[i].slot, h, region, len) &&
            learned_totem_valid(h, region, len, JAYECU_LAYOUT_HASH)) {
            next_seq_  = h.sequence + 1;
            next_slot_ = (cand[i].slot + 1) % NUM_TOTEMS;   // don't overwrite the totem we just loaded
            return true;
        }
    }
    for (uint32_t i = 0; i < len; ++i) region[i] = 0;   // nothing valid → neutral region
    return false;
}

bool LearnedStore::save(const uint8_t* region, uint32_t len) {
    if (!sd_ready()) return false;
    if (!write_totem(next_slot_, next_seq_, region, len)) return false;
    ++next_seq_;
    next_slot_ = (next_slot_ + 1) % NUM_TOTEMS;
    return true;
}
