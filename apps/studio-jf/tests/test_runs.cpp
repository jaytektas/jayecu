// A RUN — "these values are one thing" — and the ways of asking for one that must NOT succeed.
//
// A run is MetaModel::locate() returning Kind::Run: a 1-D array ("fixture.tune_table") or a field taken
// across one element's nested sub-array ("fixture.strip[1].cell[].v"). This used to be its own function
// with its own descriptor struct; both are gone, because a run is a location like any other and
// Location already carried every field that struct had.
//
// The golden in test_locate_equivalence pins every VALID path. It cannot pin the invalid ones, and those
// are where the silent failures live — so they are here.
//
// Two shapes in the meta are the same thing to a strip widget: a 1-D array ("fixture.tune_table") and a
// nested sub-array inside ONE element of a struct array ("fixture.strip[1].cell[].v"). The second is the
// one worth testing, and specifically its arithmetic — because every way of getting it wrong is silent:
//
//   * dropping the element index reads element 0's run while claiming to be element 1's, so editing a
//     stream's cells would quietly edit another stream's;
//   * using the FIELD's size as the stride instead of the SUB-STRUCT's walks through the neighbouring
//     field of each entry, which for {v, tag} pairs reads v, tag, v, tag and looks almost plausible.
//
// Runs against tests/fixtures/test-meta.json, whose `strip` array exists for this: 3 elements of stride
// 40, each holding an 8-entry cell[] of {v, tag} at rel_offset 4, stride 4.
//
//   cmake --build build --target strip_ref_test && ./build/strip_ref_test
#include "model/MetaModel.h"
#include "model/Cache.h"

#include <cstdio>

static int fails = 0;
static void check(const char *what, bool ok, const std::string &detail = "")
{
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : " — ", detail.c_str());
    if (!ok)
        ++fails;
}

int main()
{
    std::printf("=== runs, and what must not resolve as one ===\n");
    MetaModel m;
    if (!m.loadFile(FIXTURE_META)) {
        std::fprintf(stderr, "cannot load %s\n", FIXTURE_META);
        return 1;
    }

    // Fixture geometry, restated so a change to the fixture fails HERE rather than somewhere downstream.
    constexpr int BASE = 4096, ESTRIDE = 40, CELL_REL = 4, CELL_STRIDE = 4, CELL_N = 8;

    const MetaModel::Location s = m.locate("fixture.strip[0].cell[].v");
    {
        const bool ok = s.kind == MetaModel::Location::Kind::Run;
        check("a nested sub-array resolves as a RUN", ok && s.valid());
        check("  ...at element base + sub rel_offset", ok && s.offset == BASE + CELL_REL,
              "got " + std::to_string(s.offset));
        check("  ...with the SUB-STRUCT stride, not the field size",
              ok && s.stride == CELL_STRIDE, "got " + std::to_string(s.stride));
        check("  ...and the sub-array's count", ok && s.count == CELL_N);
        check("  ...carrying the field's own type and bounds",
              ok && s.datatype == "S16" && s.minV == -1000 && s.maxV == 1000);
    }

    // THE arithmetic that must not be dropped: a later element is a whole element-stride further on.
    {
        const MetaModel::Location s2 = m.locate("fixture.strip[2].cell[].v");
        const bool ok = s2.kind == MetaModel::Location::Kind::Run;
        check("element 2's run is 2 element-strides on",
              ok && s2.offset == BASE + 2 * ESTRIDE + CELL_REL,
              "got " + std::to_string(s2.offset) + ", want " + std::to_string(BASE + 2 * ESTRIDE + CELL_REL));
    }

    // A different FIELD of the same sub-struct shares the stride and shifts by its own rel_offset — this
    // is what proves the stride is the struct's: `tag` steps by 4 as well, not by 2.
    {
        const MetaModel::Location s3 = m.locate("fixture.strip[0].cell[].tag");
        const bool ok = s3.kind == MetaModel::Location::Kind::Run;
        check("a sibling field shares the stride, offset by its own rel",
              ok && s3.stride == CELL_STRIDE && s3.offset == BASE + CELL_REL + 2,
              "got offset " + std::to_string(s3.offset) + " stride " + std::to_string(s3.stride));
    }

    // Shape 1 — a plain 1-D array, where the elements ARE adjacent so the stride is the datatype size.
    {
        const MetaModel::Location s4 = m.locate("fixture.bins");
        const bool ok = s4.kind == MetaModel::Location::Kind::Run;
        check("a 1-D array resolves, stride = element size",
              ok && s4.count == 12 && s4.stride == 2 && s4.offset == 200,
              "count " + std::to_string(s4.count) + " stride " + std::to_string(s4.stride));
        // A MAP is not a run: it has rows, so it is not a run of scalars and must not resolve as one.
        check("a 2-D map is not a run", m.locate("fixture.tune_table").kind != MetaModel::Location::Kind::Run);
    }

    // Rejections. A strip is the RUN; one member of it is not, and must not silently resolve to the run
    // (which would make a windowed widget draw from the wrong place).
    {
        auto notRun = [&](const char* p) { return m.locate(p).kind != MetaModel::Location::Kind::Run; };
        check("one member is not a run",   notRun("fixture.strip[0].cell[3].v"));
        check("a plain element field is not", notRun("fixture.strip[0].n"));
        check("unknown sub-array rejected",  notRun("fixture.strip[0].nope[].v"));
        check("unknown field rejected",      notRun("fixture.strip[0].cell[].nope"));
        check("out-of-range element rejected", notRun("fixture.strip[9].cell[].v"));
        check("empty path rejected",         notRun(""));
    }

    // The dictionary node and the DROP RULE are two halves of one behaviour: a tree row carrying a path
    // that nothing classifies drops a spin box over element 0, which looks like the widget simply not
    // working. So assert the classification here rather than only the arithmetic above.
    {
        Cache &c = Cache::instance();
        c.setMeta(&m);
        const std::string run = "fixture.strip[0].cell[].v";
        check("a run drops a 1D Array", c.widgetTypeFor(run) == "array1d", c.widgetTypeFor(run));
        check("one member still drops a scalar editor",
              c.widgetTypeFor("fixture.strip[0].cell[3].v") != "array1d");
        check("a 1-D array still drops a 1D Array", c.widgetTypeFor("fixture.bins") == "array1d");
        check("the run is captioned by its field", c.label(run) == "Cell", "'" + c.label(run) + "'");
        check("the run hovers with its field's help", !c.help(run).empty(), c.help(run));
    }

    std::printf(fails ? "=== %d FAILED ===\n" : "=== runs resolve, and nothing else does ===\n", fails);
    return fails ? 1 : 0;
}
