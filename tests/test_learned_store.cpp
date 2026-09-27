#include "test_helpers.h"
#include "../firmware/Storage/LearnedHeader.h"
#include <cstdint>
#include <vector>
#include <cstring>

// Pure-logic test for the learned-region totem codec (LearnedHeader) and the newest-valid pick that
// LearnedStore::load performs. No FatFs / SD — the file glue is bench-verified; this pins the integrity
// and rotation-arbitration logic that decides which totem survives a torn write or a layout change.

static constexpr uint32_t LAYOUT = 0xABCD1234u;   // stand-in for JAYECU_LAYOUT_HASH

// Emulates LearnedStore::load's pick: among header-ok slots, verify crc newest-first; first valid wins.
struct Slot { LearnedHeader h; std::vector<uint8_t> pay; };
static int pick_newest_valid(Slot* s, int n, uint32_t len, uint32_t layout) {
    int order[8];
    for (int i = 0; i < n; ++i) order[i] = i;
    for (int a = 1; a < n; ++a) {                       // insertion sort by sequence, newest first
        int k = order[a], j = a - 1;
        while (j >= 0 && s[order[j]].h.sequence < s[k].h.sequence) { order[j + 1] = order[j]; --j; }
        order[j + 1] = k;
    }
    for (int a = 0; a < n; ++a) {
        int i = order[a];
        if (learned_header_ok(s[i].h, len, layout) &&
            learned_totem_valid(s[i].h, s[i].pay.data(), len, layout))
            return i;
    }
    return -1;
}

int main() {
    SECTION("header crc round-trips; payload + header tamper are detected");
    {
        uint8_t payload[256];
        for (int i = 0; i < 256; ++i) payload[i] = static_cast<uint8_t>(i * 7 + 3);
        LearnedHeader h;
        learned_fill_header(h, 42, sizeof(payload), LAYOUT, 0x20260726u, payload);
        CHECK(h.magic == LEARNED_MAGIC);
        CHECK(learned_header_ok(h, sizeof(payload), LAYOUT));
        CHECK(learned_totem_valid(h, payload, sizeof(payload), LAYOUT));

        payload[100] ^= 0x01;                                                  // flip a payload byte
        CHECK(!learned_totem_valid(h, payload, sizeof(payload), LAYOUT));
        payload[100] ^= 0x01;                                                  // restore
        CHECK(learned_totem_valid(h, payload, sizeof(payload), LAYOUT));

        LearnedHeader h2 = h; h2.sequence = 99;                                // tamper a crc-covered field
        CHECK(!learned_totem_valid(h2, payload, sizeof(payload), LAYOUT));
    }

    SECTION("wrong layout_hash or region length is rejected");
    {
        uint8_t payload[64] = {};
        LearnedHeader h;
        learned_fill_header(h, 1, sizeof(payload), LAYOUT, 0, payload);
        CHECK(!learned_header_ok(h, sizeof(payload), LAYOUT + 1));   // totem from a different firmware layout
        CHECK(!learned_header_ok(h, sizeof(payload) + 4, LAYOUT));   // region resized
        CHECK(!learned_totem_valid(h, payload, sizeof(payload), LAYOUT + 1));

        LearnedHeader blank{};                                       // an unwritten/zeroed slot -> magic 0
        CHECK(!learned_header_ok(blank, sizeof(payload), LAYOUT));
    }

    SECTION("newest valid totem wins; a torn newest falls back to the previous");
    {
        auto make = [](uint32_t seq, uint8_t fill) {
            Slot s; s.pay.assign(32, fill);
            learned_fill_header(s.h, seq, 32, LAYOUT, 0, s.pay.data());
            return s;
        };
        Slot slots[3] = { make(7, 0x11), make(9, 0x99), make(8, 0x88) };
        CHECK(pick_newest_valid(slots, 3, 32, LAYOUT) == 1);   // seq 9 is newest

        slots[1].pay[5] ^= 0xFF;                                // corrupt the seq-9 payload (torn write)
        CHECK(pick_newest_valid(slots, 3, 32, LAYOUT) == 2);   // fall back to seq 8

        for (auto& s : slots) s.pay[0] ^= 0xFF;                 // corrupt every slot
        CHECK(pick_newest_valid(slots, 3, 32, LAYOUT) == -1);  // nothing valid -> caller stays neutral
    }

    SECTION("a totem written while the region is still learning validates on load (S1)");
    {
        // The engine keeps writing LTFT/LTT while a flush is on the card. The old writer CRC'd the region
        // first and wrote it after, so any cell that moved in between made the totem invalid.
        std::vector<uint8_t> region(100, 0x40);
        std::vector<uint8_t> file(sizeof(LearnedHeader) + region.size(), 0);
        uint8_t buf[16];
        int calls = 0;
        LearnedHeader h;
        const bool ok = learned_write_stable(h, 5, 100, LAYOUT, 0, region.data(), buf, sizeof(buf),
            [&](uint32_t off, const uint8_t* d, uint32_t n) {
                for (uint32_t i = 0; i < n; ++i) file[off + i] = d[i];
                // the engine learns between every chunk, all over the region
                for (auto& b : region) ++b;
                ++calls;
                return true;
            });
        CHECK(ok);
        CHECK(calls == 8);                                     // 7 payload chunks + the header, last
        LearnedHeader back;
        std::memcpy(&back, file.data(), sizeof(back));
        CHECK(learned_totem_valid(back, file.data() + sizeof(LearnedHeader), 100, LAYOUT));

        // Torn before the header landed: the slot's header is not a valid one for this payload.
        std::vector<uint8_t> torn(file.size(), 0);
        int left = 3;
        CHECK(!learned_write_stable(h, 6, 100, LAYOUT, 0, region.data(), buf, sizeof(buf),
            [&](uint32_t off, const uint8_t* d, uint32_t n) {
                if (left-- == 0) return false;
                for (uint32_t i = 0; i < n; ++i) torn[off + i] = d[i];
                return true;
            }));
        std::memcpy(&back, torn.data(), sizeof(back));
        CHECK(!learned_totem_valid(back, torn.data() + sizeof(LearnedHeader), 100, LAYOUT));
    }

    return test_summary();
}
