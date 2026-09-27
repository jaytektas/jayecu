// test_studio_meta_contract.cpp — can the studio still read what codegen emits?
//
// This is the ONE place that link is checked, and it belongs here rather than in the studio: the studio
// is a tuning studio for whatever ECU is in front of it, and most people running it have no jayecu
// firmware, no ECU and no shared/tuneit-meta.json. Its own tests therefore run against a fixture. Here,
// both artefacts always exist — codegen just produced the meta — so here is where drift shows up.
//
// It uses the studio's REAL reader (MetaModel), not a re-description of the format. A schema self-check
// would pass happily while the studio failed to load the file, which is the only failure that matters.
//
//   cmake --build tests/build --target test_studio_meta_contract && ./tests/build/test_studio_meta_contract

#include "model/MetaModel.h"

#include <cstdio>
#include <set>
#include <string>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

int main()
{
    MetaModel meta;
    if (!meta.loadFile(META_PATH)) {
        // Not a skip. Codegen runs before the tests and this file is its output; if the studio cannot
        // load it, that IS the regression this test exists to catch.
        std::printf("the studio's MetaModel could not load %s\n"
                    "  (run `make codegen` first; if it has been run, the studio can no longer read what\n"
                    "   codegen emits, which is exactly the drift this test is here to report)\n", META_PATH);
        return 2;
    }
    CHECK(meta.isValid());

    // --- identity ---------------------------------------------------------------------------------
    CHECK(!meta.board().empty());
    CHECK(!meta.layoutHash().empty());
    CHECK(meta.configSize() > 0);
    CHECK(meta.telemetrySize() > 0);
    std::printf("board=%s layout=%s config=%dB telem=%dB\n",
                meta.board().c_str(), meta.layoutHash().c_str(), meta.configSize(), meta.telemetrySize());

    // --- the document was actually consumed, not merely parsed ------------------------------------
    // Counts, because a reader that silently dropped every field of some kind would still "load".
    CHECK(!meta.config().empty());
    CHECK(!meta.configTables().empty());
    CHECK(!meta.telemetry().empty());
    CHECK(!meta.segments().empty());
    std::printf("%zu scalars, %zu tables, %zu 1-D arrays, %zu channels, %zu segment(s)\n",
                meta.config().size(), meta.configTables().size(), meta.arrays1d().size(),
                meta.telemetry().size(), meta.segments().size());

    // --- the default tune is the size the definition says the config is ---------------------------
    CHECK(static_cast<int>(meta.defaultImage().size()) == meta.configSize());

    // --- segments: the tune block is block 0, and every non-tune block is addressable --------------
    std::set<std::string> segIds;
    bool haveTune = false;
    for (const auto &s : meta.segments()) {
        CHECK(!s.id.empty());
        CHECK(s.size > 0);
        CHECK(segIds.insert(s.id).second);          // ids are unique, or a field cannot name one
        if (s.id == "config") { haveTune = true; CHECK(s.base == 0); }
    }
    CHECK(haveTune);

    // --- EVERY field lands inside a declared segment ------------------------------------------------
    // This is the assertion that catches a layout disagreement: a field whose offset is in no segment,
    // or which runs off the end of the one it is in, cannot be read by anything — and neither side has
    // to notice on its own, because each is internally consistent.
    auto segmentFor = [&](uint32_t offset) -> const MetaModel::Segment * {
        for (const auto &s : meta.segments())
            if (offset >= s.base && offset < s.base + static_cast<uint32_t>(s.size)) return &s;
        return nullptr;
    };
    int checkedFields = 0, checkedTables = 0;
    for (const auto &[path, f] : meta.config()) {
        CHECK(!f.datatype.empty());
        const int width = MetaModel::dataSize(f.datatype);
        CHECK(width > 0);                                       // an unknown datatype decodes as garbage
        const MetaModel::Segment *seg = segmentFor(static_cast<uint32_t>(f.offset));
        if (!seg) { std::printf("  FAIL: %s at offset %d is in no segment\n", path.c_str(), f.offset); ++g_fails; continue; }
        CHECK(static_cast<uint32_t>(f.offset) - seg->base + static_cast<uint32_t>(width) <= static_cast<uint32_t>(seg->size));
        ++checkedFields;
    }
    for (const auto &[path, t] : meta.configTables()) {
        CHECK(t.cols > 0 && t.rows > 0);
        const int cell = MetaModel::dataSize(t.datatype);
        CHECK(cell > 0);
        const uint32_t bytes = static_cast<uint32_t>(t.cols) * static_cast<uint32_t>(t.rows)
                             * static_cast<uint32_t>(t.depth > 0 ? t.depth : 1) * static_cast<uint32_t>(cell);
        const MetaModel::Segment *seg = segmentFor(static_cast<uint32_t>(t.offset));
        if (!seg) { std::printf("  FAIL: table %s at offset %d is in no segment\n", path.c_str(), t.offset); ++g_fails; continue; }
        CHECK(static_cast<uint32_t>(t.offset) - seg->base + bytes <= static_cast<uint32_t>(seg->size));
        ++checkedTables;
    }
    std::printf("bounds-checked %d scalar(s) and %d table(s) against their segments\n", checkedFields, checkedTables);

    // --- telemetry channels fit the frame the firmware sends ---------------------------------------
    for (const auto &[name, tf] : meta.telemetry()) {
        CHECK(tf.offset >= 0);
        CHECK(tf.offset + tf.size <= meta.telemetrySize());
    }

    // --- the navigation tree, which is what a studio with no project seeds itself from -------------
    CHECK(!meta.navigationTree().arr().empty());

    std::printf(g_fails ? "studio meta contract: FAILED (%d)\n" : "studio meta contract: OK\n", g_fails);
    return g_fails ? 1 : 0;
}
