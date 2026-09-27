// Host test for SentDecoder — SAE J2716 SENT fast-channel decode from edge times.

#include "Sensors/SentDecoder.h"
#include <cassert>
#include <cstdio>
#include <cstdint>

// Emit the falling edges of one fast-channel frame at `tick_us` µs/tick, starting
// at time t. Frame = sync(56t) + status + 6 data + CRC. Returns the end time.
static uint32_t emit_frame(SentDecoder& d, uint32_t t, uint32_t tick_us,
                           uint8_t status, const uint8_t data[6], int crc_override = -1) {
    d.on_edge(t);                                   // sync start
    t += 56u * tick_us;                 d.on_edge(t);   // sync end
    t += (12u + status) * tick_us;      d.on_edge(t);   // status nibble
    for (int i = 0; i < 6; ++i) { t += (12u + data[i]) * tick_us; d.on_edge(t); }
    uint8_t crc = (crc_override >= 0) ? (uint8_t)crc_override
                                      : SentDecoder::crc4(data, 6);
    t += (12u + crc) * tick_us;         d.on_edge(t);   // CRC nibble
    t += 12u * tick_us;                                // (pause before next frame)
    return t;
}

int main() {
    // A 12-bit value 0xABC -> data nibbles A,B,C then 3 more (don't care for value).
    const uint8_t data[6] = {0xA, 0xB, 0xC, 0x1, 0x2, 0x3};
    const uint16_t expect = 0xABC;

    // Basic decode at the nominal 3 µs/tick.
    {
        SentDecoder d;
        uint32_t t = 100000;
        for (int f = 0; f < 3; ++f) t = emit_frame(d, t, 3, 0x0, data);
        uint16_t v = 0;
        assert(d.value(t, /*enforce_crc=*/true, v));
        assert(v == expect);
        printf("ok  3us/tick  -> 0x%03X\n", v);
    }

    // Self-clocking: a different tick time still decodes (sync calibrates it).
    {
        SentDecoder d;
        uint32_t t = 0;
        for (int f = 0; f < 3; ++f) t = emit_frame(d, t, 5, 0x7, data);
        uint16_t v = 0;
        assert(d.value(t, true, v) && v == expect);
        printf("ok  5us/tick  -> 0x%03X\n", v);
    }

    // Latest frame wins: send an old value then a new one.
    {
        SentDecoder d;
        const uint8_t d1[6] = {0x1, 0x2, 0x3, 0, 0, 0};
        const uint8_t d2[6] = {0xF, 0xE, 0xD, 0, 0, 0};
        uint32_t t = 0;
        t = emit_frame(d, t, 3, 0, d1);
        t = emit_frame(d, t, 3, 0, d2);
        uint16_t v = 0;
        assert(d.value(t, true, v) && v == 0xFED);
        printf("ok  latest frame -> 0x%03X\n", v);
    }

    // CRC enforcement: a frame with a wrong CRC is rejected when enforced, but the
    // value still decodes when enforcement is off. Pick a CRC nibble that matches NONE of the
    // accepted checksums (crc4, and crc4_table with and without augmentation) so the frame is corrupt under all of them.
    {
        SentDecoder d;
        uint32_t t = 0;
        const uint8_t c0 = SentDecoder::crc4(data, 6);
        const uint8_t c1 = SentDecoder::crc4_table(data, 6, false);
        const uint8_t c2 = SentDecoder::crc4_table(data, 6, true);
        int bad = -1;
        for (int cand = 0; cand < 16; ++cand)
            if (cand != c0 && cand != c1 && cand != c2) { bad = cand; break; }
        assert(bad >= 0);
        t = emit_frame(d, t, 3, 0, data, bad);                   // corrupt CRC (all variants)
        uint16_t v = 0;
        assert(!d.value(t, /*enforce_crc=*/true, v));            // dropped
        assert(d.crc_errors() >= 1);                             // and counted
        assert(d.value(t, /*enforce_crc=*/false, v) && v == expect);  // decoded anyway
        printf("ok  CRC enforce drops bad frame; lax decodes; err counted\n");
    }

    // J2716 table form without augmentation (sensors built to the pre-2010 text) validates too.
    {
        SentDecoder d;
        const uint8_t gdata[6] = {0x5, 0x6, 0x7, 0x8, 0x9, 0xA};
        uint32_t t = 0;
        const uint8_t legacy = SentDecoder::crc4_table(gdata, 6, false);
        for (int f = 0; f < 3; ++f) t = emit_frame(d, t, 3, 0, gdata, legacy);
        uint16_t v = 0;
        assert(d.value(t, /*enforce_crc=*/true, v) && v == 0x567);
        printf("ok  J2716 table CRC (pre-2010) accepted -> 0x%03X\n", v);
    }

    // Not enough edges yet -> no value.
    {
        SentDecoder d;
        d.on_edge(10); d.on_edge(100);
        uint16_t v = 0;
        assert(!d.value(200, true, v));
        printf("ok  partial -> no value\n");
    }

    // Staleness: a decoded stream that then goes quiet returns no value.
    {
        SentDecoder d;
        uint32_t t = 0;
        for (int f = 0; f < 3; ++f) t = emit_frame(d, t, 3, 0, data);
        uint16_t v = 0;
        assert(d.value(t, true, v));               // fresh: ok
        assert(!d.value(t + 60000u, true, v));     // 60 ms later: stopped
        printf("ok  staleness -> stopped\n");
    }

    // CRC round-trips through the decoder's own crc4 (sanity on the polynomial).
    {
        const uint8_t z[6] = {0, 0, 0, 0, 0, 0};
        (void)SentDecoder::crc4(z, 6);             // must not crash; value is the seed-derived crc
        printf("ok  crc4 callable\n");
    }

    printf("\nall sent decoder tests passed\n");
    return 0;
}
