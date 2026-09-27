#include "test_helpers.h"
#include "../firmware/Storage/StorageManager.h"
#include "../firmware/Storage/Crc32.h"
#include "../firmware/Platform/stubs/FlashBankStub.h"
#include "../firmware/Platform/stubs/RamConfigStore.h"
#include <cstring>

static constexpr uint32_t BANK_SIZE = 8192;
using Stub = FlashBankStub<BANK_SIZE>;

// Payload byte 0..3 is layout_hash (field 0 of EcuConfig), so the fixtures carry one.
static constexpr uint32_t HASH      = 0x04030201;   // little-endian of {01,02,03,04}
static constexpr uint32_t HASH_OLD  = 0xDEADBEEF;
// Where boot streams an SD record to. In the firmware this is g_config itself; a record cannot
// be staged anywhere else, so every caller has one and the tests must too.
static uint8_t sd_dest[8192];

static const uint8_t CFG_V1[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
static const uint8_t CFG_V2[] = { 0x01, 0x02, 0x03, 0x04, 0x0E };
static const uint8_t CFG_V3[] = { 0x01, 0x02, 0x03, 0x04, 0x50 };
// Same size, DIFFERENT layout — what an older firmware's tune looks like to this one.
static const uint8_t CFG_STALE[] = { 0xEF, 0xBE, 0xAD, 0xDE, 0x99 };

int main() {
    fprintf(stdout, "=== StorageManager ===\n");

    // -----------------------------------------------------------------------
    SECTION("a stale-layout tune LOSES to a valid one, even with a higher sequence");
    {
        // Exactly what was found on the bench: bank B held a complete, CRC-valid record from an
        // older layout at sequence 6, while bank A's seeded default is always sequence 1. Arbitrating
        // on sequence alone made B win and then get rejected, so the ECU ran compiled defaults with
        // no stored tune — permanently, because every burn outranks bank A forever.
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        SdArbitrator arb;
        StorageManager sm(ba, bb, nullptr, arb);

        ba.save(CFG_V1,    sizeof(CFG_V1),    1, ConfigSource::FLASH_BANK_A);   // this layout, seq 1
        bb.save(CFG_STALE, sizeof(CFG_STALE), 6, ConfigSource::FLASH_BANK_A);   // OLD layout, seq 6

        const uint8_t* p = nullptr;
        uint32_t len = 0;
        CHECK(sm.boot_arbitrate(HASH, sizeof(CFG_V1), &p, &len, sd_dest, sizeof(sd_dest)) == BootResult::OK_FLASH_A);
        CHECK(p != nullptr);
        CHECK(memcmp(p, CFG_V1, sizeof(CFG_V1)) == 0);        // the VALID one, not the newer one
    }

    SECTION("a wrong-SIZE record is not a candidate either");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        SdArbitrator arb;
        StorageManager sm(ba, bb, nullptr, arb);
        ba.save(CFG_V1, sizeof(CFG_V1), 1, ConfigSource::FLASH_BANK_A);
        // Right layout hash, but a payload of a different length — a config from a firmware whose
        // struct grew. This is what the bench's bank B actually was: len 105075 vs today's 111173.
        const uint8_t big[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
        bb.save(big, sizeof(big), 9, ConfigSource::FLASH_BANK_A);

        const uint8_t* p = nullptr;
        uint32_t len = 0;
        CHECK(sm.boot_arbitrate(HASH, sizeof(CFG_V1), &p, &len, sd_dest, sizeof(sd_dest)) == BootResult::OK_FLASH_A);
        CHECK(len == sizeof(CFG_V1));
    }

    SECTION("when NOTHING matches this layout -> OK_DEFAULT, not a stale winner");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        SdArbitrator arb;
        StorageManager sm(ba, bb, nullptr, arb);
        ba.save(CFG_STALE, sizeof(CFG_STALE), 3, ConfigSource::FLASH_BANK_A);
        bb.save(CFG_STALE, sizeof(CFG_STALE), 8, ConfigSource::FLASH_BANK_A);

        const uint8_t* p = nullptr;
        uint32_t len = 0;
        CHECK(sm.boot_arbitrate(HASH, sizeof(CFG_V1), &p, &len, sd_dest, sizeof(sd_dest)) == BootResult::OK_DEFAULT);
        CHECK(p == nullptr);
    }

    SECTION("among VALID candidates, highest sequence still wins");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        SdArbitrator arb;
        StorageManager sm(ba, bb, nullptr, arb);
        ba.save(CFG_V1, sizeof(CFG_V1), 2, ConfigSource::FLASH_BANK_A);
        bb.save(CFG_V2, sizeof(CFG_V2), 7, ConfigSource::FLASH_BANK_A);

        const uint8_t* p = nullptr;
        uint32_t len = 0;
        CHECK(sm.boot_arbitrate(HASH, sizeof(CFG_V1), &p, &len, sd_dest, sizeof(sd_dest)) == BootResult::OK_FLASH_B);
        CHECK(memcmp(p, CFG_V2, sizeof(CFG_V2)) == 0);
    }

    // -----------------------------------------------------------------------
    SECTION("boot hands back a POINTER into the bank — no staging copy");
    {
        // The whole point of the pointer API: flash is memory-mapped, the CRC already runs in
        // place, so the caller copies ONCE into its own config. There used to be a full EcuConfig
        // staging buffer in DTCM for a candidate that had already been validated.
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        SdArbitrator arb;
        StorageManager sm(ba, bb, nullptr, arb);
        ba.save(CFG_V1, sizeof(CFG_V1), 7, ConfigSource::FLASH_BANK_A);

        const uint8_t* p = nullptr;
        uint32_t len = 0;
        CHECK(sm.boot_arbitrate(HASH, sizeof(CFG_V1), &p, &len, sd_dest, sizeof(sd_dest)) == BootResult::OK_FLASH_A);
        CHECK(len == sizeof(CFG_V1));
        CHECK(p != nullptr);
        // It points AT the bank's own storage, past the header — not at a copy.
        CHECK(p == fa.raw() + 24);
        CHECK(memcmp(p, CFG_V1, sizeof(CFG_V1)) == 0);
    }

    SECTION("no valid tune -> null pointer, so the caller keeps its defaults");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        SdArbitrator arb;
        StorageManager sm(ba, bb, nullptr, arb);

        const uint8_t* p = reinterpret_cast<const uint8_t*>(0xDEADBEEF);
        uint32_t len = 99;
        CHECK(sm.boot_arbitrate(HASH, sizeof(CFG_V1), &p, &len, sd_dest, sizeof(sd_dest)) == BootResult::OK_DEFAULT);
        CHECK(p == nullptr);      // cleared, so a caller cannot copy from a stale pointer
        CHECK(len == 0);
    }

    SECTION("a corrupt payload is refused even when the header claims a sequence");
    {
        // The header carries the sequence, the CRC covers the payload. Corrupting the payload must
        // make the bank lose, not hand back a pointer to bad bytes.
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        SdArbitrator arb;
        StorageManager sm(ba, bb, nullptr, arb);
        ba.save(CFG_V1, sizeof(CFG_V1), 5, ConfigSource::FLASH_BANK_A);
        fa.corrupt_byte(24);                     // flip a payload byte behind the CRC's back

        const uint8_t* p = nullptr;
        uint32_t len = 0;
        const BootResult r = sm.boot_arbitrate(HASH, sizeof(CFG_V1), &p, &len, sd_dest, sizeof(sd_dest));
        CHECK(r == BootResult::OK_DEFAULT);
        CHECK(p == nullptr);
    }


    // -----------------------------------------------------------------------
    SECTION("boot — both banks blank → OK_DEFAULT");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        SdArbitrator arb;
        // arb defaults to ECU ownership — config sync may read the card.
        StorageManager sm(ba, bb, nullptr, arb);

        const uint8_t* out = nullptr;
        uint32_t len = 0;
        CHECK(sm.boot_arbitrate(HASH, sizeof(CFG_V1), &out, &len, sd_dest, sizeof(sd_dest)) == BootResult::OK_DEFAULT);
        CHECK(len == 0);
        CHECK(sm.next_sequence() == 1);
    }

    SECTION("boot — bank A only valid → OK_FLASH_A");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        ba.save(CFG_V1, sizeof(CFG_V1), 3, ConfigSource::FLASH_BANK_A);

        SdArbitrator arb;
        // arb defaults to ECU ownership — config sync may read the card.
        StorageManager sm(ba, bb, nullptr, arb);

        const uint8_t* out = nullptr;
        uint32_t len = 0;
        CHECK(sm.boot_arbitrate(HASH, sizeof(CFG_V1), &out, &len, sd_dest, sizeof(sd_dest)) == BootResult::OK_FLASH_A);
        CHECK(len == sizeof(CFG_V1));
        CHECK(memcmp(out, CFG_V1, sizeof(CFG_V1)) == 0);
        CHECK(sm.next_sequence() == 4);
    }

    SECTION("boot — bank B higher sequence → OK_FLASH_B");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        ba.save(CFG_V1, sizeof(CFG_V1), 2, ConfigSource::FLASH_BANK_A);
        bb.save(CFG_V2, sizeof(CFG_V2), 5, ConfigSource::FLASH_BANK_B);

        SdArbitrator arb;
        // arb defaults to ECU ownership — config sync may read the card.
        StorageManager sm(ba, bb, nullptr, arb);

        const uint8_t* out = nullptr;
        uint32_t len = 0;
        CHECK(sm.boot_arbitrate(HASH, sizeof(CFG_V1), &out, &len, sd_dest, sizeof(sd_dest)) == BootResult::OK_FLASH_B);
        CHECK(len == sizeof(CFG_V2));
        CHECK(memcmp(out, CFG_V2, sizeof(CFG_V2)) == 0);
    }

    SECTION("boot — SD higher sequence → OK_SD; inactive bank gets SD data");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        // Flash has sequence 2
        ba.save(CFG_V1, sizeof(CFG_V1), 2, ConfigSource::FLASH_BANK_A);
        bb.save(CFG_V2, sizeof(CFG_V2), 3, ConfigSource::FLASH_BANK_B);

        // SD has sequence 10 — wins
        RamConfigStore<> sd;
        // SD file must contain a ConfigHeader followed by payload.
        // Build the raw bytes the same way StorageManager reads them.
        ConfigHeader sd_hdr{};
        sd_hdr.magic    = CONFIG_MAGIC;
        sd_hdr.sequence = 10;
        sd_hdr.data_len = sizeof(CFG_V3);
        sd_hdr.source   = static_cast<uint8_t>(ConfigSource::SD_CARD);
        // CRC covers [magic,seq,data_len] + payload
        {
            uint32_t crc = crc32_init();
            crc = crc32_update(crc, reinterpret_cast<const uint8_t*>(&sd_hdr), 12);
            crc = crc32_update(crc, CFG_V3, sizeof(CFG_V3));
            sd_hdr.crc32 = crc32_final(crc);
        }
        uint8_t sd_file[sizeof(ConfigHeader) + sizeof(CFG_V3)];
        memcpy(sd_file, &sd_hdr, sizeof(ConfigHeader));
        memcpy(sd_file + sizeof(ConfigHeader), CFG_V3, sizeof(CFG_V3));
        sd.write(sd_file, sizeof(sd_file));

        SdArbitrator arb;
        // arb defaults to ECU ownership — config sync may read the card.
        StorageManager sm(ba, bb, &sd, arb);

        const uint8_t* out = nullptr;
        uint32_t len = 0;
        BootResult result = sm.boot_arbitrate(HASH, sizeof(CFG_V1), &out, &len, sd_dest, sizeof(sd_dest));
        CHECK(result == BootResult::OK_SD);
        CHECK(len == sizeof(CFG_V3));
        CHECK(memcmp(out, CFG_V3, sizeof(CFG_V3)) == 0);
        CHECK(sm.next_sequence() == 11);

        // The inactive bank (bank A — lower seq) should now hold SD data.
        CHECK(ba.sequence() == 10);
        CHECK(ba.verify(CFG_V3, sizeof(CFG_V3)));
    }

    SECTION("boot — SD wins but the flash copy fails verify -> still runs the SD tune");
    {
        // S7. The SD record was read in full and CRC'd; only its copy into the inactive bank went
        // wrong. That used to return ERR_VERIFY with no payload and boot came up with NO TUNE.
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        ba.save(CFG_V1, sizeof(CFG_V1), 2, ConfigSource::FLASH_BANK_A);
        bb.save(CFG_V2, sizeof(CFG_V2), 3, ConfigSource::FLASH_BANK_B);

        RamConfigStore<> sd;
        ConfigHeader h{};
        h.magic = CONFIG_MAGIC; h.sequence = 10; h.data_len = sizeof(CFG_V3);
        h.source = static_cast<uint8_t>(ConfigSource::SD_CARD);
        {
            uint32_t crc = crc32_init();
            crc = crc32_update(crc, reinterpret_cast<const uint8_t*>(&h), 12);
            crc = crc32_update(crc, CFG_V3, sizeof(CFG_V3));
            h.crc32 = crc32_final(crc);
        }
        sd.write_record(reinterpret_cast<const uint8_t*>(&h), sizeof(h), CFG_V3, sizeof(CFG_V3));

        SdArbitrator arb;
        StorageManager sm(ba, bb, &sd, arb);
        fa.flip_next_payload();                          // bank A is the inactive target

        const uint8_t* out = nullptr;
        uint32_t len = 0;
        CHECK(sm.boot_arbitrate(HASH, sizeof(CFG_V1), &out, &len, sd_dest, sizeof(sd_dest)) == BootResult::OK_SD);
        CHECK(out != nullptr);
        CHECK(len == sizeof(CFG_V3));
        CHECK(memcmp(out, CFG_V3, sizeof(CFG_V3)) == 0);  // the SD tune, from its destination
        CHECK(sm.next_sequence() == 11);
        CHECK(bb.verify(CFG_V2, sizeof(CFG_V2)));         // the good bank is untouched

        // The next burn goes to the damaged bank, not over the good one.
        CHECK(sm.burn(CFG_V1, sizeof(CFG_V1), false, 0) == BurnResult::OK_FLASH);
        CHECK(ba.sequence() == 11);
        CHECK(bb.verify(CFG_V2, sizeof(CFG_V2)));
    }

    // THE REGRESSION. Every SD test above uses a FIVE BYTE config, so all of them fit the 2048-byte
    // landing buffer boot used to read into — which is precisely why nothing caught that a real
    // 141 KB config could never fit it. The header validated, the SD record won the sequence
    // comparison, the length check could not pass, and the ECU fell back to the older flash bank
    // without a word. On the bench that silently discarded every burn taken while the engine was
    // running. Any record LARGER than that buffer reproduces it; this one is 6 KB.
    SECTION("boot — an SD record too big for any landing buffer still loads (streamed)");
    {
        static uint8_t BIG[6144];
        for (size_t i = 0; i < sizeof(BIG); ++i) BIG[i] = static_cast<uint8_t>(i * 7 + 3);
        memcpy(BIG, &HASH, sizeof(HASH));            // layout_hash is field 0 and the gate reads it

        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        ba.save(CFG_V1, sizeof(CFG_V1), 9, ConfigSource::FLASH_BANK_A, 0);

        RamConfigStore<sizeof(BIG) + sizeof(ConfigHeader)> sd;
        ConfigHeader h{};
        h.magic = CONFIG_MAGIC; h.sequence = 20; h.data_len = sizeof(BIG);
        h.source = static_cast<uint8_t>(ConfigSource::SD_CARD); h.timestamp = 0;
        {
            uint32_t crc = crc32_init();
            crc = crc32_update(crc, reinterpret_cast<const uint8_t*>(&h), 12);
            crc = crc32_update(crc, BIG, sizeof(BIG));
            h.crc32 = crc32_final(crc);
        }
        sd.write_record(reinterpret_cast<const uint8_t*>(&h), sizeof(h), BIG, sizeof(BIG));

        SdArbitrator arb;
        StorageManager sm(ba, bb, &sd, arb);
        static uint8_t dest[sizeof(BIG)];
        const uint8_t* out = nullptr; uint32_t len = 0;
        const BootResult r = sm.boot_arbitrate(HASH, sizeof(BIG), &out, &len, dest, sizeof(dest));
        CHECK(r == BootResult::OK_SD);
        CHECK(len == sizeof(BIG));
        CHECK(out && memcmp(out, BIG, sizeof(BIG)) == 0);
        // Retained into a flash bank. Bank A takes it: candidate_seq() rejects A's existing record
        // because its data_len is CFG_V1's, not BIG's, so both banks score 0 and A is the inactive one.
        CHECK(ba.verify(BIG, sizeof(BIG)));
    }

    // A record whose CRC does not cover its payload must not be taken. The old path could not check
    // this at all — it validated a header against bytes it had never read.
    SECTION("boot — an SD record with a bad payload CRC is refused, not loaded");
    {
        static uint8_t BIG[4096];
        for (size_t i = 0; i < sizeof(BIG); ++i) BIG[i] = static_cast<uint8_t>(i);
        memcpy(BIG, &HASH, sizeof(HASH));

        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        ba.save(CFG_V1, sizeof(CFG_V1), 9, ConfigSource::FLASH_BANK_A, 0);

        RamConfigStore<sizeof(BIG) + sizeof(ConfigHeader)> sd;
        ConfigHeader h{};
        h.magic = CONFIG_MAGIC; h.sequence = 20; h.data_len = sizeof(BIG);
        h.source = static_cast<uint8_t>(ConfigSource::SD_CARD); h.timestamp = 0;
        h.crc32 = 0xDEADBEEF;                        // wrong on purpose
        sd.write_record(reinterpret_cast<const uint8_t*>(&h), sizeof(h), BIG, sizeof(BIG));

        SdArbitrator arb;
        StorageManager sm(ba, bb, &sd, arb);
        static uint8_t dest[sizeof(BIG)];
        const uint8_t* out = nullptr; uint32_t len = 0;
        const BootResult r = sm.boot_arbitrate(HASH, sizeof(BIG), &out, &len, dest, sizeof(dest));
        CHECK(r != BootResult::OK_SD);               // fell back rather than trusting it
    }

    // WHO GETS DISABLED, AND WHEN. The pre-stall hook exists to park the engine's outputs before a
    // flash write blocks all instruction fetch for ~1-2 s. It must fire on exactly the burns that
    // stall — engine running, no SD to defer to — and on no others, because closing the firing gate
    // stops the engine and doing that to a burn that was never going to stall is its own fault.
    SECTION("burn — the engine is disabled ONLY for a burn that will stall the CPU");
    {
        static int hook_calls = 0;
        auto reset_hook = []{ hook_calls = 0; };

        // (a) engine running, NO SD -> flash path, stalls, hook must fire
        {
            Stub fa, fb; ConfigBank ba(fa), bb(fb);
            SdArbitrator arb;
            StorageManager sm(ba, bb, nullptr, arb);
            sm.set_prestall_hook([](void*){ ++hook_calls; }, nullptr);
            reset_hook();
            sm.burn(CFG_V1, sizeof(CFG_V1), /*engine_running=*/true, 0);
            CHECK(hook_calls == 1);
        }
        // (b) engine STOPPED, no SD -> flash path too, but nothing is turning: leave it alone
        {
            Stub fa, fb; ConfigBank ba(fa), bb(fb);
            SdArbitrator arb;
            StorageManager sm(ba, bb, nullptr, arb);
            sm.set_prestall_hook([](void*){ ++hook_calls; }, nullptr);
            reset_hook();
            sm.burn(CFG_V1, sizeof(CFG_V1), /*engine_running=*/false, 0);
            CHECK(hook_calls == 0);
        }
        // (c) engine running WITH an SD card -> the record goes to SD, no flash write, no stall
        {
            Stub fa, fb; ConfigBank ba(fa), bb(fb);
            RamConfigStore<> sd; SdArbitrator arb;
            StorageManager sm(ba, bb, &sd, arb);
            sm.set_prestall_hook([](void*){ ++hook_calls; }, nullptr);
            reset_hook();
            const BurnResult r = sm.burn(CFG_V1, sizeof(CFG_V1), /*engine_running=*/true, 0);
            CHECK(r == BurnResult::OK_SD);
            CHECK(hook_calls == 0);
        }
    }

    SECTION("boot — SD unavailable → falls back to best flash");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        ba.save(CFG_V1, sizeof(CFG_V1), 7, ConfigSource::FLASH_BANK_A);

        RamConfigStore<> sd;
        sd.set_available(false);

        SdArbitrator arb;
        // arb defaults to ECU ownership — config sync may read the card.
        StorageManager sm(ba, bb, &sd, arb);

        const uint8_t* out = nullptr;
        uint32_t len = 0;
        CHECK(sm.boot_arbitrate(HASH, sizeof(CFG_V1), &out, &len, sd_dest, sizeof(sd_dest)) == BootResult::OK_FLASH_A);
        CHECK(len == sizeof(CFG_V1));
    }

    // -----------------------------------------------------------------------
    SECTION("burn — engine stopped → OK_FLASH, swaps active bank");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        ba.save(CFG_V1, sizeof(CFG_V1), 1, ConfigSource::FLASH_BANK_A);

        SdArbitrator arb;
        // arb defaults to ECU ownership — config sync may read the card.
        StorageManager sm(ba, bb, nullptr, arb);
        const uint8_t* tmp = nullptr; uint32_t l = 0;
        sm.boot_arbitrate(HASH, sizeof(CFG_V1), &tmp, &l, sd_dest, sizeof(sd_dest));   // establishes active=A, next_seq=2

        // Engine stopped (USB/bench): straight to flash.
        CHECK(sm.burn(CFG_V2, sizeof(CFG_V2), false, 5000) == BurnResult::OK_FLASH);
        // Inactive (B) should now hold V2 with sequence 2.
        CHECK(bb.sequence() == 2);
        CHECK(bb.verify(CFG_V2, sizeof(CFG_V2)));
    }

    SECTION("burn — engine running + SD → OK_SD, no flash write");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);

        RamConfigStore<> sd;
        SdArbitrator arb;
        // arb defaults to ECU ownership — config sync may read the card.
        StorageManager sm(ba, bb, &sd, arb);
        const uint8_t* tmp = nullptr; uint32_t l = 0;
        sm.boot_arbitrate(HASH, sizeof(CFG_V1), &tmp, &l, sd_dest, sizeof(sd_dest));

        CHECK(sm.burn(CFG_V1, sizeof(CFG_V1), true, 1000) == BurnResult::OK_SD);
        // SD file contains ConfigHeader + payload
        CHECK(sd.stored_len() == sizeof(ConfigHeader) + sizeof(CFG_V1));
        // Flash should not have been touched (it reconciles from SD at boot).
        CHECK(ba.sequence() == 0);
        CHECK(bb.sequence() == 0);
    }

    SECTION("burn — engine stopped + SD present → flash now, SD untouched");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);

        RamConfigStore<> sd;
        SdArbitrator arb;
        // arb defaults to ECU ownership — config sync may read the card.
        StorageManager sm(ba, bb, &sd, arb);
        const uint8_t* tmp = nullptr; uint32_t l = 0;
        sm.boot_arbitrate(HASH, sizeof(CFG_V1), &tmp, &l, sd_dest, sizeof(sd_dest));

        // Stopped → no glitch risk, so go straight to flash even with a card;
        // no point detouring through SD to reconcile later.
        CHECK(sm.burn(CFG_V2, sizeof(CFG_V2), false, 1000) == BurnResult::OK_FLASH);
        CHECK(bb.sequence() == 1);
        CHECK(bb.verify(CFG_V2, sizeof(CFG_V2)));
        CHECK(sd.stored_len() == 0);   // SD not written
    }

    SECTION("burn — engine running, no SD → flash now (accept glitch)");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);

        SdArbitrator arb;
        // arb defaults to ECU ownership — config sync may read the card.
        StorageManager sm(ba, bb, nullptr, arb);
        const uint8_t* tmp = nullptr; uint32_t l = 0;
        sm.boot_arbitrate(HASH, sizeof(CFG_V1), &tmp, &l, sd_dest, sizeof(sd_dest));

        // Running but no SD: no safe defer (engine-off == power-off), so flash
        // is written immediately.
        CHECK(sm.burn(CFG_V2, sizeof(CFG_V2), true, 1000) == BurnResult::OK_FLASH);
        CHECK(bb.sequence() == 1);
        CHECK(bb.verify(CFG_V2, sizeof(CFG_V2)));
    }

    SECTION("sequential burns increment sequence numbers");
    {
        Stub fa, fb;
        ConfigBank ba(fa), bb(fb);
        SdArbitrator arb;
        // arb defaults to ECU ownership — config sync may read the card.
        StorageManager sm(ba, bb, nullptr, arb);
        const uint8_t* tmp = nullptr; uint32_t l = 0;
        sm.boot_arbitrate(HASH, sizeof(CFG_V1), &tmp, &l, sd_dest, sizeof(sd_dest));   // both blank → DEFAULT, next_seq=1

        sm.burn(CFG_V1, sizeof(CFG_V1), false, 0); // seq=1 → bank B active
        CHECK(sm.next_sequence() == 2);
        sm.burn(CFG_V2, sizeof(CFG_V2), false, 0); // seq=2 → bank A active
        CHECK(sm.next_sequence() == 3);

        // Bank A should now hold V2 seq=2, bank B holds V1 seq=1
        CHECK(ba.sequence() == 2);
        CHECK(bb.sequence() == 1);
    }

    return test_summary();
}
