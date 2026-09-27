#include "TuneDiff.h"

#include "TuneFile.h"

#include "../surface/PanelLibrary.h"
#include "../surface/PanelModel.h"

#include <algorithm>
#include <functional>
#include <cmath>
#include <map>
#include <set>

namespace tunediff {
namespace {

// A value as the page would read it: raw bytes through the field's own datatype and scale. Comparing
// raw counts would be enough to spot a difference, but the dialog has to SHOW both, and a tuner reads
// 850 rpm, not 8500.
double valueAt(const MetaModel& m, const std::vector<uint8_t>& img, const MetaModel::Location& L,
               int cell = 0) {
    const int off = L.offset + cell * std::max(1, MetaModel::dataSize(L.datatype));
    if (off < 0 || off + MetaModel::dataSize(L.datatype) > static_cast<int>(img.size())) return 0.0;
    const double raw = MetaModel::decodeRaw(L.datatype, img.data() + off, m.bigEndian());
    return L.bits.packed() ? L.bits.extract(raw, MetaModel::dataSize(L.datatype)) * L.scale
                          : raw * L.scale;
}

// A text field's characters, as the page would read them: the field's bytes up to its first NUL, or the
// whole field when it is full (a VIN fills all seventeen of its bytes and has no terminator).
std::string textAt(const MetaModel& m, const std::vector<uint8_t>& img, const std::string& path) {
    int off = 0, size = 0;
    if (!m.resolveBlob(path, off, size)) return {};
    if (off < 0 || size <= 0 || off + size > static_cast<int>(img.size())) return {};
    const char* p = reinterpret_cast<const char*>(img.data()) + off;
    const size_t n = static_cast<size_t>(std::find(p, p + size, '\0') - p);
    return std::string(p, n);
}

// EQUAL TO THE PRECISION THE FIELD IS READ AT. A float cell that differs in the last bit of its
// mantissa is not a difference anybody can see or act on, and reporting it would fill the report with
// noise that hides the one setting that matters.
bool same(double a, double b, double scale) {
    const double eps = std::max(std::fabs(scale) * 0.5, 1e-9);
    return std::fabs(a - b) < eps;
}

// The decimals a field is read at, which is a fact about its scale: a raw count worth 0.01 of a lambda
// is read to two places, a whole rpm to none.
int decimalsFor(double scale) {
    const double s = std::fabs(scale);
    if (s <= 0.0) return 0;
    for (int d = 0; d <= 6; ++d)
        if (s * std::pow(10.0, d) >= 0.999) return d;
    return 6;
}

// WHICH PAGE SHOWS THIS BINDING. Built once from the document: every element's Data Source is a
// binding path, so the library IS the map from setting to page. A binding on several pages takes the
// first in tree order — the report needs one home for it, not a list.
//
// NESTED, because the document is. A page's elements() stops at its top level, and a panel keeps its
// children as JSON inside itself: the O2 Control page is one checkbox and seven panels, so a walk over
// elements() alone finds ONE of its bindings and calls the other twenty homeless. That is how
// Long-Term Trim came to be reported as a setting on no page while sitting on the same page as the O2
// enable, three rows below it. collectBindings recurses.
std::map<std::string, std::string> pageOfBinding(const PanelLibrary* lib) {
    std::map<std::string, std::string> out;
    if (!lib) return out;
    for (const auto& [node, page] : lib->pages()) {
        if (!page) continue;
        for (const PanelElement& e : page->elements())
            collectBindings(e, [&](const std::string& b) {
                out.emplace(b, node);        // emplace: first page in tree order wins
            });
    }
    return out;
}

std::string leafOf(const std::string& node) {
    const size_t s = node.rfind('/');
    return s == std::string::npos ? node : node.substr(s + 1);
}

}  // namespace

Report compare(const MetaModel& meta, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
               const PanelLibrary* lib) {
    Report r;
    if (a.empty() || b.empty()) return r;
    const std::map<std::string, std::string> home = pageOfBinding(lib);
    std::map<std::string, std::vector<Item>> byPage;

    // ---- scalars -------------------------------------------------------------------------------
    for (const auto& [path, f] : meta.config()) {
        const MetaModel::Location L = meta.locate(path);
        if (L.kind != MetaModel::Location::Kind::Scalar) continue;
        // TEXT IS COMPARED AS TEXT. Decoding a name through a numeric datatype reads the first four bytes
        // and calls everything past them equal, so two tunes could differ in a VIN or an engine name and
        // this would report nothing — the studio then says the ECU has changed and cannot say how.
        if (f.isText()) {
            const std::string sa = textAt(meta, a, path), sb = textAt(meta, b, path);
            if (sa == sb) continue;
            Item it;
            it.path = path;
            it.label = f.label.empty() ? path : f.label;
            it.isText = true; it.textA = sa; it.textB = sb;
            const auto h = home.find(path);
            (h == home.end() ? r.unpaged : byPage[h->second]).push_back(it);
            ++r.settings;
            continue;
        }
        const double va = valueAt(meta, a, L), vb = valueAt(meta, b, L);
        if (same(va, vb, L.scale)) continue;
        Item it;
        it.path  = path;
        it.label = f.label.empty() ? path : f.label;
        it.a = va; it.b = vb;
        it.decimals = decimalsFor(L.scale);
        const auto h = home.find(path);
        (h == home.end() ? r.unpaged : byPage[h->second]).push_back(it);
        ++r.settings;
    }

    // ---- tables: one entry per map, counting the cells that differ -------------------------------
    // A 22x23 map with one changed cell is ONE line in the report saying so, not 506 lines. The count
    // is the thing a tuner weighs: "VE Table — 1 cell" is a tweak, "VE Table — 313 cells" is a
    // different tune.
    for (const auto& [path, t] : meta.configTables()) {
        const MetaModel::Location L = meta.locate(path);
        if (L.kind != MetaModel::Location::Kind::Table || L.count <= 0) continue;
        int n = 0;
        for (int c = 0; c < L.count; ++c)
            if (!same(valueAt(meta, a, L, c), valueAt(meta, b, L, c), L.scale)) ++n;
        if (n == 0) continue;
        Item it;
        it.path  = path;
        it.label = t.label.empty() ? path : t.label;
        it.table = true; it.cells = n;
        const auto h = home.find(path);
        (h == home.end() ? r.unpaged : byPage[h->second]).push_back(it);
        ++r.settings;
    }

    // ---- COMPLETENESS ------------------------------------------------------------------------------
    // "They differ" is decided by comparing what TuneFile::serialise writes. This walk covers config
    // scalars and tables, which is most of that but not all of it — and when the two disagreed the studio
    // announced a changed ECU and then had nothing to show, dropping to a bare "keep which one?" prompt.
    // So whatever serialise can see and the walk could not is picked up here, by path. It cannot produce
    // the nice per-cell counts above; it can produce the one thing that matters, which is naming the
    // setting rather than leaving the reader to guess.
    {
        std::set<std::string> reported;
        for (const auto& [n, items] : byPage) for (const Item& it : items) reported.insert(it.path);
        for (const Item& it : r.unpaged) reported.insert(it.path);

        const auto sa = TuneFile::serialise(a, meta), sb = TuneFile::serialise(b, meta);
        if (sa != sb) {
            const auto ja = jf::JJson::tryParse(std::string(sa.begin(), sa.end()));
            const auto jb = jf::JJson::tryParse(std::string(sb.begin(), sb.end()));
            if (ja && jb) {
                for (const char* section : { "scalars", "tables", "axes", "arrays", "expressions" }) {
                    const jf::JJson& A = (*ja)[section];
                    const jf::JJson& B = (*jb)[section];
                    if (!A.isObject() || !B.isObject()) continue;
                    for (const auto& [key, va] : A.obj()) {
                        if (reported.count(key)) continue;
                        const jf::JJson& vb = B[key];
                        if (va.dump(-1) == vb.dump(-1)) continue;
                        Item it;
                        it.path  = key;
                        it.isText = true;                    // shown as written, whatever shape it is
                        it.textA = va.dump(-1); it.textB = vb.dump(-1);
                        if (it.textA.size() > 40) it.textA = it.textA.substr(0, 37) + "...";
                        if (it.textB.size() > 40) it.textB = it.textB.substr(0, 37) + "...";
                        const auto cf = meta.config().find(key);
                        it.label = (cf != meta.config().end() && !cf->second.label.empty())
                                 ? cf->second.label : key;
                        const auto h = home.find(key);
                        (h == home.end() ? r.unpaged : byPage[h->second]).push_back(it);
                        ++r.settings;
                        reported.insert(key);
                    }
                }
            }
        }
    }

    for (auto& [node, items] : byPage) {
        std::sort(items.begin(), items.end(),
                  [](const Item& x, const Item& y) { return x.label < y.label; });
        r.pages.push_back(Page{ node, leafOf(node), std::move(items) });
    }
    std::sort(r.pages.begin(), r.pages.end(),
              [](const Page& x, const Page& y) { return x.node < y.node; });
    std::sort(r.unpaged.begin(), r.unpaged.end(),
              [](const Item& x, const Item& y) { return x.label < y.label; });
    return r;
}

namespace {

// Every setting a firmware's tune can hold, by path, with its value in `img` — the five sections
// TuneFile::serialise writes, which is the complete set the migration moves by name.
std::map<std::string, std::string> settingsOf(const MetaModel& m, const std::vector<uint8_t>& img) {
    std::map<std::string, std::string> out;
    const auto bytes = TuneFile::serialise(img, m);
    const auto j = jf::JJson::tryParse(std::string(bytes.begin(), bytes.end()));
    if (!j) return out;
    for (const char* section : { "scalars", "tables", "axes", "arrays", "expressions" })
        for (const auto& [key, v] : (*j)[section].obj()) {
            // ALIGNMENT PADDING IS NOT A SETTING. The codegen names it "_align_pad_N" and it holds no
            // value anyone sets; a layout that moved a few fields reported a thousand "new settings".
            const size_t dot = key.find_last_of(".]");
            const std::string leaf = key.substr(dot == std::string::npos ? 0 : dot + 1);
            if (leaf.rfind("_align_pad", 0) == 0 || leaf.rfind("._align_pad", 0) == 0) continue;
            out.emplace(key, v.dump(-1));
        }
    return out;
}

// One setting as a report line, valued from `img` through its own firmware's meta.
Item itemFor(const MetaModel& m, const std::vector<uint8_t>& img, const std::string& path, bool leftSide) {
    Item it;
    it.path = path;
    const auto cf = m.config().find(path);
    const auto ct = m.configTables().find(path);
    it.label = (cf != m.config().end() && !cf->second.label.empty()) ? cf->second.label
             : (ct != m.configTables().end() && !ct->second.label.empty()) ? ct->second.label : path;
    const MetaModel::Location L = m.locate(path);
    double v = 0.0;
    if (cf != m.config().end() && cf->second.isText()) {
        it.isText = true;
        (leftSide ? it.textA : it.textB) = textAt(m, img, path);
    } else if (L.kind == MetaModel::Location::Kind::Scalar) {
        v = valueAt(m, img, L);
        it.decimals = decimalsFor(L.scale);
    } else if (L.kind == MetaModel::Location::Kind::Table) {
        it.table = true; it.cells = L.count;
    } else {
        it.isText = true;                                 // an axis, an array cell, an expression: as written
        (leftSide ? it.textA : it.textB) = "";
    }
    (leftSide ? it.a : it.b) = v;
    return it;
}

// The deepest navigation-tree node whose target covers `path` ("config.engine" covers engine.anything),
// as the names down to it: "Configuration > Engine". Empty when the tree does not file it anywhere.
std::string navPlace(const MetaModel& m, const std::string& path) {
    const std::string target = "config." + path;
    std::string best;
    size_t bestLen = 0;
    std::function<void(const jf::JJson&, const std::string&)> walk = [&](const jf::JJson& nodes, const std::string& trail) {
        for (const jf::JJson& n : nodes.arr()) {
            const std::string name = n["name"].str();
            const std::string here = trail.empty() ? name : trail + " > " + name;
            const std::string tp = n["target_path"].str();
            if (!tp.empty() && tp.size() > bestLen &&
                (target == tp || (target.rfind(tp, 0) == 0 && (target[tp.size()] == '.' || target[tp.size()] == '[')))) {
                best = here; bestLen = tp.size();
            }
            walk(n["children"], here);
        }
    };
    walk(m.navigationTree(), "");
    return best;
}

Report group(std::vector<Item> items, const PanelLibrary* lib) {
    Report r;
    const std::map<std::string, std::string> home = pageOfBinding(lib);
    std::map<std::string, std::vector<Item>> byPage;
    for (Item& it : items) {
        const auto h = home.find(it.path);
        (h == home.end() ? r.unpaged : byPage[h->second]).push_back(std::move(it));
        ++r.settings;
    }
    for (auto& [node, list] : byPage) {
        std::sort(list.begin(), list.end(), [](const Item& x, const Item& y) { return x.label < y.label; });
        r.pages.push_back(Page{ node, leafOf(node), std::move(list) });
    }
    std::sort(r.pages.begin(), r.pages.end(), [](const Page& x, const Page& y) { return x.node < y.node; });
    std::sort(r.unpaged.begin(), r.unpaged.end(), [](const Item& x, const Item& y) { return x.label < y.label; });
    return r;
}

}  // namespace

FirmwareChanges compareFirmware(const MetaModel& oldMeta, const std::vector<uint8_t>& tune, const PanelLibrary* oldLib,
                                const MetaModel& newMeta, const std::vector<uint8_t>& migrated, const PanelLibrary* newLib,
                                const std::vector<std::string>& unresolved) {
    FirmwareChanges fc;
    const auto oldSet      = settingsOf(oldMeta, tune);
    const auto oldDefaults = settingsOf(oldMeta, oldMeta.defaultImage());
    const auto newSet      = settingsOf(newMeta, migrated);

    std::vector<Item> retired, added;
    for (const auto& [path, value] : oldSet) {
        if (newSet.count(path)) continue;                 // still there: it came across
        const auto d = oldDefaults.find(path);
        if (d != oldDefaults.end() && d->second == value) continue;   // never set: nothing is lost
        retired.push_back(itemFor(oldMeta, tune, path, true));
    }
    for (const auto& [path, value] : newSet)
        if (!oldSet.count(path)) added.push_back(itemFor(newMeta, migrated, path, false));

    fc.retired = group(std::move(retired), oldLib);
    fc.added   = group(std::move(added), newLib);
    for (Item& it : fc.retired.unpaged) it.where = navPlace(oldMeta, it.path);
    for (Item& it : fc.added.unpaged)   it.where = navPlace(newMeta, it.path);
    // A reference to something the new firmware no longer has: the setting survives, its target does not.
    for (const std::string& u : unresolved) {
        Item it;
        it.path = u.substr(0, u.find(' '));
        it.label = it.path;
        it.isText = true;
        it.textA = u.find("-> ") != std::string::npos ? u.substr(u.find("-> ") + 3) + " (no longer exists)" : u;
        fc.retired.unpaged.push_back(it);
        ++fc.retired.settings;
    }
    return fc;
}

}  // namespace tunediff
