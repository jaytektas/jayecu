#pragma once
#include <cstdint>
#include "../../Storage/SdArbitrator.h"
#include "../../Storage/LearnedHeader.h"

// Persists the RAM-backed learned region (LTFT/LTT) to rotating "totem" files on the SD card — the exact
// parallel of a config bank, but for runtime learned data instead of the tune. Each totem is
// [LearnedHeader][payload]; the newest with a valid header AND crc wins on load, so a torn write during a
// key-off just falls back to the previous good totem. The header's layout_hash rejects a totem written
// under a different firmware layout (the role the old BKPSRAM flash-guard played).
//
// Reuses the SD volume already mounted for config (StorageManager / SdConfigStore mount "0:" at boot) —
// it does NOT register its own FATFS. If the volume isn't mounted or no card is present, load()/save()
// degrade gracefully to false and the region stays neutral (re-learn each boot).
class LearnedStore {
public:
    static constexpr uint8_t NUM_TOTEMS = 3;

    explicit LearnedStore(SdArbitrator& arb) : arb_(arb) {}

    // Scan every totem, pick the newest whose header (magic + len + layout_hash) AND crc validate, and copy
    // its payload into region[0..len). Returns true if a valid totem was loaded (region overwritten with it);
    // false leaves the region zeroed (neutral). Advances the write cursor past the winner so the next save()
    // writes a DIFFERENT slot, preserving the loaded totem until a fresh one lands.
    bool load(uint8_t* region, uint32_t len);

    // Write the region to the next totem slot (round-robin) under the next sequence number. Returns true on a
    // fully-flushed, fully-written record. Caller gates on a dirty check so this runs only when the region
    // actually changed. SD is over SPI → safe to write while the engine runs (never stalls the flash bus).
    bool save(const uint8_t* region, uint32_t len);

    uint32_t next_sequence() const { return next_seq_; }

private:
    SdArbitrator& arb_;
    uint32_t next_seq_  = 1;
    uint8_t  next_slot_ = 0;

    bool sd_ready() const;
    bool read_header(uint8_t slot, LearnedHeader& h) const;
    bool read_totem(uint8_t slot, LearnedHeader& h, uint8_t* region, uint32_t len) const;
    bool write_totem(uint8_t slot, uint32_t sequence, const uint8_t* region, uint32_t len) const;
};
