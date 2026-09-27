#include "StorageManager.h"
#include "Crc32.h"
#include "../Scheduler/Log.h"
#include <cstring>

// Chunk size for streaming the SD record at boot. NOT a landing buffer for the whole record — this
// used to be a 2048-byte array that the entire config was supposed to fit in, and the config is
// 141 KB. The header validated (magic, data_len and layout_hash all live in the first bytes), so the
// SD record won the sequence comparison, and then the length check below could never pass and the
// boot fell through to the older flash bank. Every burn taken while the engine was RUNNING goes to
// SD by design (burn() below: SD is over SPI and cannot stall the flash bus) — so every one of them
// was silently discarded at the next power-up, and nothing anywhere said so.
//
// Measured on the bench 2026-09-05, same image and same burn both ways: engine stopped persisted
// byte-for-byte, engine running lost 1217 bytes across 198 fields. It reads as a corrupted push
// rather than a lost one, because only what changed since the last engine-stopped burn reverts.
static constexpr uint32_t SD_CHUNK = 512;

StorageManager::StorageManager(ConfigBank& bank_a, ConfigBank& bank_b,
                                IConfigStore* sd, SdArbitrator& arb)
    : bank_a_(bank_a), bank_b_(bank_b), sd_(sd), arb_(arb) {}

ConfigBank& StorageManager::inactive_bank() { return active_is_a_ ? bank_b_ : bank_a_; }

// Build a self-describing config header with CRC over header(12) + payload.
// The payload itself is never copied — only referenced for the CRC.
static ConfigHeader make_header(uint32_t seq, const uint8_t* data, uint32_t len,
                                uint32_t ts) {
    ConfigHeader hdr{};
    hdr.magic     = CONFIG_MAGIC;
    hdr.sequence  = seq;
    hdr.data_len  = len;
    hdr.source    = static_cast<uint8_t>(ConfigSource::SD_CARD);
    hdr.timestamp = ts;
    uint32_t crc  = crc32_init();
    crc = crc32_update(crc, reinterpret_cast<const uint8_t*>(&hdr), 12);
    crc = crc32_update(crc, data, len);
    hdr.crc32 = crc32_final(crc);
    return hdr;
}

// A bank qualifies only if it is a record FOR THIS FIRMWARE: CRC valid (checked in place), the right
// payload length, and the right layout_hash. Returns its sequence, or 0 for "not a candidate" —
// which is the same value an erased bank reports, so the caller needs no special case.
static uint32_t candidate_seq(const ConfigBank& bank, uint32_t expect_hash, uint32_t expect_len) {
    ConfigHeader hdr{};
    if (!bank.is_valid(&hdr))        return 0;      // absent, corrupt, or wrong magic
    if (hdr.data_len != expect_len)  return 0;      // a different firmware's config size
    const uint8_t* p = bank.payload();
    if (!p)                          return 0;
    uint32_t hash = 0;
    memcpy(&hash, p, sizeof(hash));                 // layout_hash is field 0 of EcuConfig
    if (hash != expect_hash)         return 0;      // right size, wrong layout
    return hdr.sequence;
}

BootResult StorageManager::boot_arbitrate(uint32_t expect_hash, uint32_t expect_len,
                                          const uint8_t** payload_out, uint32_t* len_out,
                                          uint8_t* scratch, uint32_t scratch_len) {
    if (payload_out) *payload_out = nullptr;
    if (len_out)     *len_out = 0;
    // Sequence of each source, or 0 if it does not qualify for THIS firmware.
    const uint32_t seq_a  = candidate_seq(bank_a_, expect_hash, expect_len);
    const uint32_t seq_b  = candidate_seq(bank_b_, expect_hash, expect_len);

    // Read the SD record's HEADER only — twelve bytes plus the payload's first word, which is all the
    // gate needs (magic, size, layout_hash). The payload is not read here; it cannot be held.
    uint32_t     seq_sd = 0;
    ConfigHeader sd_hdr{};
    if (sd_ && sd_->available() && arb_.ecu_has_card()) {
        uint8_t  head[sizeof(ConfigHeader) + sizeof(uint32_t)] = {};
        uint32_t got = 0;
        if (sd_->read_at(0, head, sizeof(head), &got) && got == sizeof(head)) {
            memcpy(&sd_hdr, head, sizeof(sd_hdr));
            uint32_t sd_hash = 0;
            memcpy(&sd_hash, head + sizeof(ConfigHeader), sizeof(sd_hash));
            if (sd_hdr.magic == CONFIG_MAGIC && sd_hdr.data_len == expect_len &&
                sd_hash == expect_hash)
                seq_sd = sd_hdr.sequence;
        }
    }

    // Determine the winner.
    const uint32_t best = (seq_a > seq_b ? seq_a : seq_b);
    const bool sd_wins  = (seq_sd > 0 && seq_sd > best);

    if (sd_wins) {
        // STREAM THE PAYLOAD INTO ITS DESTINATION, then program the bank from there. `scratch` is the
        // caller's own config object — the place the bytes were going anyway — so this costs no extra
        // RAM, which is the whole reason the old code could not do it. Without a destination we cannot
        // take the SD record at all, so say so and use flash rather than pretend.
        const uint32_t plen = sd_hdr.data_len;
        if (plen == 0 || scratch == nullptr || scratch_len < plen) {
            // LOUD. Falling back silently is the exact failure this rewrite exists to remove: the SD
            // record is newer than either bank, and using flash instead means quietly serving an older
            // tune. A caller that passes no destination cannot be served, but it must be told.
            // Kept inside EFI_LOG_WARN's 96-byte buffer: the long form plus two ten-digit numbers
            // could overrun it, and the part that would have been cut is the part that says what
            // happened instead.
            EFI_LOG_WARN("boot", "SD record seq=%lu not taken: no destination (%lu B) "
                                 "-> older flash bank",
                         (unsigned long)sd_hdr.sequence, (unsigned long)plen);
            goto load_flash;
        }

        uint32_t done = 0;
        while (done < plen) {
            const uint32_t want = (plen - done) < SD_CHUNK ? (plen - done) : SD_CHUNK;
            uint32_t got = 0;
            if (!sd_->read_at(static_cast<uint32_t>(sizeof(ConfigHeader)) + done,
                              scratch + done, want, &got) || got == 0)
                goto load_flash;                      // short or failed read — the record is not usable
            done += got;
        }

        // CRC THE WHOLE RECORD before it is trusted. The old path never could: it validated a header
        // against a payload it had not read. header(12) + payload, exactly as make_header() computes it.
        {
            uint32_t crc = crc32_init();
            crc = crc32_update(crc, reinterpret_cast<const uint8_t*>(&sd_hdr), 12);
            crc = crc32_update(crc, scratch, plen);
            if (crc32_final(crc) != sd_hdr.crc32) goto load_flash;
        }

        const uint8_t* payload = scratch;

        // Pick the bank with the lower sequence as the inactive one.
        const bool a_is_inactive = (seq_a <= seq_b);
        active_is_a_ = !a_is_inactive;
        ConfigBank& target = inactive_bank();

        if (!target.save(payload, plen, sd_hdr.sequence,
                         ConfigSource::FLASH_BANK_A, sd_hdr.timestamp) ||
            !target.verify(payload, plen)) {
            // THE TUNE IS STILL GOOD — only the copy of it into flash failed (program or read-back). The SD record was read in
            // full and CRC'd above, and it is already in its destination; the bank that failed to verify
            // was the INACTIVE one, so the other bank is untouched. This used to return ERR_VERIFY with no
            // payload, and boot came up with NO TUNE at all over a copy failure. Run on the SD record,
            // leave the good bank active, and the next boot tries the copy again.
            EFI_LOG_WARN("boot", "flash copy of SD seq=%lu failed: running on the SD record",
                         (unsigned long)sd_hdr.sequence);
            active_is_a_ = !a_is_inactive;                // the untouched bank stays the active one
            next_seq_    = sd_hdr.sequence + 1;
            if (payload_out) *payload_out = payload;
            if (len_out)     *len_out = plen;
            return BootResult::OK_SD;
        }

        // Swap: the freshly programmed bank is now the active one.
        active_is_a_ = a_is_inactive;
        next_seq_ = sd_hdr.sequence + 1;

        // The SD record has been programmed into a flash bank, so the winner is now mapped like
        // any other — hand back its in-place payload.
        if (payload_out) *payload_out = target.payload();
        if (len_out)     *len_out = plen;
        return BootResult::OK_SD;
    }

load_flash:
    // Choose the qualifying bank with the higher sequence. A bank that failed the layout/size gate
    // reports 0 here, so it simply cannot win — the other bank does, even with a lower sequence.
    if (seq_a == 0 && seq_b == 0) {
        // Nothing on this ECU is a config for this firmware. next_seq_ starts at 1 so the first
        // burn supersedes bank A's seeded default (also sequence 1) on the following boot.
        if (len_out) *len_out = 0;
        next_seq_ = 1;
        return BootResult::OK_DEFAULT;
    }

    // The winning bank is memory-mapped, so hand back a pointer into it. is_valid() (inside
    // payload()) has already CRC'd it where it lies — nothing is copied here.
    ConfigHeader hdr{};
    if (seq_a >= seq_b && seq_a != 0) {
        active_is_a_ = true;
        const uint8_t* p = bank_a_.payload();
        if (!p) return BootResult::OK_DEFAULT;          // header said a sequence, CRC disagrees
        bank_a_.is_valid(&hdr);
        if (payload_out) *payload_out = p;
        if (len_out)     *len_out = hdr.data_len;
        next_seq_ = seq_a + 1;
        return BootResult::OK_FLASH_A;
    } else {
        active_is_a_ = false;
        const uint8_t* p = bank_b_.payload();
        if (!p) return BootResult::OK_DEFAULT;
        bank_b_.is_valid(&hdr);
        if (payload_out) *payload_out = p;
        if (len_out)     *len_out = hdr.data_len;
        next_seq_ = seq_b + 1;
        return BootResult::OK_FLASH_B;
    }
}

BurnResult StorageManager::burn(const uint8_t* data, uint32_t len,
                                 bool engine_running, uint32_t tick_ms) {
    EFI_LOG_DEBUG("burn", "burn() len=%lu running=%d active=%c",
                  (unsigned long)len, (int)engine_running, active_is_a_ ? 'A' : 'B');
    if (!data || len == 0) {
        EFI_LOG_WARN("burn", "%s", "burn() bad args -> ERR");
        return BurnResult::ERR;
    }

    // Engine running with an SD card: SD is over SPI, so it never stalls the
    // flash bus / instruction fetch — the only glitch-free way to persist while
    // running. Flash reconciles from SD at the next boot. Write the header then
    // the config straight from its source; no staging buffer.
    if (engine_running && sd_ && sd_->available() && arb_.ecu_has_card()) {
        const ConfigHeader hdr = make_header(next_seq_, data, len, tick_ms / 1000u);
        if (sd_->write_record(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr),
                              data, len)) {
            EFI_LOG_DEBUG("burn", "%s", "burn() -> SD (running)");
            next_seq_++;
            return BurnResult::OK_SD;
        }
        EFI_LOG_WARN("burn", "%s", "burn() SD write failed -> flash fallback");
    }

    // Otherwise write flash now:
    //   - engine stopped (USB/bench): glitch-free and immediately persistent;
    //   - engine running, no SD: the flash write blocks all instruction fetch for ~1-2 s, so DISABLE
    //     THE ENGINE FIRST. Every coil and injector edge is delivered from an ISR, and no ISR runs
    //     during the stall — a pin freezes at whatever level it held, so a burn that began mid-dwell
    //     would hold the coil energised for a thousand times its design dwell. Parking the outputs
    //     costs the engine (it is going to stop anyway: 1-2 s without spark at any rpm), and that is
    //     the point — a stopped engine is the safe state, a frozen half-scheduled one is not.
    //
    //     NOT deferred until the engine stops of its own accord. That would be right on
    //     hardware where the ECU outlives the engine; here the engine stopping IS the power going
    //     off, so a deferred burn is a lost burn — the same silent loss this area has already
    //     produced twice.
    if (engine_running && prestall_) {
        EFI_LOG_WARN("burn", "%s", "burn() with the engine running and no SD -> disabling the engine "
                                   "before the flash stall");
        prestall_(prestall_ctx_);
    }
    EFI_LOG_DEBUG("burn", "%s", "burn() -> program flash now");
    return program_inactive_and_swap(data, len, ConfigSource::FLASH_BANK_A, tick_ms);
}

BurnResult StorageManager::program_inactive_and_swap(const uint8_t* data, uint32_t len,
                                                       ConfigSource src, uint32_t tick_ms) {
    const uint32_t seq = next_seq_;
    const uint32_t ts  = tick_ms / 1000u;

    ConfigBank& target = inactive_bank();
    EFI_LOG_DEBUG("burn", "program_inactive: target=%c seq=%lu",
                  (&target == &bank_a_) ? 'A' : 'B', (unsigned long)seq);
    if (!target.save(data, len, seq, src, ts)) {
        EFI_LOG_WARN("burn", "%s", "program_inactive: save FAILED -> ERR");
        return BurnResult::ERR;
    }
    EFI_LOG_DEBUG("burn", "%s", "program_inactive: verify");
    if (!target.verify(data, len)) {
        EFI_LOG_WARN("burn", "%s", "program_inactive: verify FAILED -> ERR");
        return BurnResult::ERR;
    }

    active_is_a_ = !active_is_a_;
    next_seq_++;
    EFI_LOG_DEBUG("burn", "program_inactive: swap done, active=%c", active_is_a_ ? 'A' : 'B');

    // No SD mirror here: this path is only reached when SD is unavailable
    // (the burn() SD branch handles the card case). Flash is the system of
    // record; SD reconciliation happens via boot_arbitrate.
    return BurnResult::OK_FLASH;
}
