// MlgLog — see the header for the format and why it is not one of our own.

#include "MlgLog.h"

#include <cstring>

namespace mlg {
namespace {

// Big-endian stores. The whole format is big-endian and the M7 is little, so every multi-byte value
// goes out through one of these rather than through a cast — there is no memcpy of a struct anywhere
// in this file, and that is deliberate.
inline void be16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8);  p[1] = uint8_t(v); }
inline void be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
// A float goes out as its IEEE-754 bits, big-endian — the same four bytes the reader parses back.
inline void be_float(uint8_t* p, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);       // type-punning through memcpy, not a cast
    be32(p, bits);
}

// NUL-PADDED TO THE FULL WIDTH, not just NUL-terminated. The reader takes a fixed number of bytes
// per string, so a short name that left the remaining bytes untouched would hand it whatever was in
// the buffer — which is how a channel ends up named after the one before it.
inline void padded(uint8_t* p, const char* s, uint16_t width) {
    std::memset(p, 0, width);
    if (!s) return;
    uint16_t n = 0;
    while (s[n] && n < width - 1) ++n;         // leave at least the final byte NUL
    std::memcpy(p, s, n);
}

}  // namespace

Selection resolve_selection(const uint8_t* mask, uint16_t mask_len) {
    Selection sel;
    bool any = false;
    for (uint16_t i = 0; i < MLG_FIELD_COUNT; ++i) {
        const uint16_t byte = i >> 3;
        if (mask && byte < mask_len && (mask[byte] & (1u << (i & 7)))) { any = true; break; }
    }
    for (uint16_t i = 0; i < MLG_FIELD_COUNT; ++i) {
        const uint16_t byte = i >> 3;
        const bool on = any ? (mask && byte < mask_len && (mask[byte] & (1u << (i & 7))))
                            : MLG_FIELDS[i].by_default;
        if (!on) continue;
        sel.bits[i >> 3] |= uint8_t(1u << (i & 7));
        sel.count++;
        sel.record_len = uint16_t(sel.record_len + MLG_FIELDS[i].size);
    }
    return sel;
}

void write_file_header(uint8_t out[kHeaderSize], const Selection& sel, uint32_t epoch) {
    std::memset(out, 0, kHeaderSize);
    out[0] = 'M'; out[1] = 'L'; out[2] = 'V'; out[3] = 'L'; out[4] = 'G'; out[5] = 0;
    be16(out + 6, 2);                       // format version
    be32(out + 8, epoch);                   // 0 = unknown, which the spec allows
    be32(out + 12, 0);                      // no info-data block
    be32(out + 16, header_bytes(sel));      // data begins straight after the descriptors
    // "Record Length" is the FIELD DATA only — the prefix and checksum are not counted, which is the
    // spec's own definition and the one the reader strides by.
    be16(out + 20, sel.record_len);
    be16(out + 22, sel.count);
}

void write_field_descriptor(uint16_t index, uint8_t out[kDescriptorSize]) {
    std::memset(out, 0, kDescriptorSize);
    if (index >= MLG_FIELD_COUNT) return;    // a caller that walks past the end gets zeros, not junk
    const MlgFieldDesc& f = MLG_FIELDS[index];
    out[0] = f.type;                         // 0=U08 1=S08 2=U16 3=S16 4=U32 5=S32 6=S64 7=F32
    padded(out + 1,  f.name,  34);
    padded(out + 35, f.units, 10);
    out[45] = 0;                             // display style: Float. Every channel is a number here.
    be_float(out + 46, f.scale);
    be_float(out + 50, f.transform);
    out[54] = static_cast<uint8_t>(f.digits);
    padded(out + 55, f.category, 34);        // MLV groups its channel list by this
}

uint16_t write_record(uint8_t* out, const Selection& sel, const uint8_t* frame, uint32_t frame_len,
                      uint8_t counter, uint32_t now_ms) {
    out[0] = 0;                              // block type: a standard data record
    out[1] = counter;
    // The spec's timestamp is 16 bits at 10 us, so it wraps every 655 ms. It is a fine-grained offset
    // and NOT a clock — record order comes from the file, and the rolling counter above is what
    // survives the wrap. Feeding it milliseconds x100 keeps the units right at the resolution we have.
    be16(out + 2, static_cast<uint16_t>((now_ms * 100u) & 0xFFFFu));

    uint8_t* p = out + 4;
    uint8_t  sum = 0;                        // the checksum covers the FIELD BYTES only
    for (uint16_t i = 0; i < MLG_FIELD_COUNT; ++i) {
        if (!sel.has(i)) continue;               // the tune says this channel is not in this log
        const MlgFieldDesc& f = MLG_FIELDS[i];
        // A field that would read past the frame writes zeros rather than whatever follows it in
        // memory. The generated table comes from the same list the frame is packed from, so this
        // cannot happen today — it is here so it stays impossible if one of them ever moves.
        const bool ok = frame && (static_cast<uint32_t>(f.off) + f.size <= frame_len);
        for (uint8_t b = 0; b < f.size; ++b) {
            // BYTE-REVERSED: the frame is little-endian, the file is big.
            const uint8_t v = ok ? frame[f.off + (f.size - 1 - b)] : 0u;
            *p++ = v;
            sum += v;
        }
    }
    *p++ = sum;
    return uint16_t(4 + sel.record_len + 1);
}

uint16_t write_record_indexed(uint8_t* out, const uint16_t* idx, uint16_t n, uint16_t record_len,
                              const uint8_t* frame, uint32_t frame_len,
                              uint8_t counter, uint32_t now_ms) {
    out[0] = 0;
    out[1] = counter;
    be16(out + 2, static_cast<uint16_t>((now_ms * 100u) & 0xFFFFu));

    uint8_t* p = out + 4;
    uint8_t  sum = 0;
    for (uint16_t k = 0; k < n; ++k) {
        const MlgFieldDesc& f = MLG_FIELDS[idx[k]];
        const bool ok = frame && (static_cast<uint32_t>(f.off) + f.size <= frame_len);
        for (uint8_t b = 0; b < f.size; ++b) {
            const uint8_t v = ok ? frame[f.off + (f.size - 1 - b)] : 0u;   // little -> big endian
            *p++ = v;
            sum += v;
        }
    }
    *p++ = sum;
    return uint16_t(4 + record_len + 1);
}

}  // namespace mlg
