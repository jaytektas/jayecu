// Write a firmware image to an ECU that is waiting in its DFU bootloader, with the studio's own DFU
// engine (comms/Dfu.h) — erase what the image covers, write, verify, leave. The recovery path for an
// update that was interrupted, and a way to exercise the engine without the studio around it.
//
//   cmake --build build --target dfu_write && ./build/dfu_write <image.bin>
//
#include "../src/comms/Dfu.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: dfu_write <image.bin>\n"); return 2; }
    std::ifstream f(argv[1], std::ios::binary);
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (image.empty()) { std::printf("cannot read %s\n", argv[1]); return 1; }
    std::string err;
    auto dev = dfu::openDevice(err);
    if (!dev) { std::printf("open failed: %s\n", err.c_str()); return 1; }
    std::printf("device: %s, %u-byte blocks; writing %zu bytes\n", dev->layoutString().c_str(), dev->transferSize(), image.size());
    std::atomic<bool> cancel{ false };
    std::string last;
    const bool ok = dfu::flash(*dev, image, dfu::kFlashBase, [&](const dfu::Progress& p) {
        if (p.stage != last) { std::printf("\n%s ", p.stage.c_str()); last = p.stage; }
        std::printf("%d%% ", p.percent); std::fflush(stdout);
    }, cancel, err);
    std::printf("\n%s\n", ok ? "done — the ECU is starting its new firmware" : ("FAILED: " + err).c_str());
    return ok ? 0 : 1;
}
