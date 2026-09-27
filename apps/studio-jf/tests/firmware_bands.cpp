// The FIRMWARE's own verdict on a multi-position switch's bands, for test_bands_widget.
//
// A separate translation unit because the firmware's namespace is `pipe`, and POSIX declares a function of
// that name (unistd.h, which the studio's headers pull in) — the two cannot be seen together. This file sees
// only firmware and generated headers and hands back a plain int.
#include "Integration/PipelineBuilder.h"
#include <cstring>

int firmware_multi_switch_bands(const unsigned char* element, unsigned size) {
    SensorConfig c{};
    if (size < sizeof(c)) return -1;
    std::memcpy(&c, element, sizeof(c));
    return pipe::multi_switch_bands(c);
}
