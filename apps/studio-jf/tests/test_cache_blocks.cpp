// Headless regression for the Cache "block" abstraction: config is block 0 (base 0, the tune); the
// RAM-backed learned region (persisted to SD totems) is a second block addressed by the SAME meta-path
// offsets but held in its
// own buffer. Proves a learned table reads/writes through the ordinary config-path API, flushes to its
// device offset, and that plain config paths still resolve against block 0.
//
// Runs against tests/fixtures/test-meta.json — a definition written FOR this test: one tune segment, one
// RAM-backed learned segment, a table in each. Not a firmware build product: the studio is a tuning
// studio for whatever ECU is in front of it, and most of the people running it will never have the
// jayecu firmware at all, so its tests cannot need a file that firmware emits.
//
//   cmake --build build --target cache_blocks_test && ./build/cache_blocks_test
#include "model/MetaModel.h"
#include "model/Cache.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

static std::vector<uint8_t> f32bytes(float v) { std::vector<uint8_t> b(4); std::memcpy(b.data(), &v, 4); return b; }

int main()
{
    MetaModel meta;
    if (!meta.loadFile(FIXTURE_META)) { std::printf("could not load fixture: %s\n", FIXTURE_META); return 2; }

    // --- meta: the segments descriptor + the learned grids as ordinary config-path tables --------------
    const MetaModel::Segment *learned = nullptr;
    for (const auto &s : meta.segments()) if (s.id == "learned") learned = &s;
    CHECK(learned != nullptr);
    if (learned) {
        std::printf("learned segment: base 0x%08X, size %d\n", learned->base, learned->size);
        CHECK(learned->base == 0x40000000u);
        CHECK(learned->size == 128);
    }
    CHECK(meta.configTables().count("fixture.learned_table") == 1);
    const auto &vt = meta.configTables().at("fixture.learned_table");
    CHECK(vt.offset == 0x40000000);         // segment base + 0 — cells start at the segment base, no magic
    CHECK(vt.cols == 8 && vt.rows == 4);

    Cache &c = Cache::instance();
    c.setMeta(&meta);
    c.initSegments();                        // allocate the non-tune blocks (learned region)

    std::vector<std::pair<int, std::vector<uint8_t>>> writes;
    c.writeRequested.connect([&](int off, const std::vector<uint8_t> &b) { writes.push_back({off, b}); });

    // --- a segment READ lands via the same splice path config reads use ------------------------------
    c.applyConfigRange(0x40000000, f32bytes(2.5f));                 // vvt_ltt cell (col0,row0)
    CHECK(std::abs(c.tableCell("fixture.learned_table", 0, 0) - 2.5) < 1e-6);   // read routes to the segment

    // --- a learned-cell EDIT flushes to the learned device offset (not the config block) -------------
    writes.clear();
    c.setTableCell("fixture.learned_table", 1, 0, 3.25);             // col1,row0 -> base + 1 cell (4 B)
    c.flushWrites();
    // The flush emits the MINIMAL changed byte range (same as the config path), so the exact width/offset
    // depends on the value — assert only that it lands inside the learned device-offset range (block 1),
    // never against the config block, and that the cell now reads back through that block.
    const int lbeg = int(learned->base), lend = lbeg + learned->size;
    bool learnedFlush = false, onlyLearned = true;
    for (const auto &w : writes) {
        const bool inLearned = w.first >= lbeg && w.first < lend;
        learnedFlush |= inLearned;
        onlyLearned &= inLearned;
    }
    CHECK(learnedFlush);
    CHECK(onlyLearned);   // a learned edit must not spill a write into the config/tune block
    CHECK(std::abs(c.tableCell("fixture.learned_table", 1, 0) - 3.25) < 1e-6);

    // --- reset (write zeros) round-trips through the block too ----------------------------------------
    c.setTableCell("fixture.learned_table", 1, 0, 0.0);
    CHECK(std::abs(c.tableCell("fixture.learned_table", 1, 0)) < 1e-9);

    // --- regression: plain config still resolves against block 0, and its flush is a small offset -----
    c.setConfigImage(meta.defaultImage());
    CHECK(meta.config().count("fixture.enabled") == 1);
    writes.clear();
    c.setConfigValue("fixture.enabled", 1);
    c.flushWrites();
    CHECK(c.configValue("fixture.enabled") == 1.0);
    bool sawConfigWrite = false;
    for (const auto &w : writes) if (w.first >= 0 && w.first < int(meta.defaultImage().size())) sawConfigWrite = true;
    CHECK(sawConfigWrite);

    std::printf(g_fails ? "cache blocks: FAILED (%d)\n" : "cache blocks: OK\n", g_fails);
    return g_fails ? 1 : 0;
}
