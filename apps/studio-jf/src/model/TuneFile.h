#pragma once

#include <cstdint>
#include <string>
#include <vector>

class MetaModel;

// Migration report produced when loading a dict tune against a (possibly different) layout. Present it to
// the user whenever needed() is true — they should know what changed across a firmware update.
struct MigrationReport {
    int                      migrated  = 0;   // fields found in both tune and meta — applied
    int                      defaulted = 0;   // fields in meta but not in tune (new firmware) — left at default
    std::vector<std::string> unmapped;        // fields in tune not found in meta (removed or renamed)
    // A reference this firmware cannot resolve: the tune names a signal or an enum option that no longer
    // exists ("h_bridge.half[0].enable_sig -> etb_en_1"). The FIELD is fine and its neighbours applied
    // normally — only this one binding is left at its default, and the user is told which, because a
    // selector silently pointing somewhere plausible is how a tune breaks without anyone noticing.
    std::vector<std::string> unresolved;

    bool needed() const { return defaulted > 0 || !unmapped.empty() || !unresolved.empty(); }
    bool empty()  const { return migrated == 0 && !needed(); }
};

// TuneFile — the on-disk `.tune` format (jayecu-tune/2): a named-field JSON document, NOT a raw config
// image, read and written through MetaModel + jf::JJson. All stored values are RAW
// (pre-scale) — the meta's scale factors are applied only at display, never on disk.
namespace TuneFile {

// Serialise a raw config image into a jayecu-tune/2 JSON document (scalars / tables / axes / arrays, each
// keyed by its meta path). Records the layout_hash so a future load can detect firmware updates.
// `host` is the PcVariable block (MetaModel::kPcSegmentBase): host-side values that live in no ECU
// storage but ARE part of the tune, since the tune is where values belong. Optional — a caller with no
// host block passes nothing and those fields are simply absent from the document.
std::vector<uint8_t> serialise(const std::vector<uint8_t> &image, const MetaModel &meta,
                               const std::vector<uint8_t> *host = nullptr);

// Deserialise a jayecu-tune/2 JSON document back to a raw config image. Starts from meta.defaultImage() as
// the merge base and overlays every stored field. Fields in both -> migrated; in meta but not tune ->
// defaulted; in tune but not meta -> unmapped (reported, not applied). Empty vector on parse failure.
// `hostOut`, when given, receives the PcVariable block rebuilt from the document (sized from the meta's
// host segment). Values are keyed by PATH like everything else here, so adding, removing or reordering a
// variable never moves anyone else's value.
std::vector<uint8_t> deserialise(const std::vector<uint8_t> &tuneJson, const MetaModel &meta,
                                 MigrationReport &report, std::vector<uint8_t> *hostOut = nullptr);

// True if the bytes look like a dict tune (JSON: first non-whitespace byte is '{'); false for a raw image.
// Legacy projects may still hold raw-byte tunes — callers fall back to using the bytes as-is.
bool isDictFormat(const std::vector<uint8_t> &data);

} // namespace TuneFile
