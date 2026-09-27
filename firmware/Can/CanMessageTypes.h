#pragma once
//
// CAN frame maths — the bit surgery and the linear transform, and nothing else.
//
// This is the one place bits are moved between a CAN payload and an engineering value. Generic CAN
// (both directions) and the CAN-interface sensors share it, so a field addressed one way cannot
// decode a different slice than it encodes. There is no protocol table here and no scheduling: the
// frames live in the tune (`CanConfig::gc_frame` / `gc_field`), because a published protocol is a
// template the studio loads into that pool rather than a table compiled into the ECU.
//
// BIT ADDRESSING IS byte*8 + bit, WITH BIT 7 THE MOST SIGNIFICANT OF ITS BYTE. Byte 0 bit 7 is
// index 7, byte 1 bit 7 is 15, byte 7 bit 0 is 56.
//
// This is not an arbitrary choice: it is what DBC files, every CAN analysis tool, and the published
// dash protocols all number with, so a field copied off a document or out of somebody else's DBC is
// the same number here. A private numbering would have been marginally tidier internally and wrong
// in every conversation the user has with another tool.
//
// A spec's own notation maps straight onto it:
//
//     single bit  2:5   -> bit_off 21, width  1      (2*8 + 5)
//     byte range 0-1    -> bit_off  7, width 16      (starts at byte 0's MSB)
//     span     6:3-7:0  -> bit_off 51, width 12      (byte 6 bit 3, running down into byte 7)
//
// It is also exactly the grid the studio draws, so what the editor shows and what the wire carries
// are the same number.
//
#include <cstdint>
#include "CanFrame.h"

namespace canmsg {

// A spec's "byte:bit" as this frame-wide index. Constexpr so a hand-written entry reads like the
// document it came from and costs nothing at run time.
constexpr uint16_t BIT_OF(uint8_t byte, uint8_t bit) {
    return static_cast<uint16_t>(byte * 8 + bit);
}
// A field that starts at the top of byte N — which is how a whole-byte range opens.
constexpr uint16_t BYTES_OF(uint8_t first) { return BIT_OF(first, 7); }

// Field flags — the low bits of a gc_field's `flags`.
enum FieldFlags : uint8_t {
    FIELD_SIGNED   = 1u << 0,   // the raw slice is two's complement
    FIELD_LITTLE   = 1u << 1,   // Intel bit order (see below); clear = Motorola/big
    // A RAW CODE MEANING "NOTHING TO REPORT". A wideband in free air sends 0x7FFF, which decodes to a
    // perfectly plausible and completely wrong mixture. A field carrying its sentinel is skipped, so
    // the value is simply not refreshed and expires on its own TTL — "no reading" rather than a lie.
    FIELD_SENTINEL = 1u << 2,
};

// Frame flags — the low bits of a gc_frame's `flags`.
enum FrameFlags : uint8_t {
    FRAME_USED = 1u << 0,     // this pool slot holds a frame; clear = free slot, skipped entirely
    FRAME_TX   = 1u << 1,     // transmit on its period; clear = receive and decode
    FRAME_EXT  = 1u << 2,     // 29-bit identifier
    // PARKED. A frame kept in the tune and not run — the way to stop sending something without
    // losing how it was set up. The bus's own enable is the other gate; there is no third.
    FRAME_OFF  = 1u << 3,
};

// WHAT A TRANSMIT FIELD DOES WHEN IT HAS NOTHING TO SAY. A frame is a fixed run of bytes, so a field
// whose channel is invalid — a sensor that is not fitted, one that has failed, a role nobody holds —
// still occupies its bits and something must go in them. The right answer is not the same for every
// field, which is why it is per-field: a value whose zero is obviously broken can be sent as zero and
// be understood, while a value whose zero is PLAUSIBLE (an acceleration, a trim, anything centred on
// nothing) reads as a real measurement and is worse than no frame at all.
//
//   ABSENT_ZERO  write raw zero and let the reader see it
//   ABSENT_HOLD  leave the last bits that were written here
//   ABSENT_SKIP  suppress the WHOLE frame this cycle
//
// ABSENT_SKIP is deliberately blunt: a frame is the unit a reader acknowledges, so there is no way to
// omit one field of one. It is for a frame whose fields stand or fall together, and it costs the other
// fields in that frame — which is the trade being made when it is chosen.
enum AbsentPolicy : uint8_t { ABSENT_ZERO = 0, ABSENT_HOLD = 1, ABSENT_SKIP = 2 };

// ---------------------------------------------------------------------------
// Bit order
//
// Two conventions exist on real buses and both are needed, so both are implemented rather than one
// being declared "the" order and the other left as a flag nothing honours.
//
//   MOTOROLA (big, the default). `bit_off` is the field's MOST significant bit. Bits run DOWN
//   through the byte and continue at bit 7 of the NEXT byte, so the field reads left to right
//   exactly as a published table prints it.
//
//   INTEL (little). `bit_off` is the field's LEAST significant bit and the index simply ascends,
//   which is why a wide Intel field looks byte-swapped when you read the payload as printed.
//
// Both are one function from "field bit i" to "frame bit index", so the packer and the unpacker
// share a single definition of the layout and cannot drift apart.
// ---------------------------------------------------------------------------

constexpr uint16_t field_bit(uint16_t bit_off, uint8_t i, bool little) {
    if (little) return static_cast<uint16_t>(bit_off + i);
    // Steps taken downward from this byte's MSB; every 8 of them moves on a byte.
    const uint16_t d = static_cast<uint16_t>((7u - (bit_off & 7u)) + i);
    return static_cast<uint16_t>(((bit_off >> 3) + (d >> 3)) * 8u + (7u - (d & 7u)));
}

// The shift that selects field bit `i` out of a right-aligned raw value. Motorola counts i down from
// the field's MSB, Intel counts it up from the LSB — the same asymmetry as the layout above.
constexpr uint8_t raw_shift(uint8_t width, uint8_t i, bool little) {
    return little ? i : static_cast<uint8_t>(width - 1 - i);
}

// ---------------------------------------------------------------------------
// Pack / unpack
//
// Deliberately a plain per-bit loop rather than a shift-and-mask special case per width. Real widths
// are 1, 2, 3, 4, 8, 12, 16 and 32, they straddle byte boundaries at arbitrary offsets, and this runs
// a few hundred times a second — so the version that is obviously correct for every alignment beats
// the version that is fast for the four alignments somebody thought of.
// ---------------------------------------------------------------------------

// Write the low `width` bits of `raw` into `data`.
inline void pack_bits(uint8_t* data, uint8_t dlc, uint16_t bit_off, uint8_t width, bool little,
                      uint32_t raw) {
    for (uint8_t i = 0; i < width; i++) {
        const uint16_t b    = field_bit(bit_off, i, little);
        const uint16_t byte = static_cast<uint16_t>(b >> 3);
        if (byte >= dlc) continue;                      // past the end of this frame: drop, never scribble
        const uint8_t  mask = static_cast<uint8_t>(1u << (b & 7));
        if ((raw >> raw_shift(width, i, little)) & 1u) data[byte] = static_cast<uint8_t>(data[byte] |  mask);
        else                                           data[byte] = static_cast<uint8_t>(data[byte] & ~mask);
    }
}

// Read `width` bits back out, sign-extending when asked.
// Does every bit of the field lie inside the bytes the frame actually carried? A short frame — a sender
// that shrank its DLC, a different message on the same id — must not decode: the missing bits would read
// as 0 and a plausible, wrong value would go out as fresh.
inline bool field_fits(uint8_t dlc, uint16_t bit_off, uint8_t width, bool little) {
    for (uint8_t i = 0; i < width; i++)
        if ((field_bit(bit_off, i, little) >> 3) >= dlc) return false;
    return true;
}

inline uint32_t unpack_bits(const uint8_t* data, uint8_t dlc, uint16_t bit_off, uint8_t width,
                            bool little, bool is_signed) {
    uint32_t raw = 0;
    for (uint8_t i = 0; i < width; i++) {
        const uint16_t b    = field_bit(bit_off, i, little);
        const uint16_t byte = static_cast<uint16_t>(b >> 3);
        const uint8_t  mask = static_cast<uint8_t>(1u << (b & 7));
        if (byte < dlc && (data[byte] & mask)) raw |= (1u << raw_shift(width, i, little));
    }
    if (is_signed && width < 32 && (raw & (1u << (width - 1)))) {
        raw |= ~((1u << width) - 1u);                   // sign-extend into the unused high bits
    }
    return raw;
}

// ---------------------------------------------------------------------------
// The transform
//
// Stated once, in the ENCODE direction — raw = value * scale + offset — so the two directions cannot
// disagree about a field. A spec written as a decode ("y = x/10 - 101.3") is entered as its inverse
// (scale 10, offset 1013), which is a substitution a tool can make and a reader can check.
//
// `F` is any struct carrying width / flags / scale / offset, which is what the tune's gc_field is.
// Templated rather than copied into a runtime struct: the config lives in RAM already, so the frame
// tables index straight into it and nothing is duplicated.
// ---------------------------------------------------------------------------

// Engineering value -> the raw slice, clamped to what the field can actually hold.
//
// CLAMPING IS NOT OPTIONAL. A 16-bit unsigned field handed 70000 wraps to 4464, and a dash then shows
// a number that is not merely wrong but plausible; a clamp shows the rail, which is readable as "at or
// beyond the top of this field". Rounding is half-away-from-zero so a signed value does not drift
// toward zero the way truncation does.
template <class F>
inline uint32_t encode_value(const F& f, float value) {
    const float scaled = value * f.scale + f.offset;
    const float r = (scaled >= 0.0f) ? (scaled + 0.5f) : (scaled - 0.5f);
    const uint32_t mask = (f.width >= 32) ? 0xFFFFFFFFu : ((1u << f.width) - 1u);
    if (f.flags & FIELD_SIGNED) {
        const float lo = -static_cast<float>(1u << (f.width - 1));
        const float hi =  static_cast<float>((1u << (f.width - 1)) - 1u);
        const int32_t v = static_cast<int32_t>((r < lo) ? lo : (r > hi) ? hi : r);
        return static_cast<uint32_t>(v) & mask;
    }
    const float hi = (f.width >= 32) ? 4294967295.0f : static_cast<float>((1ull << f.width) - 1ull);
    const float c  = (r < 0.0f) ? 0.0f : (r > hi) ? hi : r;
    return static_cast<uint32_t>(c);
}

// The raw slice -> engineering value: the inverse of encode_value.
template <class F>
inline float decode_value(const F& f, uint32_t raw) {
    const float r = (f.flags & FIELD_SIGNED) ? static_cast<float>(static_cast<int32_t>(raw))
                                             : static_cast<float>(raw);
    return (f.scale != 0.0f) ? (r - f.offset) / f.scale : 0.0f;
}

}  // namespace canmsg
