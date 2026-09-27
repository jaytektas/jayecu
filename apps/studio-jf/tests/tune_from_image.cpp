// Write a .tune DICTIONARY from a raw config image — the inverse of tune_migrate's read side.
//
// Needed because a tune's master copy is the studio's dictionary file, not the ECU. Repairing the
// ECU while the dictionary still holds the broken value fixes nothing: the next push puts it back.
// tune_migrate can already turn a dictionary into an image; this closes the loop so an image that
// has been corrected can become the dictionary again.
//
//   cmake --build build --target tune_from_image
//   ./build/tune_from_image <meta> <image.bin> <out.tune>
#include "model/MetaModel.h"
#include "model/TuneFile.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fputs("usage: tune_from_image <meta> <image.bin> <out.tune>\n", stderr);
        return 2;
    }
    MetaModel meta;
    if (!meta.loadFile(argv[1])) { std::fprintf(stderr, "cannot load meta %s\n", argv[1]); return 1; }

    std::ifstream f(argv[2], std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open image %s\n", argv[2]); return 1; }
    const std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (static_cast<int>(img.size()) != meta.configSize()) {
        std::fprintf(stderr, "image is %zu B; this layout is %d B\n", img.size(), meta.configSize());
        return 1;
    }
    const std::vector<uint8_t> doc = TuneFile::serialise(img, meta);
    std::ofstream o(argv[3], std::ios::binary);
    o.write(reinterpret_cast<const char*>(doc.data()), static_cast<std::streamsize>(doc.size()));
    std::printf("wrote %zu B dictionary -> %s\n", doc.size(), argv[3]);
    return 0;
}
