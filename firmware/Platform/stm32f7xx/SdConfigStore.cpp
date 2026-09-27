#include "SdConfigStore.h"
#include "SdCardSpi.h"
#include "SdVolume.h"

SdConfigStore::SdConfigStore(SdArbitrator& arb, const char* path)
    : arb_(arb), path_(path) {}

bool SdConfigStore::mount() {
    return sd_volume_ensure(arb_);
}

// The mount is shared and is not ours to hold: see SdVolume.h.
bool SdConfigStore::ensure_mounted() const {
    return sd_volume_ensure(arb_);
}

bool SdConfigStore::available() const {
    if (!ensure_mounted()) return false;
    recover_temp();
    return true;
}

bool SdConfigStore::read(uint8_t* out, uint32_t max_len, uint32_t* len_out) {
    if (!available()) return false;

    FIL f;
    if (f_open(&f, path_, FA_READ) != FR_OK) return false;

    UINT bytes_read = 0;
    const FRESULT res = f_read(&f, out, max_len, &bytes_read);
    f_close(&f);

    if (res != FR_OK) return false;
    if (len_out) *len_out = bytes_read;
    return bytes_read > 0;
}

// Offset read — f_lseek then f_read. Same open/close discipline as read(); the seek is the only
// difference, and it is what lets boot take a 141 KB payload in chunks it can afford.
bool SdConfigStore::read_at(uint32_t offset, uint8_t* out, uint32_t max_len, uint32_t* len_out) {
    if (!available()) return false;

    FIL f;
    if (f_open(&f, path_, FA_READ) != FR_OK) return false;
    if (f_lseek(&f, offset) != FR_OK) { f_close(&f); return false; }

    UINT bytes_read = 0;
    const FRESULT res = f_read(&f, out, max_len, &bytes_read);
    f_close(&f);

    if (res != FR_OK) return false;
    if (len_out) *len_out = bytes_read;
    return bytes_read > 0;
}

// "0:/ecucfg.bin" -> "0:/ecucfg.new": the record is written here in full, read back, and only then
// renamed over the live file.
const char* SdConfigStore::temp_path() const {
    static char tmp[40];
    uint32_t n = 0;
    while (path_[n] && n < sizeof(tmp) - 5) { tmp[n] = path_[n]; ++n; }
    tmp[n] = 0;
    char* dot = nullptr;
    for (char* c = tmp; *c; ++c) if (*c == '.') dot = c;
    if (!dot) dot = tmp + n;
    dot[0] = '.'; dot[1] = 'n'; dot[2] = 'e'; dot[3] = 'w'; dot[4] = 0;
    return tmp;
}

// A power cut between the unlink and the rename in write_record() leaves only the .new file, which
// was complete and read back before the old one was removed. Put it in place. Boot CRCs it anyway.
void SdConfigStore::recover_temp() const {
    FILINFO fi;
    if (f_stat(path_, &fi) == FR_NO_FILE && f_stat(temp_path(), &fi) == FR_OK)
        f_rename(temp_path(), path_);
}

bool SdConfigStore::write(const uint8_t* data, uint32_t len) {
    return write_record(data, len, nullptr, 0);
}

// NEVER DESTROY THE OLD RECORD BEFORE THE NEW ONE IS SAFE. This used to open the live file with
// CREATE_ALWAYS — truncating it — and then write. A power cut or a card error part way left a torn
// record, boot rejected it on CRC, and the ECU came up on an OLDER flash tune: the running burns that
// only ever reached the card were gone. Nor was anything read back.
//
// Now: write the whole record to a temp file, sync, read it back and compare, then swap it in. Until
// the swap the old record is untouched; after it, the new one is whole.
bool SdConfigStore::write_record(const uint8_t* header, uint32_t header_len,
                                 const uint8_t* payload, uint32_t payload_len) {
    if (!available()) return false;
    const char* tmp = temp_path();

    FIL f;
    if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT wh = 0, wp = 0;
    FRESULT r = f_write(&f, header, header_len, &wh);
    if (r == FR_OK && payload_len) r = f_write(&f, payload, payload_len, &wp);
    const FRESULT rs = f_sync(&f);
    f_close(&f);
    if (r != FR_OK || rs != FR_OK || wh != header_len || wp != payload_len) {
        f_unlink(tmp);
        return false;
    }

    // Read back and compare, in small pieces.
    if (f_open(&f, tmp, FA_READ) != FR_OK) return false;
    static uint8_t s_back[512];
    bool same = true;
    const uint32_t total = header_len + payload_len;
    for (uint32_t off = 0; same && off < total; ) {
        const uint32_t n = (total - off) < sizeof(s_back) ? (total - off) : sizeof(s_back);
        UINT br = 0;
        if (f_read(&f, s_back, n, &br) != FR_OK || br != n) { same = false; break; }
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t at = off + i;
            const uint8_t want = at < header_len ? header[at] : payload[at - header_len];
            if (s_back[i] != want) { same = false; break; }
        }
        off += n;
    }
    f_close(&f);
    if (!same) { f_unlink(tmp); return false; }

    // Swap. FatFs will not rename over an existing file, so the old one goes first; recover_temp()
    // covers a cut between the two.
    const FRESULT ru = f_unlink(path_);
    if (ru != FR_OK && ru != FR_NO_FILE) return false;
    return f_rename(tmp, path_) == FR_OK;
}
