#include "test_helpers.h"
#include "../firmware/Storage/ConfigBank.h"
#include "../firmware/Storage/Crc32.h"
#include "../firmware/Platform/stubs/FlashBankStub.h"
#include <cstring>

static constexpr uint32_t BANK_SIZE = 8192;
using Stub = FlashBankStub<BANK_SIZE>;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const uint8_t PAYLOAD_A[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };
static const uint8_t PAYLOAD_B[] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };

int main() {
    fprintf(stdout, "=== ConfigBank ===\n");

    // -----------------------------------------------------------------------
    SECTION("crc32 — known vector");
    {
        // CRC32 of the ASCII string "123456789" = 0xCBF43926
        static const uint8_t msg[] = "123456789";
        CHECK(crc32_buf(msg, 9) == 0xCBF43926u);
    }

    SECTION("crc32 — empty input produces final XOR of init");
    {
        CHECK(crc32_buf(nullptr, 0) == crc32_final(crc32_init()));
    }

    // -----------------------------------------------------------------------
    SECTION("blank bank — is_valid returns false");
    {
        Stub flash;
        ConfigBank bank(flash);
        CHECK(!bank.is_valid());
        CHECK(bank.sequence() == 0);
    }

    SECTION("save + is_valid — happy path");
    {
        Stub flash;
        ConfigBank bank(flash);
        CHECK(bank.save(PAYLOAD_A, sizeof(PAYLOAD_A), 1, ConfigSource::FLASH_BANK_A));
        CHECK(bank.is_valid());
        CHECK(bank.sequence() == 1);
    }

    SECTION("save + load — data round-trips correctly");
    {
        Stub flash;
        ConfigBank bank(flash);
        bank.save(PAYLOAD_A, sizeof(PAYLOAD_A), 5, ConfigSource::SD_CARD, 123456u);

        uint8_t buf[64] = {};
        ConfigHeader hdr{};
        uint32_t n = bank.load(buf, sizeof(buf), &hdr);

        CHECK(n == sizeof(PAYLOAD_A));
        CHECK(memcmp(buf, PAYLOAD_A, sizeof(PAYLOAD_A)) == 0);
        CHECK(hdr.sequence  == 5);
        CHECK(hdr.timestamp == 123456u);
        CHECK(hdr.source    == static_cast<uint8_t>(ConfigSource::SD_CARD));
        CHECK(hdr.magic     == CONFIG_MAGIC);
    }

    SECTION("verify — passes after clean save");
    {
        Stub flash;
        ConfigBank bank(flash);
        bank.save(PAYLOAD_A, sizeof(PAYLOAD_A), 1, ConfigSource::FLASH_BANK_A);
        CHECK(bank.verify(PAYLOAD_A, sizeof(PAYLOAD_A)));
    }

    SECTION("verify — fails when data differs");
    {
        Stub flash;
        ConfigBank bank(flash);
        bank.save(PAYLOAD_A, sizeof(PAYLOAD_A), 1, ConfigSource::FLASH_BANK_A);
        CHECK(!bank.verify(PAYLOAD_B, sizeof(PAYLOAD_B)));
    }

    SECTION("is_valid — detects single-byte payload corruption");
    {
        Stub flash;
        ConfigBank bank(flash);
        bank.save(PAYLOAD_A, sizeof(PAYLOAD_A), 1, ConfigSource::FLASH_BANK_A);
        // Corrupt one byte in the payload region.
        flash.corrupt_byte(sizeof(ConfigHeader) + 2);
        CHECK(!bank.is_valid());
    }

    SECTION("is_valid — detects header magic corruption");
    {
        Stub flash;
        ConfigBank bank(flash);
        bank.save(PAYLOAD_A, sizeof(PAYLOAD_A), 1, ConfigSource::FLASH_BANK_A);
        flash.corrupt_byte(0); // First byte of magic
        CHECK(!bank.is_valid());
    }

    SECTION("save overwrites previous content");
    {
        Stub flash;
        ConfigBank bank(flash);
        bank.save(PAYLOAD_A, sizeof(PAYLOAD_A), 1, ConfigSource::FLASH_BANK_A);
        bank.save(PAYLOAD_B, sizeof(PAYLOAD_B), 2, ConfigSource::FLASH_BANK_B);

        uint8_t buf[64] = {};
        uint32_t n = bank.load(buf, sizeof(buf));
        CHECK(n == sizeof(PAYLOAD_B));
        CHECK(memcmp(buf, PAYLOAD_B, sizeof(PAYLOAD_B)) == 0);
        CHECK(bank.sequence() == 2);
    }

    SECTION("load — returns 0 when bank invalid");
    {
        Stub flash;
        ConfigBank bank(flash);
        uint8_t buf[64] = {};
        CHECK(bank.load(buf, sizeof(buf)) == 0);
    }

    SECTION("load — returns 0 when buf too small");
    {
        Stub flash;
        ConfigBank bank(flash);
        bank.save(PAYLOAD_A, sizeof(PAYLOAD_A), 1, ConfigSource::FLASH_BANK_A);
        uint8_t tiny[2] = {};
        CHECK(bank.load(tiny, sizeof(tiny)) == 0);
    }

    SECTION("save — rejects zero-length payload");
    {
        Stub flash;
        ConfigBank bank(flash);
        CHECK(!bank.save(PAYLOAD_A, 0, 1, ConfigSource::FLASH_BANK_A));
        CHECK(!bank.is_valid());
    }

    SECTION("factory_reset — bank goes back to blank");
    {
        Stub flash;
        ConfigBank bank(flash);
        bank.save(PAYLOAD_A, sizeof(PAYLOAD_A), 1, ConfigSource::FLASH_BANK_A);
        CHECK(bank.is_valid());
        flash.factory_reset();
        CHECK(!bank.is_valid());
        CHECK(bank.sequence() == 0);
    }

    return test_summary();
}
