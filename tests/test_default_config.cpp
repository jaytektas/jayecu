// Guards the GENERATED default config (generated/default_config.cpp → g_config):
//   1. its size matches the externally-shared JAYECU_CONFIG_SIZE,
//   2. layout_hash == JAYECU_LAYOUT_HASH (so the boot gate accepts it),
//   3. it round-trips through a real ConfigBank CRC-valid and byte-identical.
//
// (3) is the important one: the firmware boots the compiled default into g_config when
// flash has no valid tune. If that default could not be stored + reloaded CRC-clean, a
// burn of the factory tune would fail validation and the controller would come up on a
// garbage RAM tune. The other unit tests only ever exercise tiny synthetic payloads;
// this one uses the REAL production-sized config.
#include "test_helpers.h"
#include "ecu_config.h"
#include "../firmware/Storage/ConfigBank.h"
#include "../firmware/Storage/Crc32.h"
#include "../firmware/Platform/stubs/FlashBankStub.h"
#include <cstring>

extern "C" const unsigned char DEFAULT_TUNE_IMAGE[];   // generated/default_tune.cpp
extern "C" const unsigned       DEFAULT_TUNE_IMAGE_LEN;

int main() {
    fprintf(stdout, "=== default_config ===\n");

    SECTION("size matches the shared JAYECU_CONFIG_SIZE");
    {
        CHECK(sizeof(EcuConfig) == JAYECU_CONFIG_SIZE);
    }

    SECTION("layout_hash matches the firmware's compiled JAYECU_LAYOUT_HASH");
    {
        CHECK(g_config.layout_hash == JAYECU_LAYOUT_HASH);   // boot gate would accept this tune
    }

    SECTION("default config round-trips through ConfigBank CRC-valid + byte-identical");
    {
        FlashBankStub<262144> flash;                  // 256K = real bank size (config grew past 16K)
        ConfigBank bank(flash);
        CHECK(bank.save(reinterpret_cast<const uint8_t*>(&g_config),
                        sizeof(g_config), 1, ConfigSource::FLASH_BANK_A));
        CHECK(bank.is_valid());                       // header magic + CRC over the real config

        EcuConfig back;
        memset(&back, 0xAB, sizeof(back));            // poison so a short/failed load is caught
        uint32_t n = bank.load(reinterpret_cast<uint8_t*>(&back), sizeof(back));
        CHECK(n == sizeof(g_config));
        CHECK(memcmp(&back, &g_config, sizeof(g_config)) == 0);
    }

    // The codegen-emitted bank-A image (generated/default_tune.cpp): the bytes the linker
    // drops at 0x08180000. A freshly-flashed board must find it CRC-valid and load it as
    // the default tune — and it must be byte-identical to the compiled g_config (so the
    // image's serializer can't drift from the struct).
    SECTION("emitted default-tune image is a CRC-valid bank that loads to g_config");
    {
        CHECK(DEFAULT_TUNE_IMAGE_LEN == 24 + sizeof(EcuConfig));   // header + payload

        FlashBankStub<262144> flash;                  // 256K = real bank size
        CHECK(flash.program(0, DEFAULT_TUNE_IMAGE, DEFAULT_TUNE_IMAGE_LEN));
        ConfigBank bank(flash);
        CHECK(bank.is_valid());                       // CRC computed at build time is correct

        EcuConfig back;
        memset(&back, 0xAB, sizeof(back));
        uint32_t n = bank.load(reinterpret_cast<uint8_t*>(&back), sizeof(back));
        CHECK(n == sizeof(g_config));
        CHECK(memcmp(&back, &g_config, sizeof(g_config)) == 0);
        CHECK(back.layout_hash == JAYECU_LAYOUT_HASH);   // field 0 = the layout hash
    }

    return test_summary();
}
