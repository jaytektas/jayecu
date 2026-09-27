#pragma once
//
// MlgLog — the SD datalog's file format: MLG v2, which MegaLogViewer reads directly.
//
// The log used to be raw EcuTelemetry structs written back to back with no header at all. Those
// bytes are decodable only by someone already holding the exact meta they were written against, and
// nothing in the file says which one that is — change one signal in the schema and every log on the
// card silently becomes a different format under the same name. This is the fix, and it is not a
// format of our own: MLG carries each channel's NAME, UNITS, SCALE and DIGITS in the file's own
// header, so a log found in a drawer in two years still plots correctly with no reference to
// anything else. (Spec: MLG_Binary_LogFormat_2.0, EFI Analytics.)
//
//   [24-byte header][89 bytes x MLG_FIELD_COUNT][record][record]…
//   record = [type:0][rolling counter][timestamp:u16 @10us][field values][checksum:u8]
//
// EVERYTHING IS BIG-ENDIAN, header and values alike, while the packed telemetry frame is little —
// so a record is gathered field by field and byte-reversed, never memcpy'd.
//
// NO FILESYSTEM HERE. This layer only fills buffers, which is what lets the whole format be proven
// on the host against the real generated field table (tests/test_mlg_log.cpp) instead of only on a
// card. SdProtocol does the writing.
//
#include <cstdint>
#include "../../generated/mlg_log.h"

namespace mlg {

constexpr uint16_t kHeaderSize     = 24;
constexpr uint16_t kDescriptorSize = 89;
// The 4-byte prefix and the 1-byte checksum sit OUTSIDE the spec's "record length", which counts only
// the field data — so the largest a record can be on the card is this.
constexpr uint16_t kMaxRecordBytes = 4 + MLG_MAX_RECORD_LEN + 1;

// WHICH CHANNELS THIS LOG CARRIES. A tune field (Datalog.mask), not a build-time list: what a car
// logs is a tuning decision, and a fuel session and a misfire hunt want different columns. One bit
// per channel in descriptor order, LSB first; ALL ZERO means "as shipped" — the definition's own
// `datalog:` opinion — so a tune that predates the feature logs the sensible thing rather than
// nothing at all.
//
// Resolved ONCE when a file is opened and held for its life, because a log whose columns changed
// half-way through would have lied about every record above the change.
struct Selection {
    uint16_t count      = 0;    // channels in the log
    uint16_t record_len = 0;    // sum of their sizes — the header's Record Length
    uint8_t  bits[(MLG_FIELD_COUNT + 7) / 8] = {};
    bool has(uint16_t i) const { return i < MLG_FIELD_COUNT && (bits[i >> 3] & (1u << (i & 7))); }
};

// Build the selection from a tune mask. `mask`/`mask_len` may be shorter than the channel count (an
// older tune, a shorter region) — the missing bits read as zero, and an all-zero mask falls back to
// the definition's defaults rather than producing a log with no columns.
Selection resolve_selection(const uint8_t* mask, uint16_t mask_len);

// Total size of the header block for a selection — also the offset its first record lands at.
inline uint32_t header_bytes(const Selection& sel) {
    return static_cast<uint32_t>(kHeaderSize) + static_cast<uint32_t>(sel.count) * kDescriptorSize;
}

// The fixed 24-byte file header. `epoch` is a unix timestamp or 0 when nothing on the ECU knows the
// date — the spec allows zero, and a wrong date is worse than an absent one.
void write_file_header(uint8_t out[kHeaderSize], const Selection& sel, uint32_t epoch = 0);

// One field's 89-byte descriptor, by CATALOG index (not by position in the log). Strings are
// NUL-padded to their fixed widths, which is what the reader expects — a short name must not leave
// the following bytes holding whatever was there.
void write_field_descriptor(uint16_t index, uint8_t out[kDescriptorSize]);

// One record, gathered from the packed telemetry frame. `counter` is the caller's rolling sequence
// number (it wraps, and is the only thing that orders records once the 16-bit timestamp has wrapped
// — which it does every 655 ms). `out` must hold kMaxRecordBytes; returns what was written.
uint16_t write_record(uint8_t* out, const Selection& sel, const uint8_t* frame, uint32_t frame_len,
                      uint8_t counter, uint32_t now_ms);

// THE SAME RECORD, from a FLATTENED selection. write_record walks all MLG_FIELD_COUNT descriptors
// testing a bit per record, so a 20-channel log cost almost what a 367-channel one did — measured
// at 0.50 us against 1.54 us for eighteen times the data, because the walk dominates. At 10 Hz that
// does not matter; at 1 kHz it is most of what the sampler does. `idx` is the selected catalog
// indices in ascending order and `record_len` their total size, both resolved once when the file is
// opened. Produces byte-identical output to write_record for the same selection, which is what the
// test asserts rather than assumes.
uint16_t write_record_indexed(uint8_t* out, const uint16_t* idx, uint16_t n, uint16_t record_len,
                              const uint8_t* frame, uint32_t frame_len,
                              uint8_t counter, uint32_t now_ms);

}  // namespace mlg
