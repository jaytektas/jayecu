#pragma once
#include <cstdint>

// Abstract SD-card config store.
// Implementations: SdConfigStore (FatFS), RamConfigStore (test stub).
class IConfigStore {
public:
    virtual ~IConfigStore() = default;

    // True if the underlying storage is currently accessible.
    virtual bool available() const = 0;

    // Read config bytes into out (up to max_len). Sets *len_out to actual bytes read.
    // Returns false if unavailable or file not found.
    virtual bool read(uint8_t* out, uint32_t max_len, uint32_t* len_out) = 0;

    // Read up to max_len bytes starting at `offset` in the record. Sets *len_out to the actual
    // count. Returns false if unavailable, absent, or the offset is past the end.
    //
    // BOOT NEEDS THIS BECAUSE THE RECORD DOES NOT FIT IN A BUFFER. The config is 141 KB and there is
    // no spare RAM to stage it in, so the SD path reads the 12-byte header first and then streams the
    // payload straight into its final destination. read() from the start cannot express that, and the
    // 2 KB landing buffer that stood in for it made every engine-running burn unrecoverable at the
    // next boot — the header validated, the payload could not possibly be there, and the ECU fell
    // back to the older flash bank without a word. See StorageManager::boot_arbitrate.
    virtual bool read_at(uint32_t offset, uint8_t* out, uint32_t max_len, uint32_t* len_out) = 0;

    // Write len bytes from data. Overwrites any existing config.
    virtual bool write(const uint8_t* data, uint32_t len) = 0;

    // Write a header immediately followed by a payload as a single file, so the
    // caller never has to glue [header][payload] into a staging buffer. The
    // payload is written straight from its source (e.g. &g_config).
    virtual bool write_record(const uint8_t* header, uint32_t header_len,
                              const uint8_t* payload, uint32_t payload_len) = 0;
};
