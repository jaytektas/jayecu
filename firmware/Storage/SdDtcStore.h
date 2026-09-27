#pragma once

#include "SdArbitrator.h"
#include "../Diagnostics/DtcManager.h"
#include "ff.h"

// ---------------------------------------------------------------------------
// SdDtcStore — SD persistence for the unified DTC table.
//
// Writes the DtcManager's serialized image (a single flat blob, magic+version+
// boot_id+records) to a fixed file, and restores it on boot. Unlike the
// append-only fault log, this is a whole-image rewrite: the table is small
// (<= image_max_bytes()) and we only persist the current set, not history.
//
// boot_id (the power-cycle counter) lives in the image header. boot_restore()
// reads the previous value, increments it for this session, and init()s the
// manager with it — so every record stamped this run carries a fresh boot_id
// even across power loss.
//
// Thread safety: the snapshot is taken under DtcManager's lock. Driven from the housekeeping task
// (the SD writer), never from an ISR or EngineTask.
// ---------------------------------------------------------------------------
class SdDtcStore {
public:
    explicit SdDtcStore(SdArbitrator& arb, const char* path = "0:/dtc.bin");

    // Boot path: read the image (if any), restore the table STORED-not-active,
    // and init the manager with (stored boot_id + 1). Always init()s the manager
    // even with no card / no image (cold start → boot_id 1, empty table).
    // Returns the boot_id assigned to this session.
    uint16_t boot_restore(DtcManager& dtc);

    // Write the table image to SD if it changed since the last save (temp file + swap, see the .cpp).
    // Returns false if SD is unavailable or the write fails; the table then stays dirty.
    bool save_if_dirty(DtcManager& dtc);

    // True if the SD card is accessible.
    bool available() const { return arb_.ecu_has_card(); }

private:
    SdArbitrator& arb_;
    const char*   path_;
};
