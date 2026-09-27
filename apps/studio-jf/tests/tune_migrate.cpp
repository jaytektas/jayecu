// Load a .tune against a meta and say what carried across — the migration, without the studio.
//
// A tune is by PATH, so it survives a firmware update whose layout moved; that is the whole reason it
// is a dictionary and not an image. But "survives" is a claim, and when it fails it fails quietly: the
// merge starts from meta.defaultImage() and overlays what it recognises, so a document that resolves
// nothing produces a perfectly valid image full of defaults. On an ECU that is indistinguishable from
// a tune that migrated cleanly onto a bank the flash had just wiped.
//
// So: run the same TuneFile::deserialise the studio runs, print the report, and print the fields you
// name so you can see the values rather than a count.
//
// A RAW image can be carried across too, which is the recovery path when the only surviving copy of a
// tune is a connect-point snapshot: those are images pinned to the layout they were taken on, so they
// cannot be loaded against new firmware directly. Given the meta they belong to, they can be turned
// into a dictionary first and migrated like anything else — which is the same two steps the studio
// would do, in the order it cannot do them once the firmware has already changed.
//
//   cmake --build build --target tune_migrate
//   ./build/tune_migrate <meta> <tune> [path ...]
//   ./build/tune_migrate <meta> <raw.tune> --from <old-meta> [--out <image>] [path ...]
#include "model/MetaModel.h"
#include "model/TuneFile.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fputs("usage: tune_migrate <meta> <tune> [config.path ...]\n", stderr);
        return 2;
    }
    std::string fromMeta, outPath;
    std::vector<std::string> want;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--from" && i + 1 < argc)      fromMeta = argv[++i];
        else if (a == "--out" && i + 1 < argc)  outPath  = argv[++i];
        else                                    want.push_back(a);
    }
    MetaModel meta;
    if (!meta.loadFile(argv[1])) { std::fprintf(stderr, "cannot load meta %s\n", argv[1]); return 1; }

    std::ifstream f(argv[2], std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open tune %s\n", argv[2]); return 1; }
    const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    std::printf("meta   %s  layout=%s  config=%d B\n", argv[1], meta.layoutHash().c_str(), meta.configSize());
    std::printf("tune   %s  %zu B  %s\n", argv[2], raw.size(),
                TuneFile::isDictFormat(raw) ? "dict (by path — migratable)"
                                            : "RAW IMAGE (pinned to one layout)");
    std::vector<uint8_t> doc = raw;
    if (!TuneFile::isDictFormat(raw)) {
        if (fromMeta.empty()) {
            std::printf("       %s\n", raw.size() == size_t(meta.configSize())
                        ? "size matches this layout, so it would load as-is"
                        : "size does NOT match this layout — it belongs to another firmware\n"
                          "       pass --from <its meta> to carry it across");
            return 0;
        }
        // STEP ONE: read the image with the meta it was taken on. An image only means anything beside
        // the layout that produced it; naming that layout is the whole of what makes it recoverable.
        MetaModel old;
        if (!old.loadFile(fromMeta)) { std::fprintf(stderr, "cannot load --from meta %s\n", fromMeta.c_str()); return 1; }
        if (raw.size() != size_t(old.configSize())) {
            std::fprintf(stderr, "raw image is %zu B; --from meta says %d B — wrong pairing\n",
                         raw.size(), old.configSize());
            return 1;
        }
        doc = TuneFile::serialise(raw, old);
        std::printf("       read against %s (layout %s) -> %zu B of dictionary\n",
                    fromMeta.c_str(), old.layoutHash().c_str(), doc.size());
    }

    MigrationReport rep;
    const std::vector<uint8_t> img = TuneFile::deserialise(doc, meta, rep);
    if (img.empty()) { std::puts("\nDESERIALISE FAILED — no image produced"); return 1; }

    std::printf("\nmigrated %d  defaulted %d  unmapped %zu  unresolved %zu\n",
                rep.migrated, rep.defaulted, rep.unmapped.size(), rep.unresolved.size());
    // A NAME, not just a number: "412 unmapped" says a firmware update dropped something, and which
    // ones tells you whether it mattered.
    for (size_t i = 0; i < rep.unmapped.size() && i < 12; ++i)
        std::printf("   unmapped:   %s\n", rep.unmapped[i].c_str());
    if (rep.unmapped.size() > 12) std::printf("   … and %zu more\n", rep.unmapped.size() - 12);
    for (size_t i = 0; i < rep.unresolved.size() && i < 12; ++i)
        std::printf("   unresolved: %s\n", rep.unresolved[i].c_str());

    // The requested fields, read out of the produced image — the check that actually matters, because
    // a clean report and a defaulted image can look the same from the counts alone.
    for (const std::string& w : want) {
        const char* path = w.c_str();
        const MetaModel::Location L = meta.locate(w);
        if (L.kind != MetaModel::Location::Kind::Scalar || L.offset < 0) {
            std::printf("   %-46s (not a scalar in this meta)\n", path);
            continue;
        }
        if (L.datatype == "ASCII" && L.size > 0) {
            std::printf("   %-46s = \"%s\"\n", path,
                        std::string((const char*)img.data() + L.offset).c_str());
        } else {
            std::printf("   %-46s = %g\n", path,
                        MetaModel::decodeRaw(L.datatype, img.data() + L.offset));
        }
    }
    if (!outPath.empty()) {
        std::ofstream o(outPath, std::ios::binary);
        o.write((const char*)img.data(), std::streamsize(img.size()));
        std::printf("\nwrote %zu B image -> %s\n", img.size(), outPath.c_str());
    }
    return 0;
}
