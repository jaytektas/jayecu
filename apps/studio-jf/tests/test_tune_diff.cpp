// WHAT DIFFERS BETWEEN TWO TUNES, and whether it is worth showing.
//
// This is the evidence behind the one question the studio asks that cannot be taken back: the ECU does
// not match the saved tune, choose which survives. Until there was a report, that choice was made
// blind — two buttons over 141 KB of config, with no way to tell one changed idle target from a
// different engine.
//
// Three things have to hold or the report is worse than none: it must find a change, it must not
// invent one, and it must not drown the real one in noise.
//
//   cmake --build build --target tune_diff_test && ./build/tune_diff_test
#include "model/MetaModel.h"
#include "model/TuneDiff.h"
#include "model/TuneFile.h"
#include "model/Cache.h"
#include "surface/PanelLibrary.h"
#include "surface/PanelModel.h"

#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  %s  %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : ("   - " + detail).c_str());
    if (!ok) ++fails;
}

static const tunediff::Item* find(const tunediff::Report& r, const std::string& path) {
    for (const auto& p : r.pages)
        for (const auto& i : p.items) if (i.path == path) return &i;
    for (const auto& i : r.unpaged) if (i.path == path) return &i;
    return nullptr;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    const std::vector<uint8_t> base = m.defaultImage();
    ck(!base.empty(), "the definition carries a default tune to compare against",
       std::to_string(base.size()) + " B");

    // IDENTICAL TUNES DIFFER IN NOTHING. A byte-wise compare would already pass this; the point is that
    // walking every binding and decoding both sides does too — no field reads differently from itself.
    {
        const tunediff::Report r = tunediff::compare(m, base, base, nullptr);
        ck(r.empty() && r.settings == 0, "a tune compared with itself reports nothing",
           std::to_string(r.settings) + " setting(s)");
    }

    // ONE CHANGED SETTING IS ONE LINE, with both values in the units the page shows.
    {
        const MetaModel::Location L = m.locate("idle.idle_lockout_rpm");
        ck(L.kind == MetaModel::Location::Kind::Scalar, "idle.idle_lockout_rpm resolves");
        if (L.kind != MetaModel::Location::Kind::Scalar) { std::printf("  (skipped: no such field)\n"); return fails ? 1 : 1; }
        std::vector<uint8_t> ecu = base;
        const double was = MetaModel::decodeRaw(L.datatype, base.data() + L.offset, m.bigEndian()) * L.scale;
        // +200 rpm, written through the field's own scale so the image stays legal.
        const double want = was + 200.0;
        const long long raw = std::llround(want / (L.scale == 0.0 ? 1.0 : L.scale));
        for (int b = 0; b < MetaModel::dataSize(L.datatype); ++b)
            ecu[static_cast<size_t>(L.offset) + b] = static_cast<uint8_t>((raw >> (8 * b)) & 0xFF);

        const tunediff::Report r = tunediff::compare(m, base, ecu, nullptr);
        ck(r.settings == 1, "one changed setting is one difference",
           std::to_string(r.settings) + " setting(s)");
        const tunediff::Item* it = find(r, "idle.idle_lockout_rpm");
        ck(it != nullptr, "…and it is the one that was changed");
        if (it) {
            ck(std::fabs(it->a - was) < 0.5 && std::fabs(it->b - want) < 0.5,
               "…carrying BOTH values, in the units a tuner reads",
               std::to_string(it->a) + " vs " + std::to_string(it->b));
            ck(!it->label.empty() && it->label != it->path,
               "…named the way the page names it", it->label);
        }
    }

    // A MAP IS ONE LINE AND A COUNT. 313 changed cells is a different tune; one is a tweak — and 506
    // lines saying "cell" would bury the idle target above.
    {
        const MetaModel::Location L = m.locate("fuel_calculator.ve_table");
        ck(L.kind == MetaModel::Location::Kind::Table && L.count > 100,
           "the VE table resolves as a map", std::to_string(L.count) + " cells");
        std::vector<uint8_t> ecu = base;
        const int sz = MetaModel::dataSize(L.datatype);
        for (int c = 0; c < 3; ++c) {                     // three cells, well apart
            const size_t off = static_cast<size_t>(L.offset) + static_cast<size_t>(c * 37) * sz;
            if (off + sz <= ecu.size()) ecu[off] = static_cast<uint8_t>(ecu[off] + 40);
        }
        const tunediff::Report r = tunediff::compare(m, base, ecu, nullptr);
        const tunediff::Item* it = find(r, "fuel_calculator.ve_table");
        ck(it && it->table, "a changed map is reported as a map, not as cells");
        if (it) ck(it->cells == 3, "…with the number of cells that differ",
                   std::to_string(it->cells));
        ck(r.settings == 1, "…and counts as one difference", std::to_string(r.settings));
    }

    // NOTHING IS LOST WHEN THERE IS NO DOCUMENT. Without a page library every difference still has to
    // be reported — as unpaged — because "no differences" over a real one is the failure this exists
    // to prevent.
    {
        std::vector<uint8_t> ecu = base;
        const MetaModel::Location L = m.locate("idle.idle_lockout_rpm");
        if (L.offset >= 0 && static_cast<size_t>(L.offset) < ecu.size())
            ecu[static_cast<size_t>(L.offset)] = static_cast<uint8_t>(ecu[L.offset] + 7);
        const tunediff::Report r = tunediff::compare(m, base, ecu, nullptr);
        ck(!r.empty() && !r.unpaged.empty(),
           "with no page library the difference is still reported, as unpaged",
           std::to_string(r.unpaged.size()) + " item(s)");
    }

    // ---- A SETTING INSIDE A PANEL IS STILL ON THE PAGE ----------------------------------------------
    // A panel keeps its children as JSON inside itself rather than as elements of the page, so the page
    // walk saw one checkbox and seven panels and called every control inside them homeless. On the real
    // document that is most of the document: Long-Term Trim was reported as a setting on no page while
    // it sat three rows below the O2 enable it was grouped with. Nested two deep here, because a panel
    // can hold a panel and the document does. Flatten collectBindings back to the top level and this
    // goes red.
    {
        std::vector<uint8_t> ecu = base;
        const MetaModel::Location L = m.locate("idle.idle_lockout_rpm");
        ecu[static_cast<size_t>(L.offset)] = static_cast<uint8_t>(ecu[L.offset] + 9);

        PanelLibrary lib;
        PanelModel& page = lib.forNode("Configuration/Idle Control");
        page.add("panel", 0, 0, 400, 200, {{ "children",
            "[{\"id\":2,\"type\":\"panel\",\"x\":0,\"y\":0,\"w\":380,\"h\":180,\"props\":{\"children\":"
            "\"[{\\\"id\\\":3,\\\"type\\\":\\\"numberfield\\\",\\\"x\\\":0,\\\"y\\\":0,\\\"w\\\":80,"
            "\\\"h\\\":24,\\\"props\\\":{\\\"signalName\\\":\\\"idle.idle_lockout_rpm\\\"}}]\"}}]" }});
        // The data has to be what the document's really is, or this tests nothing: one top-level
        // element, no binding on it, and the setting two levels down inside it.
        ck(page.elements().size() == 1 && page.elements()[0].prop("signalName").empty(),
           "the fixture page carries the binding only inside its panel");

        const tunediff::Report r = tunediff::compare(m, base, ecu, &lib);
        ck(r.unpaged.empty(), "a setting nested in a panel is not reported as being on no page",
           r.unpaged.empty() ? "" : (std::to_string(r.unpaged.size()) + " unpaged: " + r.unpaged[0].path));
        ck(r.pages.size() == 1 && r.pages[0].node == "Configuration/Idle Control",
           "…it is reported on the page whose panel holds it",
           r.pages.empty() ? "no pages" : r.pages[0].node);
    }

    // ---- THE SEAM THE SIDE-BY-SIDE RENDER RESTS ON --------------------------------------------------
    // The report draws the same page twice, once per tune, by swapping the cache's config image
    // underneath the second render (Cache::ImageScope). Every control reads through the singleton, so
    // if that swap does not reach an ordinary read, both halves of the dialog show the SAME tune and the
    // report is a confident lie. Reintroduce the bug by deleting the swap in ImageScope and the second
    // assertion below goes red.
    {
        const MetaModel::Location L = m.locate("idle.idle_lockout_rpm");
        std::vector<uint8_t> other = base;
        const double was = MetaModel::decodeRaw(L.datatype, base.data() + L.offset, m.bigEndian()) * L.scale;
        const double want = was + 200.0;
        const long long raw = std::llround(want / (L.scale == 0.0 ? 1.0 : L.scale));
        for (int b = 0; b < MetaModel::dataSize(L.datatype); ++b)
            other[static_cast<size_t>(L.offset) + b] = static_cast<uint8_t>((raw >> (8 * b)) & 0xFF);

        Cache& c = Cache::instance();
        c.setMeta(&m);
        c.setConfigImage(base);
        const bool roBefore = c.readOnly();
        ck(std::fabs(c.configValue("idle.idle_lockout_rpm") - was) < 0.5,
           "the cache reads the image it was given", std::to_string(c.configValue("idle.idle_lockout_rpm")));
        {
            Cache::ImageScope scope(c, other);
            ck(std::fabs(c.configValue("idle.idle_lockout_rpm") - want) < 0.5,
               "inside an image scope every read resolves against the OTHER tune",
               std::to_string(c.configValue("idle.idle_lockout_rpm")));
            // AND NOTHING CAN BE WRITTEN INTO A BORROWED BUFFER. A write here would be swapped away on
            // the way out: an edit that silently did nothing, on a page that looks live.
            ck(c.readOnly(), "…and the tune is read-only while it is open");
        }
        ck(std::fabs(c.configValue("idle.idle_lockout_rpm") - was) < 0.5,
           "…and the studio's own image is back afterwards",
           std::to_string(c.configValue("idle.idle_lockout_rpm")));
        ck(c.readOnly() == roBefore, "…along with the mode it interrupted");
        // The borrowed vector is handed back untouched, which is what lets the dialog keep and reuse it.
        std::vector<uint8_t> expect = base;
        for (int b = 0; b < MetaModel::dataSize(L.datatype); ++b)
            expect[static_cast<size_t>(L.offset) + b] = static_cast<uint8_t>((raw >> (8 * b)) & 0xFF);
        ck(other == expect, "…and the borrowed tune is unchanged, so the next page can draw it again");
    }

    // ---- THE REPORT MUST EXPLAIN THE VERDICT --------------------------------------------------------
    // Whether two tunes differ is decided by comparing what TuneFile::serialise writes. If the report
    // walks a smaller set, the studio can announce that the ECU no longer matches and then have nothing
    // to show for it — which is exactly what happened: a bare "keep which one?" prompt instead of the
    // pages. This is the invariant that cannot be allowed to drift.
    {
        std::vector<uint8_t> ecu = base;
        // Change a byte well past the fourth of a TEXT field. Compared as a number, only the first four
        // bytes are read and the two look identical; compared as text, they do not.
        int off = 0, size = 0;
        const bool haveText = m.resolveBlob("ts.vehicleName", off, size) ||
                              m.resolveBlob("vehicle.name", off, size);
        if (haveText && size > 8 && off + size <= int(ecu.size())) {
            for (int i = 0; i < 6; ++i) ecu[size_t(off + i)] = uint8_t('A' + i);
            std::vector<uint8_t> ecu2 = ecu;
            ecu2[size_t(off + 5)] = 'Z';                       // differs only at byte 6
            const bool differ = TuneFile::serialise(ecu, m) != TuneFile::serialise(ecu2, m);
            ck(differ, "two tunes differing past a text field's fourth byte are NOT in sync");
            if (differ) {
                const tunediff::Report r = tunediff::compare(m, ecu, ecu2, nullptr);
                ck(!r.empty(),
                   "…and the report can say so, rather than leaving a bare keep-which-one prompt",
                   std::to_string(r.settings) + " setting(s)");
            }
        } else {
            std::printf("  (no text field in this definition to exercise)\n");
        }
    }

    std::printf("\n%s\n", fails ? (std::to_string(fails) + " FAILED").c_str() : "ALL PASSED");
    return fails ? 1 : 0;
}
