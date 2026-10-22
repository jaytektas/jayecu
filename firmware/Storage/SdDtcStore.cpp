#include "SdDtcStore.h"
#include "../Platform/stm32f7xx/SdVolume.h"
#include "../Diagnostics/TextLog.h"
#include <cstring>

SdDtcStore::SdDtcStore(SdArbitrator& arb, const char* path)
    : arb_(arb), path_(path) {}

// "0:/dtc.bin" -> "0:/dtc.new"
static void temp_path_of(const char* path, char* out, uint32_t cap) {
    uint32_t n = 0;
    while (path[n] && n < cap - 5) { out[n] = path[n]; ++n; }
    out[n] = 0;
    char* dot = nullptr;
    for (char* c = out; *c; ++c) if (*c == '.') dot = c;
    if (!dot) dot = out + n;
    std::memcpy(dot, ".new", 5);
}

uint16_t SdDtcStore::boot_restore(DtcManager& dtc) {
    uint16_t prev_boot = 0;

    if (sd_volume_ensure(arb_)) {
        // A cut between save()'s unlink and rename leaves only the complete .new: put it in place.
        char tmp[40];
        temp_path_of(path_, tmp, sizeof(tmp));
        FILINFO fi;
        if (f_stat(path_, &fi) == FR_NO_FILE && f_stat(tmp, &fi) == FR_OK) f_rename(tmp, path_);

        FIL fil;
        if (f_open(&fil, path_, FA_READ) == FR_OK) {
            // One byte more than the largest store: a longer file then reads as a length no store has,
            // and restore()'s exact-length check refuses it rather than loading a store-sized prefix.
            static uint8_t buf[DtcManager::store_max_bytes() + 1u];
            const uint32_t size = static_cast<uint32_t>(f_size(&fil));
            UINT read = 0;
            const FRESULT fr = f_read(&fil, buf, sizeof(buf), &read);
            f_close(&fil);
            if (fr == FR_OK) {
                // STORED-not-active. A store this firmware did not write loads nothing and the first
                // save overwrites it (restore()); the console says so, because the codes it seemed to
                // hold are not coming back.
                if (dtc.restore(buf, read) == DtcManager::StoreLoad::Rejected)
                    g_text_log.printf("DTC store %s (%lu bytes) was not written by this firmware: "
                                      "discarded, starting with no stored codes\n",
                                      path_, static_cast<unsigned long>(size));
                else
                    prev_boot = DtcManager::image_boot_id(buf, read);
            }
        }
    }

    // New session id: previous + 1, wrapping past 0 (0 means "no/invalid image").
    uint16_t boot = static_cast<uint16_t>(prev_boot + 1);
    if (boot == 0) boot = 1;
    dtc.init(boot);   // boot_id only; restore() (above) already repopulated the table
    return boot;
}

// NEVER TRUNCATE THE ONLY COPY. This used to open dtc.bin with CREATE_ALWAYS and write over it, so a
// power cut mid-save — and key-off is exactly when codes change and the power goes — left a torn image
// that restore() rejects: the whole fault history gone, not just the newest change. Now the image goes
// to a temp file, is synced, and is swapped in. The snapshot is taken under the table's lock
// (snapshot_if_dirty), so it is never a record copied half-way through an update either.
bool SdDtcStore::save_if_dirty(DtcManager& dtc) {
    static uint8_t buf[DtcManager::store_max_bytes()];
    if (!sd_volume_ensure(arb_)) return false;               // stays dirty: try again later
    const uint32_t n = dtc.snapshot_if_dirty(buf, sizeof(buf));
    if (n == 0) return true;                                 // nothing changed

    char tmp[40];
    temp_path_of(path_, tmp, sizeof(tmp));
    FIL  fil;
    UINT written = 0;
    bool ok = f_open(&fil, tmp, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK;
    if (ok) {
        ok = f_write(&fil, buf, n, &written) == FR_OK && written == n;
        ok = (f_sync(&fil) == FR_OK) && ok;
        f_close(&fil);
    }
    if (ok) {
        const FRESULT ru = f_unlink(path_);
        ok = (ru == FR_OK || ru == FR_NO_FILE) && f_rename(tmp, path_) == FR_OK;
    }
    if (!ok) dtc.mark_dirty();                               // not on the card: keep it owed
    return ok;
}
