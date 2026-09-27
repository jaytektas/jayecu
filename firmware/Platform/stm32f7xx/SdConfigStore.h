#pragma once
#include "../../Storage/IConfigStore.h"
#include "../../Storage/SdArbitrator.h"
#include "ff.h"

// FatFS-backed IConfigStore.
// Reads and writes a single binary file (path set in platform_config.h).
// Automatically mounts / remounts the volume as needed.
class SdConfigStore : public IConfigStore {
public:
    // FatFS here is FF_USE_LFN=0 → 8.3 names only. The old "ecu_config.bin" (10-char
    // stem) failed f_open with FR_INVALID_NAME, so SD config sync silently never worked
    // (flash was the fallback). "ecucfg" is 6 chars → valid.
    explicit SdConfigStore(SdArbitrator& arb,
                           const char* path = "0:/ecucfg.bin");

    bool available() const override;
    bool read (uint8_t* out, uint32_t max_len, uint32_t* len_out) override;
    bool read_at(uint32_t offset, uint8_t* out, uint32_t max_len, uint32_t* len_out) override;
    bool write(const uint8_t* data, uint32_t len) override;
    bool write_record(const uint8_t* header, uint32_t header_len,
                      const uint8_t* payload, uint32_t payload_len) override;

    // Call once at startup / after SdCard_Init() succeeds.
    bool mount();
    // Ensure the shared volume is mounted (SdVolume.h).
    bool ensure_mounted() const;


private:
    const char* temp_path() const;
    void recover_temp() const;

    SdArbitrator& arb_;
    const char*   path_;
};
