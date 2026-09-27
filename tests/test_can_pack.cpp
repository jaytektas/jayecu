// CAN FIELD PACKING — bit addressing and the encode direction.
//
// The anchor for the whole file is the Haltech CAN Broadcast Protocol's OWN worked example, which
// prints the expected bytes. That makes it ground truth rather than my arithmetic restated: if
// BIT_OF() or pack_bits() has the bit order backwards, or treats a byte-spanning field as two
// separate pieces, the bytes come out different from the ones in the document.
//
// The example (spec V2.35.0, "Addressing Data"):
//
//   Byte      0                 1                 2                 3
//   Data      Manifold Pressure ................  MIL + Reserved    Throttle Position
//   Value     1013 (0x03F5)                       1        0        750 (0x02EE)
//   Raw       00000011          11110101          10000010          11101110
//   Address   0 - 1                               2:7  2:6-2:4      2:3 - 3:0
//
//   cmake --build build --target test_can_pack && ./build/test_can_pack
#include "test_helpers.h"
#include "Can/CanMessageTypes.h"

#include <cstdio>
#include <cstring>

using namespace canmsg;

// The transform is templated on "anything carrying width/flags/scale/offset", which is what the
// tune's gc_field is. A test says so in four members rather than dragging the config header in.
struct Fld {
    uint16_t bit_off;
    uint8_t  width;
    uint8_t  flags;
    float    scale;
    float    offset;
    bool     little() const { return (flags & FIELD_LITTLE) != 0; }
    bool     is_signed() const { return (flags & FIELD_SIGNED) != 0; }
};

static Fld fld(uint16_t bit_off, uint8_t width, float scale = 1.0f, float offset = 0.0f,
               uint8_t flags = 0) {
    return Fld{ bit_off, width, flags, scale, offset };
}

int main() {
    fprintf(stdout, "=== CAN field packing ===\n");

    SECTION("the spec's own worked example, byte for byte");
    {
        uint8_t d[8] = {};
        // Manifold Pressure, bytes 0-1, y = x/10 -> raw = value * 10. 101.3 kPa -> 1013.
        const Fld map = fld(BYTES_OF(0), 16, 10.0f);
        pack_bits(d, 8, map.bit_off, map.width, map.little(), encode_value(map, 101.3f));
        // MIL, the single bit 2:7.
        const Fld mil = fld(BIT_OF(2, 7), 1);
        pack_bits(d, 8, mil.bit_off, mil.width, mil.little(), encode_value(mil, 1.0f));
        // Throttle Position, 12 bits spanning 2:3 - 3:0. 75.0 % -> 750.
        const Fld tps = fld(BIT_OF(2, 3), 12, 10.0f);
        pack_bits(d, 8, tps.bit_off, tps.width, tps.little(), encode_value(tps, 75.0f));

        fprintf(stdout, "    packed: %02X %02X %02X %02X\n", d[0], d[1], d[2], d[3]);
        CHECK(d[0] == 0x03);      // big-endian high byte of 1013
        CHECK(d[1] == 0xF5);
        CHECK(d[2] == 0x82);      // MIL=1, reserved=000, then the top 4 bits of 750 (0010)
        CHECK(d[3] == 0xEE);      // …and its low 8 bits
        // The reserved bits 2:6-2:4 must be UNTOUCHED by either neighbour.
        CHECK(((d[2] >> 4) & 0x7) == 0);

        // The addressing arithmetic itself, stated once so a transcription error is visible here
        // rather than as a wrong byte three frames later.
        // Bit indices are byte*8 + bit, bit 7 the MSB of its byte — DBC's numbering and the one
        // every CAN tool prints, so a field copied from elsewhere is the same number here.
        CHECK(BYTES_OF(0)  ==  7);   // a whole-byte range opens at that byte's MSB
        CHECK(BIT_OF(2, 7) == 23);
        CHECK(BIT_OF(2, 3) == 19);
        CHECK(BIT_OF(6, 3) == 51);   // the 12-bit cruise field's start, spec 6:3 - 7:0
        CHECK(BIT_OF(7, 0) == 56);   // …and its last bit, reached by running DOWN from 51
    }

    SECTION("round trip — what is packed is what is read back");
    {
        // Every width and alignment the real specs use, including the ones that straddle bytes.
        struct Case { uint16_t off; uint8_t w; float v; float scale; float offset; uint8_t flags; };
        const Case cases[] = {
            { BYTES_OF(0), 16,  101.3f,  10.0f,    0.0f, 0 },          // y = x/10
            { BYTES_OF(2), 16,  250.0f,  10.0f, 1013.0f, 0 },          // y = x/10 - 101.3 (gauge)
            { BIT_OF(4,7),  8,   42.0f,   1.0f,    0.0f, 0 },
            { BIT_OF(5,3), 12,  -12.5f,  10.0f,    0.0f, FIELD_SIGNED },
            { BIT_OF(2,5),  1,    1.0f,   1.0f,    0.0f, 0 },
            { BYTES_OF(6), 16,  -30.0f,  10.0f,    0.0f, FIELD_SIGNED },
        };
        for (const Case& c : cases) {
            uint8_t d[8] = {};
            const Fld f = fld(c.off, c.w, c.scale, c.offset, c.flags);
            pack_bits(d, 8, f.bit_off, f.width, f.little(), encode_value(f, c.v));
            const uint32_t raw = unpack_bits(d, 8, f.bit_off, f.width, f.little(), (f.flags & FIELD_SIGNED) != 0);
            CHECK_NEAR(decode_value(f, raw), c.v, 0.05f);
        }
    }

    SECTION("a field out of range CLAMPS — it must never wrap into a plausible number");
    {
        // 70000 into 16 unsigned bits wraps to 4464, which a dash renders as a real reading. The rail
        // is readable as "at or past the top"; the wrap is not readable as anything.
        uint8_t d[8] = {};
        const Fld f = fld(BYTES_OF(0), 16, 1.0f);
        pack_bits(d, 8, f.bit_off, f.width, f.little(), encode_value(f, 70000.0f));
        CHECK(unpack_bits(d, 8, f.bit_off, f.width, f.little(), false) == 65535u);

        const Fld s = fld(BYTES_OF(2), 16, 1.0f, 0.0f, FIELD_SIGNED);
        pack_bits(d, 8, s.bit_off, s.width, s.little(), encode_value(s, -40000.0f));
        CHECK(static_cast<int32_t>(unpack_bits(d, 8, s.bit_off, s.width, s.little(), true)) == -32768);

        // …and a negative into an UNSIGNED field floors at zero rather than becoming 65000-odd.
        const Fld u = fld(BYTES_OF(4), 16, 10.0f);
        pack_bits(d, 8, u.bit_off, u.width, u.little(), encode_value(u, -5.0f));
        CHECK(unpack_bits(d, 8, u.bit_off, u.width, u.little(), false) == 0u);
    }

    SECTION("packing never scribbles outside the frame");
    {
        // A transcription that puts a 16-bit field at byte 7 would otherwise run off the end. The
        // guard keeps it inside the 8 bytes and drops the rest; nothing past dlc is written.
        uint8_t d[8] = {};
        uint8_t canary[8];
        memset(canary, 0xAA, sizeof canary);
        memcpy(d, canary, sizeof d);
        pack_bits(d, 4, BYTES_OF(6), 16, false, 0xFFFFu);       // dlc 4: bytes 6-7 are past the end
        CHECK(memcmp(d, canary, sizeof d) == 0);
    }

    SECTION("the pressure family — the -101.3 offset survives the round trip");
    {
        // The most common awkward conversion in the spec: y = x/10 - 101.3, so raw = (y + 101.3)*10.
        // Atmospheric gauge pressure is 0, which must encode as 1013 and NOT as 0.
        uint8_t d[8] = {};
        const Fld oilp = fld(BYTES_OF(0), 16, 10.0f, 1013.0f);
        pack_bits(d, 8, oilp.bit_off, oilp.width, oilp.little(), encode_value(oilp, 0.0f));
        CHECK(unpack_bits(d, 8, oilp.bit_off, oilp.width, oilp.little(), false) == 1013u);
        // …and the reading a zero-filled field would produce, which is why an absent channel must
        // not simply be left at zero for this family.
        CHECK_NEAR(decode_value(oilp, 0u), -101.3f, 0.05f);
    }

    return test_summary();
}
