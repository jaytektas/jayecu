// The SD log is a file MegaLogViewer can read — checked byte by byte, on the host.
//
// The whole point of MLG is that a log carries its own meaning: names, units, scale and digits in
// the file's own header. So this asserts the bytes, not the intent — the magic, the sizes the reader
// strides by, the fixed-width NUL padding, and above all the BIG-ENDIAN order, because the frame
// these values are gathered from is little-endian and a byte-reversal that silently stops happening
// produces a file that opens and is wrong.
#include "Comms/MlgLog.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-58s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16
                                              | uint32_t(p[2]) << 8  | uint32_t(p[3]); }

int main() {
    std::printf("=== MLG v2 log format ===\n");

    // --- the DEFAULT selection: an all-zero mask means "as shipped" ---------------------
    // A tune that predates the mask, or one nobody has configured, must log the definition's own
    // set rather than nothing at all — an empty log is the worst possible reading of "unset".
    const uint8_t zero_mask[57] = {};
    const mlg::Selection def = mlg::resolve_selection(zero_mask, sizeof zero_mask);
    uint16_t by_default = 0;
    for (uint16_t i = 0; i < MLG_FIELD_COUNT; ++i) if (MLG_FIELDS[i].by_default) ++by_default;
    ck(def.count == by_default && def.count > 0, "an all-zero mask selects the shipped set",
       std::to_string(def.count) + " of " + std::to_string(MLG_FIELD_COUNT));

    // …and a mask with bits in it means EXACTLY those, defaults ignored.
    uint8_t mask[57] = {};
    mask[0] = 0x05;                              // catalog channels 0 and 2
    const mlg::Selection two = mlg::resolve_selection(mask, sizeof mask);
    ck(two.count == 2 && two.has(0) && two.has(2) && !two.has(1),
       "a mask with bits set selects exactly those", std::to_string(two.count));
    ck(two.record_len == MLG_FIELDS[0].size + MLG_FIELDS[2].size,
       "…and the record length is the sum of their sizes", std::to_string(two.record_len));

    // --- the fixed header -------------------------------------------------------------
    uint8_t h[mlg::kHeaderSize];
    const mlg::Selection& sel = def;
    mlg::write_file_header(h, sel, 0);
    ck(std::memcmp(h, "MLVLG", 5) == 0 && h[5] == 0, "the file starts \"MLVLG\\0\"");
    ck(rd16(h + 6) == 2, "format version 2", std::to_string(rd16(h + 6)));
    ck(rd32(h + 16) == mlg::header_bytes(sel), "data begins after the descriptors",
       std::to_string(rd32(h + 16)));
    ck(rd16(h + 20) == sel.record_len, "record length is the FIELD data only",
       std::to_string(rd16(h + 20)));
    ck(rd16(h + 22) == sel.count, "the field count is what the selection holds",
       std::to_string(rd16(h + 22)));

    // …and the record length really is the sum of the field sizes. A drift here is the bug that
    // makes every row after the first land one byte out.
    uint32_t sum = 0;
    for (uint16_t i = 0; i < MLG_FIELD_COUNT; ++i) if (sel.has(i)) sum += MLG_FIELDS[i].size;
    ck(sum == sel.record_len, "…and equals the sum of the selected fields' sizes",
       std::to_string(sum) + " vs " + std::to_string(sel.record_len));

    // --- a descriptor -----------------------------------------------------------------
    uint8_t d[mlg::kDescriptorSize];
    mlg::write_field_descriptor(0, d);
    const MlgFieldDesc& f0 = MLG_FIELDS[0];
    ck(d[0] == f0.type, "the descriptor leads with the scalar type");
    ck(std::strncmp(reinterpret_cast<const char*>(d + 1), f0.name, 34) == 0,
       "…then the name, at offset 1");
    ck(std::strncmp(reinterpret_cast<const char*>(d + 35), f0.units, 10) == 0,
       "…the units, at 35");
    // The scale is a big-endian IEEE-754 float; read it back the way the viewer would.
    const uint32_t bits = rd32(d + 46);
    float back; std::memcpy(&back, &bits, 4);
    ck(back == f0.scale, "…and the scale as a big-endian float", std::to_string(back));

    // NUL PADDING, not just termination: a short name must not leak the bytes after it.
    bool padded = true;
    const size_t n = std::strlen(f0.name);
    for (size_t i = n; i < 34; ++i) if (d[1 + i] != 0) padded = false;
    ck(padded, "a short name is NUL-padded to its full width");

    // --- a record ---------------------------------------------------------------------
    // A frame with a known byte at every offset, so the gather can be checked field by field.
    std::vector<uint8_t> frame(4096, 0);
    for (size_t i = 0; i < frame.size(); ++i) frame[i] = uint8_t(i & 0xFF);

    std::vector<uint8_t> rec(mlg::kMaxRecordBytes);
    const uint16_t want_len = uint16_t(4 + two.record_len + 1);
    const uint16_t wrote = mlg::write_record(rec.data(), two, frame.data(), (uint32_t)frame.size(), 7, 1234);
    ck(wrote == want_len, "a record is prefix + selected fields + checksum",
       std::to_string(wrote) + " vs " + std::to_string(want_len));
    ck(rec[0] == 0, "block type 0 (a data record)");
    ck(rec[1] == 7, "…carrying the caller's rolling counter");

    // THE BYTE REVERSAL. The first selected field must be its frame bytes backwards.
    bool swapped = true;
    for (uint8_t b = 0; b < f0.size; ++b)
        if (rec[4 + b] != frame[f0.off + (f0.size - 1 - b)]) swapped = false;
    ck(swapped, "field values are byte-reversed to big-endian");

    // THE SECOND field in the record is catalog channel 2, not channel 1 — a selection that is not
    // honoured on the gather side would put channel 1's bytes here and every column after would be
    // describing the wrong value.
    bool skipped = true;
    const MlgFieldDesc& f2 = MLG_FIELDS[2];
    for (uint8_t b = 0; b < f2.size; ++b)
        if (rec[4 + f0.size + b] != frame[f2.off + (f2.size - 1 - b)]) skipped = false;
    ck(skipped, "the gather skips unselected channels, in catalog order");

    // The checksum is the plain sum of the field bytes — not the prefix.
    uint8_t want = 0;
    for (uint16_t i = 0; i < two.record_len; ++i) want += rec[4 + i];
    ck(rec[want_len - 1] == want, "the checksum sums the field bytes, not the prefix");

    // A frame shorter than the table describes writes zeros rather than reading past it.
    std::vector<uint8_t> rec2(mlg::kMaxRecordBytes);
    mlg::write_record(rec2.data(), two, frame.data(), 1, 0, 0);
    bool zeroed = true;
    for (uint16_t i = 0; i < two.record_len; ++i) if (rec2[4 + i] != 0) zeroed = false;
    ck(zeroed, "a short frame yields zeros, never memory past its end");

    // THE INDEXED GATHER IS THE SAME GATHER. The 1 kHz sampler walks a flattened list of selected
    // indices instead of testing a bit per descriptor — measurably cheaper, and worth nothing if it
    // produces a different record. Byte-for-byte against the original, for a small selection and for
    // the shipped default, because "it looked right" is how a log ends up subtly wrong.
    {
        std::vector<uint8_t> frame(MLG_MAX_RECORD_LEN);
        for (size_t i = 0; i < frame.size(); ++i) frame[i] = uint8_t(i * 7 + 3);

        auto same_as_bitwise = [&](const mlg::Selection& sel, const char* what) {
            std::vector<uint16_t> idx;
            for (uint16_t i = 0; i < MLG_FIELD_COUNT; ++i) if (sel.has(i)) idx.push_back(i);
            std::vector<uint8_t> a(mlg::kMaxRecordBytes), b(mlg::kMaxRecordBytes);
            const uint16_t na = mlg::write_record(a.data(), sel, frame.data(),
                                                  (uint32_t)frame.size(), 0x5A, 12345u);
            const uint16_t nb = mlg::write_record_indexed(b.data(), idx.data(), (uint16_t)idx.size(),
                                                          sel.record_len, frame.data(),
                                                          (uint32_t)frame.size(), 0x5A, 12345u);
            ck(na == nb, std::string(what) + ": same record length",
               std::to_string(na) + " vs " + std::to_string(nb));
            ck(std::memcmp(a.data(), b.data(), na) == 0,
               std::string(what) + ": byte-for-byte identical");
        };

        // A small diagnostic selection — the case the fast path exists for.
        std::vector<uint8_t> mask((MLG_FIELD_COUNT + 7) / 8, 0);
        for (uint16_t i = 0; i < MLG_FIELD_COUNT; i += 23) mask[i >> 3] |= uint8_t(1u << (i & 7));
        same_as_bitwise(mlg::resolve_selection(mask.data(), (uint16_t)mask.size()), "sparse selection");
        // …and the shipped default, where nearly every descriptor is in.
        same_as_bitwise(mlg::resolve_selection(nullptr, 0), "default selection");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All MLG format tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
