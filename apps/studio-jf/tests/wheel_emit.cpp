// Dump the config pairs the STUDIO would write for a named wheel, taken through the designer.
//
// The bench can then apply exactly those bytes and ask the ECU whether it decodes — which closes the
// gap between "the model emits the right numbers" and "the ECU syncs to what the studio sends". The
// wheel goes through decodeWheel/buildWheel first, because that is the path a user's edit takes and
// it is where `repeats` and the crank-that-does-not-exist were previously lost.
//
//   ./build/wheel_emit <meta.json> "<wheel name>"
#include "TriggerWheel.h"

#include <j/config/Json.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: wheel_emit <meta> <wheel name>\n"); return 2; }
    std::ifstream f(argv[1], std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() > 4) raw.resize(raw.size() - 4);          // the meta carries a trailing CRC
    for (const Wheel& w : wheelsFromMeta(jf::JJson::parse(raw))) {
        if (w.name != argv[2]) continue;
        for (const auto& pr : wheelPairs(buildWheel(decodeWheel(w))))
            std::printf("%s=%.6g\n", pr.first.c_str(), pr.second);
        return 0;
    }
    std::fprintf(stderr, "no such wheel: %s\n", argv[2]);
    return 1;
}
