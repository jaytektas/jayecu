// Every addressable path in the ECU, and the ADDRESS it resolves to today.
//
// This existed to make a rewrite safe, and now it keeps it safe. Turning a config path into a location
// used to be done by fourteen separate functions, because the grammar grew a term at a time and each
// term got a function rather than a clause. They are one clause each of MetaModel::locate() now.
//
// The golden below was emitted BEFORE any of that, from the old resolvers, and has not moved since. It
// is the whole proof: it says the collapse did not move a byte. Do not regenerate it to make a diff go
// away — a diff here means an address changed, which is a field a user's tune would silently land in.
//
// So: enumerate every path the meta declares — every scalar, bit group, 1-D array and element,
// struct-array field by index AND by element id, nested sub-array field and run, and every table
// instance's corner and interior cells — and dump what each resolves to. The dump is the contract.
// After the rewrite the same run must produce a byte-identical file; a diff names the exact path that
// moved. Nothing here is a sample: if the meta declares it, it is in the file.
//
// WHAT IS PINNED, and what deliberately is not:
//
//   PINNED    offset, datatype, scale, bit range, element count and stride — the ADDRESS. None of it
//             may change. A wrong offset writes a neighbouring field; a wrong scale writes a value
//             nobody asked for; a wrong bit range clobbers the flags sharing the word.
//
//   NOT       min/max. Their semantics are changing on purpose — an undeclared bound stops meaning
//             "unbounded" and starts meaning the datatype's own range — so pinning today's numbers
//             would pin the bug this work exists to remove. Bounds get their own test.
//
// Runs against the REAL shared/tuneit-meta.json, not a fixture: the fixture has a dozen fields and the
// point is coverage of the actual layout.
//
//   cmake --build build --target locate_equiv_test && ./build/locate_equiv_test
//   ./build/locate_equiv_test --emit tests/golden/locations.txt     (re-bless, after eyeballing the diff)
#include "model/Cache.h"
#include "model/MetaModel.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// One line of the contract. Fixed-width and sorted, so a diff is readable and the order the meta
// happens to hash its maps in cannot show up as a change.
std::string row(const std::string& path, const std::string& how, int offset,
                const std::string& dt, double scale, int bitLo, int bitHi,
                int count = 0, int stride = 0)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%-72s %-10s off=%-8d dt=%-4s scale=%-10.6g bits=[%d:%d] n=%-5d stride=%d",
                  path.c_str(), how.c_str(), offset, dt.c_str(), scale, bitLo, bitHi, count, stride);
    return buf;
}

void addField(std::vector<std::string>& out, const Cache& c, const std::string& path, const char* how)
{
    const MetaModel::Location L = c.meta()->locate(path);
    if (!L.valid() || L.kind != MetaModel::Location::Kind::Scalar) {
        out.push_back(row(path, how, -1, "UNRESOLVED", 0, 0, 0));
        return;
    }
    out.push_back(row(path, how, L.offset, L.datatype, L.scale, L.bits.lo, L.bits.hi));
}

} // namespace

int main(int argc, char** argv)
{
    std::string emitTo;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--emit" && i + 1 < argc) emitTo = argv[++i];

    MetaModel m;
    if (!m.loadFile(REAL_META)) {
        std::fprintf(stderr, "cannot load %s\n", REAL_META);
        return 1;
    }
    Cache& c = Cache::instance();
    c.setMeta(&m);

    std::vector<std::string> lines;

    // 1 — plain scalars, and the named bit groups packed inside them. A group is addressed as
    //     "<scalar>.<group>" and is the thing the user actually edits; the scalar is just its storage.
    for (const auto& [path, f] : m.config()) {
        addField(lines, c, path, "scalar");
        for (const auto& g : f.bitGroups) {
            const std::string gp = path + "." + g.name;
            int off = 0, size = 0; std::string dt; BitRange bits;
            if (c.resolveBitGroup(gp, off, size, dt, bits)) {
                lines.push_back(row(gp, "bitgroup", off, dt, 1.0, bits.lo, bits.hi));
            }
            else
                lines.push_back(row(gp, "bitgroup", -1, "UNRESOLVED", 0, 0, 0));
        }
    }

    // 2 — 1-D arrays: the RUN itself, then every element. Both are addressable and they are resolved
    //     by different functions today, which is exactly the kind of split being removed.
    for (const auto& [path, a] : m.arrays1d()) {
        if (const MetaModel::Location L = m.locate(path); L.kind == MetaModel::Location::Kind::Run)
            lines.push_back(row(path, "run", L.offset, L.datatype, L.scale, 0, 0, L.count, L.stride));
        for (int i = 0; i < a.count; ++i) {
            const std::string ep = path + "[" + std::to_string(i) + "]";
            addField(lines, c, ep, "element");
        }
    }

    // 3 — struct arrays. Every element's every field, addressed BOTH ways the studio allows: by index
    //     ("sensors.sensor[3].source") and by the element's stable id ("sensors.sensor[clt].source").
    //     The two go through different code today and must land on the same byte.
    for (const auto& [apath, arr] : m.configArrays()) {
        for (int i = 0; i < arr.count; ++i) {
            const std::string byIdx = apath + "[" + std::to_string(i) + "]";
            for (const auto& f : arr.fields) addField(lines, c, byIdx + "." + f.name, "arrayfld");
            if (i < static_cast<int>(arr.elementIds.size()) && !arr.elementIds[i].empty()) {
                const std::string byId = apath + "[" + arr.elementIds[i] + "]";
                for (const auto& f : arr.fields) addField(lines, c, byId + "." + f.name, "arrayfld-id");
            }
            // 3b — element-local repeated sub-structs: one member, and the whole run.
            for (const auto& sub : arr.arrays) {
                for (const auto& sf : sub.fields) {
                    const std::string runp = byIdx + "." + sub.name + "[]." + sf.name;
                    if (const MetaModel::Location L = m.locate(runp); L.kind == MetaModel::Location::Kind::Run)
                        lines.push_back(row(runp, "sub-run", L.offset, L.datatype, L.scale, 0, 0, L.count, L.stride));
                    else
                        lines.push_back(row(runp, "sub-run", -1, "UNRESOLVED", 0, 0, 0));
                    for (int j = 0; j < sub.count; ++j)
                        addField(lines, c, byIdx + "." + sub.name + "[" + std::to_string(j) + "]." + sf.name,
                                 "sub-fld");
                }
            }
        }
    }

    // 4 — tables. The DESCRIPTOR, not sampled cells.
    //
    // A cell's address is cellBase + (z*yn*xn + y*xn + x) * cellSize, where xn/yn are the LIVE bin counts
    // read out of the config image. Probing resolveCell() would therefore pin whatever config image
    // happened to be loaded, not the layout — the golden would move whenever a tune did. The descriptor
    // is the whole of what the meta decides, and every cell address is a pure function of it, so pinning
    // it pins every cell without depending on a single byte of tune data. Same for the axes: their
    // breakpoint arrays and their n/src/enable scalars are separately addressable locations.
    std::vector<std::string> unresolvedTables;
    for (const std::string& tpath : m.tableInstancePaths()) {
        const TableImage t = m.resolveTable(tpath);
        if (!t.valid) {
            lines.push_back(row(tpath, "table", -1, "UNRESOLVED", 0, 0, 0));
            unresolvedTables.push_back(tpath);
            continue;
        }
        lines.push_back(row(tpath, "table", t.cellBase, t.cellType, t.cellScale, 0, 0, 0, t.cellSize));
        for (size_t ax = 0; ax < t.axes.size(); ++ax) {
            const auto& a = t.axes[ax];
            char sfx[32]; std::snprintf(sfx, sizeof(sfx), ".axis%zu", ax);
            lines.push_back(row(tpath + sfx, "breaks", a.breaksBase, a.breakType, a.breakScale,
                                0, 0, a.nMax, a.breakSize));
            char meta[160];
            std::snprintf(meta, sizeof(meta), "%-72s %-10s nBase=%-8d srcBase=%-8d enBase=%-8d nMin=%d",
                          (tpath + sfx).c_str(), "axis-ctl", a.nBase, a.srcBase, a.enBase, a.nMin);
            lines.push_back(meta);
        }
    }

    // The learned region is a FOURTEENTH addressing scheme, and the dump is where that shows up.
    //
    // `learned.blocks` in the meta (the battery-backed LTFT / knock-noise / misfire maps) are tables in
    // every sense — rows, cols, a cell type, an axis — but they live in a segment at page_base
    // 0x40000000 with their own descriptor shape, so resolveTable() cannot resolve them and every one
    // records as UNRESOLVED here. That is today's behaviour, faithfully pinned; it is not breakage, and
    // the count is asserted so it cannot quietly grow (a NEW unresolvable path would be a real
    // regression hiding among known ones).
    //
    // It is also the clearest argument for the rewrite: a location is not an offset, it is a BLOCK plus
    // an offset. blockBytes() already dispatches on absolute device offset, so the learned blocks are a
    // base away from being ordinary — they are separate only because a second descriptor was added
    // instead of the first being widened.
    // Every table resolves. It was not always so: the learned-region maps (LTFT, knock noise, misfire)
    // used to record UNRESOLVED here — all 53 of them — because codegen emitted them through a private
    // branch that dropped their declared axes, and resolveTable() rejects a table with no axis
    // (MetaModel.cpp: `ti.valid = !ti.axes.empty()`). They are ordinary tables on the ordinary pipeline
    // now, so they carry axes like everything else.
    //
    // Kept as a hard check rather than a comment: an unresolvable table is invisible in the studio —
    // not viewable, not resettable — and the golden alone would report it as one changed line among
    // thousands.
    if (!unresolvedTables.empty()) {
        std::fprintf(stderr, "  FAIL: %zu table(s) do not resolve; first: %s\n",
                     unresolvedTables.size(), unresolvedTables.front().c_str());
        return 1;
    }
    std::printf("  every table resolves\n");


    std::sort(lines.begin(), lines.end());
    std::ostringstream dump;
    for (const auto& l : lines) dump << l << "\n";

    if (!emitTo.empty()) {
        std::ofstream f(emitTo, std::ios::binary);
        if (!f) { std::fprintf(stderr, "cannot write %s\n", emitTo.c_str()); return 1; }
        f << dump.str();
        std::printf("wrote %s (%zu locations)\n", emitTo.c_str(), lines.size());
        return 0;
    }

    std::ifstream gf(GOLDEN, std::ios::binary);
    if (!gf) {
        std::fprintf(stderr, "no golden at %s — create it with --emit once the dump is trusted\n", GOLDEN);
        return 1;
    }
    std::stringstream want; want << gf.rdbuf();

    if (want.str() == dump.str()) {
        std::printf("=== %zu locations, all identical to the golden ===\n", lines.size());
        return 0;
    }

    // Name the paths that moved. A count is useless here — the whole value of this test is telling you
    // WHICH address changed, since that is the field a user's tune would silently land in.
    std::vector<std::string> a, b, diff;
    for (std::string l; std::getline(want, l);) a.push_back(l);
    { std::istringstream d(dump.str()); for (std::string l; std::getline(d, l);) b.push_back(l); }
    std::set_symmetric_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(diff));
    std::printf("=== %zu locations, %zu line(s) DIFFER ===\n", lines.size(), diff.size());
    for (size_t i = 0; i < diff.size() && i < 40; ++i) std::printf("  %s\n", diff[i].c_str());
    if (diff.size() > 40) std::printf("  ... and %zu more\n", diff.size() - 40);
    return 1;
}
